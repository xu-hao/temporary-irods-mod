// =-=-=-=-=-=-=-
// irods includes
#include "msParam.hpp"
#include "reGlobalsExtern.hpp"
#include "generalAdmin.hpp"
#include "miscServerFunct.hpp"

// =-=-=-=-=-=-=-
#include "irods_resource_plugin.hpp"
#include "irods_file_object.hpp"
#include "irods_physical_object.hpp"
#include "irods_collection_object.hpp"
#include "irods_string_tokenize.hpp"
#include "irods_hierarchy_parser.hpp"
#include "irods_resource_redirect.hpp"
#include "irods_stacktrace.hpp"
#include "irods_re_plugin.hpp"

// =-=-=-=-=-=-=-
// stl includes
#include <iostream>
#include <sstream>
#include <vector>
#include <string>

// =-=-=-=-=-=-=-
// boost includes
#include <boost/lexical_cast.hpp>
#include <boost/function.hpp>
#include <boost/any.hpp>

#include "configuration.hpp"
#include "irods_server_properties.hpp"
#include "zmq.hpp"
#include "irods_error.hpp"
#include <unistd.h>

template<typename L, typename R>
class Either {
    Either(bool l): isLeft(l){} 
    bool isLeft;
};

template<typename L, typename R>
class Left : public Either< L,  R> {
    Left(L left) : Either<L,R>(true), left(left) {}
    L left;
};

template<typename L, typename R>
class Right : public Either< L,  R> {
    Right(R right) : Either<L,R>(false), right(right) {}
    R right;
};

template<typename L, typename R, typename LF, typename RF>
decltype(lf(std::declval<L>())) elim(Either<L, R> a, LF lf, RF rf) {
    return a.isLeft ?
        lf(dynamic_cast<Left <L,R> >(a).left) :
        rf(dynamic_cast<Right<L,R> >(a).right);
}

template<typename Key, typename Ret, typename ... Ts>
Error<Ret> match(Key val0, Key val1, std::function<Error<Ret>() f, Ts... ts>) {
    if(val0 == val1) {
        return f;
    } else {
        return match<Key
    }
}

template<typename Key, typename Ret>
Error<Ret> match(Key val0) {
    return failure<Ret>( Error (-1, std::string("unexpected key ") ++ val0 ) );
}

template<typename T>
using Error = Either<irods::error, T>;

template<typename T>
using Success = Right<irods::error, T>;

template<typename T>
using failure = Left<irods::error, T>;

template<typename T>
Success<T> success(T t) {
    return Success<T>(t);
}

zmq::context_t context(1);
zmq::socket_t sock (context, ZMQ_REQ);
Error<Json::Value> parse_reply(std::string reply_str) {
    Json::Value root;   // will contains the root value after parsing.
    Json::Reader reader;
    std::stringstream ss;
    bool parsingSuccessful = reader.parse( reply_str, root );
    if ( !parsingSuccessful ) {
        // report to the user the failure and their locations in the document.
        ss  << "Failed to parse configuration\n"
               << reader.getFormattedErrorMessages();
        return failure<Json::Value>( ERROR(-1, ss.str()) );
    } else {
        return success( ss.str() );
    }
};

Error<std::string> compose_request(Json::Value root) {
    Json::StyledWriter writer;
    std::string output = writer.write( root );
    return success(output);
};

Error<irods::unit> send_zmq(std::string str) {
    zmq::message_t request((void*)str.c_str(), str.length(), NULL);
    rodsLog (LOG_NOTICE, "Sending %s", str.c_str());
    sock.send ( &request, 0);
    return success(irods::unit());
};


Error<std::string> recv_zmq (irods::unit) {
    zmq::message_t reply;
    sock.recv ( &reply, 0);
    return success( std::string (reinterpret_cast<char *>( reply.data()), reply.size()) );
};

template<typename U, typename S, typename T>
std::function<Error<T>(U)> operator|(std::function<Error<S>(U)> f, std::function<Error<T>(S)> g) {
    return [f, g](U u) {
        auto fu = f(u);
        return elim(fu,
            [&fu](irods::error) { return fu; },
            [&g](S s) { return g(s); }
        );
    };
    
}

auto send_json = compose_request | send_zmq;

auto recv_json = recv_zmq | parse_reply;


Error<irods::unit> m_rule_exists(irods::default_re_ctx&, std::string _rn, bool& _ret) {
    Json::value root;
    root["cmd"] = "rule_exists";
    root["rn"] = _rn;
    
    auto action = send_json | recv_json | [&_ret] (Json::Value root) -> Error<bool> {
        match(root["status"], "error", [&root] () {
            return failure<json::unit>( ERROR( root["errorcode"].asInt(), root["errormsg"].asCString()) );
        }, "success", [&_ret, &root] () {
            _ret = root["ret"].asBool();
            return success( irods::unit() );
        });
    };
    
    return action(root);
}


Error<irods::unit> m_exec_rule = (irods::default_re_ctx&, std::string _rn, std::list<boost::any>& _ps, irods::callback _eff_hdlr) {
    
    Json::value root;
    root["cmd"] = "exec_rule";
    root["rn"] = _rn;
    
    int i = 0;
    for (auto itr = begin (_ps) ; itr != end (_ps); ++ itr) {
        if (itr->type() == typeid(std::string)) {
            std::string str = boost::any_cast<std::string>(*itr);
            root["ps"][i] = str;
        } else if (itr->type() == typeid(std::string *)) {
            std::string *str = boost::any_cast<std::string *>(*itr);
            root["ps"][i] = *str;
        } else {
            root["ps"][i] = "<unconvertible>";
        }
        i++;
    }
    
    std::function<Error<irods::unit>(Json::Value)> cycle = send_json | recv_json | [&cycle] (Json::Value root) {
        return match(root["cmd"], "callback", [&root]() {
            std::list<boost::any> ps;
            std::list<std::string> pss;
            for(i =0;i<root["ps"].size();i++) {
                pss.push_back(root["ps"]);
                ps.push_back(boost::any(&pss.back()));
            }
                    
            irods::error err = _eff_hdlr(root["rn"], irods::unpack(ps));
            if(err.ok()) {
                root["status"] = "success";
                i = 0;
                for(auto itr = begin(pss); itr != end(pss); ++itr) {
                    root["ps"][i] = *itr;
                    i++;
                }
            } else {
                root["status"] = "error";
                root["errorcode"] = err.code();
                root["errormsg"] = err.result();
            }
                    
            return cycle ( root);
        }, "done", [&root] {
        
            return match(root["status"], "success", [&root]() {
                i = 0;
                for (auto itr = begin (_ps) ; itr != end (_ps); ++ itr) {
                    
                    if (itr->type() == typeid(std::string *)) {
                        std::string *str = boost::any_cast<std::string *>(*itr);
                        if(root["ps"].size() != _ps.size()) {
                            std::stringstream ss2;
                            ss2 <<"wrong reply size expected "<< _ps.size() <<", encountered "<< root["ps"].size();
                            return failure<irods::unit>( ERROR(-1, ss2.str() ) );
                        }
                        *str = root["ps"][i].asCString();
                    }
                    i++;
                }
            
                return success(irods::unit);

            }, "error", [&root]() {
                return failure<irods::unit>( ERROR( root["errorcode"].asInt(), root["errormsg"].asCString() ) );
            });
        });
    };
    
    return cycle(root);
    
};
    
    
extern "C" {
            
    irods::error start(irods::default_re_ctx& _u) {
        sock.connect("ipc:///tmp/irods/plugins/re/test");
        
        return SUCCESS();
    }
    
    irods::error stop(irods::default_re_ctx& _u) {
        sock.close();
        
        return SUCCESS();
        
    }
    
    irods::error rule_exists(irods::default_re_ctx& _ctx, std::string _rn, bool& _ret) {
        return elim( m_rule_exists(_ctx, _rn, _ret),
              [](irods::error e) {
                  return PASS(e);
              },
              [](irods::unit) {
                  return SUCCESS();
              });
    }
    
    irods::error exec_rule(irods::default_re_ctx& _ctx, std::string _rn, std::list<boost::any> &_ps, irods::callback _eff_hdlr) {
        
        return elim( m_exec_rule(_ctx, _rn, _ps, _eff_hdlr),
              [](irods::error e) {
                  return PASS(e);
              },
              [](irods::unit) {
                  return SUCCESS();
              });
    }
    
    irods::pluggable_rule_engine<irods::default_re_ctx>* plugin_factory( const std::string& _inst_name,
                                     const std::string& _context ) {
        irods::pluggable_rule_engine<irods::default_re_ctx>* re = new irods::pluggable_rule_engine<irods::default_re_ctx>( _inst_name , _context);
        return re;

    }

}; // extern "C"




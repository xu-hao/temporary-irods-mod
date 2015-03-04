#ifdef IRODS_RE_PLUGIN_HPP

// =-=-=-=-=-=-=-
// Boost Includes
#include <boost/shared_ptr.hpp>
#include <boost/any.hpp>

// =-=-=-=-=-=-=-
#include "irods_plugin_base.hpp"
#include "irods_lookup_table.hpp"
#include "irods_plugin_context.hpp"
#include "irods_error.hpp"
#include "irods_operation_rule_execution_manager.hpp"
#include "irods_operation_rule_execution_manager_no_op.hpp"

// =-=-=-=-=-=-=-
// irods includes
#include "reGlobalsExtern.hpp"
#include "dataObjInpOut.hpp"
#else
#ifndef ___IRODS_OPERATION_WRAPPER_HPP__
#define ___IRODS_OPERATION_WRAPPER_HPP__

// =-=-=-=-=-=-=-
// STL Includes
#include <string>
#include <utility>

// =-=-=-=-=-=-=-
// Boost Includes
#include <boost/shared_ptr.hpp>
#include <boost/any.hpp>

// =-=-=-=-=-=-=-
#include "irods_plugin_base.hpp"
#include "irods_lookup_table.hpp"
#include "irods_plugin_context.hpp"
#include "irods_error.hpp"
#include "irods_operation_rule_execution_manager.hpp"
#include "irods_operation_rule_execution_manager_no_op.hpp"
#include "rcMisc.h"

// =-=-=-=-=-=-=-
// irods includes
#include "reGlobalsExtern.hpp"
#include "dataObjInpOut.h"

#include <iostream>

namespace irods {
// =-=-=-=-=-=-=-
// const signifying failure of operation call for post dynPEP
    static std::string OP_FAILED( "OPERATION_FAILED" );

// =-=-=-=-=-=-=-
    /**
     * \author Jason M. Coposky
     * \brief
     *
     **/
    class operation_wrapper {
        public:
            // =-=-=-=-=-=-=-
            /// @brief constructors
            operation_wrapper( );
            operation_wrapper(
                oper_rule_exec_mgr_ptr, // rule exec mgr
                const std::string&,     // instance name
                const std::string&,     // operation name
                plugin_operation );     // fcn ptr to loaded operation

            // =-=-=-=-=-=-=-
            /// @brief destructor
            virtual ~operation_wrapper();

            // =-=-=-=-=-=-=-
            /// @brief copy constructor - necessary for stl containers
            operation_wrapper( const operation_wrapper& _rhs );

            // =-=-=-=-=-=-=-
            /// @brief assignment operator - necessary for stl containers
            operation_wrapper& operator=( const operation_wrapper& _rhs );

           template< typename... T1 >
            error call(
                plugin_context& _ctx,
                T1            ... _t1 ) {
                if ( operation_ ) {
                    // =-=-=-=-=-=-=-
                    // get vars from fco
                    keyValPair_t kvp;
                    bzero( &kvp, sizeof( kvp ) );
                    _ctx.fco()->get_re_vars( kvp );
                    
                    ruleExecInfo_t rei;
                    memset( ( char* )&rei, 0, sizeof( ruleExecInfo_t ) );
                    // rei.rsComm        = _comm;
                    rei.condInputData = &kvp; // give rule scope to our key value pairs
        

                    // =-=-=-=-=-=-=-
                    // call the pep-rule for this op
                    dynamic_operation_execution_manager<default_re_ctx, default_ms_ctx, DONT_AUDIT_RULE> rex_mgr (std::shared_ptr<rule_engine_context_manager<default_re_ctx, default_ms_ctx, DONT_AUDIT_RULE> > (new rule_engine_context_manager<default_re_ctx, default_ms_ctx, DONT_AUDIT_RULE >(global_re_mgr, &rei)));
                    error op_err = rex_mgr.call(instance_name_, operation_name_, std::function<error(plugin_context&, T1...)>(operation_), _ctx, _t1...);

                    

                    // =-=-=-=-=-=-=-
                    // clean up kvp struct
                    clearKeyVal( &kvp );

                    return op_err;

                }
                else {
                    return ERROR( NULL_VALUE_ERR, "null resource operation." );
                }

            } // operator() - T1


        private:
            /// =-=-=-=-=-=-=-
            /// @brief instance name used for calling rules
            std::string instance_name_;
            /// =-=-=-=-=-=-=-
            /// @brief name of this operation, use for calling rules
            std::string operation_name_;
            /// =-=-=-=-=-=-=-=-=-=-=-=-=-=-
            /// @brief function pointer to actual operation
            plugin_operation operation_;
            ///

    }; // class operation_wrapper

}; // namespace irods

#endif // __IRODS_OPERATION_WRAPPER_HPP__



#endif
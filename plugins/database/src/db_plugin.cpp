// =-=-=-=-=-=-=-
// irods includes
#include "rodsDef.h"
#include "authenticate.h"
#include "rodsQuota.h"
#include "msParam.h"
#include "rcConnect.h"
#include "icatStructs.hpp"
#include "icatHighLevelRoutines.hpp"
#include "mid_level.hpp"
#include "low_level.hpp"

// =-=-=-=-=-=-=-
// new irods includes
#include "irods_database_plugin.hpp"
#include "irods_database_constants.hpp"
#include "irods_postgres_object.hpp"
#include "irods_stacktrace.hpp"
#include "irods_catalog_properties.hpp"
#include "irods_sql_logger.hpp"
#include "irods_hierarchy_parser.hpp"
#include "irods_children_parser.hpp"
#include "irods_auth_object.hpp"
#include "irods_pam_auth_object.hpp"
#include "irods_auth_factory.hpp"
#include "irods_auth_plugin.hpp"
#include "irods_auth_manager.hpp"
#include "irods_auth_constants.hpp"
#include "irods_server_properties.hpp"
#include "irods_resource_manager.hpp"
#include "irods_virtual_path.hpp"
#include "modAccessControl.h"
#include "checksum.hpp"

// =-=-=-=-=-=-=-
// irods includes
#include "rods.h"
#include "rcMisc.h"
#include "miscServerFunct.hpp"

// =-=-=-=-=-=-=-
// stl includes
#include <sstream>
#include <string>
#include <iostream>
#include <vector>
#include <boost/regex.hpp>
#include <boost/lexical_cast.hpp>

#include "Plugin_stub.h"
#include "PluginGen_stub.h"
#include "Local_stub.h"
#include "TCP_stub.h"
#include "UnixDomainSocket_stub.h"

#include "irods_lexical_cast.hpp"

using leaf_bundle_t = irods::resource_manager::leaf_bundle_t;
extern irods::resource_manager resc_mgr;

extern int get64RandomBytes( char *buf );
extern int icatApplyRule( rsComm_t *rsComm, char *ruleName, char *arg1 );

static char prevChalSig[200]; /* a 'signature' of the previous
                          challenge.  This is used as a sessionSignature on the ICAT server
                          side.  Also see getSessionSignatureClientside function. */


//   Legal values for accessLevel in  chlModAccessControl (Access Parameter).
//   Defined here since other code does not need them (except for help messages)
#define AP_READ "read"
#define AP_WRITE "write"
#define AP_OWN "own"
#define AP_NULL "null"

static rodsLong_t MAX_PASSWORDS = 40;
/* TEMP_PASSWORD_TIME is the number of seconds the temporary, one-time
   password can be used.  chlCheckAuth also checks for this column
   to be < TEMP_PASSWORD_MAX_TIME (1000) to differentiate the row
   from regular passwords and PAM passwords.
   This time, 120 seconds, should be long enough to give the iDrop and
   iDrop-lite applets enough time to download and go through their
   startup sequence.  iDrop and iDrop-lite disconnect when idle to
   reduce the number of open connections and active agents.  */

#define PASSWORD_SCRAMBLE_PREFIX ".E_"
#define PASSWORD_KEY_ENV_VAR "IRODS_DATABASE_USER_PASSWORD_SALT"
#define PASSWORD_DEFAULT_KEY "a9_3fker"

#define MAX_HOST_STR 2700

// =-=-=-=-=-=-=-
// local variables externed for config file setting in
bool irods_pam_auth_no_extend = false;
size_t irods_pam_password_len = 20;
char irods_pam_password_min_time[ NAME_LEN ]     = { "121" };
char irods_pam_password_max_time[ NAME_LEN ]     = { "1209600" };
char irods_pam_password_default_time[ NAME_LEN ] = { "1209600" };

size_t log_sql_flg = 0;
void* icss; // JMC :: only for testing!!!
void* svc;
int icss_status = 0;
extern int logSQL;

int  creatingUserByGroupAdmin; // JMC - backport 4772
char mySessionTicket[NAME_LEN];
char mySessionClientAddr[NAME_LEN];

// =-=-=-=-=-=-=-
// property constants
const std::string ICSS_PROP( "irods_icss_property" );
const std::string ZONE_PROP( "irods_zone_property" );

// =-=-=-=-=-=-=-
// virtual path management
#define PATH_SEPARATOR irods::get_virtual_path_separator().c_str()

/*
   Parse the input fullUserNameIn into an output userName and userZone
   and check that the username is a valid format, meaning at most one
   '@' and at most one '#'.
   Full userNames are of the form user@department[#zone].
   It is assumed the output strings are at least NAME_LEN characters long
   and the input string is at most that long.
 */
int
validateAndParseUserName( const char *fullUserNameIn, char *userName, char *userZone ) {
    const std::string input( fullUserNameIn );
    boost::smatch matches;
    // This regex matches usernames with no hashes and optionally one at symbol,
    // and then optionally a hash followed by a zone name containing no hashes.
    //
    // Username must be between 1 and NAME_LEN-1 characters.
    // Username may contain any combination of word characters, @ symbols, dashes, and dots.
    // Username may not be . or .., as we create home directories for users
    const boost::regex expression( "((\\w|[-.@])+)(#([^#]*))?" );
    try {
        const bool matched = boost::regex_match( input, matches, expression );
        if ( !matched || matches.str( 1 ).size() >= NAME_LEN ||
                matches.str( 1 ).size() < 1 ||
                matches.str( 4 ).size() >= NAME_LEN ||
                matches.str( 1 ) == "." ||
                matches.str( 1 ) == ".." ) {
            if ( userName != NULL ) {
                userName[0] = '\0';
            }
            if ( userZone != NULL ) {
                userZone[0] = '\0';
            }
            return USER_INVALID_USERNAME_FORMAT;
        }
        if ( userName != NULL ) {
            snprintf( userName, NAME_LEN, "%s", matches.str( 1 ).c_str() );
        }
        if ( userZone != NULL ) {
            snprintf( userZone, NAME_LEN, "%s", matches.str( 4 ).c_str() );
        }
    }
    catch ( const boost::exception& ) {
        return SYS_INTERNAL_ERR;
    }
    return 0;
}

// =-=-=-=-=-=-=-
// helper fcn to handle cast to pg object
irods::error make_db_ptr(
    const irods::first_class_object_ptr& _fc,
    irods::postgres_object_ptr&          _pg ) {
    if ( !_fc.get() ) {
        return ERROR(
                   SYS_INVALID_INPUT_PARAM,
                   "incoming fco is null" );

    }

    _pg = boost::dynamic_pointer_cast <
          irods::postgres_object > (
              _fc );

    if ( _pg.get() ) {
        return SUCCESS();

    }
    else {
        return ERROR(
                   INVALID_DYNAMIC_CAST,
                   "failed to dynamic cast to postgres_object_ptr" );
    }

} // make_db_ptr

// =-=-=-=-=-=-=-
//  Called internally to rollback current transaction after an error.
int _rollback( const char *functionName ) {
    // =-=-=-=-=-=-=-
    // This type of rollback is needed for Postgres since the low-level
    // now does an automatic 'begin' to create a sql block */
    int status =  hs_rollback(svc,  icss );
    if ( status == 0 ) {
        rodsLog( LOG_NOTICE,
                 "%s cmlExecuteNoAnswerSql(rollback) succeeded", functionName );
    }
    else {
        rodsLog( LOG_NOTICE,
                 "%s cmlExecuteNoAnswerSql(rollback) failure %d",
                 functionName, status );
    }

    return status;

} // _rollback

// =-=-=-=-=-=-=-
//  Internal function to return the local zone (which is the default
//  zone).  The first time it's called, it gets the zone from the DB and
//  subsequent calls just return that value.
irods::error getLocalZone(
    irods::plugin_property_map& _prop_map,
    void*          _icss,
    std::string&                _zone ) {
    // =-=-=-=-=-=-=-
    // try to get the zone prop, if it is not cached
    // then we hit the catalog and request it
    irods::error ret = _prop_map.get< std::string >( ZONE_PROP, _zone );
    if ( !ret.ok() ) {
        char local_zone[ MAX_NAME_LEN ];
        int status;
        status =
            hs_get_local_zone(svc, _icss, local_zone, MAX_NAME_LEN);

        if ( status != 0 ) {
            _rollback( "getLocalZone" );
            return ERROR( status, "getLocalZone failure" );
        }

        // =-=-=-=-=-=-=-
        // set the zone property
        _zone = local_zone;
        ret = _prop_map.set< std::string >( ZONE_PROP, _zone );
        if ( !ret.ok() ) {
            return PASS( ret );

        }

    } // if no zone prop

    return SUCCESS();

} // getLocalZone

// =-=-=-=-=-=-=-
// @brief query for object found of a resource
int get_object_count_of_resource_by_name(
    void* _icss,
    const std::string& _resc_name,
    rodsLong_t&         _count ) {

    rodsLong_t resc_id;
    irods::error ret = resc_mgr.hier_to_leaf_id(
                         _resc_name,
                         resc_id);
    if(!ret.ok()) {
        // if we have a bad resource in the database we need
        // to ignore this in order to still remove it
        if(SYS_RESC_DOES_NOT_EXIST == ret.code()) {
            _count = 0;
            return 0;
        }
        irods::log(PASS(ret));
        return ret.code();
    }

    std::string resc_id_str;
    ret = irods::lexical_cast<std::string>(resc_id, resc_id_str);
    if(!ret.ok()) {
        irods::log(PASS(ret));
        return ret.code();
    }

    int status = hs_get_int_num_data_by_resc(svc, _icss, (void *) resc_id_str.c_str(),
                     &_count);

    return status;

} // get_object_count_of_resource_by_name

// =-=-=-=-=-=-=-
// @brief determine if user had write permission to data object
irods::error determine_user_has_modify_metadata_access(
    const std::string& _data_name,
    const std::string& _collection,
    const std::string& _user_name,
    const std::string& _zone ) {

    int status = 0;

    rodsLog(
        LOG_DEBUG,
        "%s :: [%s] [%s] [%s] [%s]",
        __FUNCTION__,
        _data_name.c_str(),
        _collection.c_str(),
        _user_name.c_str(),
        _zone.c_str() );

    // get the number of data object to which this will apply
    rodsLong_t num_data_objects = -1;
    {
        status = hs_get_int_num_data_by_name(svc,
					  icss,
					  (void *) _collection.c_str(),
					  (void *) _data_name.c_str(),
            &num_data_objects);
        if( 0 != status ) {
            _rollback( "chlAddAVUMetadataWild" );
            return ERROR(
                       status,
                       "failed to get object count" );
        }

        if( 0 == num_data_objects ) {
            std::string msg = "no data objects found for collection ";
            msg += _collection;
            msg += " and object name ";
            msg += _data_name;
            _rollback( "chlAddAVUMetadataWild" );
            return ERROR(
                       CAT_NO_ROWS_FOUND,
                       msg );
        }
    }

    // get the baseline 'access needed' value from the token table
    rodsLong_t access_needed = -1;
    {
        int tokenid;
        int status = hs_get_int_token_id(svc, icss, (void *) "access_type", (void *) "modify metadata", &tokenid);
        if( status < 0 ) {
            return ERROR(
                       status,
                       "query for modify metadata token_id failed" );
        }
    }

    // reproduce the creation of access permission entries
    // of "ACCESS_VIEW_ONE" and "ACCESS_VIEW_TWO"
    rodsLong_t access_permission = -1;
    {
        status = hs_get_int_access_permission_use_wildcard(svc, icss, (void *) _user_name.c_str(), (void *) _zone.c_str(), (void *) _collection.c_str(), (void *) _data_name.c_str(), &access_permission);
        if ( status == CAT_NO_ROWS_FOUND ) {
            _rollback( "chlAddAVUMetadataWild" );
            return ERROR(
                       CAT_NO_ACCESS_PERMISSION,
                       "access denied" );
        }

        if ( access_permission < access_needed ) {
            _rollback( "chlAddAVUMetadataWild" );
            return ERROR(
                       CAT_NO_ACCESS_PERMISSION,
                       "access denied" );
        }
    }

    // reproduce the count of access permission entries in "ACCESS_VIEW_TWO"
    rodsLong_t access_permission_count = -1;
    {
        status = hs_get_int_num_access_permission_use_wildcard(svc, icss, (void *) _user_name.c_str(), (void *) _zone.c_str(), (void *) _collection.c_str(), (void *) _data_name.c_str(), &access_permission_count);
        if( 0 != status ) {
            _rollback( "chlAddAVUMetadataWild" );
            return ERROR(
                       status,
                       "query for access permission count failed" );
        }

        if( num_data_objects > access_permission_count ) {
            std::stringstream msg;
            msg << "access denined - num_data_objects "
                << num_data_objects
                << " > access_permission_count "
                << access_permission_count;
            _rollback( "chlAddAVUMetadataWild" );
            return ERROR(
                       CAT_NO_ACCESS_PERMISSION,
                       msg.str() );
        }
    }

    // return number of data objects, keeping the semantics of the
    // original imeta addw operation
    return CODE( num_data_objects );

} // user_has_modify_metadata_access


/*
 * removeMetaMapAndAVU - remove AVU (user defined metadata) for an object,
 *   the metadata mapping information, if any.  Optionally, also remove
 *   any unused AVUs, if any, if some mapping information was removed.
 *
 */
void removeMetaMapAndAVU( char *dataObjNumber ) {
    // char tSQL[MAX_SQL_SIZE];
    int status;
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "removeMetaMapAndAVU SQL 1 " );
    }
    status = hs_delete_metamap_by_obj_id(svc, icss, dataObjNumber);

    /* Note, the status will be CAT_SUCCESS_BUT_WITH_NO_INFO (not 0) if
       there were no rows deleted from R_OBJT_METAMAP, in which case there
       is no need to do the SQL below.
    */

    return;
}

/*
 * removeAVUs - remove unused AVUs (user defined metadata), if any.
 */
static int removeAVUs() {
    // char tSQL[MAX_SQL_SIZE];

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "removeAVUs SQL 1 " );
    }

    const int status =  hs_delete_avus(svc,  icss );
    rodsLog( LOG_DEBUG, "removeAVUs status=%d\n", status );

    return status;
}

int
_canConnectToCatalog(
    rsComm_t* _rsComm ) {
    int result = 0;
    if ( !icss_status ) {
        result = CATALOG_NOT_CONNECTED;
    }
    else if ( _rsComm->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        result = CAT_INSUFFICIENT_PRIVILEGE_LEVEL;
    }
    else if ( _rsComm->proxyUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        result = CAT_INSUFFICIENT_PRIVILEGE_LEVEL;
    }
    return result;
}

int
_resolveHostName(
    rsComm_t* _rsComm,
    const char* _hostAddress,
    struct hostent *& _hostEnt ) {

    const int status = gethostbyname_with_retry( _hostAddress, &_hostEnt );

    if ( status != 0 ) {
        char errMsg[155];
        snprintf( errMsg, 150,
                  "Warning, resource host address '%s' is not a valid DNS entry, gethostbyname failed.",
                  _hostAddress );
        addRErrorMsg( &_rsComm->rError, 0, errMsg );
    }
    if ( strcmp( _hostAddress, "localhost" ) == 0 ) {
        addRErrorMsg( &_rsComm->rError, 0,
                      "Warning, resource host address 'localhost' will not work properly as it maps to the local host from each client." );
    }

    return 0;
}

// =-=-=-=-=-=-=-
//
irods::error _childIsValid(
    irods::plugin_property_map& _prop_map,
    const std::string&          _new_child ) {
    // =-=-=-=-=-=-=-
    // Lookup the child resource and make sure its parent field is empty
    char parent[MAX_NAME_LEN];
    int status;

    // Get the resource name from the child string
    std::string resc_name;
    irods::children_parser parser;
    parser.set_string( _new_child );
    parser.first_child( resc_name );

    std::string zone;
    irods::error ret = getLocalZone( _prop_map, icss, zone );
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // Get resource's parent
    irods::sql_logger logger( "_childIsValid", logSQL );
    logger.log();
    parent[0] = '\0';
    {
        status = hs_get_resc_parent_by_zone_and_name(svc, icss, (void *) zone.c_str(), (void *) resc_name.c_str(), parent, MAX_NAME_LEN);
    }
    if ( status != 0 ) {
        if ( status == CAT_NO_ROWS_FOUND ) {
            std::stringstream ss;
            ss << "Child resource \"" << resc_name << "\" not found";
            irods::log( LOG_NOTICE, ss.str() );
            return ERROR( CHILD_NOT_FOUND, "child resource not found" );
        }
        else {
            _rollback( "_childIsValid" );
            return ERROR( status, "error encountered in query for _childIsValid" );
        }
    }
    else if ( strlen( parent ) != 0 ) {
        // If the resource already has a parent it cannot be added as a child of another one
        std::stringstream ss;
        ss << "Child resource \"" << resc_name << "\" already has a parent \"" << parent << "\"";
        irods::log( LOG_NOTICE, ss.str() );
        return ERROR( CHILD_HAS_PARENT, "child resource already has a parent" );
    }
    return SUCCESS();
}

irods::error _updateChildParent(
    const std::string& _child_resc_id,
    const std::string& _parent_resc_id,
    const std::string& _parent_child_context ) {
    irods::sql_logger logger( "_updateChildParent", logSQL );

    // Update the parent for the child resource
    // have to do this to get around const
    char myTime[50];
    getNowStr( myTime );
    cllBindVarCount = 0;
    logger.log();

    int status = hs_create_resc_parent_ts(svc,
								icss,
								(void *) _child_resc_id.c_str(),
								(void *) _parent_resc_id.c_str(),
								(void *) _parent_child_context.c_str(),
								(void *) myTime
			                     );
    if( status != 0 ) {
        _rollback( "_updateChildParent" );
        return ERROR( status, "cmlExecuteNoAnswerSql failed" );
    }

    return SUCCESS();

} // _updateChildParent

/**
 * @brief Returns true if the specified resource has associated data objects
 */
int
_rescHasData(
    void* _icss,
    const std::string& _resc_name,
    bool&              _has_data ) {
    irods::sql_logger logger( "_rescHasData", logSQL );
    rodsLong_t obj_count;

    logger.log();

    int status = get_object_count_of_resource_by_name(
                      _icss,
                      _resc_name,
                      obj_count );
    if( 0 == status ) {
        if ( 0 == obj_count ) {
            _has_data = false;
        }
    }

    return status;
}

/// @brief function for validating a resource name
irods::error validate_resource_name( std::string _resc_name ) {

    // Must be between 1 and NAME_LEN-1 characters.
    // Must start and end with a word character.
    // May contain non consecutive dashes.
    boost::regex re( "^(?=.{1,63}$)\\w+(-\\w+)*$" );

    if ( !boost::regex_match( _resc_name, re ) ) {
        std::stringstream msg;
        msg << "validate_resource_name failed for resource [";
        msg << _resc_name;
        msg << "]";
        return ERROR( SYS_INVALID_INPUT_PARAM, msg.str() );
    }

    return SUCCESS();

} // validate_resource_name

bool
_rescHasParentOrChild( char* rescId ) {

    char parent[MAX_NAME_LEN];
    char children[MAX_NAME_LEN];
    int status;
    irods::sql_logger logger( "_rescHasParentOrChild", logSQL );

    logger.log();
    parent[0] = '\0';
    children[0] = '\0';
    {
        status = hs_get_resc_parent(svc,
					 icss,
                     (void *) rescId,
                     parent, MAX_NAME_LEN);
    }
    if ( status != 0 ) {
        if ( status == CAT_NO_ROWS_FOUND ) {
            std::stringstream ss;
            ss << "Resource \"" << rescId << "\" not found";
            irods::log( LOG_NOTICE, ss.str() );
        }
        else {
            _rollback( "_rescHasParentOrChild" );
        }
        return false;
    }
    if ( strlen( parent ) != 0 ) {
        return true;
    }
    {
        status = hs_get_resc_child(svc,
						icss,
						rescId,
                     	children, MAX_NAME_LEN);
    }
    if ( status != 0 ) {
        if ( status != CAT_NO_ROWS_FOUND ) {
            _rollback( "_rescHasParentOrChild" );
        }
        return false;
    }
    if ( strlen( children ) != 0 ) {
        return true;
    }
    return false;

}

// =-=-=-=-=-=-=-
/// @brief function which determines if a char is allowed in a zone name
static bool allowed_zone_char( const char _c ) {
    return ( !std::isalnum( _c ) &&
             !( '.' == _c )      &&
             !( '_' == _c ) );
} // allowed_zone_char

// =-=-=-=-=-=-=-
/// @brief function for validating the name of a zone
irods::error validate_zone_name(
    std::string _zone_name ) {
    std::string::iterator itr = std::find_if( _zone_name.begin(),
                                _zone_name.end(),
                                allowed_zone_char );
    if ( itr != _zone_name.end() || _zone_name.length() >= NAME_LEN ) {
        std::stringstream msg;
        msg << "validate_zone_name failed for zone [";
        msg << _zone_name;
        msg << "]";
        return ERROR( SYS_INVALID_INPUT_PARAM, msg.str() );
    }

    return SUCCESS();

} // validate_zone_name

/* delCollection (internally called),
   does not do the commit.
*/
static int _delColl( rsComm_t *rsComm, collInfo_t *collInfo ) {
    // rodsLong_t iVal;
    char logicalEndName[MAX_NAME_LEN];
    char logicalParentDirName[MAX_NAME_LEN];
    char collIdNum[MAX_NAME_LEN];
    char parentCollIdNum[MAX_NAME_LEN];
    rodsLong_t status;

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "_delColl" );
    }

    if ( !icss_status ) {
        return CATALOG_NOT_CONNECTED;
    }

    status = splitPathByKey( collInfo->collName,
                             logicalParentDirName, MAX_NAME_LEN, logicalEndName, MAX_NAME_LEN, '/' );

    if ( strlen( logicalParentDirName ) == 0 ) {
        snprintf( logicalParentDirName, sizeof( logicalParentDirName ), "%s", PATH_SEPARATOR );
        snprintf( logicalEndName, sizeof( logicalEndName ), "%s", collInfo->collName + 1 );
    }

    /* Check that the parent collection exists and user has write permission,
       and get the collectionID */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "_delColl SQL 1 " );
    }
    status = cmlCheckDir( logicalParentDirName,
                          rsComm->clientUser.userName,
                          rsComm->clientUser.rodsZone,
                          ACCESS_MODIFY_OBJECT, svc,
                          icss );
    if ( status < 0 ) {
        char errMsg[105];
        if ( status == CAT_UNKNOWN_COLLECTION ) {
            snprintf( errMsg, 100, "collection '%s' is unknown",
                      logicalParentDirName );
            addRErrorMsg( &rsComm->rError, 0, errMsg );
            return status;
        }
        _rollback( "_delColl" );
        return status;
    }
    snprintf( parentCollIdNum, MAX_NAME_LEN, "%lld", status );

    /* Check that the collection exists and user has DELETE or better
       permission */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "_delColl SQL 2" );
    }
    status = cmlCheckDir( collInfo->collName,
                          rsComm->clientUser.userName,
                          rsComm->clientUser.rodsZone,
                          ACCESS_DELETE_OBJECT, svc,
                          icss );
    if ( status < 0 ) {
        return status;
    }
    snprintf( collIdNum, MAX_NAME_LEN, "%lld", status );

    /* check that the collection is empty (both subdirs and files) */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "_delColl SQL 3" );
    }
    {
        char coll[MAX_NAME_LEN];
        status = hs_get_coll_child_by_name(svc, icss, collInfo->collName, coll, MAX_NAME_LEN);
    }
    if ( status != CAT_NO_ROWS_FOUND ) {
        return CAT_COLLECTION_NOT_EMPTY;
    }

    /* delete the row if it exists */
    /* The use of coll_id isn't really needed but may add a little safety.
       Previously, we included a check that it was owned by the user but
       the above cmlCheckDir is more accurate (handles group access). */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "_delColl SQL 4" );
    }
    status = hs_delete_coll_by_name(svc, icss, collInfo->collName);
    if ( status != 0 ) { /* error, odd one as everything checked above */
        rodsLog( LOG_NOTICE,
                 "_delColl cmlExecuteNoAnswerSql delete failure %d",
                 status );
        _rollback( "_delColl" );
        return status;
    }

    /* remove any access rows */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "_delColl SQL 5" );
    }
    status =  hs_delete_access_by_obj_id(svc, icss, collIdNum);
    if ( status != 0 ) { /* error, odd one as everything checked above */
        rodsLog( LOG_NOTICE,
                 "_delColl cmlExecuteNoAnswerSql delete access failure %d",
                 status );
        _rollback( "_delColl" );
    }

    /* Remove associated AVUs, if any */
    removeMetaMapAndAVU( collIdNum );

    return status;

} // _delColl

// Modifies a given resource name in all resource hierarchies (i.e for all objects)
// gets called after a resource has been modified (iadmin modresc <oldname> name <newname>)
static int _modRescInHierarchies( const std::string& old_resc, const std::string& new_resc ) {
    // char update_sql[MAX_SQL_SIZE];
    int status = 0;
    const char *sep = irods::hierarchy_parser::delimiter().c_str();
    std::string std_conf_str;        // to store value of STANDARD_CONFORMING_STRINGS

    // =-=-=-=-=-=-=-
    // SQL update
    status = hs_create_data_resc_hier(svc,  icss, (void *) sep, (void *) old_resc.c_str(), (void *) new_resc.c_str());

    // =-=-=-=-=-=-=-
    // Log error. Rollback is done in calling function
    if ( status < 0 && status != CAT_SUCCESS_BUT_WITH_NO_INFO ) {
        std::stringstream ss;
        ss << "_modRescInHierarchies: cmlExecuteNoAnswerSql update failure, status = " << status;
        irods::log( LOG_NOTICE, ss.str() );
        // _rollback("_modRescInHierarchies");
    }

    return status;
}

// =-=-=-=-=-=-=-
// local function to delegate the response
// verification to an authentication plugin
static
irods::error verify_auth_response(
    const char* _scheme,
    const char* _challenge,
    const char* _user_name,
    const char* _response ) {
    // =-=-=-=-=-=-=-
    // validate incoming parameters
    if ( !_scheme ) {
        return ERROR( SYS_INVALID_INPUT_PARAM, "null _scheme ptr" );
    }
    else if ( !_challenge ) {
        return ERROR( SYS_INVALID_INPUT_PARAM, "null _challenge ptr" );
    }
    else if ( !_user_name ) {
        return ERROR( SYS_INVALID_INPUT_PARAM, "null _user_name ptr" );
    }
    else if ( !_response ) {
        return ERROR( SYS_INVALID_INPUT_PARAM, "null _response ptr" );
    }

    // =-=-=-=-=-=-=-
    // construct an auth object given the scheme
    irods::auth_object_ptr auth_obj;
    irods::error ret = irods::auth_factory( _scheme, 0, auth_obj );
    if ( !ret.ok() ) {
        return ret;
    }

    // =-=-=-=-=-=-=-
    // resolve an auth plugin given the auth object
    irods::plugin_ptr ptr;
    ret = auth_obj->resolve( irods::AUTH_INTERFACE, ptr );
    if ( !ret.ok() ) {
        return ret;
    }
    irods::auth_ptr auth_plugin = boost::dynamic_pointer_cast< irods::auth >( ptr );

    // =-=-=-=-=-=-=-
    // call auth verify on plugin
    ret = auth_plugin->call <const char*, const char*, const char* > ( 0, irods::AUTH_AGENT_AUTH_VERIFY, auth_obj, _challenge, _user_name, _response );
    if ( !ret.ok() ) {
        irods::log( PASS( ret ) );
        return ret;
    }

    return SUCCESS();

} // verify_auth_response

/*
   Possibly descramble a password (for user passwords stored in the ICAT).
   Called internally, from various chl functions.
*/
static int
icatDescramble( char *pw ) {
    char *cp1, *cp2, *cp3;
    int i, len;
    char pw2[MAX_PASSWORD_LEN + 10];
    char unscrambled[MAX_PASSWORD_LEN + 10];

    len = strlen( PASSWORD_SCRAMBLE_PREFIX );
    cp1 = pw;
    cp2 = PASSWORD_SCRAMBLE_PREFIX; /* if starts with this, it is scrambled */
    for ( i = 0; i < len; i++ ) {
        if ( *cp1++ != *cp2++ ) {
            return 0;                /* not scrambled, leave as is */
        }
    }
    snprintf( pw2, sizeof( pw2 ), "%s", cp1 );
    cp3 = getenv( PASSWORD_KEY_ENV_VAR );
    if ( cp3 == NULL ) {
        cp3 = PASSWORD_DEFAULT_KEY;
    }
    obfDecodeByKey( pw2, cp3, unscrambled );
    strncpy( pw, unscrambled, MAX_PASSWORD_LEN );

    return 0;
}

/*
   Scramble a password (for user passwords stored in the ICAT).
   Called internally.
*/
static int
icatScramble( char *pw ) {
    char *cp1;
    char newPw[MAX_PASSWORD_LEN + 10];
    char scrambled[MAX_PASSWORD_LEN + 10];

    cp1 = getenv( PASSWORD_KEY_ENV_VAR );
    if ( cp1 == NULL ) {
        cp1 = PASSWORD_DEFAULT_KEY;
    }
    obfEncodeByKey( pw, cp1, scrambled );
    snprintf( newPw, sizeof( newPw ), "%s%s", PASSWORD_SCRAMBLE_PREFIX, scrambled );
    strncpy( pw, newPw, MAX_PASSWORD_LEN );
    return 0;
}

/*
  de-scramble a password sent from the client.
  This isn't real encryption, but does obfuscate the pw on the network.
  Called internally, from chlModUser.
*/
int decodePw( rsComm_t *rsComm, const char *in, char *out ) {
    int status;
    char *cp;
    char password[MAX_PASSWORD_LEN];
    char upassword[MAX_PASSWORD_LEN + 10];
    char rand[] =
        "1gCBizHWbwIYyWLo";  /* must match clients */
    int pwLen1, pwLen2;

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "decodePw - SQL 1 " );
    }
    {
        status = hs_get_password_by_user_zone_and_name(svc, icss,
					 rsComm->clientUser.rodsZone,
					 rsComm->clientUser.userName,
                     password, MAX_PASSWORD_LEN);
    }
    if ( status != 0 ) {
        if ( status == CAT_NO_ROWS_FOUND ) {
            status = CAT_INVALID_USER; /* Be a little more specific */
        }
        else {
            _rollback( "decodePw" );
        }
        return status;
    }

    icatDescramble( password );

    obfDecodeByKeyV2( in, password, prevChalSig, upassword );

    pwLen1 = strlen( upassword );

    memset( password, 0, MAX_PASSWORD_LEN );

    cp = strstr( upassword, rand );
    if ( cp != NULL ) {
        *cp = '\0';
    }

    pwLen2 = strlen( upassword );

    if ( pwLen2 > MAX_PASSWORD_LEN - 5 && pwLen2 == pwLen1 ) {
        /* probable failure */
        char errMsg[260];
        snprintf( errMsg, 250,
                  "Error with password encoding.  This can be caused by not connecting directly to the ICAT host, not using password authentication (using GSI or Kerberos instead), or entering your password incorrectly (if prompted)." );
        addRErrorMsg( &rsComm->rError, 0, errMsg );
        return CAT_PASSWORD_ENCODING_ERROR;
    }
    strcpy( out, upassword );
    memset( upassword, 0, MAX_PASSWORD_LEN );

    return 0;
}

int
convertTypeOption( const char *typeStr ) {
    if ( strcmp( typeStr, "-d" ) == 0 ) {
        return ( 1 );   /* dataObj */
    }
    if ( strcmp( typeStr, "-D" ) == 0 ) {
        return ( 1 );   /* dataObj */
    }
    if ( strcmp( typeStr, "-c" ) == 0 ) {
        return ( 2 );   /* collection */
    }
    if ( strcmp( typeStr, "-C" ) == 0 ) {
        return ( 2 );   /* collection */
    }
    if ( strcmp( typeStr, "-r" ) == 0 ) {
        return ( 3 );   /* resource */
    }
    if ( strcmp( typeStr, "-R" ) == 0 ) {
        return ( 3 );   /* resource */
    }
    if ( strcmp( typeStr, "-u" ) == 0 ) {
        return ( 4 );   /* user */
    }
    if ( strcmp( typeStr, "-U" ) == 0 ) {
        return ( 4 );   /* user */
    }
    return 0;
}

/*
  Check object - get an object's ID and check that the user has access.
  Called internally.
*/
rodsLong_t checkAndGetObjectId(
    rsComm_t*                   rsComm,
    irods::plugin_property_map& _prop_map,
    const char*                 type,
    const char*                 name,
    const char*                 access ) {
    int itype;
    char logicalEndName[MAX_NAME_LEN];
    char logicalParentDirName[MAX_NAME_LEN];
    rodsLong_t status;
    rodsLong_t objId;
    char userName[NAME_LEN];
    char userZone[NAME_LEN];


    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "checkAndGetObjectId" );
    }

    if ( !icss_status ) {
        return CATALOG_NOT_CONNECTED;
    }

    if ( type == NULL ) {
        return CAT_INVALID_ARGUMENT;
    }

    if ( *type == '\0' ) {
        return CAT_INVALID_ARGUMENT;
    }


    if ( name == NULL ) {
        return CAT_INVALID_ARGUMENT;
    }

    if ( *name == '\0' ) {
        return CAT_INVALID_ARGUMENT;
    }


    itype = convertTypeOption( type );
    if ( itype == 0 ) {
        return CAT_INVALID_ARGUMENT;
    }

    if ( itype == 1 ) {
        status = splitPathByKey( name,
                                 logicalParentDirName, MAX_NAME_LEN, logicalEndName, MAX_NAME_LEN, '/' );
        if ( strlen( logicalParentDirName ) == 0 ) {
            snprintf( logicalParentDirName, sizeof( logicalParentDirName ), "%s", PATH_SEPARATOR );
            snprintf( logicalEndName, sizeof( logicalEndName ), "%s", name );
        }
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "checkAndGetObjectId SQL 1 " );
        }
        status = cmlCheckDataObjOnly( logicalParentDirName, logicalEndName,
                                      rsComm->clientUser.userName,
                                      rsComm->clientUser.rodsZone,
                                      access, svc, icss );
        if ( status < 0 ) {
            _rollback( "checkAndGetObjectId" );
            return status;
        }
        objId = status;
    }

    if ( itype == 2 ) {
        /* Check that the collection exists and user has create_metadata permission,
           and get the collectionID */
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "checkAndGetObjectId SQL 2" );
        }
        status = cmlCheckDir( name,
                              rsComm->clientUser.userName,
                              rsComm->clientUser.rodsZone,
                              access, svc, icss );
        if ( status < 0 ) {
            char errMsg[105];
            if ( status == CAT_UNKNOWN_COLLECTION ) {
                snprintf( errMsg, 100, "collection '%s' is unknown",
                          name );
                addRErrorMsg( &rsComm->rError, 0, errMsg );
            }
            return status;
        }
        objId = status;
    }

    if ( itype == 3 ) {
        if ( rsComm->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
            return CAT_INSUFFICIENT_PRIVILEGE_LEVEL;
        }

        std::string zone;
        irods::error ret = getLocalZone( _prop_map, icss, zone );
        if ( !ret.ok() ) {
            return PASS( ret ).code();
        }

        objId = 0;
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "checkAndGetObjectId SQL 3" );
        }
        {
            status = hs_get_int_resc_id(svc, icss,
                         (void *) zone.c_str(), (void *) name,
                         &objId);
        }
        if ( status != 0 ) {
            if ( status == CAT_NO_ROWS_FOUND ) {
                return CAT_INVALID_RESOURCE;
            }
            _rollback( "checkAndGetObjectId" );
            return status;
        }
    }

    if ( itype == 4 ) {
        if ( rsComm->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
            return CAT_INSUFFICIENT_PRIVILEGE_LEVEL;
        }

        status = validateAndParseUserName( name, userName, userZone );
        if ( status ) {
            return status;
        }
        if ( userZone[0] == '\0' ) {
            std::string zone;
            irods::error ret = getLocalZone( _prop_map, icss, zone );
            if ( !ret.ok() ) {
                return PASS( ret ).code();
            }
            snprintf( userZone, sizeof( userZone ), "%s",  zone.c_str() );
        }

        objId = 0;
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "checkAndGetObjectId SQL 4" );
        }
        {
			status = hs_get_int_user_id(svc, icss,
                         userZone, userName,
                         &objId);
        }
        if ( status != 0 ) {
            if ( status == CAT_NO_ROWS_FOUND ) {
                return CAT_INVALID_USER;
            }
            _rollback( "checkAndGetObjectId" );
            return status;
        }
    }

    return objId;
}

/*
// =-=-=-=-=-=-=-
// JMC - backport 4836
+ Find existing AVU triplet.
+ Return code is error or the AVU ID.
+*/
rodsLong_t
findAVU( const char *attribute, const char *value, const char *units ) {
    rodsLong_t status;
// =-=-=-=-=-=-=-

    rodsLong_t iVal;
    iVal = 0;
    if ( *units != '\0' ) {
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "findAVU SQL 1" );    // JMC - backport 4836
        }
        {
			status = hs_get_int_meta_id(svc, icss, (void *) attribute, (void *) value, (void *) units,
                         &iVal);
        }
    }
    else {
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "findAVU SQL 2" );
        }
        {
            status = hs_get_int_meta_id_by_attribute_and_value(svc, icss,
							(void *) attribute, (void *) value,
	                        &iVal);
        }
    }
    if ( status == 0 ) {
        status = iVal; /* use existing R_META_MAIN row */
        return status;
    }
// =-=-=-=-=-=-=-
// JMC - backport 4836
    return ( status ); // JMC - backport 4836
}

/*
  Find existing or insert a new AVU triplet.
  Return code is error, or the AVU ID.
*/
int
findOrInsertAVU( const char *attribute, const char *value, const char *units ) {
    char nextStr[MAX_NAME_LEN];
    char myTime[50];
    rodsLong_t status, seqNum;
    rodsLong_t iVal;
    iVal = findAVU( attribute, value, units );
    if ( iVal > 0 ) {
        return iVal;
    }
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "findOrInsertAVU SQL 1" );
    }
// =-=-=-=-=-=-=-
    status = hs_get_int_next_id(svc,  icss, &seqNum );
    if ( status < 0 ) {
        rodsLog( LOG_NOTICE, "findOrInsertAVU cmlGetNextSeqVal failure %d",
                 status );
        return status;
    }

    snprintf( nextStr, sizeof nextStr, "%lld", seqNum );

    getNowStr( myTime );



    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "findOrInsertAVU SQL 2" );    // JMC - backport 4836
    }
    status =  hs_create_meta(svc, icss, (void *) nextStr, (void *) attribute, (void *) value, (void *) units, (void *) myTime, (void *) myTime);
    if ( status < 0 ) {
        rodsLog( LOG_NOTICE, "findOrInsertAVU insert failure %d", status );
        return status;
    }
    return seqNum;
}


/* create a path name with escaped SQL special characters (% and _) */
std::string
makeEscapedPath( const std::string &inPath ) {
    return boost::regex_replace( inPath, boost::regex( "[%_\\\\]" ), "\\\\$&" );
}

/* Internal routine to modify inheritance */
/* inheritFlag =1 to set, 2 to remove */
int _modInheritance( int inheritFlag, int recursiveFlag, const char *collIdStr, const char *pathName ) {

    const char* newValue = inheritFlag == 1 ? "1" : "0";

    char myTime[50];
    getNowStr( myTime );

    rodsLong_t status;
    /* non-Recursive mode */
    if ( recursiveFlag == 0 ) {

        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "_modInheritance SQL 1" );
        }

        status =  hs_create_coll_inheritance_ts(svc, icss, (void *) collIdStr, (void *) newValue, (void *) myTime);
    }
    else {
        /* Recursive mode */
        std::string pathStart = makeEscapedPath( pathName ) + "/%";

        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "_modInheritance SQL 2" );
        }
        status =  hs_create_coll_inheritance_by_name(svc, icss, (void *) pathName, (void *) pathStart.c_str(), (void *) newValue, (void *) myTime);
    }
    if ( status != 0 ) {
        _rollback( "_modInheritance" );
        return status;
    }

    status =  hs_commit(svc,  icss );
    return status;
}

/*
  Set the over_quota values (if any) using the limits and
  and the current usage; handling the various types: per-user per-resource,
  per-user total-usage, group per-resource, and group-total.

  The over_quota column is positive if over_quota and the negative value
  indicates how much space is left before reaching the quota.
*/
int setOverQuota( rsComm_t *rsComm ) {
    return 0;
}

int
icatGetTicketUserId( irods::plugin_property_map& _prop_map, const char *userName, char *userIdStr ) {

    char userId[NAME_LEN];
    char userZone[NAME_LEN];
    char zoneToUse[NAME_LEN];
    char userName2[NAME_LEN];
    int status;

    std::string zone;
    irods::error ret = getLocalZone( _prop_map, icss, zone );
    if ( !ret.ok() ) {
        return ret.code();
    }

    snprintf( zoneToUse, sizeof( zoneToUse ), "%s", zone.c_str() );
    status = validateAndParseUserName( userName, userName2, userZone );
    if ( status ) {
        return status;
    }
    if ( userZone[0] != '\0' ) {
        rstrcpy( zoneToUse, userZone, NAME_LEN );
    }

    userId[0] = '\0';
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "icatGetTicketUserId SQL 1 " );
    }
    {
        status = hs_get_non_group_user_id(svc, icss, zoneToUse, userName2, userId, NAME_LEN);
    }
    if ( status != 0 ) {
        if ( status == CAT_NO_ROWS_FOUND ) {
            return CAT_INVALID_USER;
        }
        return status;
    }
    strncpy( userIdStr, userId, NAME_LEN );
    return 0;
}

int
icatGetTicketGroupId( irods::plugin_property_map& _prop_map, const char *groupName, char *groupIdStr ) {
    char groupId[NAME_LEN];
    char groupZone[NAME_LEN];
    char zoneToUse[NAME_LEN];
    char groupName2[NAME_LEN];
    int status;

    std::string zone;
    irods::error ret = getLocalZone( _prop_map, icss, zone );
    if ( !ret.ok() ) {
        return ret.code();
    }

    snprintf( zoneToUse, sizeof( zoneToUse ), "%s", zone.c_str() );
    status = validateAndParseUserName( groupName, groupName2, groupZone );
    if ( status ) {
        return status;
    }
    if ( groupZone[0] != '\0' ) {
        rstrcpy( zoneToUse, groupZone, NAME_LEN );
    }

    groupId[0] = '\0';
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "icatGetTicketGroupId SQL 1 " );
    }
    {
        status = hs_get_group_id(svc, icss, zoneToUse, groupName2,
                     groupId, NAME_LEN);
    }
    if ( status != 0 ) {
        if ( status == CAT_NO_ROWS_FOUND ) {
            return CAT_INVALID_GROUP;
        }
        return status;
    }
    strncpy( groupIdStr, groupId, NAME_LEN );
    return 0;
}

char *
convertHostToIp( const char *inputName ) {
    struct hostent *myHostent;
    static char ipAddr[50];
    const int status = gethostbyname_with_retry( inputName, &myHostent );
    if ( status != 0 ) {
        rodsLog( LOG_ERROR, "convertHostToIp gethostbyname_with_retry error. status [%d]", status );
        return NULL;
    }
    snprintf( ipAddr, sizeof( ipAddr ), "%s",
              ( char * )inet_ntoa( *( struct in_addr* )( myHostent->h_addr_list[0] ) ) );
    return ipAddr;
}

// XXXX HELPER FUNCTIONS ABOVE

// =-=-=-=-=-=-=-
// read a message body off of the socket
irods::error db_start_op(
    irods::plugin_context& _ctx ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    return ret;


} // db_start_op

// =-=-=-=-=-=-=-
// set debug behavior for plugin
irods::error db_debug_op(
    irods::plugin_context& _ctx,
    const char*            _mode ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // check incoming param
    if ( !_mode ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "mode is null" );
    }

    // =-=-=-=-=-=-=-
    // run tolower on mode
    std::string mode( _mode );
    std::transform(
        mode.begin(),
        mode.end(),
        mode.begin(),
        ::tolower );

    // =-=-=-=-=-=-=-
    // if mode contains 'sql' then turn SQL logging on
    if ( mode.find( "sql" ) != std::string::npos ) {
        logSQL = 1;
    }
    else {
        logSQL = 0;
    }

    return SUCCESS();

} // db_debug_op

// =-=-=-=-=-=-=-
// open a database connection
irods::error db_open_op(
    irods::plugin_context& _ctx ) {

    std::string prop; // server property

    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // check incoming param
//        if ( !_cfg ) {
//            return ERROR(
//                       CAT_INVALID_ARGUMENT,
//                       "config is null" );
//        }

    // =-=-=-=-=-=-=-
    // log as appropriate
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlOpen" );
    }

    // =-=-=-=-=-=-=-
    // cache db creds
    try {
    } catch ( const irods::exception& e ) {
        return irods::error(e);
    } catch ( const boost::exception& e ) {
        return ERROR(INVALID_ANY_CAST, "Failed any_cast in the database configuration");
    }

    int argc=1;
    char* args[]={"dummy"};
    char** argv=args;
    hs_init(&argc, &argv);
    // =-=-=-=-=-=-=-
    // call open in mid level
    // int status = hs_local(&svc);
    // int status = hs_tcp(&svc);
    int status = hs_unix_domain_socket(&svc);
    if ( 0 != status ) {
        return ERROR(
                   status,
                   "failed to obtain db service" );
    }
    // char config [] = "/etc/QueryArrow/tdb-plugin-gen-abs.yaml";
    // char config [] = "{\"tcpServerAddr\":\"*\",\"tcpServerPort\":12345}";
    char config [] = "/tmp/QueryArrow";
    status = hs_connect(svc, config, &icss );
    if ( 0 != status ) {
        return ERROR(
                   status,
                   "failed to open db connection" );
    }

    // =-=-=-=-=-=-=-
    // set success flag
    icss_status = 1;

    // =-=-=-=-=-=-=-
    // Capture ICAT properties
#if MY_ICAT
#elif ORA_ICAT
#else
    // irods::catalog_properties::instance().capture_if_needed( icss );
#endif

    // =-=-=-=-=-=-=-
    // set pam properties
    try {
        irods_pam_auth_no_extend = irods::get_server_property<const bool>(std::vector<std::string>{irods::PLUGIN_TYPE_AUTHENTICATION, irods::AUTH_PAM_SCHEME, irods::CFG_PAM_NO_EXTEND_KW});
        irods_pam_password_len = irods::get_server_property<const size_t>(std::vector<std::string>{irods::PLUGIN_TYPE_AUTHENTICATION, irods::AUTH_PAM_SCHEME, irods::CFG_PAM_PASSWORD_LENGTH_KW});
        snprintf(irods_pam_password_min_time, NAME_LEN, "%s", irods::get_server_property<const std::string>(std::vector<std::string>{irods::PLUGIN_TYPE_AUTHENTICATION, irods::AUTH_PAM_SCHEME, irods::CFG_PAM_PASSWORD_MIN_TIME_KW}).c_str());
        snprintf(irods_pam_password_max_time, NAME_LEN, "%s", irods::get_server_property<const std::string>(std::vector<std::string>{irods::PLUGIN_TYPE_AUTHENTICATION, irods::AUTH_PAM_SCHEME, irods::CFG_PAM_PASSWORD_MAX_TIME_KW}).c_str());
    } catch ( const irods::exception& e ) {
        rodsLog(LOG_DEBUG, "[%s:%d] PAM property not found", __FUNCTION__, __LINE__);
        return CODE( status );
    }

    if ( irods_pam_auth_no_extend ) {
        snprintf( irods_pam_password_default_time,
                  sizeof( irods_pam_password_default_time ),
                  "%s", "28800" );
    }

    return CODE( status );

} // db_open_op

// =-=-=-=-=-=-=-
// close a database connection
irods::error db_close_op(
    irods::plugin_context& _ctx ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );

    // =-=-=-=-=-=-=-
    // call open in mid level
    int status = hs_disconnect(svc,  icss );
    hs_exit();
    if ( 0 != status ) {
        return ERROR(
                   status,
                   "failed to close db connection" );
    }

    // =-=-=-=-=-=-=-
    // set success flag
    // icss_status = 0;

    return CODE( status );

} // db_close_op

// =-=-=-=-=-=-=-
// return the local zone
irods::error db_check_and_get_object_id_op(
    irods::plugin_context& _ctx,
    const char*            _type,
    const char*            _name,
    const char*            _access ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    rodsLong_t status = checkAndGetObjectId(
                            _ctx.comm(),
                            _ctx.prop_map(),
                            _type,
                            _name,
                            _access );
    if ( status < 0 ) {
        return ERROR( status, "checkAndGetObjectId failed" );
    }
    else {
        return SUCCESS();

    }

}

// =-=-=-=-=-=-=-
// return the local zone
irods::error db_get_local_zone_op(
    irods::plugin_context& _ctx,
    std::string*           _zone ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    ret = getLocalZone( _ctx.prop_map(), icss, ( *_zone ) );
    if ( !ret.ok() ) {
        return PASS( ret );

    }
    else {
        return SUCCESS();

    }

} // db_get_local_zone_op

// =-=-=-=-=-=-=-
// update the data obj count of a resource
irods::error db_update_resc_obj_count_op(
    irods::plugin_context& _ctx,
    const std::string*     _resc,
    int                    _delta ) {

    return SUCCESS();

} // db_update_resc_obj_count_op

// =-=-=-=-=-=-=-
// update the data obj count of a resource
irods::error db_mod_data_obj_meta_op(
    irods::plugin_context& _ctx,
    dataObjInfo_t*         _data_obj_info,
    keyValPair_t*          _reg_param ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if ( !_data_obj_info ||
            !_reg_param ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "null parameter" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );


    int i = 0, j = 0, status = 0, upCols = 0;
    rodsLong_t iVal = 0; // JMC cppcheck - uninit var
    int status2 = 0;

    int mode = 0;

    char logicalFileName[MAX_NAME_LEN];
    char logicalDirName[MAX_NAME_LEN];
    char *theVal = 0;
    char replNum1[MAX_NAME_LEN];

    const char* whereColsAndConds[10];
    const char* whereValues[10];
    char idVal[MAX_NAME_LEN];
    int numConditions = 0;
    char oldCopy[NAME_LEN];
    char newCopy[NAME_LEN];
    int adminMode = 0;

    std::vector<const char *> updateCols;
    std::vector<const char *> updateVals;

    /* regParamNames has the argument names (in _reg_param) that this
       routine understands and colNames has the corresponding column
       names; one for one. */
    int dataTypeIndex = 1; /* matches table below for quick check */
    // Using the keyword defines so there is one point of truth - hcj
    const char *regParamNames[] = {
        REPL_NUM_KW,        DATA_TYPE_KW,       DATA_SIZE_KW,
        RESC_NAME_KW,       FILE_PATH_KW,       DATA_OWNER_KW,
        DATA_OWNER_ZONE_KW, REPL_STATUS_KW,     CHKSUM_KW,
        DATA_EXPIRY_KW,     DATA_COMMENTS_KW,   DATA_CREATE_KW,
        DATA_MODIFY_KW,     DATA_MODE_KW,       RESC_HIER_STR_KW,
        RESC_ID_KW, "END"

    };

    /* If you update colNames, be sure to update DATA_EXPIRY_TS_IX if
     * you add items before "data_expiry_ts" and */
    const char *colNames[] = {
        "data_repl_num",   "data_type_name", "data_size",
        "resc_name",       "data_path",      "data_owner_name",
        "data_owner_zone", "data_is_dirty",  "data_checksum",
        "data_expiry_ts",  "r_comment",      "create_ts",
        "modify_ts",       "data_mode",      "resc_hier",
        "resc_id"
    };
    int DATA_EXPIRY_TS_IX = 9; /* must match index in above colNames table */
    int MODIFY_TS_IX = 12;   /* must match index in above colNames table */

    int DATA_SIZE_IX = 2;    /* must match index in above colNames table */
    int doingDataSize = 0;
    char dataSizeString[NAME_LEN] = "";
    char objIdString[MAX_NAME_LEN];
    char *neededAccess = 0;

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlModDataObjMeta" );
    }

    adminMode = 0;
    theVal = getValByKey( _reg_param, ADMIN_KW );
    if ( theVal != NULL ) {
        adminMode = 1;
    }

    std::string update_resc_id_str;

    bool update_resc_id = false;
    /* Set up the updateCols and updateVals arrays */
    for ( i = 0, j = 0; strcmp( regParamNames[i], "END" ); i++ ) {
        theVal = getValByKey( _reg_param, regParamNames[i] );
        if ( theVal != NULL ) {
            if( std::string( "resc_name") == colNames[i]) {
                continue;
            }
            else if( std::string( "resc_hier") == colNames[i]) {
                updateCols.push_back( "resc_id" );

                rodsLong_t resc_id;
                resc_mgr.hier_to_leaf_id(theVal,resc_id);

                update_resc_id_str = boost::lexical_cast<std::string>(resc_id);
                updateVals.push_back( update_resc_id_str.c_str() );
            } else {
                updateCols.push_back( colNames[i] );
                updateVals.push_back( theVal );
            }

            if ( std::string( "resc_id" ) == colNames[i] || std::string( "resc_hier") == colNames[i]) {
                update_resc_id = true;
            }

            if ( i == DATA_EXPIRY_TS_IX ) {
                /* if data_expiry, make sure it's
                                               in the standard time-stamp
                                               format: "%011d" */
                if ( strcmp( colNames[i], "data_expiry_ts" ) == 0 ) { /* double check*/
                    if ( strlen( theVal ) < 11 ) {
                        static char theVal2[20];
                        time_t myTimeValue;
                        myTimeValue = atoll( theVal );
                        snprintf( theVal2, sizeof theVal2, "%011d", ( int )myTimeValue );
                        updateVals[j] = theVal2;
                    }
                }
            }

            if ( i == MODIFY_TS_IX ) {
                /* if modify_ts, also make sure it's
                                                in the standard time-stamp
                                                format: "%011d" */
                if ( strcmp( colNames[i], "modify_ts" ) == 0 ) { /* double check*/
                    if ( strlen( theVal ) < 11 ) {
                        static char theVal3[20];
                        time_t myTimeValue;
                        myTimeValue = atoll( theVal );
                        snprintf( theVal3, sizeof theVal3, "%011d", ( int )myTimeValue );
                        updateVals[j] = theVal3;
                    }
                }
            }
            if ( i == DATA_SIZE_IX ) {
                doingDataSize = 1; /* flag to check size */
                snprintf( dataSizeString, sizeof( dataSizeString ), "%s", theVal );
            }

            j++;

            /* If the datatype is being updated, check that it is valid */
            if ( i == dataTypeIndex ) {
                status = cmlCheckNameToken( "data_type",
                                            theVal, svc, icss );
                if ( status != 0 ) {
                    std::stringstream msg;
                    msg << __FUNCTION__;
                    msg << " - Invalid data type specified.";
                    addRErrorMsg( &_ctx.comm()->rError, 0, msg.str().c_str() );
                    return ERROR(
                               CAT_INVALID_DATA_TYPE,
                               msg.str() );
                }
            }
        }
    }
    upCols = j;

    /* If the only field is the chksum then the user only needs read
       access since we can trust that the server-side code is
       calculating it properly and checksum is a system-managed field.
       For example, when doing an irsync the server may calculate a
       checksum and want to set it in the source copy.
    */
    neededAccess = ACCESS_MODIFY_METADATA;
    if ( upCols == 1 && strcmp( updateCols[0], "chksum" ) == 0 ) {
        neededAccess = ACCESS_READ_OBJECT;
    }

    /* If dataExpiry is being updated, user needs to have
       a greater access permission */
    theVal = getValByKey( _reg_param, DATA_EXPIRY_KW );
    if ( theVal != NULL ) {
        neededAccess = ACCESS_DELETE_OBJECT;
    }

    if ( _data_obj_info->dataId <= 0 ) {

        status = splitPathByKey( _data_obj_info->objPath,
                                 logicalDirName, MAX_NAME_LEN, logicalFileName, MAX_NAME_LEN, '/' );

        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModDataObjMeta SQL 1 " );
        }
        {
            status = hs_get_int_coll_id_by_name(svc, icss, logicalDirName, &iVal );
        }

        if ( status != 0 ) {
            char errMsg[105];
            snprintf( errMsg, 100, "collection '%s' is unknown",
                      logicalDirName );
            addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
            _rollback( "chlModDataObjMeta" );
            return ERROR(
                       CAT_UNKNOWN_COLLECTION,
                       "failed with unknown collection" );
        }
        snprintf( objIdString, MAX_NAME_LEN, "%lld", iVal );

        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModDataObjMeta SQL 2" );
        }
        {
            status = hs_get_int_data_id(svc, icss, objIdString, logicalFileName,&iVal );
        }
        if ( status != 0 ) {
            std::stringstream msg;
            msg << __FUNCTION__;
            msg << " - Failed to find file in database by its logical path.";
            addRErrorMsg( &_ctx.comm()->rError, 0, msg.str().c_str() );
            _rollback( "chlModDataObjMeta" );
            return ERROR(
                       CAT_UNKNOWN_FILE,
                       "failed with unknown file" );
        }

        _data_obj_info->dataId = iVal;  /* return it for possible use next time, */
        /* and for use below */
    }

    snprintf( objIdString, MAX_NAME_LEN, "%lld", _data_obj_info->dataId );

    if ( adminMode ) {
        if ( _ctx.comm()->clientUser.authInfo.authFlag != LOCAL_PRIV_USER_AUTH ) {
            return ERROR(
                       CAT_INSUFFICIENT_PRIVILEGE_LEVEL,
                       "failed with insufficient privilege" );
        }
    }
    else {
        if ( doingDataSize == 1 && strlen( mySessionTicket ) > 0 ) {
            status = cmlTicketUpdateWriteBytes( mySessionTicket,
                                                dataSizeString,
                                                objIdString, svc, icss );
            if ( status != 0 ) {
                return ERROR(
                           status,
                           "cmlTicketUpdateWriteBytes failed" );
            }
        }

        status = cmlCheckDataObjId(
                     objIdString,
                     _ctx.comm()->clientUser.userName,
                     _ctx.comm()->clientUser.rodsZone,
                     neededAccess,
                     mySessionTicket,
                     mySessionClientAddr, svc,
                     icss );

        if ( status != 0 ) {
            theVal = getValByKey( _reg_param, ACL_COLLECTION_KW );
            if ( theVal != NULL && upCols == 1 &&
                    strcmp( updateCols[0], "data_path" ) == 0 ) {
                int len, iVal = 0; // JMC cppcheck - uninit var ( shadows prev decl? )
                /*
                 In this case, the user is doing a 'imv' of a collection but one of
                 the sub-files is not owned by them.  We decided this should be
                 allowed and so we support it via this new ACL_COLLECTION_KW, checking
                 that the ACL_COLLECTION matches the beginning path of the object and
                 that the user has the appropriate access to that collection.
                 */
                len = strlen( theVal );
                if ( strncmp( theVal, _data_obj_info->objPath, len ) == 0 ) {

                    iVal = cmlCheckDir( theVal,
                                        _ctx.comm()->clientUser.userName,
                                        _ctx.comm()->clientUser.rodsZone,
                                        ACCESS_OWN, svc,
                                        icss );
                }
                if ( iVal > 0 ) {
                    status = 0;
                } /* Collection was found (id
                                   * returned) & user has access */
            }
            if ( status ) {
                _rollback( "chlModDataObjMeta" );
                return ERROR(
                           CAT_NO_ACCESS_PERMISSION,
                           "failed with no permission" );
            }
        }
    }

    whereColsAndConds[0] = "data_id=";
    snprintf( idVal, MAX_NAME_LEN, "%lld", _data_obj_info->dataId );
    whereValues[0] = idVal;
    numConditions = 1;

    /* This is up here since this is usually called to modify the
     * metadata of a single repl.  If ALL_KW is included, then apply
     * this change to all replicas (by not restricting the update to
     * only one).
     */
    std::string where_resc_id_str;
    if ( getValByKey( _reg_param, ALL_KW ) == NULL ) {
        // use resc_id instead of replNum as it is
        // always set, unless resc_id is to be
        // updated.  replNum is sometimes 0 in various
        // error cases
        if ( update_resc_id || strlen( _data_obj_info->rescHier ) <= 0 ) {
            j = numConditions;
            whereColsAndConds[j] = "data_repl_num=";
            snprintf( replNum1, MAX_NAME_LEN, "%d", _data_obj_info->replNum );
            whereValues[j] = replNum1;
            numConditions++;

        }
        else {
            rodsLong_t id = 0;
            resc_mgr.hier_to_leaf_id( _data_obj_info->rescHier, id );
            where_resc_id_str = boost::lexical_cast<std::string>(id);
            j = numConditions;
            whereColsAndConds[j] = "resc_id=";
            whereValues[j] = where_resc_id_str.c_str();
            numConditions++;
        }
    }

    mode = 0;
    if ( getValByKey( _reg_param, ALL_REPL_STATUS_KW ) ) {
        mode = 1;
        /* mark this one as NEWLY_CREATED_COPY and others as OLD_COPY */
    }

    std::string zone;
    ret = getLocalZone(
              _ctx.prop_map(),
              icss,
              zone );
    if ( !ret.ok() ) {
        rodsLog( LOG_ERROR, "chlModObjMeta - failed in getLocalZone with status [%d]", status );
        return PASS( ret );
    }

    // If we are moving the data object from one resource to another resource, update the object counts for those resources
    // appropriately - hcj
    char* new_resc_hier = getValByKey( _reg_param, RESC_HIER_STR_KW );
    if ( new_resc_hier != NULL ) {
        std::stringstream id_stream;
        id_stream << _data_obj_info->dataId;
        std::stringstream repl_stream;
        repl_stream << _data_obj_info->replNum;
        rodsLong_t resc_id = 0;
        std::string resc_hier;
        {
            status = hs_get_int_repl_resc_id(svc, icss, (void *) id_stream.str().c_str(), (void *) repl_stream.str().c_str(),
                         &resc_id  );
        }
        if ( status != 0 ) {
            std::stringstream msg;
            msg << __FUNCTION__;
            msg << " - Failed to get the resc hierarchy from object with id: ";
            msg << id_stream.str();
            msg << " and replNum: ";
            msg << repl_stream.str();
            irods::log( LOG_NOTICE, msg.str() );
            return ERROR(
                       status,
                       msg.str() );
        }

    }

    if ( mode == 0 ) {
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModDataObjMeta SQL 4" );
        }
        status = hs_modify_data(svc, icss ,
                     &( updateCols[0] ),
                     &( updateVals[0] ),
                     whereColsAndConds,
                     whereValues,
                     upCols,
                     numConditions
                     );
    }
    else {
        /* mark this one as NEWLY_CREATED_COPY and others as OLD_COPY */
        j = upCols;
        updateCols.push_back( "data_is_dirty" );
        snprintf( newCopy, NAME_LEN, "%d", NEWLY_CREATED_COPY );
        updateVals.push_back( newCopy );
        upCols++;
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModDataObjMeta SQL 5" );
        }
        status = hs_modify_data(svc, icss,
                     &( updateCols[0] ),
                     &( updateVals[0] ),
                     whereColsAndConds,
                     whereValues,
                     upCols,
                     numConditions
                      );
        if ( status == 0 ) {
            j = numConditions - 1;
            whereColsAndConds[j] = "data_repl_num!=";
            snprintf( replNum1, MAX_NAME_LEN, "%d", _data_obj_info->replNum );
            whereValues[j] = replNum1;

            updateCols[0] = "data_is_dirty";
            snprintf( oldCopy, NAME_LEN, "%d", OLD_COPY );
            updateVals[0] = oldCopy;
            if ( logSQL != 0 ) {
                rodsLog( LOG_SQL, "chlModDataObjMeta SQL 6" );
            }
            status2 = hs_modify_data(svc,  icss, &( updateCols[0] ), &( updateVals[0] ),
                                            whereColsAndConds, whereValues, 1,
                                            numConditions );

            if ( status2 != 0 && status2 != CAT_SUCCESS_BUT_WITH_NO_INFO ) {
                /* Ignore NO_INFO errors but not others */
                rodsLog( LOG_NOTICE,
                         "chlModDataObjMeta cmlModifySingleTable failure for other replicas %d",
                         status2 );
                _rollback( "chlModDataObjMeta" );
                return ERROR(
                           status2,
                           "cmlModifySingleTable failure for other replicas" );
            }

        }
    }
    if ( status != 0 ) {
        _rollback( "chlModDataObjMeta" );
        rodsLog( LOG_NOTICE,
                 "chlModDataObjMeta cmlModifySingleTable failure %d",
                 status );
        return ERROR(
                   status,
                   "cmlModifySingleTable failure" );
    }

    if ( !( _data_obj_info->flags & NO_COMMIT_FLAG ) ) {
        status =  hs_commit(svc,  icss );
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlModDataObjMeta cmlExecuteNoAnswerSql commit failure %d",
                     status );
            return ERROR(
                       status,
                       "commit failure" );
        }
    }

    return CODE( status );

} // db_mod_data_obj_meta_op


// =-=-=-=-=-=-=-
// update the data obj count of a resource
irods::error db_reg_data_obj_op(
    irods::plugin_context& _ctx,
    dataObjInfo_t*         _data_obj_info ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if ( !_data_obj_info ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "null parameter" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );

    // =-=-=-=-=-=-=-
    //
    char myTime[50];
    char logicalFileName[MAX_NAME_LEN];
    char logicalDirName[MAX_NAME_LEN];
    rodsLong_t seqNum;
    rodsLong_t iVal;
    char dataIdNum[MAX_NAME_LEN];
    char collIdNum[MAX_NAME_LEN];
    char dataReplNum[MAX_NAME_LEN];
    char dataSizeNum[MAX_NAME_LEN];
    char dataStatusNum[MAX_NAME_LEN];
    char data_expiry_ts[] = { "00000000000" };
    int status;
    int inheritFlag;

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegDataObj" );
    }
    if ( !icss_status ) {
        return ERROR( CATALOG_NOT_CONNECTED, "catalog not connected" );
    }

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegDataObj SQL 1 " );
    }
    status = hs_get_int_next_id(svc,  icss, &seqNum );
    if ( status < 0 ) {
        rodsLog( LOG_NOTICE, "chlRegDataObj cmlGetNextSeqVal failure %d",
                 status );
        _rollback( "chlRegDataObj" );
        return ERROR( status, "chlRegDataObj cmlGetNextSeqVal failure" );
    }
    snprintf( dataIdNum, MAX_NAME_LEN, "%lld", seqNum );
    _data_obj_info->dataId = seqNum; /* store as output parameter */

    status = splitPathByKey( _data_obj_info->objPath,
                             logicalDirName, MAX_NAME_LEN, logicalFileName, MAX_NAME_LEN, '/' );


    /* Check that collection exists and user has write permission.
       At the same time, also get the inherit flag */
    iVal = cmlCheckDirAndGetInheritFlag( logicalDirName,
                                         _ctx.comm()->clientUser.userName,
                                         _ctx.comm()->clientUser.rodsZone,
                                         ACCESS_MODIFY_OBJECT,
                                         &inheritFlag,
                                         mySessionTicket,
                                         mySessionClientAddr, svc,
                                         icss );
    if ( iVal < 0 ) {
        if ( iVal == CAT_UNKNOWN_COLLECTION ) {
            std::stringstream errMsg;
            errMsg << "collection '" << logicalDirName << "' is unknown";
            addRErrorMsg( &_ctx.comm()->rError, 0, errMsg.str().c_str() );
        }
        else if ( iVal == CAT_NO_ACCESS_PERMISSION ) {
            std::stringstream errMsg;
            errMsg << "no permission to update collection '" << logicalDirName << "'";
            addRErrorMsg( &_ctx.comm()->rError, 0, errMsg.str().c_str() );
        }
        return ERROR( iVal, "" );
    }
    snprintf( collIdNum, MAX_NAME_LEN, "%lld", iVal );

    /* Make sure no collection already exists by this name */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegDataObj SQL 4" );
    }
    {
        status = hs_get_int_coll_id_by_name(svc, icss, _data_obj_info->objPath,
                     &iVal );
    }
    if ( status == 0 ) {
        return ERROR( CAT_NAME_EXISTS_AS_COLLECTION, "collection exists" );
    }

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegDataObj SQL 5" );
    }
    status = cmlCheckNameToken( "data_type",
                                _data_obj_info->dataType, svc, icss );
    if ( status != 0 ) {
        return ERROR( CAT_INVALID_DATA_TYPE, "invalid data type" );
    }

    snprintf( dataReplNum, MAX_NAME_LEN, "%d", _data_obj_info->replNum );
    snprintf( dataStatusNum, MAX_NAME_LEN, "%d", _data_obj_info->replStatus );
    snprintf( dataSizeNum, MAX_NAME_LEN, "%lld", _data_obj_info->dataSize );
    getNowStr( myTime );

    std::string resc_id_str = boost::lexical_cast<std::string>(_data_obj_info->rescId);

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegDataObj SQL 6" );
    }
    status =  hs_create_repl(svc, icss,
      dataIdNum,
      (void *) resc_id_str.c_str(),
      collIdNum,
      logicalFileName,
      dataReplNum,
      _data_obj_info->version,
      _data_obj_info->dataType,
      dataSizeNum,
      _data_obj_info->filePath,
      _ctx.comm()->clientUser.userName,
      _ctx.comm()->clientUser.rodsZone,
      (void *) "1",
      dataStatusNum,
      _data_obj_info->chksum,
      _data_obj_info->dataMode,
      myTime, myTime,
      data_expiry_ts, (void *) "EMPTY_RESC_NAME");
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlRegDataObj cmlExecuteNoAnswerSql failure %d", status );
        _rollback( "chlRegDataObj" );
        return ERROR( status, "chlRegDataObj cmlExecuteNoAnswerSql failure" );
    }
    std::string zone;
    ret = getLocalZone(
              _ctx.prop_map(),
              icss,
              zone );
    if ( !ret.ok() ) {
        rodsLog( LOG_ERROR, "chlRegDataInfo - failed in getLocalZone with status [%d]", status );
        return PASS( ret );
    }

    if ( inheritFlag ) {
        /* If inherit is set (sticky bit), then add access rows for this
           dataobject that match those of the parent collection */
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlRegDataObj SQL 7" );
        }
        status = hs_create_access_inherit_coll(svc, icss, dataIdNum, collIdNum, myTime, myTime);
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlRegDataObj cmlExecuteNoAnswerSql insert access failure %d",
                     status );
            _rollback( "chlRegDataObj" );
            return ERROR( status, "cmlExecuteNoAnswerSql insert access failure" );
        }
    }
    else {
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlRegDataObj SQL 8" );
        }
        status = hs_create_access_by_user_zone_and_name(svc, icss, dataIdNum, _ctx.comm()->clientUser.rodsZone, _ctx.comm()->clientUser.userName, (void *) ACCESS_OWN, myTime, myTime);
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlRegDataObj cmlExecuteNoAnswerSql insert access failure %d",
                     status );
            _rollback( "chlRegDataObj" );
            return ERROR( status, "cmlExecuteNoAnswerSql insert access failure" );
        }
    }


    if ( !( _data_obj_info->flags & NO_COMMIT_FLAG ) ) {
        status =  hs_commit(svc,  icss );
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlRegDataObj cmlExecuteNoAnswerSql commit failure %d",
                     status );
            return ERROR( status, "cmlExecuteNoAnswerSql commit failure" );
        }
    }

    return SUCCESS();

} // db_reg_data_obj_op


// =-=-=-=-=-=-=-
// register a data object into the catalog
irods::error db_reg_replica_op(
    irods::plugin_context& _ctx,
    dataObjInfo_t*         _src_data_obj_info,
    dataObjInfo_t*         _dst_data_obj_info,
    keyValPair_t*          _cond_input ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_src_data_obj_info ||
        !_dst_data_obj_info ||
        !_cond_input ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "null parameter" );
    }

    if( _dst_data_obj_info->rescId <= 0 ) {
        std::stringstream msg;
        msg << "invalid resource id "
            << _dst_data_obj_info->rescId
            << " for ["
            << _dst_data_obj_info->objPath
            << "]";
        return ERROR(
                SYS_INVALID_INPUT_PARAM,
                msg.str() );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );


    char myTime[50];
    char logicalFileName[MAX_NAME_LEN];
    char logicalDirName[MAX_NAME_LEN];
    rodsLong_t iVal;
    rodsLong_t status;
    // char tSQL[MAX_SQL_SIZE];
    char *cVal[30];
    // int i;
    int statementNumber;
    int nextReplNum;
    char nextRepl[30];
    /* char theColls[] = "data_id, \
                       coll_id,  \
                       data_name, \
                       data_repl_num, \
                       data_version, \
                       data_type_name, \
                       data_size, \
                       resc_group_name, \
                       resc_name, \
                       resc_hier, \
                       resc_id, \
                       data_path, \
                       data_owner_name, \
                       data_owner_zone, \
                       data_is_dirty, \
                       data_status, \
                       data_checksum, \
                       data_expiry_ts, \
                       data_map_id, \
                       data_mode, \
                       r_comment, \
                       create_ts, \
                       modify_ts"; */
//    const int IX_DATA_REPL_NUM = 3; /* index of data_repl_num in theColls */
//        int IX_RESC_GROUP_NAME = 7; /* index into theColls */
//    const int IX_RESC_ID = 10;
//    const int IX_DATA_PATH = 11;    /* index into theColls */

//    const int IX_DATA_MODE = 19;
//    const int IX_CREATE_TS = 21;
//    const int IX_MODIFY_TS = 22;
//    const int IX_RESC_NAME2 = 23;
//    const int IX_DATA_PATH2 = 24;
//    const int IX_DATA_ID2 = 25;
    int nColumns = 19;

    char objIdString[MAX_NAME_LEN];
    char replNumString[MAX_NAME_LEN];
    int adminMode;
    char *theVal;

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegReplica" );
    }

    adminMode = 0;
    if ( _cond_input != NULL ) {
        theVal = getValByKey( _cond_input, ADMIN_KW );
        if ( theVal != NULL ) {
            adminMode = 1;
        }
    }

    if ( !icss_status ) {
        return ERROR( CATALOG_NOT_CONNECTED, "catalog not connected" );
    }

    status = splitPathByKey( _src_data_obj_info->objPath,
                             logicalDirName, MAX_NAME_LEN, logicalFileName, MAX_NAME_LEN, '/' );

    if ( adminMode ) {
        if ( _ctx.comm()->clientUser.authInfo.authFlag != LOCAL_PRIV_USER_AUTH ) {
            return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
        }
    }
    else {
        /* Check the access to the dataObj */
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlRegReplica SQL 1 " );
        }
        status = cmlCheckDataObjOnly( logicalDirName, logicalFileName,
                                      _ctx.comm()->clientUser.userName,
                                      _ctx.comm()->clientUser.rodsZone,
                                      ACCESS_READ_OBJECT, svc, icss );
        if ( status < 0 ) {
            _rollback( "chlRegReplica" );
            return ERROR( status, "cmlCheckDataObjOnly failed" );
        }
    }

    /* Get the next replica number */
    snprintf( objIdString, MAX_NAME_LEN, "%lld", _src_data_obj_info->dataId );
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegReplica SQL 2" );
    }
    {
        status = hs_get_int_max_repl_num(svc, icss, objIdString,
                     &iVal );
    }

    if ( status != 0 ) {
        _rollback( "chlRegReplica" );
        return ERROR( status, "cmlGetIntegerValueFromSql failed" );
    }

    nextReplNum = iVal + 1;
    snprintf( nextRepl, sizeof nextRepl, "%d", nextReplNum );
    _dst_data_obj_info->replNum = nextReplNum; /* return new replica number */
    snprintf( replNumString, MAX_NAME_LEN, "%d", _src_data_obj_info->replNum );
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegReplica SQL 3" );
    }
    char **dVal;
    int nCols;
    {
        status = hs_get_all_repl2_by_repl_num(svc, icss, objIdString, replNumString, &dVal, &nCols);
    }
    if ( status < 0 ) {
        _rollback( "chlRegReplica" );
        return ERROR( status, "cmlGetOneRowFromSqlV2 failed" );
    }
    statementNumber = status;

    std::string resc_id_str = boost::lexical_cast<std::string>(_dst_data_obj_info->rescId);

    cVal[0] = objIdString;
    cVal[1] = (char*)resc_id_str.c_str();
    cVal[2] = dVal[1]; // coll id
    cVal[3] = dVal[2]; // data name
    cVal[4] = nextRepl; // repl num
    //cVal[IX_RESC_NAME]       = _dst_data_obj_info->rescName;
    cVal[5] = dVal[3]; // version
    cVal[6] = dVal[4]; // data type
    cVal[7] = dVal[5]; // data size
    cVal[8]       = _dst_data_obj_info->filePath;
    cVal[9] = dVal[7]; // user name
    cVal[10] = dVal[8]; // zone name
    cVal[11] = dVal[9]; // is dirty
    cVal[12] = dVal[10]; // status
    cVal[13] = dVal[11]; // chksum
    cVal[14]       = _dst_data_obj_info->dataMode;

    getNowStr( myTime );
    cVal[15] = myTime;
    cVal[16] = myTime;
    cVal[17] = "";
    cVal[18] = "";

//    for ( i = 0; i < nColumns; i++ ) {
//        cllBindVars[i] = cVal[i];
//    }
//    cllBindVarCount = nColumns;
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegReplica SQL 4" );
    }
    status = hs_array_create_repl(svc, icss, cVal, nColumns);
    if ( status < 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlRegReplica cmlExecuteNoAnswerSql(insert) failure %d",
                 status );
        _rollback( "chlRegReplica" );
        return ERROR( status, "cmlExecuteNoAnswerSql(insert) failure" );
    }

    std::string zone;
    ret = getLocalZone( _ctx.prop_map(), icss, zone );
    if ( !ret.ok() ) {
        rodsLog( LOG_ERROR, "chlRegReplica - failed in getLocalZone with status [%d]", status );
        return PASS( ret );
    }

    status =  hs_commit(svc, icss);
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlRegReplica cmlExecuteNoAnswerSql commit failure %d",
                 status );
        return ERROR( status, "cmlExecuteNoAnswerSql commit failure" );
    }

    return SUCCESS();

} // db_reg_replica_op

// =-=-=-=-=-=-=-
// unregister a data object
irods::error db_unreg_replica_op(
    irods::plugin_context& _ctx,
    dataObjInfo_t*         _data_obj_info,
    keyValPair_t*          _cond_input ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_data_obj_info ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "null parameter" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );


    // =-=-=-=-=-=-=-
    //
    char logicalFileName[MAX_NAME_LEN];
    char logicalDirName[MAX_NAME_LEN];
    rodsLong_t status;
    // char tSQL[MAX_SQL_SIZE];
    char replNumber[30];
    char dataObjNumber[30];
    char cVal[MAX_NAME_LEN];
    int adminMode;
    int trashMode;
    char *theVal;
    char checkPath[MAX_NAME_LEN];

    dataObjNumber[0] = '\0';
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlUnregDataObj" );
    }

    if ( !icss_status ) {
        return ERROR( CATALOG_NOT_CONNECTED, "catalog not connected" );
    }

    adminMode = 0;
    trashMode = 0;
    if ( _cond_input != NULL ) {
        theVal = getValByKey( _cond_input, ADMIN_KW );
        if ( theVal != NULL ) {
            adminMode = 1;
        }
        theVal = getValByKey( _cond_input, ADMIN_RMTRASH_KW );
        if ( theVal != NULL ) {
            adminMode = 1;
            trashMode = 1;
        }
    }

    status = splitPathByKey( _data_obj_info->objPath,
                             logicalDirName, MAX_NAME_LEN, logicalFileName, MAX_NAME_LEN, '/' );


    if ( adminMode == 0 ) {
        /* Check the access to the dataObj */
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlUnregDataObj SQL 1 " );
        }
        status = cmlCheckDataObjOnly( logicalDirName, logicalFileName,
                                      _ctx.comm()->clientUser.userName,
                                      _ctx.comm()->clientUser.rodsZone,
                                      ACCESS_DELETE_OBJECT, svc, icss );
        if ( status < 0 ) {
            _rollback( "chlUnregDataObj" );
            return ERROR( status, "cmlCheckDataObjOnly failed" ); /* convert long to int */
        }
        snprintf( dataObjNumber, sizeof dataObjNumber, "%lld", status );
    }
    else {
        if ( _ctx.comm()->clientUser.authInfo.authFlag != LOCAL_PRIV_USER_AUTH ) {
            return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
        }
        if ( trashMode ) {
            int len;
            std::string zone;
            ret = getLocalZone( _ctx.prop_map(), icss, zone );
            if ( !ret.ok() ) {
                return PASS( ret );
            }
            snprintf( checkPath, MAX_NAME_LEN, "/%s/trash", zone.c_str() );
            len = strlen( checkPath );
            if ( strncmp( checkPath, logicalDirName, len ) != 0 ) {
                addRErrorMsg( &_ctx.comm()->rError, 0,
                              "TRASH_KW but not zone/trash path" );
                return ERROR( CAT_INVALID_ARGUMENT, "TRASH_KW but not zone/trash path" );
            }
            if ( _data_obj_info->dataId > 0 ) {
                snprintf( dataObjNumber, sizeof dataObjNumber, "%lld",
                          _data_obj_info->dataId );
            }
        }
        else {
            if ( _data_obj_info->replNum >= 0 && _data_obj_info->dataId >= 0 ) {
                /* Check for a different replica */
                snprintf( dataObjNumber, sizeof dataObjNumber, "%lld",
                          _data_obj_info->dataId );
                snprintf( replNumber, sizeof replNumber, "%d", _data_obj_info->replNum );
                if ( logSQL != 0 ) {
                    rodsLog( LOG_SQL, "chlUnregDataObj SQL 2" );
                }
                {
                    status = hs_get_diff_repl_num(svc, icss, dataObjNumber, replNumber,
                                 cVal, sizeof cVal);
                }
                if ( status != 0 ) {
                    addRErrorMsg( &_ctx.comm()->rError, 0,
                                  "This is the last replica, removal by admin not allowed" );
                    return ERROR( CAT_LAST_REPLICA, "This is the last replica, removal by admin not allowed" );
                }
            }
            else {
                addRErrorMsg( &_ctx.comm()->rError, 0,
                              "dataId and replNum required" );
                _rollback( "chlUnregDataObj" );
                return ERROR( CAT_INVALID_ARGUMENT, "dataId and replNum required" );
            }
        }
    }

    // Get the resource name so we can update its data object count later
    std::string resc_hier;
    if ( strlen( _data_obj_info->rescHier ) == 0 ) {
        rodsLong_t resc_id = 0;
        if ( _data_obj_info->replNum >= 0 ) {
            snprintf( replNumber, sizeof replNumber, "%d", _data_obj_info->replNum );
            {
                status = hs_get_int_repl_resc_id(svc, icss, dataObjNumber,replNumber, &resc_id );
            }
            if ( status < 0 ) {
                return ERROR( status, "cmlGetStringValueFromSql failed" );
            }
        }
        else {
            {
                status = hs_get_int_resc_id_by_data_id(svc, icss, dataObjNumber, &resc_id);
            }
            if ( status < 0 ) {
                return ERROR( status, "cmlGetStringValueFromSql failed" );
            }
        }
        resc_mgr.leaf_id_to_hier( resc_id, resc_hier );
    }
    else {
        resc_hier = std::string( _data_obj_info->rescHier );
    }

    if ( _data_obj_info->replNum >= 0 ) {
        snprintf( replNumber, sizeof replNumber, "%d", _data_obj_info->replNum );
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlUnregDataObj SQL 4" );
        }
		status =  hs_delete_repl_by_name(svc, icss, logicalDirName, logicalFileName, replNumber);
    }
    else {
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlUnregDataObj SQL 5" );
        }
		status =  hs_delete_data_by_name(svc, icss, logicalDirName, logicalFileName);
    }

    if ( status != 0 ) {
        if ( status == CAT_SUCCESS_BUT_WITH_NO_INFO ) {
            char errMsg[105];
            status = CAT_UNKNOWN_FILE;  /* More accurate, in this case */
            snprintf( errMsg, 100, "data object '%s' is unknown",
                      logicalFileName );
            addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
            return ERROR( status, "data object unknown" );
        }
        _rollback( "chlUnregDataObj" );
        return ERROR( status, "cmlExecuteNoAnswerSql failed" );
    }

    std::string zone;
    ret = getLocalZone( _ctx.prop_map(), icss, zone );
    if ( !ret.ok() ) {
        rodsLog( LOG_ERROR, "chlUnRegDataObj - failed in getLocalZone with status [%d]", status );
        return PASS( ret );
    }

    /* delete the access rows, if we just deleted the last replica */
    if ( dataObjNumber[0] != '\0' ) {
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlUnregDataObj SQL 3" );
        }
        rodsLong_t id2 = 0;
        status = hs_get_int_data_id_by_data_id(svc, icss, dataObjNumber, &id2);
        if ( status < 0 ) {
            hs_delete_access_by_data_id(svc, icss, dataObjNumber);
            removeMetaMapAndAVU( dataObjNumber ); /* remove AVU metadata, if any */
        }
    }



    status =  hs_commit(svc,  icss );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlUnregDataObj cmlExecuteNoAnswerSql commit failure %d",
                 status );
        return ERROR( status, "cmlExecuteNoAnswerSql commit failure" );
    }

    return SUCCESS();

} // db_unreg_replica_op

// =-=-=-=-=-=-=-
//
irods::error db_reg_rule_exec_op(
    irods::plugin_context& _ctx,
    ruleExecSubmitInp_t*   _re_sub_inp ) {

    return SUCCESS();

} // db_reg_rule_exec_op

// =-=-=-=-=-=-=-
// unregister a data object
irods::error db_mod_rule_exec_op(
    irods::plugin_context& _ctx,
    const char*            _re_id,
    keyValPair_t*          _reg_param ) {

    return SUCCESS ();

} // db_mod_rule_exec_op

// =-=-=-=-=-=-=-
// unregister a data object
irods::error db_del_rule_exec_op(
    irods::plugin_context& _ctx,
    const char*            _re_id ) {
    return SUCCESS ();

} // db_del_rule_exec_op




static irods::error extract_resource_properties_for_operations(
    const std::string& _resc_name,
    std::string& _resc_id,
    std::string& _resc_parent ) {

    irods::resource_ptr resc;
    irods::error ret = resc_mgr.resolve(
                           _resc_name,
                           resc);
    if(!ret.ok()) {
        return PASS(ret);
    }

    ret = resc->get_property<std::string>(
                    irods::RESOURCE_PARENT,
                    _resc_parent);
    if(!ret.ok()) {
        return PASS(ret);
    }

    rodsLong_t resc_id;
    ret = resc->get_property<rodsLong_t>(
                    irods::RESOURCE_ID,
                    resc_id);
    if(!ret.ok()) {
        return PASS(ret);
    }

    try {
        _resc_id = boost::lexical_cast<std::string>(resc_id);
    } catch( boost::bad_lexical_cast& ) {
        std::stringstream msg;
        msg << "failed to cast " << resc_id;
        return ERROR(
                INVALID_LEXICAL_CAST,
                msg.str());
    }

    return SUCCESS();

} // extract_resource_properties_for_operations

irods::error db_add_child_resc_op(
    irods::plugin_context& _ctx,
    std::map<std::string, std::string> *_resc_input ) {
    // =-=-=-=-=-=-=-
    // for readability
    std::map<std::string, std::string>& resc_input = *_resc_input;

    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    irods::sql_logger logger( __FUNCTION__, logSQL );

    logger.log();

    std::string new_child_string( resc_input[irods::RESOURCE_CHILDREN] );

    irods::children_parser child_parser;
    child_parser.set_string( new_child_string );

    irods::children_parser::children_map_t c_map;
    child_parser.list( c_map );

    if(c_map.empty()) {
       return ERROR(
                  SYS_INVALID_INPUT_PARAM,
                  "child map is empty" );
    }

    std::string child_name    = c_map.begin()->first;
    std::string child_context = c_map.begin()->second;

    std::string child_resource_id;
    std::string child_parent_name;
    ret = extract_resource_properties_for_operations(
              child_name,
              child_resource_id,
              child_parent_name);

    if(!ret.ok()) {
	if( SYS_RESC_DOES_NOT_EXIST == ret.code() ) {
	    return ERROR(
                       CHILD_NOT_FOUND,
                       child_parent_name.c_str());
	}
        return PASS(ret);
    }

    // check parent name, it must be empty
    if(!child_parent_name.empty()) {
        std::stringstream msg;
        msg << "Encountered an error adding '"
            << child_name
            << "' as a child resource.";
        addRErrorMsg(
            &_ctx.comm()->rError, 0,
            msg.str().c_str());
        return ERROR(
                   CHILD_HAS_PARENT,
                   msg.str() );
    }


    std::string& parent_name = resc_input[irods::RESOURCE_NAME];
    std::string parent_resource_id;
    std::string parent_parent_name;
    ret = extract_resource_properties_for_operations(
              parent_name,
              parent_resource_id,
              parent_parent_name);
    if(!ret.ok()) {
        if( SYS_RESC_DOES_NOT_EXIST == ret.code() ) {
            return ERROR(
                       CAT_INVALID_RESOURCE,
                       child_parent_name.c_str());
        }
        return PASS(ret);
    }

    int status = _canConnectToCatalog( _ctx.comm() );
    if(0 != status) {
        return ERROR(
                   status,
                   "_canConnectToCatalog failed");
    }

    std::string zone;
    ret = getLocalZone( _ctx.prop_map(), icss, zone );
    if(!ret.ok()) {
        return PASS(ret);
    }

    if(resc_input[irods::RESOURCE_ZONE].length() > 0 &&
       resc_input[irods::RESOURCE_ZONE] != zone ) {
        addRErrorMsg(
            &_ctx.comm()->rError, 0,
            "Currently, resources must be in the local zone" );

        return ERROR(
                   CAT_INVALID_ZONE,
                   "resources must be in the local zone");
    }

    logger.log();

    ret = _updateChildParent(
              child_resource_id,
              parent_resource_id,
              child_context );
    if(!ret.ok()) {
        return PASS(ret);
    }

    status = hs_commit(svc, icss);
    if(status != 0) {
        return ERROR(
                   status,
                   "commit failure");
    }

    return SUCCESS();

} // db_add_child_resc_op

// =-=-=-=-=-=-=-
//
irods::error db_reg_resc_op(
    irods::plugin_context& _ctx,
    std::map<std::string, std::string> *_resc_input ) {

    // =-=-=-=-=-=-=-
    // check the params
    if ( !_resc_input ) {
        return ERROR( SYS_INTERNAL_NULL_INPUT_ERR, "NULL parameter" );
    }

    // =-=-=-=-=-=-=-
    // for readability
    std::map<std::string, std::string>& resc_input = *_resc_input;

    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    rodsLong_t seqNum;
    char idNum[MAX_SQL_SIZE];
    int status;
    char myTime[50];
    struct hostent *myHostEnt; // JMC - backport 4597

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegResc" );
    }

    // =-=-=-=-=-=-=-
    // error trap empty resc name
    if ( resc_input[irods::RESOURCE_NAME].length() < 1 ) {
        addRErrorMsg( &_ctx.comm()->rError, 0, "resource name is empty" );
        return ERROR( CAT_INVALID_RESOURCE_NAME, "resource name is empty" );
    }

    // =-=-=-=-=-=-=-
    // error trap empty resc type
    if ( resc_input[irods::RESOURCE_TYPE].length() < 1 ) {
        addRErrorMsg( &_ctx.comm()->rError, 0, "resource type is empty" );
        return ERROR( CAT_INVALID_RESOURCE_TYPE, "resource type is empty" );
    }

    if ( !icss_status ) {
        return ERROR( CATALOG_NOT_CONNECTED, "catalog not connected" );
    }

    if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
    }
    if ( _ctx.comm()->proxyUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege level" );
    }

    // =-=-=-=-=-=-=-
    // Validate resource name format
    ret = validate_resource_name( resc_input[irods::RESOURCE_NAME] );
    if ( !ret.ok() ) {
        irods::log( ret );
        return PASS( ret );
    }
    // =-=-=-=-=-=-=-


    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegResc SQL 1 " );
    }
    status = hs_get_int_next_id(svc,  icss, &seqNum );
    if ( status < 0 ) {
        rodsLog( LOG_NOTICE, "chlRegResc cmlGetNextSeqVal failure %d",
                 status );
        _rollback( "chlRegResc" );
        return ERROR( status, "cmlGetNextSeqVal failure" );
    }
    snprintf( idNum, MAX_SQL_SIZE, "%lld", seqNum );

    std::string zone;
    ret = getLocalZone( _ctx.prop_map(), icss, zone );
    if ( !ret.ok() ) {
        return PASS( ret );

    }

    if ( resc_input[irods::RESOURCE_ZONE].length() > 0 ) {
        if ( resc_input[irods::RESOURCE_ZONE] != zone ) {
            addRErrorMsg( &_ctx.comm()->rError, 0,
                          "Currently, resources must be in the local zone" );
            return ERROR( CAT_INVALID_ZONE, "resources must be in the local zone" );
        }
    }
    // =-=-=-=-=-=-=-
    // JMC :: resources may now have an empty location if they
    //     :: are coordinating nodes
    //    if (strlen(_resc_info->rescLoc)<1) {
    //        return(CAT_INVALID_RESOURCE_NET_ADDR);
    //    }
    // =-=-=-=-=-=-=-
    // if the resource is not the 'empty resource' test it
    if ( resc_input[irods::RESOURCE_LOCATION] != irods::EMPTY_RESC_HOST ) {
        // =-=-=-=-=-=-=-
        // JMC - backport 4597
        _resolveHostName( _ctx.comm(), resc_input[irods::RESOURCE_LOCATION].c_str(), myHostEnt );
    }

    getNowStr( myTime );

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegResc SQL 4" );
    }
    status = hs_create_resc(svc, icss,
                  idNum,(void *) resc_input[irods::RESOURCE_NAME].c_str(),( char* )zone.c_str(),(void *) resc_input[irods::RESOURCE_TYPE].c_str(),(void *) resc_input[irods::RESOURCE_CLASS].c_str(),(void *) resc_input[irods::RESOURCE_LOCATION].c_str(),(void *) resc_input[irods::RESOURCE_PATH].c_str(),myTime,myTime,(void *) resc_input[irods::RESOURCE_CHILDREN].c_str(),(void *) resc_input[irods::RESOURCE_CONTEXT].c_str(),(void *) resc_input[irods::RESOURCE_PARENT].c_str()
                   );

    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlRegResc cmlExectuteNoAnswerSql(insert) failure %d",
                 status );
        _rollback( "chlRegResc" );
        return ERROR( status, "cmlExectuteNoAnswerSql(insert) failure" );
    }

    status =  hs_commit(svc,  icss );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlRegResc cmlExecuteNoAnswerSql commit failure %d", status );
        return ERROR( status, "cmlExecuteNoAnswerSql commit failure" );
    }

    return CODE( status );

} // db_reg_resc_op

// =-=-=-=-=-=-=-
//
irods::error db_del_child_resc_op(
    irods::plugin_context& _ctx,
    std::map<std::string, std::string> *_resc_input ) {

    // =-=-=-=-=-=-=-
    // check the params
    if ( !_resc_input ) {
        return ERROR( SYS_INTERNAL_NULL_INPUT_ERR, "NULL parameter" );
    }

    // =-=-=-=-=-=-=-
    // for readability
    std::map<std::string, std::string>& resc_input = *_resc_input;

    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    irods::sql_logger logger( "chlDelChildResc", logSQL );
    std::string child_string( resc_input[irods::RESOURCE_CHILDREN] );

    std::string& parent_name = resc_input[irods::RESOURCE_NAME];
    std::string parent_resource_id;
    std::string parent_parent_name;
    ret = extract_resource_properties_for_operations(
              parent_name,
              parent_resource_id,
              parent_parent_name);
    if(!ret.ok()) {
           return PASS(ret);
    }

    irods::children_parser parser;
    parser.set_string( child_string );

    std::string child_name;
    parser.first_child( child_name );

    std::string child_resource_id;
    std::string child_parent_name;
    ret = extract_resource_properties_for_operations(
              child_name,
              child_resource_id,
              child_parent_name);
    if(!ret.ok()) {
           return PASS(ret);
    }

    int status = _canConnectToCatalog( _ctx.comm() );
    if(0 != status) {
        return ERROR(
                   status,
                   "_canConnectToCatalog failed");
    }

    std::string zone;
    ret = getLocalZone( _ctx.prop_map(), icss, zone );
    if(!ret.ok()) {
        return PASS(ret);
    }

    ret = _updateChildParent(
              child_resource_id,
              std::string(""),
              std::string(""));
    if(!ret.ok()) {
        return PASS(ret);
    }

    status = hs_commit(svc,  icss );
    if(status != 0) {
        return ERROR(
                   status,
                   "commit failure");
    }

    return SUCCESS();

} // db_del_child_resc_op

// =-=-=-=-=-=-=-
// delete a resource
irods::error db_del_resc_op(
    irods::plugin_context& _ctx,
    const char *_resc_name,
    int                    _dry_run ) {

    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if ( !_resc_name ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "null parameter" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );

    int status;
    char rescId[MAX_NAME_LEN];

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlDelResc" );
    }

    if ( !icss_status ) {
        return ERROR( CATALOG_NOT_CONNECTED, "catalog not connected" );
    }

    if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
    }
    if ( _ctx.comm()->proxyUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
    }

    // =-=-=-=-=-=-=-
    // JMC - backport 4629
    if ( strncmp( _resc_name, BUNDLE_RESC, strlen( BUNDLE_RESC ) ) == 0 ) {
        char errMsg[155];
        snprintf( errMsg, 150,
                  "%s is a built-in resource needed for bundle operations.",
                  BUNDLE_RESC );
        addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
        return ERROR( CAT_PSEUDO_RESC_MODIFY_DISALLOWED, "cannot delete bundle resc" );
    }
    // =-=-=-=-=-=-=-

    bool has_data = true; // default to error case
    status = _rescHasData( icss, _resc_name, has_data );
    if( status < 0 ) {
        rodsLog(
            LOG_ERROR,
            "%s - _rescHasData failed for [%s] %d",
            __FUNCTION__,
            _resc_name,
            status );
        return ERROR(
                  status,
                  "failed to get object count for resource" );
    }

    if( has_data   ) {
        char errMsg[105];
        snprintf( errMsg, 100,
                  "resource '%s' contains one or more dataObjects",
                  _resc_name );
        addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
        return ERROR( CAT_RESOURCE_NOT_EMPTY, "resc not empty" );
    }

    std::string zone;
    ret = getLocalZone( _ctx.prop_map(), icss, zone );
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    /* get rescId for possible audit call; won't be available after delete */
    rescId[0] = '\0';
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlDelResc SQL 2 " );
    }
    {
        status = hs_get_resc_id_by_name(svc, icss,
                     (void *) _resc_name, rescId, MAX_NAME_LEN );
    }
    if ( status != 0 ) {
        if ( status == CAT_SUCCESS_BUT_WITH_NO_INFO ) {
            char errMsg[105];
            snprintf( errMsg, 100,
                      "resource '%s' does not exist",
                      _resc_name );
            addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
            return ERROR( status, "resource does not exits" );
        }
        _rollback( "chlDelResc" );
        return ERROR( status, "resource does not exist" );
    }

    if ( _rescHasParentOrChild( rescId ) ) {
        char errMsg[105];
        snprintf( errMsg, 100,
                  "resource '%s' has a parent or child",
                  _resc_name );
        addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
        return ERROR( CAT_RESOURCE_NOT_EMPTY, "resource not empty" );
    }

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlDelResc SQL 3" );
    }
    status = hs_delete_resc_by_name(svc, icss, (void *) _resc_name);
    if ( status != 0 ) {
        if ( status == CAT_SUCCESS_BUT_WITH_NO_INFO ) {
            char errMsg[105];
            snprintf( errMsg, 100,
                      "resource '%s' does not exist",
                      _resc_name );
            addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
            return ERROR( status, "resource does not exist" );
        }
        _rollback( "chlDelResc" );
        return ERROR( status, "resource does not exist" );
    }

    /* Remove associated AVUs, if any */
    removeMetaMapAndAVU( rescId );


    if ( _dry_run ) { // JMC
        _rollback( "chlDelResc" );
        return CODE( status );
    }

    status =  hs_commit(svc,  icss );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlDelResc cmlExecuteNoAnswerSql commit failure %d",
                 status );
        return ERROR( status, "cmlExecuteNoAnswerSql commit failure" );
    }
    return CODE( status );

} // db_del_resc_op

// =-=-=-=-=-=-=-
// rollback the db
irods::error db_rollback_op(
    irods::plugin_context& _ctx ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRollback - SQL 1 " );
    }

    int status =  hs_rollback(svc,  icss );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlRollback cmlExecuteNoAnswerSql failure %d",
                 status );
        return ERROR( status, "chlRollback cmlExecuteNoAnswerSql failure" );
    }

    return CODE( status );

} // db_rollback_op

// =-=-=-=-=-=-=-
// commit the transaction
irods::error db_commit_op(
    irods::plugin_context& _ctx ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlCommit - SQL 1 " );
    }
    int status =  hs_commit(svc,  icss );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlCommit cmlExecuteNoAnswerSql failure %d",
                 status );
        return ERROR( status, "chlCommit cmlExecuteNoAnswerSql failure" );
    }

    return CODE( status );

} // db_commit_op

// =-=-=-=-=-=-=-
// commit the transaction
irods::error db_del_user_re_op(
    irods::plugin_context& _ctx,
    userInfo_t*            _user_info ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_user_info ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "null parameter" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    int status;
    char iValStr[200];
    char zoneToUse[MAX_NAME_LEN];
    // char userStr[200];
    char userName2[NAME_LEN];
    char zoneName[NAME_LEN];

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlDelUserRE" );
    }

    if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
    }
    if ( _ctx.comm()->proxyUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege level" );
    }

    std::string zone;
    ret = getLocalZone( _ctx.prop_map(), icss, zone );
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    snprintf( zoneToUse, sizeof( zoneToUse ), "%s", zone.c_str() );
    if ( strlen( _user_info->rodsZone ) > 0 ) {
        snprintf( zoneToUse, sizeof( zoneToUse ), "%s", _user_info->rodsZone );
    }

    status = validateAndParseUserName( _user_info->userName, userName2, zoneName );
    if ( status ) {
        return ERROR( status, "Invalid username format" );
    }
    if ( zoneName[0] != '\0' ) {
        rstrcpy( zoneToUse, zoneName, NAME_LEN );
    }

    if ( strncmp( _ctx.comm()->clientUser.userName, userName2, sizeof( userName2 ) ) == 0 &&
            strncmp( _ctx.comm()->clientUser.rodsZone, zoneToUse, sizeof( zoneToUse ) ) == 0 ) {
        addRErrorMsg( &_ctx.comm()->rError, 0, "Cannot remove your own admin account, probably unintended" );
        return ERROR( CAT_INVALID_USER, "invalid user" );
    }


    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlDelUserRE SQL 1 " );
    }
    {
        status = hs_get_user_id(svc, icss, zoneToUse, userName2, 
                     iValStr, 200);
    }
    if ( status == CAT_SUCCESS_BUT_WITH_NO_INFO ||
            status == CAT_NO_ROWS_FOUND ) {
        addRErrorMsg( &_ctx.comm()->rError, 0, "Invalid user" );
        return ERROR( CAT_INVALID_USER, "invalid user" );
    }
    if ( status != 0 ) {
        _rollback( "chlDelUserRE" );
        return ERROR( status, "failed getting user from table" );
    }

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlDelUserRE SQL 2" );
    }
    status = hs_delete_user_by_zone_and_name(svc, icss, zoneToUse, userName2
                  );
    if ( status == CAT_SUCCESS_BUT_WITH_NO_INFO ) {
        return ERROR( CAT_INVALID_USER, "invalid user" );
    }
    if ( status != 0 ) {
        _rollback( "chlDelUserRE" );
        return ERROR( status, "cmlExecuteNoAnswerSql for delete user failed" );
    }

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlDelUserRE SQL 3" );
    }
    status = hs_delete_user_password_by_user_id(svc, icss, iValStr);
    if ( status != 0 && status != CAT_SUCCESS_BUT_WITH_NO_INFO ) {
        char errMsg[MAX_NAME_LEN + 40];
        rodsLog( LOG_NOTICE,
                 "chlDelUserRE delete password failure %d",
                 status );
        snprintf( errMsg, sizeof errMsg, "Error removing password entry" );
        addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
        _rollback( "chlDelUserRE" );
        return ERROR( status, "Error removing password entry" );
    }

    /* Remove both the special user_id = group_user_id entry and any
       other access entries for this user (or group) */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlDelUserRE SQL 4" );
    }
    status = hs_delete_user_group_(svc, icss, iValStr, iValStr);
    if ( status != 0 && status != CAT_SUCCESS_BUT_WITH_NO_INFO ) {
        char errMsg[MAX_NAME_LEN + 40];
        rodsLog( LOG_NOTICE,
                 "chlDelUserRE delete user_group entry failure %d",
                 status );
        snprintf( errMsg, sizeof errMsg, "Error removing user_group entry" );
        addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
        _rollback( "chlDelUserRE" );
        return ERROR( status, "Error removing user_group entry" );
    }

    /* Remove any R_USER_AUTH rows for this user */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlDelUserRE SQL 4" );
    }
    status = hs_delete_user_auth(svc, icss, iValStr
                  );
    if ( status != 0 && status != CAT_SUCCESS_BUT_WITH_NO_INFO ) {
        char errMsg[MAX_NAME_LEN + 40];
        rodsLog( LOG_NOTICE,
                 "chlDelUserRE delete user_auth entries failure %d",
                 status );
        snprintf( errMsg, sizeof errMsg, "Error removing user_auth entries" );
        addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
        _rollback( "chlDelUserRE" );
        return ERROR( status, "Error removing user_auth entries" );
    }

    /* Remove associated AVUs, if any */
    removeMetaMapAndAVU( iValStr );

    return SUCCESS();

} // db_del_user_re_op

// =-=-=-=-=-=-=-
// commit the transaction
irods::error db_reg_coll_by_admin_op(
    irods::plugin_context& _ctx,
    collInfo_t*            _coll_info ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_coll_info ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "null parameter" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    char myTime[50];
    char logicalEndName[MAX_NAME_LEN];
    char logicalParentDirName[MAX_NAME_LEN];
    rodsLong_t iVal;
    char collIdNum[MAX_NAME_LEN];
    char nextStr[MAX_NAME_LEN];
    // char currStr[MAX_NAME_LEN];
    // char currStr2[MAX_SQL_SIZE];
    int status;
    // char tSQL[MAX_SQL_SIZE];
    char userName2[NAME_LEN];
    char zoneName[NAME_LEN];

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegCollByAdmin" );
    }

    if ( !icss_status ) {
        return ERROR( CATALOG_NOT_CONNECTED, "catalog not connected" );
    }

    // =-=-=-=-=-=-=-
    // JMC - backport 4772
    if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ||
            _ctx.comm()->proxyUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        int status2;
        status2  = cmlCheckGroupAdminAccess(
                       _ctx.comm()->clientUser.userName,
                       _ctx.comm()->clientUser.rodsZone,
                       "", svc, icss );
        if ( status2 != 0 ) {
            return ERROR( status2, "no group admin access" );
        }
        if ( creatingUserByGroupAdmin == 0 ) {
            return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficent privilege" );
        }
        // =-=-=-=-=-=-=-
    }

    status = splitPathByKey( _coll_info->collName,
                             logicalParentDirName, MAX_NAME_LEN, logicalEndName, MAX_NAME_LEN, '/' );

    if ( strlen( logicalParentDirName ) == 0 ) {
        snprintf( logicalParentDirName, sizeof( logicalParentDirName ), "%s", PATH_SEPARATOR );
        snprintf( logicalEndName, sizeof( logicalEndName ), "%s", _coll_info->collName + 1 );
    }

    /* Check that the parent collection exists */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegCollByAdmin SQL 1 " );
    }
    {
        status = hs_get_int_coll_id_by_name(svc, icss, logicalParentDirName,
                     &iVal);
    }
    if ( status < 0 ) {
        char errMsg[MAX_NAME_LEN + 40];
        if ( status == CAT_NO_ROWS_FOUND ) {
            snprintf( errMsg, sizeof errMsg,
                      "collection '%s' is unknown, cannot create %s under it",
                      logicalParentDirName, logicalEndName );
            addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
            return ERROR( status, "collection is unknown" );
        }
        _rollback( "chlRegCollByAdmin" );
        return ERROR( status, "collection not found" );
    }

    snprintf( collIdNum, MAX_NAME_LEN, "%d", status );

    /* String to get next sequence item for objects */
    hs_get_next_id(svc,  icss, nextStr, MAX_NAME_LEN );

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegCollByAdmin SQL 2" );
    }
    getNowStr( myTime );

    std::string zone;
    ret = getLocalZone( _ctx.prop_map(), icss, zone );
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    /* Parse input name into user and zone */
    status = validateAndParseUserName( _coll_info->collOwnerName, userName2, zoneName );
    if ( status ) {
        return ERROR( status, "Invalid username format" );
    }
    if ( zoneName[0] == '\0' ) {
        rstrcpy( zoneName, zone.c_str(), NAME_LEN );
    }

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegCollByAdmin SQL 3" );
    }
    status =  hs_create_coll(svc,  icss, nextStr, logicalParentDirName,_coll_info->collName,userName2,
	strlen( _coll_info->collOwnerZone ) > 0 ? _coll_info->collOwnerZone : zoneName,
    _coll_info->collType, _coll_info->collInfo1, _coll_info->collInfo2, myTime, myTime
                                      );
    if ( status != 0 ) {
        char errMsg[105];
        if ( status == CATALOG_ALREADY_HAS_ITEM_BY_THAT_NAME ) {
            snprintf( errMsg, 100, "Error %d %s",
                      status,
                      "CATALOG_ALREADY_HAS_ITEM_BY_THAT_NAME"
                    );
            addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
        }

        rodsLog( LOG_NOTICE,
                 "chlRegCollByAdmin cmlExecuteNoAnswerSQL(insert) failure %d"
                 , status );
        _rollback( "chlRegCollByAdmin" );
        return ERROR( status, "cmlExecuteNoAnswerSQL(insert) failure" );
    }

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegCollByAdmin SQL 4" );
    }
    status =  hs_create_access_by_user_zone_and_name(svc, icss, nextStr, zoneName, userName2, (void *) ACCESS_OWN, myTime, myTime);
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlRegCollByAdmin cmlExecuteNoAnswerSql(insert access) failure %d",
                 status );
        _rollback( "chlRegCollByAdmin" );
        return ERROR( status, "cmlExecuteNoAnswerSql(insert access) failure" );
    }

    return SUCCESS();

} // db_reg_coll_by_admin_op

// =-=-=-=-=-=-=-
// commit the transaction
irods::error db_reg_coll_op(
    irods::plugin_context& _ctx,
    collInfo_t*            _coll_info ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_coll_info ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "null parameter" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    char myTime[50];
    char logicalEndName[MAX_NAME_LEN];
    char logicalParentDirName[MAX_NAME_LEN];
    rodsLong_t iVal;
    char collIdNum[MAX_NAME_LEN];
    char nextStr[MAX_NAME_LEN];
    // char currStr[MAX_NAME_LEN];
    // char currStr2[MAX_SQL_SIZE];
    rodsLong_t status;
    // char tSQL[MAX_SQL_SIZE];
    int inheritFlag;

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegColl" );
    }

    if ( !icss_status ) {
        return ERROR( CATALOG_NOT_CONNECTED, "catalog not connected" );
    }

    status = splitPathByKey( _coll_info->collName,
                             logicalParentDirName, MAX_NAME_LEN, logicalEndName, MAX_NAME_LEN, '/' );

    if ( strlen( logicalParentDirName ) == 0 ) {
        snprintf( logicalParentDirName, sizeof( logicalParentDirName ), "%s", PATH_SEPARATOR );
        snprintf( logicalEndName, sizeof( logicalEndName ), "%s", _coll_info->collName + 1 );
    }

    /* Check that the parent collection exists and user has write permission,
       and get the collectionID.  Also get the inherit flag */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegColl SQL 1 " );
    }

    status = cmlCheckDirAndGetInheritFlag( logicalParentDirName,
                                           _ctx.comm()->clientUser.userName,
                                           _ctx.comm()->clientUser.rodsZone,
                                           ACCESS_MODIFY_OBJECT, &inheritFlag,
                                           mySessionTicket, mySessionClientAddr, svc, icss );
    if ( status < 0 ) {
        char errMsg[105];
        if ( status == CAT_UNKNOWN_COLLECTION ) {
            snprintf( errMsg, 100, "collection '%s' is unknown",
                      logicalParentDirName );
            addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
            return ERROR( status, "collection is unknown" );
        }
        _rollback( "chlRegColl" );
        return ERROR( status, "cmlCheckDirAndGetInheritFlag failed" );
    }
    snprintf( collIdNum, MAX_NAME_LEN, "%lld", status );

    /* Check that the path is not already a dataObj */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegColl SQL 2" );
    }
    {
        status = hs_get_int_data_id(svc, icss, collIdNum, logicalEndName,
                     &iVal);
    }

    if ( status == 0 ) {
        return ERROR( CAT_NAME_EXISTS_AS_DATAOBJ, "data obj alread exists" );
    }


    /* String to get next sequence item for objects */
    hs_get_next_id(svc,  icss, nextStr, MAX_NAME_LEN );

    getNowStr( myTime );

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegColl SQL 3" );
    }
    status =  hs_create_coll(svc,  icss,  nextStr,  logicalParentDirName,_coll_info->collName,_ctx.comm()->clientUser.userName,_ctx.comm()->clientUser.rodsZone,_coll_info->collType,_coll_info->collInfo1,_coll_info->collInfo2,myTime,myTime
                                      );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlRegColl cmlExecuteNoAnswerSql(insert) failure %d", status );
        _rollback( "chlRegColl" );
        return ERROR( status, "cmlExecuteNoAnswerSql(insert) failure" );
    }

    if ( inheritFlag ) {
        /* If inherit is set (sticky bit), then add access rows for this
           collection that match those of the parent collection */
        status =  hs_create_access_inherit_coll(svc, icss, nextStr, myTime, myTime, collIdNum );

        if ( status == 0 ) {
            if ( logSQL != 0 ) {
                rodsLog( LOG_SQL, "chlRegColl SQL 5" );
            }

            if ( status > 0 ) {
                status =  hs_update_coll_inheritance_ts(svc, icss, nextStr, (void *) "1", myTime);
            }

        }
    }
    else {
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlRegColl SQL 6" );
        }
        status =  hs_create_access_by_user_zone_and_name(svc,  icss, nextStr, _ctx.comm()->clientUser.rodsZone, _ctx.comm()->clientUser.userName, (void *) ACCESS_OWN, myTime, myTime );
    }
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlRegColl cmlExecuteNoAnswerSql(insert access) failure %d",
                 status );
        _rollback( "chlRegColl" );
        return ERROR( status, "cmlExecuteNoAnswerSql(insert access) failure" );
    }

    status =  hs_commit(svc,  icss );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlRegColl cmlExecuteNoAnswerSql commit failure %d",
                 status );
        return ERROR( status, "cmlExecuteNoAnswerSql commit failure" );
    }

    return CODE( status );

} // db_reg_coll_op

// =-=-=-=-=-=-=-
// commit the transaction
irods::error db_mod_coll_op(
    irods::plugin_context& _ctx,
    collInfo_t*            _coll_info ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_coll_info ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "null parameter" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    char myTime[50];
    rodsLong_t status;
    int count;
    rodsLong_t iVal;
    // char iValStr[60];

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlModColl" );
    }

    if ( !icss_status ) {
        return ERROR( CATALOG_NOT_CONNECTED, "catalog not connected" );
    }

    /* Check that collection exists and user has write permission */
    iVal = cmlCheckDir( _coll_info->collName,  _ctx.comm()->clientUser.userName,
                        _ctx.comm()->clientUser.rodsZone,
                        ACCESS_MODIFY_OBJECT, svc, icss );

    if ( iVal < 0 ) {
        if ( iVal == CAT_UNKNOWN_COLLECTION ) {
            std::stringstream errMsg;
            errMsg << "collection '" << _coll_info->collName << "' is unknown";
            addRErrorMsg( &_ctx.comm()->rError, 0, errMsg.str().c_str() );
            return ERROR( CAT_UNKNOWN_COLLECTION, "unknown collection" );
        }
        if ( iVal == CAT_NO_ACCESS_PERMISSION ) {
            std::stringstream errMsg;
            errMsg << "no permission to update collection '" << _coll_info->collName << "'";
            addRErrorMsg( &_ctx.comm()->rError, 0, errMsg.str().c_str() );
            return  ERROR( CAT_NO_ACCESS_PERMISSION, "no permission" );
        }
        return ERROR( iVal, "cmlCheckDir failed" );
    }

    char *updateVals[4];
	char *updateCols[4];
    count = 0;
    if ( strlen( _coll_info->collType ) > 0 ) {
        if ( strcmp( _coll_info->collType, "NULL_SPECIAL_VALUE" ) == 0 ) {
            /* A special value to indicate NULL */
            updateVals[count] = "";
        }
        else {
            updateVals[count] = _coll_info->collType;
        }
        updateCols[count] = "coll_type";
        count++;
    }
    if ( strlen( _coll_info->collInfo1 ) > 0 ) {
        if ( strcmp( _coll_info->collInfo1, "NULL_SPECIAL_VALUE" ) == 0 ) {
            /* A special value to indicate NULL */
            updateVals[count] = "";
        }
        else {
            updateVals[count] = _coll_info->collInfo1;
        }
        updateCols[count] = "coll_info1";
        count++;
    }
    if ( strlen( _coll_info->collInfo2 ) > 0 ) {
        if ( strcmp( _coll_info->collInfo2, "NULL_SPECIAL_VALUE" ) == 0 ) {
            /* A special value to indicate NULL */
            updateVals[count] = "";
        }
        else {
            updateVals[count] = _coll_info->collInfo2;
        }
        updateCols[count] = "coll_info2";
        count++;
    }
    if ( count == 0 ) {
        return ERROR( CAT_INVALID_ARGUMENT, "count is 0" );
    }
    getNowStr( myTime );
	updateCols[count] = "modify_ts";
	updateVals[count] = myTime;
	count++;

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlModColl SQL 1" );
    }
    status =  hs_modify_coll(svc,  icss,updateCols, updateVals, count, _coll_info->collName
                                      );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlModColl cmlExecuteNoAnswerSQL(update) failure %d", status );
        return ERROR( status, "cmlExecuteNoAnswerSQL(update) failure" );
    }

    return SUCCESS();

} // db_mod_coll_op

// =-=-=-=-=-=-=-
// commit the transaction
irods::error db_reg_zone_op(
    irods::plugin_context& _ctx,
    const char*            _zone_name,
    const char*            _zone_type,
    const char*            _zone_conn_info,
    const char*            _zone_comment ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_zone_name ||
        !_zone_type ||
        !_zone_conn_info ||
        !_zone_comment ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "null parameter" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    char nextStr[MAX_NAME_LEN];
    // char tSQL[MAX_SQL_SIZE];
    int status;
    char myTime[50];

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegZone" );
    }

    if ( !icss_status ) {
        return ERROR( CATALOG_NOT_CONNECTED, "catalog not connected" );
    }

    if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
    }
    if ( _ctx.comm()->proxyUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
    }

    if ( strncmp( _zone_type, "remote", 6 ) != 0 ) {
        addRErrorMsg( &_ctx.comm()->rError, 0,
                      "Currently, only zones of type 'remote' are allowed" );
        return ERROR( CAT_INVALID_ARGUMENT, "Currently, only zones of type 'remote' are allowed" );
    }

    // =-=-=-=-=-=-=-
    // validate the zone name does not include improper characters
    ret = validate_zone_name( _zone_name );
    if ( !ret.ok() ) {
        irods::log( ret );
        return PASS( ret );
    }

    /* String to get next sequence item for objects */
    hs_get_next_id(svc,  icss, nextStr, MAX_NAME_LEN );

    getNowStr( myTime );

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegZone SQL 1 " );
    }

    status =  hs_create_zone(svc, icss, nextStr, (void *) _zone_name, (void *) "remote", (void *) _zone_conn_info, (void *) _zone_comment, myTime, myTime);
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlRegZone cmlExecuteNoAnswerSql(insert) failure %d", status );
        _rollback( "chlRegZone" );
        return ERROR( status, "cmlExecuteNoAnswerSql(insert) failure" );
    }


    status =  hs_commit(svc,  icss );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlRegZone cmlExecuteNoAnswerSql commit failure %d",
                 status );
        return ERROR( status, "cmlExecuteNoAnswerSql commit failure" );
    }

    return SUCCESS();

} // db_reg_zone_op

// =-=-=-=-=-=-=-
// modify the zone
irods::error db_mod_zone_op(
    irods::plugin_context& _ctx,
    const char*            _zone_name,
    const char*            _option,
    const char*            _option_value ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_zone_name ||
        !_option ||
        !_option_value ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "null parameter" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    int status, OK;
    char myTime[50];
    char zoneId[MAX_NAME_LEN];
    // char commentStr[200];

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlModZone" );
    }

    if ( *_zone_name == '\0' || *_option == '\0' || *_option_value == '\0' ) {
        return  ERROR( CAT_INVALID_ARGUMENT, "invalid arument value" );
    }

    if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
    }
    if ( _ctx.comm()->proxyUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
    }

    std::string zone;
    ret = getLocalZone( _ctx.prop_map(), icss, zone );
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    zoneId[0] = '\0';
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlModZone SQL 1 " );
    }
    {
        status = hs_get_zone_id(svc, icss, (void *) _zone_name,
                     zoneId, MAX_NAME_LEN  );
    }
    if ( status != 0 ) {
        if ( status == CAT_NO_ROWS_FOUND ) {
            return ERROR( CAT_INVALID_ZONE, "invalid zone name" );
        }
        return ERROR( status, "error getting zone" );
    }

    getNowStr( myTime );
    OK = 0;
    if ( strcmp( _option, "comment" ) == 0 ) {
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModZone SQL 3" );
        }
        status =  hs_create_zone_comment_ts(svc, icss,zoneId,myTime,(void *) _option_value);
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlModZone cmlExecuteNoAnswerSql update failure %d",
                     status );
            return ERROR( status, "cmlExecuteNoAnswerSql update failure" );
        }
        OK = 1;
    }
    if ( strcmp( _option, "conn" ) == 0 ) {
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModZone SQL 5" );
        }
        status =  hs_create_zone_conn_string_ts(svc, icss,zoneId,myTime,(void *) _option_value);
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlModZone cmlExecuteNoAnswerSql update failure %d",
                     status );
            return ERROR( status, "cmlExecuteNoAnswerSql update failure" );
        }
        OK = 1;
    }
    if ( strcmp( _option, "name" ) == 0 ) {
        if ( strcmp( _zone_name, zone.c_str() ) == 0 ) {
            addRErrorMsg( &_ctx.comm()->rError, 0,
                          "It is not valid to rename the local zone via chlModZone; iadmin should use acRenameLocalZone" );
            return ERROR( CAT_INVALID_ARGUMENT, "cannot rename localzone" );
        }

        // =-=-=-=-=-=-=-
        // validate the zone name does not include improper characters
        ret = validate_zone_name( _option_value );
        if ( !ret.ok() ) {
            irods::log( ret );
            std::string msg( "zone name is invalid [" );
            msg += _option_value;
            msg += "]";
            addRErrorMsg( &_ctx.comm()->rError, 0,
                          msg.c_str() );
            return PASS( ret );
        }

        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModZone SQL 5" );
        }
        status =  hs_create_zone_name_ts(svc, icss,zoneId,myTime,(void *) _option_value);
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlModZone cmlExecuteNoAnswerSql update failure %d",
                     status );
            return ERROR( status, "cmlExecuteNoAnswerSql update failure" );
        }
        OK = 1;
    }
    if ( OK == 0 ) {
        return ERROR( CAT_INVALID_ARGUMENT, "invalid option" );
    }

    status =  hs_commit(svc,  icss );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlModZone cmlExecuteNoAnswerSql commit failure %d",
                 status );
        return ERROR( status, "cmlExecuteNoAnswerSql commit failure" );
    }

    return SUCCESS();

} // db_mod_zone_op

// =-=-=-=-=-=-=-
// modify the zone
irods::error db_rename_coll_op(
    irods::plugin_context& _ctx,
    const char*            _old_coll,
    const char*            _new_coll ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_old_coll ||
        !_new_coll ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "null parameter" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    int status;
    rodsLong_t status1;

    /* See if the input path is a collection and the user owns it,
       and, if so, get the collectionID */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRenameColl SQL 1 " );
    }

    status1 = cmlCheckDir( _old_coll,
                           _ctx.comm()->clientUser.userName,
                           _ctx.comm()->clientUser.rodsZone,
                           ACCESS_OWN, svc,
                           icss );

    if ( status1 < 0 ) {
        return ERROR( status1, "cmlCheckDir failed" );
    }

    /* call chlRenameObject to rename */
    status = chlRenameObject( _ctx.comm(), status1, _new_coll );
    if ( !status ) {
        return ERROR( status, "chlRenameObject failed" );
    }

    return CODE( status );

} // db_rename_coll_op

// =-=-=-=-=-=-=-
// modify the zone
irods::error db_mod_zone_coll_acl_op(
    irods::plugin_context& _ctx,
    const char*            _access_level,
    const char*            _user_name,
    const char*            _path_name ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_access_level ||
        !_user_name ||
        !_path_name ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "null parameter" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    int status;
    if ( *_path_name != '/' ) {
        return ERROR( CAT_INVALID_ARGUMENT, "invalid path name" );
    }
    const char* cp = _path_name + 1;
    if ( strstr( cp, PATH_SEPARATOR ) != NULL ) {
        return ERROR( CAT_INVALID_ARGUMENT, "invalid path name" );
    }
    status =  chlModAccessControl( _ctx.comm(), 0,
                                   _access_level,
                                   _user_name,
                                   _ctx.comm()->clientUser.rodsZone,
                                   _path_name );
    if ( !status ) {
        return ERROR( status, "chlModAccessControl failed" );
    }

    return CODE( status );

} // db_mod_zone_coll_acl_op

// =-=-=-=-=-=-=-
// modify the zone
irods::error db_rename_local_zone_op(
    irods::plugin_context& _ctx,
    const char*            _old_zone,
    const char*            _new_zone ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_old_zone ||
        !_new_zone ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "null parameter" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    int status;
    char zoneId[MAX_NAME_LEN];
    char myTime[50];
    // char commentStr[200];

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRenameLocalZone" );
    }

    if ( !icss_status ) {
        return ERROR( CATALOG_NOT_CONNECTED, "catalog not connected" );
    }

    if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
    }
    if ( _ctx.comm()->proxyUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
    }

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRenameLocalZone SQL 1 " );
    }

    std::string zone;
    ret = getLocalZone( _ctx.prop_map(), icss, zone );
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    if ( strcmp( zone.c_str(), _old_zone ) != 0 ) { /* not the local zone */
        return ERROR( CAT_INVALID_ARGUMENT, "not the local zone" );
    }

    /* check that the new zone does not exist */
    zoneId[0] = '\0';
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRenameLocalZone SQL 2 " );
    }
    {
        status = hs_get_zone_id(svc, icss, (void *) _new_zone,
                     zoneId, MAX_NAME_LEN);
    }
    if ( status != CAT_NO_ROWS_FOUND ) {
        return ERROR( CAT_INVALID_ZONE, "zone not found" );
    }

    getNowStr( myTime );

    /* update coll_owner_zone in R_COLL_MAIN */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRenameLocalZone SQL 3 " );
    }
    status =  hs_create_coll_owner_zone_by_zone_name(svc, icss,(void *) _old_zone,(void *) _new_zone,myTime);
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlRenameLocalZone cmlExecuteNoAnswerSql update failure %d",
                 status );
        return ERROR( status, "cmlExecuteNoAnswerSql update failure" );
    }

    /* update data_owner_zone in R_DATA_MAIN */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRenameLocalZone SQL 4 " );
    }
    status =  hs_create_data_owner_zone_by_zone_name(svc, icss,(void *) _old_zone, (void *) _new_zone, myTime );
    if ( status != 0 && status != CAT_SUCCESS_BUT_WITH_NO_INFO ) {
        rodsLog( LOG_NOTICE,
                 "chlRenameLocalZone cmlExecuteNoAnswerSql update failure %d",
                 status );
        return ERROR( status, "cmlExecuteNoAnswerSql update failure" );
    }

    /* update zone_name in R_RESC_MAIN */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRenameLocalZone SQL 5 " );
    }
    status =  hs_create_resc_zone_by_zone_name(svc, icss, (void *) _old_zone, (void *) _new_zone, myTime );
    if ( status != 0 && status != CAT_SUCCESS_BUT_WITH_NO_INFO ) {
        rodsLog( LOG_NOTICE,
                 "chlRenameLocalZone cmlExecuteNoAnswerSql update failure %d",
                 status );
        return ERROR( status, "cmlExecuteNoAnswerSql update failure" );
    }

    /* update rule_owner_zone in R_RULE_MAIN */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRenameLocalZone SQL 6 " );
    }
    status =  hs_create_rule_owner_zone_by_zone_name(svc, icss,(void *) _old_zone, (void *) _new_zone, myTime);
    if ( status != 0 && status != CAT_SUCCESS_BUT_WITH_NO_INFO ) {
        rodsLog( LOG_NOTICE,
                 "chlRenameLocalZone cmlExecuteNoAnswerSql update failure %d",
                 status );
        return ERROR( status, "cmlExecuteNoAnswerSql update failure" );
    }

    /* update zone_name in R_USER_MAIN */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRenameLocalZone SQL 7 " );
    }
    status =  hs_create_user_zone_by_zone_name(svc,  icss,  (void *) _old_zone,(void *) _new_zone,myTime);
    if ( status != 0 && status != CAT_SUCCESS_BUT_WITH_NO_INFO ) {
        rodsLog( LOG_NOTICE,
                 "chlRenameLocalZone cmlExecuteNoAnswerSql update failure %d",
                 status );
        return ERROR( status, "cmlExecuteNoAnswerSql update failure" );
    }

    /* update zone_name in R_ZONE_MAIN */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRenameLocalZone SQL 8 " );
    }
    status =  hs_create_zone_name_by_zone_name(svc,    icss, (void *) _old_zone, (void *) _new_zone, myTime );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlRenameLocalZone cmlExecuteNoAnswerSql update failure %d",
                 status );
        return ERROR( status, "cmlExecuteNoAnswerSql update failure" );
    }

    return SUCCESS();

} // db_rename_local_zone_op

// =-=-=-=-=-=-=-
// modify the zone
irods::error db_del_zone_op(
    irods::plugin_context& _ctx,
    const char*            _zone_name ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_zone_name ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "null parameter" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    int status;
    char zoneType[MAX_NAME_LEN];

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlDelZone" );
    }

    if ( !icss_status ) {
        return ERROR( CATALOG_NOT_CONNECTED, "catalog not connected" );
    }

    if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege level" );
    }
    if ( _ctx.comm()->proxyUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege level" );
    }

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlDelZone SQL 1 " );
    }

    {
        status = hs_get_zone_type_by_name(svc, icss,(void *) _zone_name,
                     zoneType, MAX_NAME_LEN);
    }
    if ( status != 0 ) {
        if ( status == CAT_NO_ROWS_FOUND ) {
            return ERROR( CAT_INVALID_ZONE, "invalid zone name" );
        }
        return ERROR( status, "failed to get zone" );
    }

    if ( strcmp( zoneType, "remote" ) != 0 ) {
        addRErrorMsg( &_ctx.comm()->rError, 0,
                      "It is not permitted to remove the local zone" );
        return ERROR( CAT_INVALID_ARGUMENT, "cannot remove local zone" );
    }

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlDelZone 2" );
    }
    status =  hs_delete_zone_by_name(svc, icss, (void *) _zone_name
                   );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlDelZone cmlExecuteNoAnswerSql delete failure %d",
                 status );
        return ERROR( status, "cmlExecuteNoAnswerSql delete failure" );
    }

    status =  hs_commit(svc,  icss );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlDelZone cmlExecuteNoAnswerSql commit failure %d",
                 status );
        return ERROR( status, "cmlExecuteNoAnswerSql commit failure" );
    }

    return SUCCESS();

} // db_del_zone_op

struct control {
    char **out;
    int index;
    int n;
    int rows;
    control() : out(NULL), index(0), n(0), rows(0) {}
    ~control() {
      if(out != NULL) {
        for(int i=0;i<n;i++) {
          free(out[i]);
        }
        free(out);
      }
    }
};

std::vector<control *> controls;
int next_control() {
  size_t i;
  for(i =0;i<controls.size();i++) {
    if(controls[i] == NULL) {
      controls[i] = new control();
      return i + 1;
    }
  }
  controls.push_back(new control());
  return controls.size();
}

void release_control(int i) {
  delete controls[i-1];
  controls[i-1] = NULL;
}

// =-=-=-=-=-=-=-
// modify the zone
irods::error db_simple_query_op_vector(
    irods::plugin_context& _ctx,
    const char*                  _sql,
    std::vector<const char *> _bindVars,
    int                    _format,
    int*                   _control,
    char*                  _out_buf,
    int                    _max_out_buf ) {
      // =-=-=-=-=-=-=-
      // check the context
      irods::error ret = _ctx.valid();
      if ( !ret.ok() ) {
          return PASS( ret );
      }

      // =-=-=-=-=-=-=-
      // get a postgres object from the context
      /*irods::postgres_object_ptr pg;
      ret = make_db_ptr( _ctx.fco(), pg );
      if ( !ret.ok() ) {
          return PASS( ret );
      }*/

      // =-=-=-=-=-=-=-
      // extract the icss property
  //        icatSessionStruct icss;
  //        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
      int status, nCols, nRows, i;
      int rowSize;
      int rows;
      int OK;

      char *allowedSQL[] = {
          "select token_name from R_TOKN_MAIN where token_namespace = 'token_namespace'",
          "select token_name from R_TOKN_MAIN where token_namespace = ?",
          "select * from R_TOKN_MAIN where token_namespace = ? and token_name like ?",
          "select resc_name from R_RESC_MAIN",
          "select * from R_RESC_MAIN where resc_name=?",
          "select zone_name from R_ZONE_MAIN",
          "select * from R_ZONE_MAIN where zone_name=?",
          "select user_name from R_USER_MAIN where user_type_name='rodsgroup'",
          "select user_name||'#'||zone_name from R_USER_MAIN, R_USER_GROUP where R_USER_GROUP.user_id=R_USER_MAIN.user_id and R_USER_GROUP.group_user_id=(select user_id from R_USER_MAIN where user_name=?)",
          "select * from R_DATA_MAIN where data_id=?",
          "select data_name, data_id, data_repl_num from R_DATA_MAIN where coll_id =(select coll_id from R_COLL_MAIN where coll_name=?)",
          "select coll_name from R_COLL_MAIN where parent_coll_name=?",
          "select * from R_USER_MAIN where user_name=?",
          "select user_name||'#'||zone_name from R_USER_MAIN where user_type_name != 'rodsgroup'",
          "select R_RESC_GROUP.resc_group_name, R_RESC_GROUP.resc_id, resc_name, R_RESC_GROUP.create_ts, R_RESC_GROUP.modify_ts from R_RESC_MAIN, R_RESC_GROUP where R_RESC_MAIN.resc_id = R_RESC_GROUP.resc_id and resc_group_name=?",
          "select distinct resc_group_name from R_RESC_GROUP",
          "select coll_id from R_COLL_MAIN where coll_name = ?",
          "select * from R_USER_MAIN where user_name=? and zone_name=?",
          "select user_name from R_USER_MAIN where zone_name=? and user_type_name != 'rodsgroup'",
          "select zone_name from R_ZONE_MAIN where zone_type_name=?",
          "select user_name, user_auth_name from R_USER_AUTH, R_USER_MAIN where R_USER_AUTH.user_id = R_USER_MAIN.user_id and R_USER_MAIN.user_name=?",
          "select user_name, user_auth_name from R_USER_AUTH, R_USER_MAIN where R_USER_AUTH.user_id = R_USER_MAIN.user_id and R_USER_MAIN.user_name=? and R_USER_MAIN.zone_name=?",
          "select user_name, user_auth_name from R_USER_AUTH, R_USER_MAIN where R_USER_AUTH.user_id = R_USER_MAIN.user_id",
          "select user_name, user_auth_name from R_USER_AUTH, R_USER_MAIN where R_USER_AUTH.user_id = R_USER_MAIN.user_id and R_USER_AUTH.user_auth_name=?",
          "select user_name, R_USER_MAIN.zone_name, resc_name, quota_limit, quota_over, R_QUOTA_MAIN.modify_ts from R_QUOTA_MAIN, R_USER_MAIN, R_RESC_MAIN where R_USER_MAIN.user_id = R_QUOTA_MAIN.user_id and R_RESC_MAIN.resc_id = R_QUOTA_MAIN.resc_id",
          "select user_name, R_USER_MAIN.zone_name, resc_name, quota_limit, quota_over, R_QUOTA_MAIN.modify_ts from R_QUOTA_MAIN, R_USER_MAIN, R_RESC_MAIN where R_USER_MAIN.user_id = R_QUOTA_MAIN.user_id and R_RESC_MAIN.resc_id = R_QUOTA_MAIN.resc_id and user_name=? and R_USER_MAIN.zone_name=?",
          "select user_name, R_USER_MAIN.zone_name, quota_limit, quota_over, R_QUOTA_MAIN.modify_ts from R_QUOTA_MAIN, R_USER_MAIN where R_USER_MAIN.user_id = R_QUOTA_MAIN.user_id and R_QUOTA_MAIN.resc_id = 0",
          "select user_name, R_USER_MAIN.zone_name, quota_limit, quota_over, R_QUOTA_MAIN.modify_ts from R_QUOTA_MAIN, R_USER_MAIN where R_USER_MAIN.user_id = R_QUOTA_MAIN.user_id and R_QUOTA_MAIN.resc_id = 0 and user_name=? and R_USER_MAIN.zone_name=?",
          ""
      };
      std::vector<const char *> resultColName[] = {
          {"token_name"},
          {"token_name"},
          {"token_namespace","token_id", "token_name", "token_value", "token_value2", "token_value3", "r_comment", "create_ts", "modify_ts"},
          {"resc_name"},
          {"resc_id", "resc_name", "zone_name", "resc_type_name", "resc_class_name", "resc_net", "resc_def_path", "free_space", "free_space_ts", "resc_info", "r_comment", "resc_status", "create_ts", "modify_ts", "resc_children", "resc_context", "resc_parent", "resc_objcount", "resc_parent_context"},
          {"zone_name"},
          {"zone_id", "zone_name", "zone_type_name", "zone_conn_string", "r_comment", "create_ts", "modify_ts"},
          {"user_name"},
          {"user_name||'#'||zone_name"},
          {"data_id", "coll_id", "data_name", "data_repl_num", "data_version", "data_type_name", "data_size", "resc_group_name", "resc_name", "data_path", "data_owner_name", "data_owner_zone", "data_is_dirty", "data_status", "data_checksum", "data_expiry_ts", "data_map_id", "data_mode", "r_comment", "create_ts", "modify_ts", "resc_hier", "resc_id"},
          {"data_name, data_id", "data_repl_num"},
          {"coll_name"},
          {"user_id", "user_name", "user_type_name", "zone_name", "user_info", "r_comment", "create_ts", "modify_ts"},
          {"user_name||'#'||zone_name"},
        {},
        {},
          {"coll_id"},
          {"user_id", "user_name", "user_type_name", "zone_name", "user_info", "r_comment", "create_ts", "modify_ts"},
          {"user_name"},
          {"zone_name"},
          {"user_name", "user_auth_name"},
          {"user_name", "user_auth_name"},
          {"user_name", "user_auth_name"},
          {"user_name", "user_auth_name"},
        {},
        {},
        {},
        {}
      };
      typedef long (*funptr)(void *, void *, void *, int, void *, void *);
      funptr qu[] = {
          &hs_get_all2_simple_query_0,
          &hs_get_all2_simple_query_1,
          &hs_get_all2_simple_query_2,
          &hs_get_all2_simple_query_3,
          &hs_get_all2_simple_query_4,
          &hs_get_all2_simple_query_5,
          &hs_get_all2_simple_query_6,
          &hs_get_all2_simple_query_7,
          &hs_get_all2_simple_query_8,
          &hs_get_all2_simple_query_9,
          &hs_get_all2_simple_query_10,
          &hs_get_all2_simple_query_11,
          &hs_get_all2_simple_query_12,
          &hs_get_all2_simple_query_13,
          NULL,
          NULL,
          &hs_get_all2_simple_query_16,
          &hs_get_all2_simple_query_17,
          &hs_get_all2_simple_query_18,
          &hs_get_all2_simple_query_19,
          &hs_get_all2_simple_query_20,
          &hs_get_all2_simple_query_21,
          &hs_get_all2_simple_query_22,
          &hs_get_all2_simple_query_23,
          NULL,
          NULL,
          NULL,
          NULL,
      };

      //rodsLog( LOG_NOTICE, "JMC :: sql - %s", sql );
      if ( logSQL != 0 ) {
          rodsLog( LOG_SQL, "chlSimpleQuery" );
      }

      if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
          return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
      }
      if ( _ctx.comm()->proxyUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
          return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
      }

      /* check that the input sql is one of the allowed forms */
      OK = 0;
      for ( i = 0;; i++ ) {
          if ( strlen( allowedSQL[i] ) < 1 ) {
              break;
          }
          if ( strcasecmp( allowedSQL[i], _sql ) == 0 ) {
              OK = 1;
              break;
          }
      }

      if ( OK == 0 ) {
          return ERROR( CAT_INVALID_ARGUMENT, "query not found" );
      }

      /* done with multiple log calls so that each form will be checked
         via checkIcatLog.pl */
      if ( i == 0 && logSQL ) {
          rodsLog( LOG_SQL, "chlSimpleQuery SQL 1 " );
      }
      if ( i == 1 && logSQL ) {
          rodsLog( LOG_SQL, "chlSimpleQuery SQL 2" );
      }
      if ( i == 2 && logSQL ) {
          rodsLog( LOG_SQL, "chlSimpleQuery SQL 3" );
      }
      if ( i == 3 && logSQL ) {
          rodsLog( LOG_SQL, "chlSimpleQuery SQL 4" );
      }
      if ( i == 4 && logSQL ) {
          rodsLog( LOG_SQL, "chlSimpleQuery SQL 5" );
      }
      if ( i == 5 && logSQL ) {
          rodsLog( LOG_SQL, "chlSimpleQuery SQL 6" );
      }
      if ( i == 6 && logSQL ) {
          rodsLog( LOG_SQL, "chlSimpleQuery SQL 7" );
      }
      if ( i == 7 && logSQL ) {
          rodsLog( LOG_SQL, "chlSimpleQuery SQL 8" );
      }
      if ( i == 8 && logSQL ) {
          rodsLog( LOG_SQL, "chlSimpleQuery SQL 9" );
      }
      if ( i == 9 && logSQL ) {
          rodsLog( LOG_SQL, "chlSimpleQuery SQL 10" );
      }
      if ( i == 10 && logSQL ) {
          rodsLog( LOG_SQL, "chlSimpleQuery SQL 11" );
      }
      if ( i == 11 && logSQL ) {
          rodsLog( LOG_SQL, "chlSimpleQuery SQL 12" );
      }
      if ( i == 12 && logSQL ) {
          rodsLog( LOG_SQL, "chlSimpleQuery SQL 13" );
      }
      if ( i == 13 && logSQL ) {
          rodsLog( LOG_SQL, "chlSimpleQuery SQL 14" );
      }
      //if (i==14 && logSQL) rodsLog(LOG_SQL, "chlSimpleQuery S Q L 15");
      //if (i==15 && logSQL) rodsLog(LOG_SQL, "chlSimpleQuery S Q L 16");
      //if (i==16 && logSQL) rodsLog(LOG_SQL, "chlSimpleQuery S Q L 17");
      if ( i == 17 && logSQL ) {
          rodsLog( LOG_SQL, "chlSimpleQuery SQL 18" );
      }
      if ( i == 18 && logSQL ) {
          rodsLog( LOG_SQL, "chlSimpleQuery SQL 19" );
      }
      if ( i == 19 && logSQL ) {
          rodsLog( LOG_SQL, "chlSimpleQuery SQL 20" );
      }
      if ( i == 20 && logSQL ) {
          rodsLog( LOG_SQL, "chlSimpleQuery SQL 21" );
      }
      if ( i == 21 && logSQL ) {
          rodsLog( LOG_SQL, "chlSimpleQuery SQL 22" );
      }
      if ( i == 22 && logSQL ) {
          rodsLog( LOG_SQL, "chlSimpleQuery SQL 23" );
      }
      if ( i == 23 && logSQL ) {
          rodsLog( LOG_SQL, "chlSimpleQuery SQL 24" );
      }
      if ( i == 24 && logSQL ) {
          rodsLog( LOG_SQL, "chlSimpleQuery SQL 25" );
      }
      if ( i == 25 && logSQL ) {
          rodsLog( LOG_SQL, "chlSimpleQuery SQL 26" );
      }
      if ( i == 26 && logSQL ) {
          rodsLog( LOG_SQL, "chlSimpleQuery SQL 27" );
      }
      if ( i == 27 && logSQL ) {
          rodsLog( LOG_SQL, "chlSimpleQuery SQL 28" );
      }

      _out_buf[0] = '\0';
      rowSize = 0;
      rows = 0;
      control *c;
      if(*_control == 0) {
          *_control = next_control();
          c = controls[*_control - 1];
          c->index = i;
          status = qu[c->index](svc, icss, _bindVars.data(), _bindVars.size(), &c->out, &c->n);
          if ( status < 0 ) {
            if ( status != CAT_NO_ROWS_FOUND ) {
              rodsLog( LOG_NOTICE,
                "chlSimpleQuery cmlGetFirstRowFromSqlBV failure %d",
                status );
              }
              return ERROR( status, "cmlGetFirstRowFromSqlBV failure" );
            }
      } else {
        c = controls[*_control - 1];
      }

      nCols = resultColName[c->index].size();
      nRows = c->n / nCols;

      if ( c->rows == 0 && _format == 3 ) {
          for ( i = 0; i < nCols ; i++ ) {
              rstrcat( _out_buf, resultColName[c->index][i], _max_out_buf );
              if ( i != nCols - 1 ) {
                  rstrcat( _out_buf, " ", _max_out_buf );
              }
          }
      }

      for (;c->rows < nRows;) {
          char **row = c->out + (c->rows++ * nCols);
          for ( i = 0; i < nCols ; i++ ) {
              if ( _format == 1 || _format == 3) {
                  if ( strlen( row[i] ) == 0 ) {
                      rstrcat( _out_buf, "- ", _max_out_buf );
                  }
                  else {
                      rstrcat( _out_buf, row[i],
                               _max_out_buf );
                      if ( i != nCols - 1 ) {
                          /* add a space except for the last column */
                          rstrcat( _out_buf, " ", _max_out_buf );
                      }
                  }
              }
              if ( _format == 2 ) {
                  rstrcat( _out_buf, resultColName[c->index][i], _max_out_buf );
                  rstrcat( _out_buf, ": ", _max_out_buf );
                  rstrcat( _out_buf, row[i], _max_out_buf );
                  rstrcat( _out_buf, "\n", _max_out_buf );
              }
          }
          rstrcat( _out_buf, "\n", _max_out_buf );
          if ( rowSize == 0 ) {
              rowSize = strlen( _out_buf );
          }
          if ( ( int ) strlen( _out_buf ) + rowSize + 20 > _max_out_buf ) {
              return CODE( 0 ); /* success so far, but more rows available */
          }
      }

      release_control(*_control);
      *_control = 0;
      return SUCCESS();

} // db_simple_query_op

irods::error db_simple_query_op(
    irods::plugin_context& _ctx,
    const char*            _sql,
    const char*            _arg1,
    const char*            _arg2,
    const char*            _arg3,
    const char*            _arg4,
    int                    _format,
    int*                   _control,
    char*                  _out_buf,
    int                    _max_out_buf ) {

    std::vector<const char *> bindVars;
    if ( _arg1 != NULL && _arg1[0] != '\0' ) {
        bindVars.push_back( _arg1 );
        if ( _arg2 != NULL && _arg2[0] != '\0' ) {
            bindVars.push_back( _arg2 );
            if ( _arg3 != NULL && _arg3[0] != '\0' ) {
                bindVars.push_back( _arg3 );
                if ( _arg4 != NULL && _arg4[0] != '\0' ) {
                    bindVars.push_back( _arg4 );
                }
            }
        }
    }
    return db_simple_query_op_vector( _ctx, _sql, bindVars, _format, _control, _out_buf, _max_out_buf );
}

// =-=-=-=-=-=-=-
// commit the transaction
irods::error db_del_coll_by_admin_op(
    irods::plugin_context& _ctx,
    collInfo_t*            _coll_info ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_coll_info ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "null parameter" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    rodsLong_t iVal;
    char logicalEndName[MAX_NAME_LEN];
    char logicalParentDirName[MAX_NAME_LEN];
    char collIdNum[MAX_NAME_LEN];
    int status;

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlDelCollByAdmin" );
    }

    if ( !icss_status ) {
        return ERROR( CATALOG_NOT_CONNECTED, "catalog not connected" );
    }

    if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
    }
    if ( _ctx.comm()->proxyUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
    }

    status = splitPathByKey( _coll_info->collName,
                             logicalParentDirName, MAX_NAME_LEN, logicalEndName, MAX_NAME_LEN, '/' );

    if ( strlen( logicalParentDirName ) == 0 ) {
        snprintf( logicalParentDirName, sizeof( logicalParentDirName ), "%s", PATH_SEPARATOR );
        snprintf( logicalEndName, sizeof( logicalEndName ), "%s", _coll_info->collName + 1 );
    }

    /* check that the collection is empty (both subdirs and files) */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlDelCollByAdmin SQL 1 " );
    }
    {
        status = hs_get_int_coll_child_by_name(svc, icss, _coll_info->collName, &iVal );
    }

    if ( status != CAT_NO_ROWS_FOUND ) {
        if ( status == 0 ) {
            char errMsg[105];
            snprintf( errMsg, 100, "collection '%s' is not empty",
                      _coll_info->collName );
            addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
            return ERROR( CAT_COLLECTION_NOT_EMPTY, "collection not empty" );
        }
        _rollback( "chlDelCollByAdmin" );
        return ERROR( status, "failed to get collection" );
    }

    /* remove any access rows */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlDelCollByAdmin SQL 2" );
    }
    status =  hs_delete_access_by_coll_name(svc, icss, _coll_info->collName
                   );
    if ( status != 0 ) {
        /* error, but let it fall thru to below, probably doesn't exist */
        rodsLog( LOG_NOTICE,
                 "chlDelCollByAdmin delete access failure %d",
                 status );
        _rollback( "chlDelCollByAdmin" );
    }

    /* Remove associated AVUs, if any */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlDelCollByAdmin SQL 3 " );
    }
    {
        status = hs_get_int_coll_id_by_name(svc, icss, _coll_info->collName,
                     &iVal );
    }

    if ( status != 0 ) {
        _rollback( "db_del_coll_by_admin_op" );
        std::stringstream msg;
        msg << "db_del_coll_by_admin_op: should be exactly one collection id corresponding to collection name ["
            << _coll_info->collName
            << "]. status ["
            << status
            << "]";
        return ERROR( status, msg.str().c_str() );
    }

    snprintf( collIdNum, MAX_NAME_LEN, "%lld", iVal );
    removeMetaMapAndAVU( collIdNum );


    /* delete the row if it exists */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlDelCollByAdmin SQL 4" );
    }
    status =  hs_delete_coll_by_name(svc, icss, _coll_info->collName
                                      );

    if ( status != 0 ) {
        char errMsg[105];
        snprintf( errMsg, 100, "collection '%s' is unknown",
                  _coll_info->collName );
        addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
        _rollback( "chlDelCollByAdmin" );
        return ERROR( CAT_UNKNOWN_COLLECTION, "unknown collection" );
    }

    return SUCCESS();

} // db_del_coll_by_admin_op

// =-=-=-=-=-=-=-
// commit the transaction
irods::error db_del_coll_op(
    irods::plugin_context& _ctx,
    collInfo_t*            _coll_info ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_coll_info ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "null parameter" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    int status;

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlDelColl" );
    }

    status = _delColl( _ctx.comm(), _coll_info );
    if ( status != 0 ) {
        return ERROR( status, "_delColl failed" );
    }

    status =  hs_commit(svc,  icss );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlDelColl cmlExecuteNoAnswerSql commit failure %d",
                 status );
        _rollback( "chlDelColl" );
        return ERROR( status, "commit failed" );
    }

    return SUCCESS();

} // db_del_coll_op

// =-=-=-=-=-=-=-
// authenticate user
irods::error db_check_auth_op(
    irods::plugin_context& _ctx,
    const char*            _scheme,
    const char*            _challenge,
    const char*            _response,
    const char*            _user_name,
    int*                   _user_priv_level,
    int*                   _client_priv_level ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if ( !_challenge || !_response || !_user_name || !_user_priv_level || !_client_priv_level ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "null parameter" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    // =-=-=-=-=-=-=-
    // All The Variable
    int status = 0;
    char md5Buf[CHALLENGE_LEN + MAX_PASSWORD_LEN + 2];
    char digest[RESPONSE_LEN + 2];
    const char *cp = NULL;
    int i = 0, OK = 0, k = 0;
    char userType[MAX_NAME_LEN];
    static int prevFailure = 0;
    char goodPw[MAX_PASSWORD_LEN + 10] = "";
    char lastPw[MAX_PASSWORD_LEN + 10] = "";
    char goodPwExpiry[MAX_PASSWORD_LEN + 10] = "";
    char goodPwTs[MAX_PASSWORD_LEN + 10] = "";
    char goodPwModTs[MAX_PASSWORD_LEN + 10] = "";
    rodsLong_t expireTime = 0;
    char *cpw = NULL;
    int nPasswords = 0;
    char myTime[50];
    time_t nowTime;
    time_t pwExpireMaxCreateTime;
    char expireStr[50];
    char expireStrCreate[50];
    char myUserZone[MAX_NAME_LEN];
    char userName2[NAME_LEN + 2];
    char userZone[NAME_LEN + 2];
    rodsLong_t pamMinTime = 0;
    rodsLong_t pamMaxTime = 0;
    int hashType = 0;
    char lastPwModTs[MAX_PASSWORD_LEN + 10];
    snprintf( lastPwModTs, sizeof( lastPwModTs ), "0" );
    char *cPwTs = NULL;
    int iTs1 = 0, iTs2 = 0;
    int temp_password_max_time = 0;
    std::vector<char> pwInfoArray( MAX_PASSWORD_LEN * MAX_PASSWORDS * 4 );

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlCheckAuth" );
    }

    if ( prevFailure > 1 ) {
        /* Somebody trying a dictionary attack? */
        if ( prevFailure > 5 ) {
            sleep( 20 );    /* at least, slow it down */
        }
        sleep( 2 );
    }
    *_user_priv_level = NO_USER_AUTH;
    *_client_priv_level = NO_USER_AUTH;

    hashType = HASH_TYPE_MD5;
    std::string user_name( _user_name );
    std::string::size_type pos = user_name.find( SHA1_FLAG_STRING );
    if ( std::string::npos != pos ) {
        // truncate off the :::sha1 string
        user_name = user_name.substr( pos );
        hashType = HASH_TYPE_SHA1;
    }

    memset( md5Buf, 0, sizeof( md5Buf ) );
    strncpy( md5Buf, _challenge, CHALLENGE_LEN );
    snprintf( prevChalSig, sizeof prevChalSig,
              "%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x",
              ( unsigned char )md5Buf[0], ( unsigned char )md5Buf[1],
              ( unsigned char )md5Buf[2], ( unsigned char )md5Buf[3],
              ( unsigned char )md5Buf[4], ( unsigned char )md5Buf[5],
              ( unsigned char )md5Buf[6], ( unsigned char )md5Buf[7],
              ( unsigned char )md5Buf[8], ( unsigned char )md5Buf[9],
              ( unsigned char )md5Buf[10], ( unsigned char )md5Buf[11],
              ( unsigned char )md5Buf[12], ( unsigned char )md5Buf[13],
              ( unsigned char )md5Buf[14], ( unsigned char )md5Buf[15] );
    status = validateAndParseUserName( user_name.c_str(), userName2, userZone );
    if ( status ) {
        return ERROR( status, "Invalid username format" );
    }

    if ( userZone[0] == '\0' ) {
        std::string zone;
        ret = getLocalZone( _ctx.prop_map(), icss, zone );
        if ( !ret.ok() ) {
            return PASS( ret );
        }
        snprintf( myUserZone, sizeof( myUserZone ), "%s", zone.c_str() );
    }
    else {
        snprintf( myUserZone, sizeof( myUserZone ), "%s", userZone );
    }

    if ( _scheme && strlen( _scheme ) > 0 ) {
        irods::error ret = verify_auth_response( _scheme, _challenge, userName2, _response );
        if ( !ret.ok() ) {
            return PASS( ret );
        }
        goto checkLevel;
    }

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlCheckAuth SQL 1 " );
    }

    {
        /* four strings per password returned */
        status = hs_get_some_user_password_by_user_zone_and_name(svc, icss, myUserZone, userName2, pwInfoArray.data(), MAX_PASSWORD_LEN, MAX_PASSWORDS * 4 ) * 4;
    }

    if ( status < 4 ) {
        if ( status == CAT_NO_ROWS_FOUND ) {
            status = CAT_INVALID_USER; /* Be a little more specific */
            if ( strncmp( ANONYMOUS_USER, userName2, NAME_LEN ) == 0 ) {
                /* anonymous user, skip the pw check but do the rest */
                goto checkLevel;
            }
        }
        return ERROR( status, "select rcat_password failed" );
    }

    nPasswords = status / 4; /* four strings per password returned */
    goodPwExpiry[0] = '\0';
    goodPwTs[0] = '\0';
    goodPwModTs[0] = '\0';

    if ( nPasswords == MAX_PASSWORDS ) {
        {
            // There are more than MAX_PASSWORDS in the database take the extra time to get them all.
            status = hs_get_int_num_password_by_user_name(svc,  icss, userName2, &MAX_PASSWORDS);
        }
        if ( status < 0 ) {
            rodsLog( LOG_ERROR, "cmlGetIntegerValueFromSql failed in db_check_auth_op with status %d", status );
        }
        nPasswords = MAX_PASSWORDS;
        pwInfoArray.resize( MAX_PASSWORD_LEN * MAX_PASSWORDS * 4 );

        {
            /* four strings per password returned */
            status = hs_get_some_user_password_by_user_zone_and_name(svc, icss, myUserZone, userName2, pwInfoArray.data(), MAX_PASSWORD_LEN, MAX_PASSWORDS * 4);
        }
        if ( status < 0 ) {
            rodsLog( LOG_ERROR, "cmlGetMultiRowStringValuesFromSql failed in db_check_auth_op with status %d", status );
        }
    }

    cpw = pwInfoArray.data();
    for ( k = 0; OK == 0 && k < MAX_PASSWORDS && k < nPasswords; k++ ) {
        memset( md5Buf, 0, sizeof( md5Buf ) );
        strncpy( md5Buf, _challenge, CHALLENGE_LEN );
        rstrcpy( lastPw, cpw, MAX_PASSWORD_LEN );
        icatDescramble( cpw );
        strncpy( md5Buf + CHALLENGE_LEN, cpw, MAX_PASSWORD_LEN );

        obfMakeOneWayHash( hashType,
                           ( unsigned char * )md5Buf, CHALLENGE_LEN + MAX_PASSWORD_LEN,
                           ( unsigned char * )digest );

        for ( i = 0; i < RESPONSE_LEN; i++ ) {
            if ( digest[i] == '\0' ) {
                digest[i]++;
            }  /* make sure 'string' doesn't end
                  early (this matches client code) */
        }

        cp = _response;
        OK = 1;
        for ( i = 0; i < RESPONSE_LEN; i++ ) {
            if ( *cp++ != digest[i] ) {
                OK = 0;
            }
        }

        memset( md5Buf, 0, sizeof( md5Buf ) );
        if ( OK == 1 ) {
            rstrcpy( goodPw, cpw, MAX_PASSWORD_LEN );
            cpw += MAX_PASSWORD_LEN;
            rstrcpy( goodPwExpiry, cpw, MAX_PASSWORD_LEN );
            cpw += MAX_PASSWORD_LEN;
            rstrcpy( goodPwTs, cpw, MAX_PASSWORD_LEN );
            cpw += MAX_PASSWORD_LEN;
            rstrcpy( goodPwModTs, cpw, MAX_PASSWORD_LEN );
        }
        else {
            cPwTs = cpw + ( MAX_PASSWORD_LEN * 3 );
            iTs1 = atoi( cPwTs );
            iTs2 = atoi( lastPwModTs );
            if ( iTs1 == iTs2 ) {
                /* MAX_PASSWORDS at same time-stamp, skip ahead to avoid infinite
                   loop; things should recover eventually */
                snprintf( lastPwModTs, sizeof lastPwModTs, "%011d", iTs1 + 1 );
            }
            else {
                /* normal case */
                rstrcpy( lastPwModTs, cPwTs, sizeof( lastPwModTs ) );
            }

            cpw += MAX_PASSWORD_LEN * 4;
        }
    }

    if ( OK == 0 ) {
        prevFailure++;
        return ERROR( CAT_INVALID_AUTHENTICATION, "invalid argument" );
    }

    expireTime = atoll( goodPwExpiry );
    getNowStr( myTime );
    nowTime = atoll( myTime );

    /* Check for PAM_AUTH type passwords */
    pamMaxTime = atoll( irods_pam_password_max_time );
    pamMinTime = atoll( irods_pam_password_min_time );

    if ( ( strncmp( goodPwExpiry, "9999", 4 ) != 0 ) &&
            expireTime >=  pamMinTime &&
            expireTime <= pamMaxTime ) {
        time_t modTime;
        /* The used pw is an iRODS-PAM type, so now check if it's expired */
        getNowStr( myTime );
        nowTime = atoll( myTime );
        modTime = atoll( goodPwModTs );

        if ( modTime + expireTime < nowTime ) {
            /* it is expired, so return the error below and first remove it */
            if ( logSQL != 0 ) {
                rodsLog( LOG_SQL, "chlCheckAuth SQL 2" );
            }
            status = hs_delete_user_password_by_user_zone_and_name_and_create_ts(svc, icss, userName2, myUserZone, lastPw, goodPwTs);
            memset( goodPw, 0, sizeof( goodPw ) );
            memset( lastPw, 0, sizeof( lastPw ) );
            if ( status != 0 ) {
                rodsLog( LOG_NOTICE,
                         "chlCheckAuth cmlExecuteNoAnswerSql delete expired password failure %d",
                         status );
                return ERROR( status, "delete expired password failure" );
            }
            status =  hs_commit(svc,  icss );
            if ( status != 0 ) {
                rodsLog( LOG_NOTICE,
                         "chlCheckAuth cmlExecuteNoAnswerSql commit failure %d",
                         status );
                return ERROR( status, "commit failure" );
            }
            return ERROR( CAT_PASSWORD_EXPIRED, "password expired" );
        }
    }

    // int temp_password_max_time;
    try {
        temp_password_max_time = irods::get_advanced_setting<const int>(irods::CFG_MAX_TEMP_PASSWORD_LIFETIME);
    } catch ( const irods::exception& e ) {
        return irods::error(e);
    }

    if ( expireTime < temp_password_max_time ) {
        int temp_password_time;
        try {
            temp_password_time = irods::get_advanced_setting<const int>(irods::CFG_DEF_TEMP_PASSWORD_LIFETIME);
        } catch ( const irods::exception& e ) {
            return irods::error(e);
        }

        /* in the form used by temporary, one-time passwords */

        time_t createTime;
        int returnExpired;

        /* check if it's expired */

        returnExpired = 0;
        getNowStr( myTime );
        nowTime = atoll( myTime );
        createTime = atoll( goodPwTs );
        if ( createTime == 0 || nowTime == 0 ) {
            returnExpired = 1;
        }
        if ( createTime + expireTime < nowTime ) {
            returnExpired = 1;
        }


        /* Remove this temporary, one-time password */
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlCheckAuth SQL 2" );
        }
        status =  hs_delete_user_password_by_password(svc, icss, goodPw);
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlCheckAuth cmlExecuteNoAnswerSql delete failure %d",
                     status );
            _rollback( "chlCheckAuth" );
            return ERROR( status, "delete failure" );
        }

        /* Also remove any expired temporary passwords */

        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlCheckAuth SQL 3" );
        }
        snprintf( expireStr, sizeof expireStr, "%d", temp_password_time );

        pwExpireMaxCreateTime = nowTime - temp_password_time;
        /* Not sure if casting to int is correct but seems OK & avoids warning:*/
        snprintf( expireStrCreate, sizeof expireStrCreate, "%011d",
                  ( int )pwExpireMaxCreateTime );

        status =  hs_delete_user_password_by_expiry_ts_and_create_ts(svc, icss, expireStr, expireStrCreate
                       );
        if ( status != 0 && status != CAT_SUCCESS_BUT_WITH_NO_INFO ) {
            rodsLog( LOG_NOTICE,
                     "chlCheckAuth cmlExecuteNoAnswerSql delete2 failure %d",
                     status );
            _rollback( "chlCheckAuth" );
            return ERROR( status, "delete2 failed" );
        }

        memset( goodPw, 0, MAX_PASSWORD_LEN );
        if ( returnExpired ) {
            return ERROR( CAT_PASSWORD_EXPIRED, "password expired" );
        }

        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlCheckAuth SQL 4" );
        }
        status =  hs_commit(svc,  icss );
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlCheckAuth cmlExecuteNoAnswerSql commit failure %d",
                     status );
            return ERROR( status, "commit failure" );
        }
        memset( goodPw, 0, MAX_PASSWORD_LEN );
        if ( returnExpired ) {
            return ERROR( CAT_PASSWORD_EXPIRED, "password is expired" );
        }
    }

    /* Get the user type so privilege level can be set */
checkLevel:

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlCheckAuth SQL 5" );
    }
    {
        status = hs_get_user_type_by_zone_and_name(svc, icss, myUserZone, userName2,
                     userType, MAX_NAME_LEN );
    }
    if ( status != 0 ) {
        if ( status == CAT_NO_ROWS_FOUND ) {
            status = CAT_INVALID_USER; /* Be a little more specific */
        }
        else {
            _rollback( "chlCheckAuth" );
        }
        return ERROR( status, "select user_type_name failed" );
    }
    *_user_priv_level = LOCAL_USER_AUTH;
    if ( strcmp( userType, "rodsadmin" ) == 0 ) {
        *_user_priv_level = LOCAL_PRIV_USER_AUTH;

        /* Since the user is admin, also get the client privilege level */
        if ( strcmp( _ctx.comm()->clientUser.userName, userName2 ) == 0 &&
                strcmp( _ctx.comm()->clientUser.rodsZone, userZone ) == 0 ) {
            *_client_priv_level = LOCAL_PRIV_USER_AUTH; /* same user, no query req */
        }
        else {
            if ( _ctx.comm()->clientUser.userName[0] == '\0' ) {
                /*
                   When using GSI, the client might not provide a user
                   name, in which case we avoid the query below (which
                   would fail) and instead set up minimal privileges.
                   This is safe since we have just authenticated the
                   remote server as an admin account.  This will allow
                   some queries (including the one needed for retrieving
                   the client's DNs).  Since the clientUser is not set,
                   some other queries are still exclued.  The non-IES will
                   reconnect once the rodsUserName is determined.  In
                   iRODS 2.3 this would return an error.
                 */
                *_client_priv_level = REMOTE_USER_AUTH;
                prevFailure = 0;
                return SUCCESS();
            }
            else {
                if ( logSQL != 0 ) {
                    rodsLog( LOG_SQL, "chlCheckAuth SQL 6" );
                }
                {
                    status = hs_get_user_type_by_zone_and_name(svc, icss, _ctx.comm()->clientUser.rodsZone, _ctx.comm()->clientUser.userName,
                                 userType, MAX_NAME_LEN);
                }
                if ( status != 0 ) {
                    if ( status == CAT_NO_ROWS_FOUND ) {
                        status = CAT_INVALID_CLIENT_USER; /* more specific */
                    }
                    else {
                        _rollback( "chlCheckAuth" );
                    }
                    return ERROR( status, "select user_type_name failed" );
                }
                *_client_priv_level = LOCAL_USER_AUTH;
                if ( strcmp( userType, "rodsadmin" ) == 0 ) {
                    *_client_priv_level = LOCAL_PRIV_USER_AUTH;
                }
            }
        }
    }

    prevFailure = 0;
    return SUCCESS();

} // db_check_auth_op

// =-=-=-=-=-=-=-
// authenticate user
irods::error db_make_temp_pw_op(
    irods::plugin_context& _ctx,
    char*                  _pw_value_to_hash,
    const char*            _other_user ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_pw_value_to_hash ||
        !_other_user ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "null parameter" );
    }

    int temp_password_time;
    try {
        temp_password_time = irods::get_advanced_setting<const int>(irods::CFG_DEF_TEMP_PASSWORD_LIFETIME);
    } catch ( const irods::exception& e ) {
        return irods::error(e);
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );

    int status;
    char md5Buf[100];
    unsigned char digest[RESPONSE_LEN + 2];
    int i;
    char password[MAX_PASSWORD_LEN + 10];
    char newPw[MAX_PASSWORD_LEN + 10];
    char myTime[50];
    char myTimeExp[50];
    char rBuf[200];
    char hashValue[50];
    int j = 0;
    char tSQL[MAX_SQL_SIZE];
    int useOtherUser = 0;

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlMakeTempPw" );
    }

    if ( _other_user != NULL && strlen( _other_user ) > 0 ) {
        if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
            return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
        }
        if ( _ctx.comm()->proxyUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
            return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
        }
        useOtherUser = 1;
    }

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlMakeTempPw SQL 1 " );
    }

    {
	snprintf(tSQL, MAX_SQL_SIZE, "%d", temp_password_time);
        status = hs_get_password_by_user_zone_and_name_and_diff_expiry_ts(svc, icss, _ctx.comm()->clientUser.rodsZone, _ctx.comm()->clientUser.userName, tSQL,
					   						password, MAX_PASSWORD_LEN);
    }
    if ( status != 0 ) {
        if ( status == CAT_NO_ROWS_FOUND ) {
            status = CAT_INVALID_USER; /* Be a little more specific */
        }
        else {
            _rollback( "chlMakeTempPw" );
        }
        return ERROR( status, "failed to get password" );
    }

    icatDescramble( password );

    j = 0;
    get64RandomBytes( rBuf );
    for ( i = 0; i < 50 && j < MAX_PASSWORD_LEN - 1; i++ ) {
        char c;
        c = rBuf[i] & 0x7f;
        if ( c < '0' ) {
            c += '0';
        }
        if ( ( c > 'a' && c < 'z' ) || ( c > 'A' && c < 'Z' ) ||
                ( c > '0' && c < '9' ) ) {
            hashValue[j++] = c;
        }
    }
    hashValue[j] = '\0';
    /*   printf("hashValue=%s\n", hashValue); */

    /* calcuate the temp password (a hash of the user's main pw and
       the hashValue) */
    memset( md5Buf, 0, sizeof( md5Buf ) );
    snprintf( md5Buf, sizeof( md5Buf ), "%s%s", hashValue, password );

    obfMakeOneWayHash( HASH_TYPE_DEFAULT,
                       ( unsigned char * ) md5Buf, 100, ( unsigned char * ) digest );

    hashToStr( digest, newPw );
    /*   printf("newPw=%s\n", newPw); */

    snprintf( _pw_value_to_hash, MAX_PASSWORD_LEN, "%s", hashValue );

    /* Insert the temporary, one-time password */

    getNowStr( myTime );
    sprintf( myTimeExp, "%d", temp_password_time );  /* seconds from create time
                                                      when it will expire */

    status =  hs_create_user_password_by_user_zone_and_name(svc,  icss,   _ctx.comm()->clientUser.rodsZone, useOtherUser == 1 ? (void *) _other_user : _ctx.comm()->clientUser.userName, newPw, myTimeExp, myTime, myTime
                   );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlMakeTempPw cmlExecuteNoAnswerSql insert failure %d",
                 status );
        _rollback( "chlMakeTempPw" );
        return ERROR( status, "insert failed" );
    }

    status =  hs_commit(svc,  icss );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlMakeTempPw cmlExecuteNoAnswerSql commit failure %d",
                 status );
        return ERROR( status, "commit failed" );
    }

    memset( newPw, 0, MAX_PASSWORD_LEN );
    return SUCCESS();

} // db_make_temp_pw_op

// =-=-=-=-=-=-=-
// authenticate user
irods::error db_make_limited_pw_op(
    irods::plugin_context& _ctx,
    int                    _ttl,
    char*                  _pw_value_to_hash ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_pw_value_to_hash ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "null parameter" );
    }

    int temp_password_time;
    try {
        temp_password_time = irods::get_advanced_setting<const int>(irods::CFG_DEF_TEMP_PASSWORD_LIFETIME);
    } catch ( const irods::exception& e ) {
        return irods::error(e);
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    int status;
    char md5Buf[100];
    unsigned char digest[RESPONSE_LEN + 2];
    int i;
    char password[MAX_PASSWORD_LEN + 10];
    char newPw[MAX_PASSWORD_LEN + 10];
    char myTime[50];
    char rBuf[200];
    char hashValue[50];
    int j = 0;
    char tSQL[MAX_SQL_SIZE];
    char expTime[50];
    int timeToLive;
    rodsLong_t pamMinTime;
    rodsLong_t pamMaxTime;

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlMakeLimitedPw" );
    }

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlMakeLimitedPw SQL 1 " );
    }

    {
        snprintf(tSQL,MAX_SQL_SIZE,"%d",temp_password_time);
        status = hs_get_password_by_user_zone_and_name_and_diff_expiry_ts(svc,  icss, _ctx.comm()->clientUser.rodsZone, _ctx.comm()->clientUser.userName, tSQL,
					   						password, MAX_PASSWORD_LEN );
    }
    if ( status != 0 ) {
        if ( status == CAT_NO_ROWS_FOUND ) {
            status = CAT_INVALID_USER; /* Be a little more specific */
        }
        else {
            _rollback( "chlMakeLimitedPw" );
        }
        return ERROR( status, "get password failed" );
    }

    icatDescramble( password );

    j = 0;
    get64RandomBytes( rBuf );
    for ( i = 0; i < 50 && j < MAX_PASSWORD_LEN - 1; i++ ) {
        char c;
        c = rBuf[i] & 0x7f;
        if ( c < '0' ) {
            c += '0';
        }
        if ( ( c > 'a' && c < 'z' ) || ( c > 'A' && c < 'Z' ) ||
                ( c > '0' && c < '9' ) ) {
            hashValue[j++] = c;
        }
    }
    hashValue[j] = '\0';

    /* calcuate the limited password (a hash of the user's main pw and
       the hashValue) */
    memset( md5Buf, 0, sizeof( md5Buf ) );
    snprintf( md5Buf, sizeof( md5Buf ), "%s%s", hashValue, password );

    obfMakeOneWayHash( HASH_TYPE_DEFAULT,
                       ( unsigned char * ) md5Buf, 100, ( unsigned char * ) digest );

    hashToStr( digest, newPw );

    icatScramble( newPw );

    snprintf( _pw_value_to_hash, MAX_PASSWORD_LEN, "%s", hashValue );

    getNowStr( myTime );

    timeToLive = _ttl * 3600; /* convert input hours to seconds */
    pamMaxTime = atoll( irods_pam_password_max_time );
    pamMinTime = atoll( irods_pam_password_min_time );
    if ( timeToLive < pamMinTime ||
            timeToLive > pamMaxTime ) {
        return ERROR( PAM_AUTH_PASSWORD_INVALID_TTL, "invalid ttl" );
    }

    /* Insert the limited password */
    snprintf( expTime, sizeof expTime, "%d", timeToLive );
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlMakeLimitedPw SQL 2" );
    }
    status =  hs_create_user_password_by_user_zone_and_name(svc, icss, _ctx.comm()->clientUser.rodsZone, _ctx.comm()->clientUser.userName, newPw, expTime, myTime, myTime);
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlMakeLimitedPw cmlExecuteNoAnswerSql insert failure %d",
                 status );
        _rollback( "chlMakeLimitedPw" );
        return ERROR( status, "insert failure" );
    }

    /* Also delete any that are expired */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlMakeLimitedPw SQL 3" );
    }
    status =  hs_delete_user_password_by_expiry_ts_and_modify_ts(svc, icss,irods_pam_password_min_time,irods_pam_password_max_time,myTime );

    status =  hs_commit(svc,  icss );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlMakeLimitedPw cmlExecuteNoAnswerSql commit failure %d",
                 status );
        return ERROR( status, "commit failed" );
    }

    memset( newPw, 0, MAX_PASSWORD_LEN );

    return SUCCESS();

} // db_make_limited_pw_op

// =-=-=-=-=-=-=-
// authenticate user
irods::error db_update_pam_password_op(
    irods::plugin_context& _ctx,
    const char*            _user_name,
    int                    _ttl,
    const char*            _test_time,
    char**                 _irods_password ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_user_name ||
        !_irods_password ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "null parameter" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );

    char myTime[50];
    char rBuf[200];
    size_t i, j;
    char randomPw[50];
    char randomPwEncoded[50];
    int status;
    char passwordInIcat[MAX_PASSWORD_LEN + 2];
    char passwordModifyTime[50];
    char *cVal[3];
    int iVal[3];
    char selUserId[MAX_NAME_LEN];
    char expTime[50];

    std::string zone;
    ret = getLocalZone( _ctx.prop_map(), icss, zone );
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    getNowStr( myTime );

    /* if ttl is unset, use the default */
    if ( _ttl == 0 ) {
        rstrcpy( expTime, irods_pam_password_default_time, sizeof expTime );
    }
    else {
        /* convert ttl to seconds and make sure ttl is within the limits */
        rodsLong_t pamMinTime, pamMaxTime;
        pamMinTime = atoll( irods_pam_password_min_time );
        pamMaxTime = atoll( irods_pam_password_max_time );
        _ttl = _ttl * 3600;
        if ( _ttl < pamMinTime ||
                _ttl > pamMaxTime ) {
            return ERROR( PAM_AUTH_PASSWORD_INVALID_TTL, "pam ttl invalid" );
        }
        snprintf( expTime, sizeof expTime, "%d", _ttl );
    }

    /* get user id */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlUpdateIrodsPamPassword SQL 1" );
    }
    {
        status = hs_get_user_id(svc, icss, (void *) zone.c_str(), (void *) _user_name,
                     selUserId, MAX_NAME_LEN );
    }
    if ( status == CAT_NO_ROWS_FOUND ) {
        return  ERROR( CAT_INVALID_USER, "invalid user" );
    }
    if ( status ) {
        return ERROR( status, "failed to get user id" );
    }

    /* first delete any that are expired */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlUpdateIrodsPamPassword SQL 2" );
    }
    status =  hs_delete_user_password_by_expiry_ts_and_modify_ts(svc, icss,irods_pam_password_min_time,irods_pam_password_max_time,myTime );
    if ( logSQL != 0 ) rodsLog( LOG_SQL, "chlUpdateIrodsPamPassword SQL 3" );
    cVal[0] = passwordInIcat;
    iVal[0] = MAX_PASSWORD_LEN;
    cVal[1] = passwordModifyTime;
    iVal[1] = sizeof( passwordModifyTime );
    {
        status = hs_get_some2_user_password_by_user_and_expiry_ts(svc, icss,selUserId,irods_pam_password_min_time,irods_pam_password_max_time,cVal, iVal, 2 );
    }

    if ( status == 0 ) {
        if ( !irods_pam_auth_no_extend ) {
            if ( logSQL != 0 ) {
                rodsLog( LOG_SQL, "chlUpdateIrodsPamPassword SQL 4" );
            }
            status =  hs_create_password_expiry_ts(svc, icss, selUserId, passwordInIcat, expTime, myTime);
            if ( status ) {
                return ERROR( status, "password update error" );
            }

            status =  hs_commit(svc,  icss );
            if ( status != 0 ) {
                rodsLog( LOG_NOTICE,
                         "chlUpdateIrodsPamPassword cmlExecuteNoAnswerSql commit failure %d",
                         status );
                return ERROR( status, "commit failure" );
            }
        } // if !irods_pam_auth_no_extend
        icatDescramble( passwordInIcat );
        strncpy( *_irods_password, passwordInIcat, irods_pam_password_len );
        return SUCCESS();
    }

    // =-=-=-=-=-=-=-
    // if the resultant scrambled password has a ' in the
    // string, this can cause issues on some systems, notably
    // Suse 12.  if this is the case we will just get another
    // random password.
    bool pw_good = false;
    while ( !pw_good ) {
        j = 0;
        get64RandomBytes( rBuf );
        for ( i = 0; i < 50 && j < irods_pam_password_len - 1; i++ ) {
            char c;
            c = rBuf[i] & 0x7f;
            if ( c < '0' ) {
                c += '0';
            }
            if ( ( c > 'a' && c < 'z' ) || ( c > 'A' && c < 'Z' ) ||
                    ( c > '0' && c < '9' ) ) {
                randomPw[j++] = c;
            }
        }
        randomPw[j] = '\0';

        snprintf( randomPwEncoded, sizeof( randomPwEncoded ), "%s", randomPw );
        icatScramble( randomPwEncoded );
        if ( !strstr( randomPwEncoded, "\'" ) ) {
            pw_good = true;

        }
        else {
            rodsLog( LOG_STATUS, "chlUpdateIrodsPamPassword :: getting a new password [%s] has a single quote", randomPwEncoded );

        }

    } // while

    if ( _test_time != NULL && strlen( _test_time ) > 0 ) {
        snprintf( myTime, sizeof( myTime ), "%s", _test_time );
    }

    if ( logSQL != 0 ) rodsLog( LOG_SQL, "chlUpdateIrodsPamPassword SQL 5" );
    status =  hs_create_user_password(svc, icss, selUserId, randomPwEncoded, expTime, myTime, myTime
                                      );
    if ( status ) return ERROR( status, "insert failure" );

    status =  hs_commit(svc,  icss );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlUpdateIrodsPamPassword cmlExecuteNoAnswerSql commit failure %d",
                 status );
        return ERROR( status, "commit failure" );
    }

    strncpy( *_irods_password, randomPw, irods_pam_password_len );
    return SUCCESS();

} // db_update_pam_password_op

// =-=-=-=-=-=-=-
// authenticate user
irods::error db_mod_user_op(
    irods::plugin_context& _ctx,
    const char*            _user_name,
    const char*            _option,
    const char*            _new_value ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_user_name ||
        !_option    ||
        !_new_value ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "null parameter" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    int status;
    int opType;
    char decoded[MAX_PASSWORD_LEN + 20];
    char tSQL[MAX_SQL_SIZE];
//     char form1[] = "update R_USER_MAIN set %s=?, modify_ts=? where user_name=? and zone_name=?";
//     char form2[] = "update R_USER_MAIN set %s=%s, modify_ts=? where user_name=? and zone_name=?";
//     char form3[] = "update R_USER_PASSWORD set rcat_password=?, modify_ts=? where user_id=?";
//     char form4[] = "insert into R_USER_PASSWORD (user_id, rcat_password, pass_expiry_ts,  create_ts, modify_ts) values ((select user_id from R_USER_MAIN where user_name=? and zone_name=?), ?, ?, ?, ?)";
//     char form5[] = "insert into R_USER_AUTH (user_id, user_auth_name, create_ts) values ((select user_id from R_USER_MAIN where user_name=? and zone_name=?), ?, ?)";
//     char form6[] = "delete from R_USER_AUTH where user_id = (select user_id from R_USER_MAIN where user_name=? and zone_name=?) and user_auth_name = ?";
// #if MY_ICAT
//     char form7[] = "delete from R_USER_PASSWORD where pass_expiry_ts not like '9999%' and cast(pass_expiry_ts as signed integer)>=? and cast(pass_expiry_ts as signed integer)<=? and user_id = (select user_id from R_USER_MAIN where user_name=? and zone_name=?)";
// #else
//     char form7[] = "delete from R_USER_PASSWORD where pass_expiry_ts not like '9999%' and cast(pass_expiry_ts as integer)>=? and cast(pass_expiry_ts as integer)<=? and user_id = (select user_id from R_USER_MAIN where user_name=? and zone_name=?)";
// #endif

    char myTime[50];
    rodsLong_t iVal;

    int userSettingOwnPassword;
    int groupAdminSettingPassword; // JMC - backport 4772

    char userName2[NAME_LEN];
    char zoneName[NAME_LEN];

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlModUser" );
    }

    if ( *_user_name == '\0' || *_option == '\0' ) {
        return ERROR( CAT_INVALID_ARGUMENT, "parameter is empty" );
    }

    userSettingOwnPassword = 0;
    // =-=-=-=-=-=-=-
    // JMC - backport 4772
    groupAdminSettingPassword = 0;
    if ( _ctx.comm()->clientUser.authInfo.authFlag >= LOCAL_PRIV_USER_AUTH && // JMC - backport 4773
            _ctx.comm()->proxyUser.authInfo.authFlag >= LOCAL_PRIV_USER_AUTH ) {
        /* user is OK */
    }
    else {
        /* need to check */
        if ( strcmp( _option, "password" ) != 0 ) {
            /* only password (in cases below) is allowed */
            return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
        }
        if ( strcmp( _user_name, _ctx.comm()->clientUser.userName ) == 0 )  {
            userSettingOwnPassword = 1;
        }
        else {
            int status2;
            status2  = cmlCheckGroupAdminAccess(
                           _ctx.comm()->clientUser.userName,
                           _ctx.comm()->clientUser.rodsZone,
                           "", svc, icss );
            if ( status2 != 0 ) {
                return ERROR( status2, "cmlCheckGroupAdminAccess failed" );
            }
            groupAdminSettingPassword = 1;
        }
    }

    std::string zone;
    ret = getLocalZone( _ctx.prop_map(), icss, zone );
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    tSQL[0] = '\0';
    opType = 0;

    getNowStr( myTime );


    status = validateAndParseUserName( _user_name, userName2, zoneName );
    if ( status ) {
        return ERROR( status, "Invalid username format" );
    }
    if ( zoneName[0] == '\0' ) {
        rstrcpy( zoneName, zone.c_str(), NAME_LEN );
    }

    if ( strcmp( _option, "type" ) == 0 ||
            strcmp( _option, "user_type_name" ) == 0 ) {
            int status2;
            if ( logSQL != 0 ) {
                rodsLog( LOG_SQL, "chlModUser SQL 11" );
            }
            {
                status2 = hs_get_int_token_id(svc, icss, (void *) "user_type", (void *) _new_value,
                              &iVal );
            }
            if ( status2 != 0 ) {
                char errMsg[105];
                snprintf( errMsg, 100, "user_type '%s' is not valid",
                          _new_value );
                addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );

                rodsLog( LOG_NOTICE,
                         "chlModUser invalid user_type" );
                return ERROR( CAT_INVALID_USER_TYPE, "invalid user type" );
            } else {
        status = hs_create_user_type_by_name_and_user_type_token(svc, icss, zoneName, userName2, (void *) _new_value, myTime);
        opType = 1;
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModUser SQL 2" );
        }
            }
    } else
    if ( strcmp( _option, "zone" ) == 0 ||
            strcmp( _option, "zone_name" ) == 0 ) {
        status = hs_create_user_zone_name_by_zone_and_name(svc, icss, zoneName, userName2, (void *) _new_value, myTime);
    } else
    if ( strcmp( _option, "addAuth" ) == 0 ) {
            /* check if user exists */
            int status2;
            if ( logSQL != 0 ) {
                rodsLog( LOG_SQL, "chlModUser SQL 12" );
            }
            {
                status2 = hs_get_int_user_id(svc, icss, zoneName,userName2,
                              &iVal );
            }
            if ( status2 != 0 ) {
                rodsLog( LOG_NOTICE,
                         "chlModUser invalid user %s zone %s", userName2, zoneName );
                return ERROR( CAT_INVALID_USER, "invalid user" );
            } else {
        opType = 4;
        status = hs_create_user_auth_by_user_zone_and_name(svc, icss, zoneName, userName2, (void *) _new_value, myTime);
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModUser SQL 4" );
        }
        if(status != 0) {
            _rollback( "chlModUser" );
        }
            }
    } else
    if ( strcmp( _option, "rmAuth" ) == 0 ) {
        status = hs_delete_user_auth_by_user_name(svc, icss, zoneName, userName2, (void *) _new_value);
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModUser SQL 5" );
        }

    } else

    if ( strncmp( _option, "rmPamPw", 9 ) == 0 ) {
		status = hs_delete_user_password_by_user_zone_and_name_and_expiry_ts(svc, icss,
 zoneName,
userName2,
irods_pam_password_min_time, irods_pam_password_max_time);
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModUser SQL 6" );
        }
    } else

    if ( strcmp( _option, "info" ) == 0 ||
            strcmp( _option, "user_info" ) == 0 ) {
        status = hs_create_user_info_by_zone_and_name(svc, icss, zoneName, userName2, (void *) _new_value, myTime);
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModUser SQL 6" );
        }
    } else
    if ( strcmp( _option, "comment" ) == 0 ||
            strcmp( _option, "r_comment" ) == 0 ) {
        status = hs_create_user_comment_by_zone_and_name(svc, icss, zoneName, userName2, (void *) _new_value, myTime);
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModUser SQL 7" );
        }
    } else
    if ( strcmp( _option, "password" ) == 0 ) {
        int i;
        char userIdStr[MAX_NAME_LEN];
        i = decodePw( _ctx.comm(), _new_value, decoded );
        int status2 = icatApplyRule( _ctx.comm(), ( char* )"acCheckPasswordStrength", decoded );
        if ( status2 == NO_RULE_OR_MSI_FUNCTION_FOUND_ERR ) {
            int status3;
            status3 = addRErrorMsg( &_ctx.comm()->rError, 0,
                                    "acCheckPasswordStrength rule not found" );
        }

        if ( status2 ) {
            return ERROR( status2, "icatApplyRule failed" );
        }

        icatScramble( decoded );

        if ( i ) {
            return ERROR( i, "password scramble failed" );
        }
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModUser SQL 8" );
        }
        {
            i = hs_get_user_id_with_password(svc, icss,zoneName,userName2,
                    userIdStr, MAX_NAME_LEN );
        }
        printf("***********************************get password %d\n", i);
        if ( i != 0 && i != CAT_NO_ROWS_FOUND ) {
            return ERROR( i, "get user password failed" );
        }
        if ( i == 0 ) {
            if ( groupAdminSettingPassword == 1 ) { // JMC - backport 4772
                /* Group admin can only set the initial password, not update */
                return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege level" );
            }
			status = hs_create_user_password_by_user(svc, icss, userIdStr, decoded, myTime);
            if ( logSQL != 0 ) {
                rodsLog( LOG_SQL, "chlModUser SQL 9" );
            }
        }
        else {
            /* check if user exists */
            int status2;
            if ( logSQL != 0 ) {
                rodsLog( LOG_SQL, "chlModUser SQL 12" );
            }
            {
                status2 = hs_get_int_user_id(svc, icss, zoneName,userName2,
                              &iVal );
            }
            if ( status2 != 0 ) {
                rodsLog( LOG_NOTICE,
                         "chlModUser invalid user %s zone %s", userName2, zoneName );
                return ERROR( CAT_INVALID_USER, "invalid user" );
            } else {
            opType = 4;
			status = hs_create_user_password_by_user_zone_and_name(svc, icss, zoneName, userName2, decoded, (void *) "9999-12-31-23.59.01", myTime, myTime);
            if (status != 0) {
                _rollback( "chlModUser" );
            }
            if ( logSQL != 0 ) {
                rodsLog( LOG_SQL, "chlModUser SQL 10" );
            }
            }
        }
    } else {
        return ERROR( CAT_INVALID_ARGUMENT, "invalid option" );
    }

//    if ( tSQL[0] == '\0' ) {
//        return ERROR( CAT_INVALID_ARGUMENT, "invalid argument" );
//    }

    memset( decoded, 0, MAX_PASSWORD_LEN );

    if ( status != 0 ) { /* error */
        rodsLog( LOG_NOTICE,
                 "chlModUser cmlExecuteNoAnswerSql failure %d",
                 status );
        return ERROR( status, "get user_id failed" );
    }

    status =  hs_commit(svc,  icss );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlModUser cmlExecuteNoAnswerSql commit failure %d",
                 status );
        return ERROR( status, "commit failed" );
    }

    return SUCCESS();

} // db_mod_user_op

// =-=-=-=-=-=-=-
// authenticate user
irods::error db_mod_group_op(
    irods::plugin_context& _ctx,
    const char*            _group_name,
    const char*            _option,
    const char*            _user_name,
    const char*            _user_zone ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_group_name ||
        !_option     ||
        !_user_name ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "null parameter" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    int status, OK;
    char myTime[50];
    char userId[MAX_NAME_LEN];
    char groupId[MAX_NAME_LEN];
    // char commentStr[100];
    char zoneToUse[MAX_NAME_LEN];

    char userName2[NAME_LEN];
    char zoneName[NAME_LEN];

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlModGroup" );
    }

    if ( *_group_name == '\0' || *_option == '\0' || *_user_name == '\0' ) {
        return ERROR( CAT_INVALID_ARGUMENT, "argument is empty" );
    }

    if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ||
            _ctx.comm()->proxyUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        int status2;
        status2  = cmlCheckGroupAdminAccess(
                       _ctx.comm()->clientUser.userName,
                       _ctx.comm()->clientUser.rodsZone, _group_name, svc, icss );
        if ( status2 != 0 ) {
            /* User is not a groupadmin that is a member of this group. */
            /* But if we're doing an 'add' and they are a groupadmin
                and the group is empty, allow it */
            if ( strcmp( _option, "add" ) == 0 ) {
                int status3 =  cmlCheckGroupAdminAccess(
                                   _ctx.comm()->clientUser.userName,
                                   _ctx.comm()->clientUser.rodsZone, "", svc, icss );
                if ( status3 == 0 ) {
                    int status4 = cmlGetGroupMemberCount( _group_name, svc, icss );
                    if ( status4 == 0 ) { /* call succeeded and the total is 0 */
                        status2 = 0;    /* reset the error to success to allow it */
                    }
                }
            }
        }
        if ( status2 != 0 ) {
            return ERROR( status2, "group admin access invalid" );
        }
    }

    std::string zone;
    ret = getLocalZone( _ctx.prop_map(), icss, zone );
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    if ( _user_zone != NULL && *_user_zone != '\0' ) {
        snprintf( zoneToUse, MAX_NAME_LEN, "%s", _user_zone );
    }
    else {
        snprintf( zoneToUse, MAX_NAME_LEN, "%s", zone.c_str() );
    }

    status = validateAndParseUserName( _user_name, userName2, zoneName );
    if ( status ) {
        return ERROR( status, "Invalid username format" );
    }
    if ( zoneName[0] != '\0' ) {
        rstrcpy( zoneToUse, zoneName, NAME_LEN );
    }

    userId[0] = '\0';
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlModGroup SQL 1 " );
    }
    {
        status = hs_get_non_group_user_id(svc, icss, zoneToUse, userName2,
                     userId, MAX_NAME_LEN);
    }
    if ( status != 0 ) {
        if ( status == CAT_NO_ROWS_FOUND ) {
            return ERROR( CAT_INVALID_USER, "user not found" );
        }
        _rollback( "chlModGroup" );
        return ERROR( status, "failed to get user" );
    }

    groupId[0] = '\0';
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlModGroup SQL 2" );
    }
    {
        status = hs_get_group_id(svc, icss, (void *) zone.c_str(),(void *) _group_name,
                     groupId, MAX_NAME_LEN);
    }
    if ( status != 0 ) {
        if ( status == CAT_NO_ROWS_FOUND ) {
            return ERROR( CAT_INVALID_GROUP, "invalid group" );
        }
        _rollback( "chlModGroup" );
        return ERROR( status, "failed to get group" );
    }
    OK = 0;
    if ( strcmp( _option, "remove" ) == 0 ) {
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModGroup SQL 3" );
        }
        status =  hs_delete_user_group_(svc, icss, groupId, userId);
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlModGroup cmlExecuteNoAnswerSql delete failure %d",
                     status );
            _rollback( "chlModGroup" );
            return ERROR( status, "delete failure" );
        }
        OK = 1;
    }

    if ( strcmp( _option, "add" ) == 0 ) {
        getNowStr( myTime );
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModGroup SQL 4" );
        }
        status =  hs_create_user_group(svc, icss, groupId, userId, myTime, myTime);
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlModGroup cmlExecuteNoAnswerSql add failure %d",
                     status );
            _rollback( "chlModGroup" );
            return ERROR( status, "add failure" );
        }
        OK = 1;
    }

    if ( OK == 0 ) {
        return ERROR( CAT_INVALID_ARGUMENT, "invalid option" );
    }

    status =  hs_commit(svc,  icss );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlModGroup cmlExecuteNoAnswerSql commit failure %d",
                 status );
        return ERROR( status, "commit failure" );
    }

    return SUCCESS();

} // db_mod_group_op

// =-=-=-=-=-=-=-
// authenticate user
irods::error db_mod_resc_op(
    irods::plugin_context& _ctx,
    const char*            _resc_name,
    const char*            _option,
    const char*            _option_value ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_resc_name  ||
        !_option     ||
        !_option_value ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "null parameter" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    int status = 0, OK = 0;
    char myTime[50];
    char rescId[MAX_NAME_LEN];
    char rescPath[MAX_NAME_LEN] = "";
    char rescPathMsg[MAX_NAME_LEN + 100];
    // char commentStr[200];
    struct hostent *myHostEnt; // JMC - backport 4597

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlModResc" );
    }

    if ( *_resc_name == '\0' || *_option == '\0' ) {
        return ERROR( CAT_INVALID_ARGUMENT, "argument is empty" );
    }

    if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
    }
    if ( _ctx.comm()->proxyUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
    }

    // =-=-=-=-=-=-=-
    // JMC - backport 4629
    if ( strncmp( _resc_name, BUNDLE_RESC, strlen( BUNDLE_RESC ) ) == 0 ) {
        char errMsg[155];
        snprintf( errMsg, 150,
                  "%s is a built-in resource needed for bundle operations.",
                  BUNDLE_RESC );
        addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
        return ERROR( CAT_PSEUDO_RESC_MODIFY_DISALLOWED, "cannot mod bundle resc" );
    }
    // =-=-=-=-=-=-=-

    std::string zone;
    ret = getLocalZone( _ctx.prop_map(), icss, zone );
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    rescId[0] = '\0';
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlModResc SQL 1 " );
    }
    {
        status = hs_get_resc_id(svc, icss, (void *) zone.c_str(),(void *) _resc_name,
                     rescId, MAX_NAME_LEN);
    }
    if ( status != 0 ) {
        if ( status == CAT_NO_ROWS_FOUND ) {
            return ERROR( CAT_INVALID_RESOURCE, "invalid resource" );
        }
        _rollback( "chlModResc" );
        return ERROR( status, "failed to get resource id" );
    }

    getNowStr( myTime );
    OK = 0;

    if ( strcmp( _option, "comment" ) == 0 ) {
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModResc SQL 3" );
        }
        status =  hs_create_resc_comment_ts(svc, icss,rescId,(void *) _option_value, myTime
                       );
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlModResc cmlExecuteNoAnswerSql update failure %d",
                     status );
            _rollback( "chlModResc" );
            return ERROR( status, "failed to update comment" );
        }

        OK = 1;

    }
    else if ( *_option_value == '\0' ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "argument is empty" );
    }

    if ( strcmp( _option, "info" ) == 0 ) {
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModResc SQL 2" );
        }
        status =  hs_create_resc_info_ts(svc, icss,rescId,(void *) _option_value, myTime
                       );
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlModResc cmlExecuteNoAnswerSql update failure %d",
                     status );
            _rollback( "chlModResc" );
            return ERROR( status, "failed to update info" );
        }
        OK = 1;
    }


    if ( strcmp( _option, "freespace" ) == 0 ) {
        int inType = 0;    /* regular mode, just set as provided */
        if ( *_option_value == '+' ) {
            inType = 1;     /* increment by the input value */
            _option_value++;  /* skip over the + */
        }
        if ( *_option_value == '-' ) {
            inType = 2;    /* decrement by the value */
            _option_value++; /* skip over the - */
        }
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModResc SQL 4" );
        }
        if ( inType == 0 ) {
			status =  hs_create_resc_free_space_ts(svc, rescId,(void *) _option_value, myTime, myTime,
	                      icss );
		}
        if ( inType == 1 ) {
          return ERROR( -1, "failed to update freespace" );
        }
        if ( inType == 2 ) {
          return ERROR( -1, "failed to update freespace" );
        }
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlModResc cmlExecuteNoAnswerSql update failure %d",
                     status );
            _rollback( "chlModResc" );
            return ERROR( status, "failed to update freespace" );
        }
        OK = 1;
    }

    if ( strcmp( _option, "host" ) == 0 ) {
        // =-=-=-=-=-=-=-
        // JMC - backport 4597
        _resolveHostName( _ctx.comm(), _option_value, myHostEnt );

        // =-=-=-=-=-=-=-
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModResc SQL 5" );
        }
        status =  hs_create_resc_net_ts(svc, icss, rescId,(void *) _option_value, myTime
                       );
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlModResc cmlExecuteNoAnswerSql update failure %d",
                     status );
            _rollback( "chlModResc" );
            return ERROR( status, "failed to set host" );
        }
        OK = 1;
    }

    if ( strcmp( _option, "type" ) == 0 ) {
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModResc SQL 6" );
        }

        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModResc SQL 7" );
        }
        status =  hs_create_resc_type_name_ts(svc, icss,rescId,(void *) _option_value, myTime
                       );
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlModResc cmlExecuteNoAnswerSql update failure %d",
                     status );
            _rollback( "chlModResc" );
            return ERROR( status, "failed to set type" );
        }
        OK = 1;
    }

    if ( strcmp( _option, "path" ) == 0 ) {
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModResc SQL 10" );
        }
        {
            status = hs_get_resc_def_path(svc, icss, rescId,
                         rescPath, MAX_NAME_LEN );
        }
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlModResc cmlGetStringValueFromSql query failure %d",
                     status );
            _rollback( "chlModResc" );
            return ERROR( status, "failed to get path" );
        }

        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModResc SQL 11" );
        }

        status =  hs_create_resc_def_path_ts(svc, icss,rescId,(void *) _option_value, myTime
                       );
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlModResc cmlExecuteNoAnswerSql update failure %d",
                     status );
            _rollback( "chlModResc" );
            return ERROR( status, "failed to set path" );
        }
        OK = 1;
    }

    if ( strcmp( _option, "status" ) == 0 ) {
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModResc SQL 12" );
        }
        status =  hs_create_resc_status_ts(svc, icss, rescId,(void *) _option_value, myTime
                       );
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlModResc cmlExecuteNoAnswerSql update failure %d",
                     status );
            _rollback( "chlModResc" );
            return ERROR( status, "failed to set status" );
        }
        OK = 1;
    }

    if ( strcmp( _option, "name" ) == 0 ) {
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModResc SQL 13" );
        }
        /*    If the new name is not unique, this will return an error */
        status =  hs_create_resc_name_ts(svc, icss, rescId,(void *) _option_value, myTime
                       );
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlModResc cmlExecuteNoAnswerSql update failure %d",
                     status );
            _rollback( "chlModResc" );
            return ERROR( status, "failed to set resc name with modify time" );
        }

        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModResc SQL 14" );
        }

        // JMC :: remove update r_data_main with resc_name
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModResc SQL 15" );
        }
        status =  hs_create_server_load_resc_name_by_resc_name(svc, icss, (void *) _resc_name, (void *) _option_value
                       );
        if ( status == CAT_SUCCESS_BUT_WITH_NO_INFO ) {
            status = 0;
        }
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlModResc cmlExecuteNoAnswerSql update failure %d",
                     status );
            _rollback( "chlModResc" );
            return ERROR( status, "failed to update server load" );
        }

        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModResc SQL 16" );
        }
        status =  hs_create_server_load_digest_resc_name_by_resc_name(svc, icss, (void *) _resc_name, (void *) _option_value);
        if ( status == CAT_SUCCESS_BUT_WITH_NO_INFO ) {
            status = 0;
        }
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlModResc cmlExecuteNoAnswerSql update failure %d",
                     status );
            _rollback( "chlModResc" );
            return ERROR( status, "failed to set load digest" );
        }

        OK = 1;

    } // if name

    if ( strcmp( _option, "context" ) == 0 ) {
        status =  hs_create_resc_context_ts(svc, icss, rescId, (void *) _option_value, myTime
                       );
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlModResc cmlExecuteNoAnswerSql update failure for resc context %d",
                     status );
            _rollback( "chlModResc" );
            return ERROR( status, "failed to set context" );
        }
        OK = 1;
    }

    if ( OK == 0 ) {
        return ERROR( CAT_INVALID_ARGUMENT, "invalid option" );
    }

    status =  hs_commit(svc,  icss );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlModResc cmlExecuteNoAnswerSql commit failure %d",
                 status );
        return ERROR( status, "commit failure" );
    }

    if ( rescPath[0] != '\0' ) {
        /* if the path was gotten, return it */

        snprintf( rescPathMsg, sizeof( rescPathMsg ), "Previous resource path: %s",
                  rescPath );
        addRErrorMsg( &_ctx.comm()->rError, 0, rescPathMsg );
    }

    return SUCCESS();

} // db_mod_resc_op

// =-=-=-=-=-=-=-
// authenticate user
irods::error db_mod_resc_data_paths_op(
    irods::plugin_context& _ctx,
    const char*            _resc_name,
    const char*            _old_path,
    const char*            _new_path,
    const char*            _user_name ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_resc_name ||
        !_old_path  ||
        !_new_path ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "null parameter" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    char rescId[MAX_NAME_LEN];
    int status, len, rows;
    const char *cptr;
    //   char userId[NAME_LEN]="";
    char userZone[NAME_LEN];
    char zoneToUse[NAME_LEN];
    char userName2[NAME_LEN];


    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlModRescDataPaths" );
    }

    if ( *_resc_name == '\0' || *_old_path == '\0' || *_new_path == '\0' ) {
        return ERROR( CAT_INVALID_ARGUMENT, "argument is empty" );
    }

    /* the paths must begin and end with / */
    if ( *_old_path != '/' or * _new_path != '/' ) {
        return ERROR( CAT_INVALID_ARGUMENT, "invalid path" );
    }
    len = strlen( _old_path );
    cptr = _old_path + len - 1;
    if ( *cptr != '/' ) {
        return ERROR( CAT_INVALID_ARGUMENT, "invalid old path" );
    }
    len = strlen( _new_path );
    cptr = _new_path + len - 1;
    if ( *cptr != '/' ) {
        return ERROR( CAT_INVALID_ARGUMENT, "invalid new path" );
    }

    if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
    }
    if ( _ctx.comm()->proxyUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
    }

    std::string zone;
    ret = getLocalZone( _ctx.prop_map(), icss, zone );
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    rescId[0] = '\0';
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlModRescDataPaths SQL 1 " );
    }
    {
        status = hs_get_resc_id(svc, icss, (void *) zone.c_str(), (void *) _resc_name,
                     rescId, MAX_NAME_LEN );
    }
    if ( status != 0 ) {
        if ( status == CAT_NO_ROWS_FOUND ) {
            return ERROR( CAT_INVALID_RESOURCE, "invalid resource" );
        }
        _rollback( "chlModRescDataPaths" );
        return ERROR( status, "failed to get resource id" );
    }

    /* This is needed for like clause which is needed to get the
       correct number of rows that were updated (seems like the DBMS will
       return a row count for rows looked at for the replace). */
    char oldPath2[MAX_NAME_LEN];
    snprintf( oldPath2, sizeof( oldPath2 ), "%s%%", _old_path );

    if ( _user_name != NULL && *_user_name != '\0' ) {
        status = validateAndParseUserName( _user_name, userName2, userZone );
        if ( status ) {
            return ERROR( status, "Invalid username format" );
        }
        if ( userZone[0] != '\0' ) {
            snprintf( zoneToUse, sizeof( zoneToUse ), "%s", userZone );
        }
        else {
            snprintf( zoneToUse, sizeof( zoneToUse ), "%s", zone.c_str() );
        }

        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModRescDataPaths SQL 2" );
        }
        status =  hs_create_data_path_by_user_zone_and_name_and_resc_name(svc, icss,zoneToUse,userName2,(void *) _resc_name,(void *) _old_path,(void *) _new_path );
    }
    else {
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModRescDataPaths SQL 3" );
        }
        status =  hs_create_data_path(svc, icss,(void *) _resc_name,(void *) _old_path,(void *) _new_path );
    }
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlModRescDataPaths cmlExecuteNoAnswerSql update failure %d",
                 status );
        _rollback( "chlModResc" );
        return ERROR( status, "failed to update path" );
    }

    status =  hs_commit(svc,  icss );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlModResc cmlExecuteNoAnswerSql commit failure %d",
                 status );
        return ERROR( status, "commit failed" );
    }
    (void) rows;
    return SUCCESS();

} // db_mod_resc_data_paths_op

// =-=-=-=-=-=-=-
// authenticate user
irods::error db_mod_resc_freespace_op(
    irods::plugin_context& _ctx,
    const char*            _resc_name,
    int                    _update_value ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_resc_name ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "null parameter" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    int status;
    char myTime[50];
    char updateValueStr[MAX_NAME_LEN];

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlModRescFreeSpace" );
    }

    if ( *_resc_name == '\0' ) {
        return ERROR( CAT_INVALID_ARGUMENT, "resc name is empty" );
    }

    /* The following checks may not be needed long term, but
       shouldn't hurt, for now.
    */

    if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege level" );
    }
    if ( _ctx.comm()->proxyUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege level" );
    }

    getNowStr( myTime );

    snprintf( updateValueStr, MAX_NAME_LEN, "%d", _update_value );


    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlModRescFreeSpace SQL 1 " );
    }
    status =  hs_create_resc_free_space_by_name(svc, icss,(void *) _resc_name,updateValueStr, myTime
                   );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlModRescFreeSpace cmlExecuteNoAnswerSql update failure %d",
                 status );
        _rollback( "chlModRescFreeSpace" );
        return ERROR( status, "update freespace error" );
    }

    return SUCCESS();

} // db_mod_resc_freespace_op

// =-=-=-=-=-=-=-
// commit the transaction
irods::error db_reg_user_re_op(
    irods::plugin_context& _ctx,
    userInfo_t*            _user_info ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_user_info ) {
        return ERROR(
                   CAT_INVALID_ARGUMENT,
                   "null parameter" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    char myTime[50];
    int status;
    char seqStr[MAX_NAME_LEN];
    char userZone[MAX_NAME_LEN];
    char zoneId[MAX_NAME_LEN];

    int zoneForm;
    char userName2[NAME_LEN];
    char zoneName[NAME_LEN];

    static char lastValidUserType[MAX_NAME_LEN] = "";
    static char userTypeTokenName[MAX_NAME_LEN] = "";

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegUserRE" );
    }

    if ( !icss_status ) {
        return ERROR( CATALOG_NOT_CONNECTED, "catalog is not connected" );
    }

    trimWS( _user_info->userName );
    trimWS( _user_info->userType );

    if ( !strlen( _user_info->userType ) || !strlen( _user_info->userName ) ) {
        return ERROR( CAT_INVALID_ARGUMENT, "user type or user name empty" );
    }

    // =-=-=-=-=-=-=-
    // JMC - backport 4772
    if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ||
            _ctx.comm()->proxyUser.authInfo.authFlag  < LOCAL_PRIV_USER_AUTH ) {
        int status2;
        status2  = cmlCheckGroupAdminAccess(
                       _ctx.comm()->clientUser.userName,
                       _ctx.comm()->clientUser.rodsZone,
                       "", svc,
                       icss );
        if ( status2 != 0 ) {
            return ERROR( status2, "invalid group admin access" );
        }
        creatingUserByGroupAdmin = 1;
    }
    // =-=-=-=-=-=-=-
    /*
      Check if the user type is valid.
      This check is skipped if this process has already verified this type
      (iadmin doing a series of mkuser subcommands).
    */
    if ( *_user_info->userType == '\0' ||
            strcmp( _user_info->userType, lastValidUserType ) != 0 ) {
        char errMsg[105];
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlRegUserRE SQL 1 " );
        }
        {
            status = hs_get_token_name_by_namespace_and_name(svc, icss, (void *) "user_type", _user_info->userType,
                         userTypeTokenName, MAX_NAME_LEN );
        }
        if ( status == 0 ) {
            snprintf( lastValidUserType, sizeof( lastValidUserType ), "%s", _user_info->userType );
        }
        else {
            snprintf( errMsg, 100, "user_type '%s' is not valid",
                      _user_info->userType );
            addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
            return ERROR( CAT_INVALID_USER_TYPE, "invalid user type" );
        }
    }

    std::string zone;
    ret = getLocalZone( _ctx.prop_map(), icss, zone );
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    if ( strlen( _user_info->rodsZone ) > 0 ) {
        zoneForm = 1;
        snprintf( userZone, sizeof( userZone ), "%s", _user_info->rodsZone );
    }
    else {
        zoneForm = 0;
        snprintf( userZone, sizeof( userZone ), "%s", zone.c_str() );
    }

    status = validateAndParseUserName( _user_info->userName, userName2, zoneName );
    if ( status ) {
        return ERROR( status, "Invalid username format" );
    }
    if ( zoneName[0] != '\0' ) {
        snprintf( userZone, sizeof( userZone ), "%s", zoneName );
        zoneForm = 2;
    }

    if ( zoneForm ) {
        /* check that the zone exists (if not defaulting to local) */
        zoneId[0] = '\0';
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlRegUserRE SQL 5 " );
        }
        {
            status = hs_get_zone_id(svc, icss, userZone,
                         zoneId, MAX_NAME_LEN );
        }
        if ( status != 0 ) {
            if ( status == CAT_NO_ROWS_FOUND ) {
                char errMsg[105];
                snprintf( errMsg, 100,
                          "zone '%s' does not exist",
                          userZone );
                addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
                return ERROR( CAT_INVALID_ZONE, "invalid zone name" );
            }
            return ERROR( status, "get zone id failure" );
        }
    }

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegUserRE SQL 2" );
    }
    status = hs_get_next_id(svc,  icss, seqStr, MAX_NAME_LEN);
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE, "chlRegUserRE cmlGetNextSeqStr failure %d",
                 status );
        return ERROR( status, "cmlGetNextSeqStr failure" );
    }

    getNowStr( myTime );


    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegUserRE SQL 3" );
    }
    status =  hs_create_user(svc, icss, seqStr, userName2, userTypeTokenName, userZone, myTime, myTime
                   );

    if ( status != 0 ) {
        if ( status == CATALOG_ALREADY_HAS_ITEM_BY_THAT_NAME ) {
            char errMsg[105];
            snprintf( errMsg, 100, "Error %d %s",
                      status,
                      "CATALOG_ALREADY_HAS_ITEM_BY_THAT_NAME"
                    );
            addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
        }
        _rollback( "chlRegUserRE" );
        rodsLog( LOG_NOTICE,
                 "chlRegUserRE insert failure %d", status );
        return ERROR( status, "insert failure" );
    }


    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegUserRE SQL 4" );
    }
    status =  hs_create_user_group(svc, icss, seqStr, seqStr, myTime, myTime
                   );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlRegUserRE insert into R_USER_GROUP failure %d", status );
        _rollback( "chlRegUserRE" );
        return ERROR( status, "insert into r_user_group failure" );
    }


    /*
      The case where the caller is specifying an authstring is used in
      some specialized cases.  Using the new table (Aug 12, 2009), this
      is now set via the chlModUser call below.  This is untested, though.
    */
    if ( strlen( _user_info->authInfo.authStr ) > 0 ) {
        status = chlModUser( _ctx.comm(), _user_info->userName, "addAuth",
                             _user_info->authInfo.authStr );
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlRegUserRE chlModUser insert auth failure %d", status );
            _rollback( "chlRegUserRE" );
            return ERROR( status, "insert auth failure" );
        }
    }

    return CODE( status );

} // db_reg_user_re_op

// =-=-=-=-=-=-=-
// commit the transaction
irods::error db_set_avu_metadata_op(
    irods::plugin_context& _ctx,
    const char*            _type,
    const char*            _name,
    const char*            _attribute,
    const char*            _new_value,
    const char*            _new_unit ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_type   ||
        !_name   ||
        !_attribute ) {
        return ERROR( CAT_INVALID_ARGUMENT, "null parameter" );

    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    int status;
    char myTime[50];
    rodsLong_t objId;
    char metaIdStr[MAX_NAME_LEN * 2]; /* twice as needed to query multiple */
    char objIdStr[MAX_NAME_LEN];

    memset( metaIdStr, 0, sizeof( metaIdStr ) );
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlSetAVUMetadata" );
    }

    if ( !icss_status ) {
        return ERROR( CATALOG_NOT_CONNECTED, "catalog not connected" );
    }

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlSetAVUMetadata SQL 1 " );
    }
    objId = checkAndGetObjectId( _ctx.comm(), _ctx.prop_map(), _type, _name, ACCESS_CREATE_METADATA );
    if ( objId < 0 ) {
        return ERROR( objId, "checkAndGetObjectId failed" );
    }
    snprintf( objIdStr, MAX_NAME_LEN, "%lld", objId );

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlSetAVUMetadata SQL 2" );
    }

    /* Treat unspecified unit as empty string */
    if ( _new_unit == NULL ) {
        _new_unit = "";
    }

    /* Query to see if the attribute exists for this object
     *
     * If status == 0: then object has zero AVUs with matching A
     * If status == 1: then object has *exactly one* AVU with this A AND said AVU is not shared with any other object
     * If status >= 2: then at least one of:
     *                     object has multiple AVUs with this A
     *                     object has an AVU with this A and said AVU is shared with another object
     */

    {
        status = hs_get_some_meta_id_by_attribute_and_obj_id(svc, icss,(void *) _attribute, objIdStr,
						                 metaIdStr, MAX_NAME_LEN, 2);
    }

    if ( status <= 0 ) {
        if ( status == CAT_NO_ROWS_FOUND ) {
            /* Need to add the metadata */
            status = chlAddAVUMetadata( _ctx.comm(), 0, _type, _name, _attribute,
                                        _new_value, _new_unit );
        }
        else {
            rodsLog( LOG_NOTICE,
                     "chlSetAVUMetadata cmlGetMultiRowStringValuesFromSql failure %d",
                     status );
        }
        return ERROR( status, "get avu failed" );
    }

    if ( status > 1 ) {
        /* Cannot update AVU in-place, need to do a delete with wildcards then add */
        status = chlDeleteAVUMetadata( _ctx.comm(), 1, _type, _name, _attribute, "%",
                                       "%", 1 );
        if ( status != 0 ) {
            /* Give it a second chance
             * as per r5350:
             *   "Improve the handling of an ICAT AVU metadata concurrency case.
             *
             *    When setting user-defined meta-data (AVUs), if two Agents try to
             *    modify the the same metadata (which exists with different values in
             *    R_META_MAIN), one of the chlSetAVUMetadata fails.  These changes allow
             *    the agent to retry a second time.
             *
             *    Since this is in the case of "delete then add", the
             *    chlDeleteAVUMetadata is called with noCommit=1, but the delete fails
             *    (since the AVU was changed by the other agent), so in this case
             *    (noCommit set) it should not do the rollback.  And then the caller,
             *    chlSetAVUMetadata, can try a second time to modify the AVU."
             *
             * Essentially, this is a MASSSIVE hack that attempts to bypass a race condition
             * by TRYING TWICE. This implies other major race conditions also exist related
             * to the AVUMetadata, as well as the fact that this race condition is not
             * actually fixed. Leaving it in for now, as it is marginally better than the
             * alternative, but this is a definite TODO: Implement AVUMetadata locks.
             */
            status = chlDeleteAVUMetadata( _ctx.comm(), 1, _type, _name, _attribute, "%",
                                           "%", 1 );
        }
        if ( status != 0 ) {
            _rollback( "chlSetAVUMetadata" );
            return ERROR( status, "delete avu metadata failed" );
        }
        status = chlAddAVUMetadata( _ctx.comm(), 0, _type, _name, _attribute,
                                    _new_value, _new_unit );
        return ERROR( status, "delete avu metadata failed" );
    }

    /* Only one metaId for this Attribute and Object has been found, and the metaID is not shared */
    rodsLog( LOG_NOTICE, "chlSetAVUMetadata found metaId %s", metaIdStr );

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlSetAVUMetadata SQL 4" );
    }

    getNowStr( myTime );

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlSetAVUMetadata SQL 5" );
    }
    status = hs_create_meta_value_and_units_ts(svc, icss,metaIdStr,(void *) _new_value, (void *) _new_unit, myTime
                                     );

    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlSetAVUMetadata cmlExecuteNoAnswerSql update failure %d",
                 status );
        _rollback( "chlSetAVUMetadata" );
        return ERROR( status, "set avu failed" );
    }

    status =  hs_commit(svc,  icss );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlSetAVUMetadata cmlExecuteNoAnswerSql commit failure %d",
                 status );
        return ERROR( status, "commit failed" );
    }

    return CODE( status );

} // db_set_avu_metadata_op

#define ACCESS_MAX 999999  /* A large access value (larger than the
maximum used (i.e. for fail safe)) and
    also indicates not initialized*/
// =-=-=-=-=-=-=-
// Add an Attribute-Value [Units] pair/triple metadata item to one or
// more data objects.  This is the Wildcard version, where the
// collection/data-object name can match multiple objects).

// The return value is error code (negative) or the number of objects
// to which the AVU was associated.
irods::error db_add_avu_metadata_wild_op(
    irods::plugin_context& _ctx,
    int                    _admin_mode,
    const char*            _type,
    const char*            _name,
    const char*            _attribute,
    const char*            _value,
    const char*            _units ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_type   ||
        !_name   ||
        !_attribute ) {
        return ERROR( CAT_INVALID_ARGUMENT, "null parameter" );

    }

    rodsLong_t status;//, status2;
    rodsLong_t seqNum;
    char collection[MAX_NAME_LEN];
    char objectName[MAX_NAME_LEN];
    char myTime[50];
    char seqNumStr[MAX_NAME_LEN];

    status = splitPathByKey( _name, collection, MAX_NAME_LEN, objectName, MAX_NAME_LEN, '/' );

    if ( strlen( collection ) == 0 ) {
        snprintf( collection, sizeof( collection ), "%s", PATH_SEPARATOR );
        snprintf( objectName, sizeof( objectName ), "%s", _name );
    }

    ret = determine_user_has_modify_metadata_access(
              objectName,
              collection,
              _ctx.comm()->clientUser.userName,
              _ctx.comm()->clientUser.rodsZone );
    if( !ret.ok() ) {
        return PASS( ret );
    }

    // user has write access, set up the AVU and associate it with the data-objects
    status = findOrInsertAVU( _attribute, _value, _units );
    if ( status < 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlAddAVUMetadataWild findOrInsertAVU failure %d",
                 status );
        _rollback( "chlAddAVUMetadata" );
        return ERROR( status, "findOrInsertAVU failure" );
    }
    seqNum = status;

    getNowStr( myTime );
    snprintf( seqNumStr, sizeof seqNumStr, "%lld", seqNum );
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlAddAVUMetadataWild SQL 8" );
    }
    status =  hs_create_metamap_by_name(svc, icss, collection, objectName,seqNumStr,  myTime, myTime
                   );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlAddAVUMetadataWild cmlExecuteNoAnswerSql insert failure %d",
                 status );
        _rollback( "chlAddAVUMetadataWild" );
        return ERROR( status, "insert failure" );
    }


    /* Commit */
    status =  hs_commit(svc,  icss );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlAddAVUMetadataWild cmlExecuteNoAnswerSql commit failure %d",
                 status );
        return ERROR( status, "commit failure" );
    }

    return CODE( ret.code() );

} // db_add_avu_metadata_wild_op

irods::error db_add_avu_metadata_op(
    irods::plugin_context& _ctx,
    int                    _admin_mode,
    const char*            _type,
    const char*            _name,
    const char*            _attribute,
    const char*            _value,
    const char*            _units ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_type   ||
        !_name   ||
        !_attribute ) {
        return ERROR( CAT_INVALID_ARGUMENT, "null parameter" );

    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    int itype;
    char myTime[50];
    char logicalEndName[MAX_NAME_LEN];
    char logicalParentDirName[MAX_NAME_LEN];
    rodsLong_t seqNum, iVal;
    rodsLong_t objId, status;
    char objIdStr[MAX_NAME_LEN];
    char seqNumStr[MAX_NAME_LEN];
    char userName[NAME_LEN];
    char userZone[NAME_LEN];

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlAddAVUMetadata" );
    }

    if ( !icss_status ) {
        return ERROR( CATALOG_NOT_CONNECTED, "catalog not connected" );
    }

    if ( _type == NULL || *_type == '\0' ) {
        return ERROR( CAT_INVALID_ARGUMENT, "type null or empty" );
    }

    if ( _name == NULL || *_name == '\0' ) {
        return ERROR( CAT_INVALID_ARGUMENT, "name null or empty" );
    }

    if ( _attribute == NULL || *_attribute == '\0' ) {
        return ERROR( CAT_INVALID_ARGUMENT, "attribute null or empty" );
    }

    if ( _value == NULL || *_value == '\0' ) {
        return  ERROR( CAT_INVALID_ARGUMENT, "value null or empty" );
    }

    if ( _admin_mode == 1 ) {
        if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
            return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
        }
    }

    if ( _units == NULL ) {
        _units = "";
    }

    itype = convertTypeOption( _type );
    if ( itype == 0 ) {
        return ERROR( CAT_INVALID_ARGUMENT, "invalid type argument" );
    }

    if ( itype == 1 ) {
        status = splitPathByKey( _name,
                                 logicalParentDirName, MAX_NAME_LEN, logicalEndName, MAX_NAME_LEN, '/' );
        if ( strlen( logicalParentDirName ) == 0 ) {
            snprintf( logicalParentDirName, sizeof( logicalParentDirName ), "%s", PATH_SEPARATOR );
            snprintf( logicalEndName, sizeof( logicalEndName ), "%s", _name );
        }
        if ( _admin_mode == 1 ) {
            if ( logSQL != 0 ) {
                rodsLog( LOG_SQL, "chlAddAVUMetadata SQL 1 " );
            }
            {
                status = hs_get_int_data_id_by_name(svc, icss, logicalParentDirName,logicalEndName,
                             &iVal);
            }
            if ( status == 0 ) {
                status = iVal;    /*like cmlCheckDataObjOnly, status is objid */
            }
        }
        else {
            if ( logSQL != 0 ) {
                rodsLog( LOG_SQL, "chlAddAVUMetadata SQL 2" );
            }
            status = cmlCheckDataObjOnly( logicalParentDirName, logicalEndName,
                                          _ctx.comm()->clientUser.userName,
                                          _ctx.comm()->clientUser.rodsZone,
                                          ACCESS_CREATE_METADATA, svc, icss );
        }
        if ( status < 0 ) {
            _rollback( "chlAddAVUMetadata" );
            return ERROR( status, "select data_id failed" );
        }
        objId = status;
    }

    if ( itype == 2 ) {
        if ( _admin_mode == 1 ) {
            if ( logSQL != 0 ) {
                rodsLog( LOG_SQL, "chlAddAVUMetadata SQL 3" );
            }
            {
                status = hs_get_int_coll_id_by_name(svc, icss,(void *) _name,
                             &iVal);
            }
            if ( status == 0 ) {
                status = iVal;    /*like cmlCheckDir, status is objid*/
            }
        }
        else {
            /* Check that the collection exists and user has create_metadata
               permission, and get the collectionID */
            if ( logSQL != 0 ) {
                rodsLog( LOG_SQL, "chlAddAVUMetadata SQL 4" );
            }
            status = cmlCheckDir( _name,
                                  _ctx.comm()->clientUser.userName,
                                  _ctx.comm()->clientUser.rodsZone,
                                  ACCESS_CREATE_METADATA, svc, icss );
        }
        if ( status < 0 ) {
            char errMsg[105];
            _rollback( "chlAddAVUMetadata" );
            if ( status == CAT_UNKNOWN_COLLECTION ) {
                snprintf( errMsg, 100, "collection '%s' is unknown",
                          _name );
                addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
            }
            else {
                _rollback( "chlAddAVUMetadata" );
            }
            return ERROR( status, "cmlCheckDir failed" );
        }
        objId = status;
    }

    if ( itype == 3 ) {
        if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
            return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
        }

        std::string zone;
        ret = getLocalZone( _ctx.prop_map(), icss, zone );
        if ( !ret.ok() ) {
            return PASS( ret );
        }

        objId = 0;
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlAddAVUMetadata SQL 5" );
        }
        {
            status = hs_get_int_resc_id(svc, icss,(void *) zone.c_str(), (void *)_name,
                         &objId  );
        }
        if ( status != 0 ) {
            _rollback( "chlAddAVUMetadata" );
            if ( status == CAT_NO_ROWS_FOUND ) {
                return ERROR( CAT_INVALID_RESOURCE, "invalid resource" );
            }
            return ERROR( status, "select resc_id failed" );
        }
    }

    if ( itype == 4 ) {
        if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
            return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
        }

        status = validateAndParseUserName( _name, userName, userZone );
        if ( status ) {
            return ERROR( status, "Invalid username format" );
        }
        if ( userZone[0] == '\0' ) {
            std::string zone;
            ret = getLocalZone( _ctx.prop_map(), icss, zone );
            if ( !ret.ok() ) {
                return PASS( ret );
            }
            snprintf( userZone, NAME_LEN, "%s", zone.c_str() );
        }

        objId = 0;
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlAddAVUMetadata SQL 6" );
        }
        {
            status = hs_get_int_user_id(svc, icss, userZone, userName,
                         &objId);
        }
        if ( status != 0 ) {
            _rollback( "chlAddAVUMetadata" );
            if ( status == CAT_NO_ROWS_FOUND ) {
                return ERROR( CAT_INVALID_USER, "invalid user" );
            }
            return ERROR( status, "select user_id failed" );
        }
    }

    if ( itype == 5 ) {
        if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
            return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL , "insufficient privilege" );
        }

        std::string zone;
        ret = getLocalZone( _ctx.prop_map(), icss, zone );
        if ( !ret.ok() ) {
            return PASS( ret );
        }

        objId = 0;
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlAddAVUMetadata SQL 7" );
        }
        {
            status = hs_get_int_resc_group_id(svc, icss, (void *) _name,
                         &objId);
        }
        if ( status != 0 ) {
            _rollback( "chlAddAVUMetadata" );
            if ( status == CAT_NO_ROWS_FOUND ) {
                return ERROR( CAT_INVALID_RESOURCE, "invalid resource" );
            }
            return ERROR( status, "select failure" );
        }
    }

    status = findOrInsertAVU( _attribute, _value, _units );
    if ( status < 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlAddAVUMetadata findOrInsertAVU failure %d",
                 status );
        _rollback( "chlAddAVUMetadata" );
        return ERROR( status, "findOrInsertAVU failure" );
    }
    seqNum = status;

    getNowStr( myTime );
    snprintf( objIdStr, sizeof objIdStr, "%lld", objId );
    snprintf( seqNumStr, sizeof seqNumStr, "%lld", seqNum );
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlAddAVUMetadata SQL 7" );
    }
    status =  hs_create_metamap(svc, icss, objIdStr, seqNumStr, myTime, myTime
                   );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlAddAVUMetadata cmlExecuteNoAnswerSql insert failure %d",
                 status );
        _rollback( "chlAddAVUMetadata" );
        return ERROR( status, "insert failure" );
    }

    status =  hs_commit(svc,  icss );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlAddAVUMetadata cmlExecuteNoAnswerSql commit failure %d",
                 status );
        return ERROR( status, "commit failure" );
    }

    return CODE( status );

} // db_add_avu_metadata_op

irods::error db_mod_avu_metadata_op(
    irods::plugin_context& _ctx,
    const char*                  _type,
    const char*                  _name,
    const char*                  _attribute,
    const char*                  _value,
    const char*                  _unitsOrArg0,
    const char*                  _arg1,
    const char*                  _arg2,
    const char*                  _arg3 ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_type   ||
        !_name   ||
        !_attribute ) {
        return ERROR( CAT_INVALID_ARGUMENT, "null parameter" );

    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    int status, atype;
    char myUnits[MAX_NAME_LEN] = "";
    const char *addAttr = "";
    const char *addValue = "";
    const char  *addUnits = "";
    int newUnits = 0;
    if ( _unitsOrArg0 == NULL || *_unitsOrArg0 == '\0' ) {
        return ERROR( CAT_INVALID_ARGUMENT, "unitsOrArg0 empty or null" );
    }
    atype = checkModArgType( _unitsOrArg0 );
    if ( atype == 0 ) {
        snprintf( myUnits, sizeof( myUnits ), "%s", _unitsOrArg0 );
    }

    status = chlDeleteAVUMetadata( _ctx.comm(), 0, _type, _name, _attribute, _value,
                                   myUnits, 1 );
    if ( status != 0 ) {
        _rollback( "chlModAVUMetadata" );
        return ERROR( status, "delete avu metadata failed" );
    }

    if ( atype == 1 ) {
        addAttr = _unitsOrArg0 + 2;
    }
    if ( atype == 2 ) {
        addValue = _unitsOrArg0 + 2;
    }
    if ( atype == 3 ) {
        addUnits = _unitsOrArg0 + 2;
    }

    atype = checkModArgType( _arg1 );
    if ( atype == 1 ) {
        addAttr = _arg1 + 2;
    }
    if ( atype == 2 ) {
        addValue = _arg1 + 2;
    }
    if ( atype == 3 ) {
        addUnits = _arg1 + 2;
    }

    atype = checkModArgType( _arg2 );
    if ( atype == 1 ) {
        addAttr = _arg2 + 2;
    }
    if ( atype == 2 ) {
        addValue = _arg2 + 2;
    }
    if ( atype == 3 ) {
        addUnits = _arg2 + 2;
    }

    atype = checkModArgType( _arg3 );
    if ( atype == 1 ) {
        addAttr = _arg3 + 2;
    }
    if ( atype == 2 ) {
        addValue = _arg3 + 2;
    }
    if ( atype == 3 ) {
        addUnits = _arg3 + 2;
        newUnits = 1;
    }

    if ( *addAttr  == '\0' &&
            *addValue == '\0' &&
            *addUnits == '\0' ) {
        _rollback( "chlModAVUMetadata" );
        return ERROR( CAT_INVALID_ARGUMENT, "arg check failed" );
    }

    if ( *addAttr == '\0' ) {
        addAttr = _attribute;
    }
    if ( *addValue == '\0' ) {
        addValue = _value;
    }
    if ( *addUnits == '\0' && newUnits == 0 ) {
        addUnits = myUnits;
    }

    status = chlAddAVUMetadata( _ctx.comm(), 0, _type, _name, addAttr, addValue,
                                addUnits );
    return CODE( status );

} // db_mod_avu_metadata_op

irods::error db_del_avu_metadata_op(
    irods::plugin_context& _ctx,
    int                    _option,
    const char*            _type,
    const char*            _name,
    const char*            _attribute,
    const char*            _value,
    const char*            _unit,
    int                    _nocommit ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_type   ||
        !_name   ||
        !_attribute ) {
        return ERROR( CAT_INVALID_ARGUMENT, "null parameter" );

    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    int itype;
    char logicalEndName[MAX_NAME_LEN];
    char logicalParentDirName[MAX_NAME_LEN];
    rodsLong_t status;
    rodsLong_t objId;
    char objIdStr[MAX_NAME_LEN];
    int allowNullUnits;
    char userName[NAME_LEN];
    char userZone[NAME_LEN];

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlDeleteAVUMetadata" );
    }

    if ( !icss_status ) {
        return ERROR( CATALOG_NOT_CONNECTED, "catalog not connected" );
    }

    if ( _type == NULL || *_type == '\0' ) {
        return ERROR( CAT_INVALID_ARGUMENT, "invalid type" );
    }

    if ( _name == NULL || *_name == '\0' ) {
        return ERROR( CAT_INVALID_ARGUMENT, "invalid name" );
    }
    if ( _option != 2 ) {
        if ( _attribute == NULL || *_attribute == '\0' ) {
            return ERROR( CAT_INVALID_ARGUMENT, "invalid attribute" );
        }

        if ( _value == NULL || *_value == '\0' ) {
            return ERROR( CAT_INVALID_ARGUMENT, "invalid value" );
        }
    }

    if ( _unit == NULL ) {
        _unit = "";
    }

    itype = convertTypeOption( _type );
    if ( itype == 0 ) {
        return ERROR( CAT_INVALID_ARGUMENT, "invalid type" );
    }

    if ( itype == 1 ) {
        status = splitPathByKey( _name,
                                 logicalParentDirName, MAX_NAME_LEN, logicalEndName, MAX_NAME_LEN, '/' );
        if ( strlen( logicalParentDirName ) == 0 ) {
            snprintf( logicalParentDirName, sizeof( logicalParentDirName ), "%s", PATH_SEPARATOR );
            snprintf( logicalEndName, sizeof( logicalEndName ), "%s", _name );
        }
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlDeleteAVUMetadata SQL 1 " );
        }
        status = cmlCheckDataObjOnly( logicalParentDirName, logicalEndName,
                                      _ctx.comm()->clientUser.userName,
                                      _ctx.comm()->clientUser.rodsZone,
                                      ACCESS_DELETE_METADATA, svc, icss );
        if ( status < 0 ) {
            if ( _nocommit != 1 ) {
                _rollback( "chlDeleteAVUMetadata" );
            }
            return ERROR( status, "delete avu failed" );
        }
        objId = status;
    }

    if ( itype == 2 ) {
        /* Check that the collection exists and user has delete_metadata permission,
           and get the collectionID */
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlDeleteAVUMetadata SQL 2" );
        }
        status = cmlCheckDir( _name,
                              _ctx.comm()->clientUser.userName,
                              _ctx.comm()->clientUser.rodsZone,
                              ACCESS_DELETE_METADATA, svc, icss );
        if ( status < 0 ) {
            char errMsg[105];
            if ( status == CAT_UNKNOWN_COLLECTION ) {
                snprintf( errMsg, 100, "collection '%s' is unknown",
                          _name );
                addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
            }
            return ERROR( status, "cmlCheckDir failed" );
        }
        objId = status;
    }

    if ( itype == 3 ) {
        if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
            return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
        }

        std::string zone;
        ret = getLocalZone( _ctx.prop_map(), icss, zone );
        if ( !ret.ok() ) {
            return PASS( ret );
        }

        objId = 0;
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlDeleteAVUMetadata SQL 3" );
        }
        {
            status = hs_get_int_resc_id(svc, icss, (void *) zone.c_str(),(void *) _name,
                         &objId  );
        }
        if ( status != 0 ) {
            if ( status == CAT_NO_ROWS_FOUND ) {
                return ERROR( CAT_INVALID_RESOURCE, "invalid resource" );
            }
            if ( _nocommit != 1 ) {
                _rollback( "chlDeleteAVUMetadata" );
            }
            return ERROR( status, "select resc_id failed" );
        }
    }

    if ( itype == 4 ) {
        if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
            return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
        }

        status = validateAndParseUserName( _name, userName, userZone );
        if ( status ) {
            return ERROR( status, "Invalid username format" );
        }
        if ( userZone[0] == '\0' ) {
            std::string zone;
            ret = getLocalZone( _ctx.prop_map(), icss, zone );
            if ( !ret.ok() ) {
                return PASS( ret );
            }
            snprintf( userZone, sizeof( userZone ), "%s", zone.c_str() );
        }

        objId = 0;
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlDeleteAVUMetadata SQL 4" );
        }
        {
   status = hs_get_int_user_id(svc, icss, userZone,userName,
                         &objId  );
        }
        if ( status != 0 ) {
            if ( status == CAT_NO_ROWS_FOUND ) {
                return ERROR( CAT_INVALID_USER, "invalid user" );
            }
            if ( _nocommit != 1 ) {
                _rollback( "chlDeleteAVUMetadata" );
            }
            return ERROR( status, "select user_id failed" );
        }
    }

    if ( itype == 5 ) {
        if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
            return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
        }

        std::string zone;
        ret = getLocalZone( _ctx.prop_map(), icss, zone );
        if ( !ret.ok() ) {
            return PASS( ret );
        }

        objId = 0;
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlDeleteAVUMetadata SQL 5" );
        }
        {
            status = hs_get_int_resc_group_id(svc, icss, (void *) _name,
                         &objId);
        }
        if ( status != 0 ) {
            if ( status == CAT_NO_ROWS_FOUND ) {
                return ERROR( CAT_INVALID_RESOURCE, "invalid resource" );
            }
            if ( _nocommit != 1 ) {
                _rollback( "chlDeleteAVUMetadata" );
            }
            return ERROR( status, "select failure" );
        }
    }


    snprintf( objIdStr, MAX_NAME_LEN, "%lld", objId );
    if ( _option == 2 ) {

        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlDeleteAVUMetadata SQL 9" );
        }
        status =  hs_delete_metamap_by_attribute_and_obj_id(svc, icss, (void *) _attribute /* attribute is really id */, objIdStr
                       );
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlDeleteAVUMetadata cmlExecuteNoAnswerSql delete failure %d",
                     status );
            if ( _nocommit != 1 ) {
                _rollback( "chlDeleteAVUMetadata" );
            }

            return ERROR( status, "delete failure" );
        }

        if ( _nocommit != 1 ) {
            status =  hs_commit(svc,  icss );
            if ( status != 0 ) {
                rodsLog( LOG_NOTICE,
                         "chlDeleteAVUMetadata cmlExecuteNoAnswerSql commit failure %d",
                         status );
                return ERROR( status, "commit failure" );
            }
        }
        return ERROR( status, "delete failure" );
    }

    allowNullUnits = 0;
    if ( *_unit == '\0' ) {
        allowNullUnits = 1; /* null or empty-string units */
    }
    if ( _option == 1 && *_unit == '%' && *( _unit + 1 ) == '\0' ) {
        allowNullUnits = 1; /* wildcard and just % */
    }

    if ( allowNullUnits ) {
        if ( _option == 1 ) { /* use wildcards ('like') */
            if ( logSQL != 0 ) {
                rodsLog( LOG_SQL, "chlDeleteAVUMetadata SQL 5" );
            }
            status =  hs_delete_metamap_allow_null_units_use_wildcards(svc, icss, (void *) _attribute, (void *) _value, (void *) _unit,objIdStr
                           );
        }
        else {
            if ( logSQL != 0 ) {
                rodsLog( LOG_SQL, "chlDeleteAVUMetadata SQL 6" );
            }
            status =  hs_delete_metamap_allow_null_units(svc, icss, (void *) _attribute, (void *) _value, (void *) _unit,objIdStr
                           );
        }
    }
    else {
        if ( _option == 1 ) { /* use wildcards ('like') */
            if ( logSQL != 0 ) {
                rodsLog( LOG_SQL, "chlDeleteAVUMetadata SQL 7" );
            }
            status =  hs_delete_metamap_use_wildcards(svc, icss, (void *) _attribute, (void *) _value, (void *) _unit,objIdStr
                           );
        }
        else {
            if ( logSQL != 0 ) {
                rodsLog( LOG_SQL, "chlDeleteAVUMetadata SQL 8" );
            }
            status =  hs_delete_metamap_by_avu_and_obj_id(svc, icss, (void *) _attribute, (void *) _value, (void *) _unit, objIdStr);
        }
    }
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlDeleteAVUMetadata cmlExecuteNoAnswerSql delete failure %d",
                 status );
        if ( _nocommit != 1 ) {
            _rollback( "chlDeleteAVUMetadata" );
        }
        return ERROR( status, "delete failure" );
    }

    if ( _nocommit != 1 ) {
        status =  hs_commit(svc,  icss );
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlDeleteAVUMetadata cmlExecuteNoAnswerSql commit failure %d",
                     status );
            return ERROR( status, "comit failure" );
        }
    }

    return CODE( status );

} // db_del_avu_metadata_op

irods::error db_copy_avu_metadata_op(
    irods::plugin_context& _ctx,
    const char*            _type1,
    const char*            _type2,
    const char*            _name1,
    const char*            _name2 ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if (
        !_type1  ||
        !_type2  ||
        !_name1  ||
        !_name2 ) {
        return ERROR( CAT_INVALID_ARGUMENT, "null parameter" );

    }

    char myTime[50];
    int status;
    rodsLong_t objId1, objId2;
    char objIdStr1[MAX_NAME_LEN];
    char objIdStr2[MAX_NAME_LEN];

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlCopyAVUMetadata" );
    }

    if ( !icss_status ) {
        return ERROR( CATALOG_NOT_CONNECTED, "catalog not connected" );
    }

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlCopyAVUMetadata SQL 1 " );
    }
    objId1 = checkAndGetObjectId( _ctx.comm(), _ctx.prop_map(), _type1, _name1, ACCESS_READ_METADATA );
    if ( objId1 < 0 ) {
        return ERROR( objId1, "checkAndGetObjectId failure" );
    }

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlCopyAVUMetadata SQL 2" );
    }
    objId2 = checkAndGetObjectId( _ctx.comm(), _ctx.prop_map(), _type2, _name2, ACCESS_CREATE_METADATA );

    if ( objId2 < 0 ) {
        return ERROR( objId2, "checkAndGetObjectId failure" );
    }

    snprintf( objIdStr1, MAX_NAME_LEN, "%lld", objId1 );
    snprintf( objIdStr2, MAX_NAME_LEN, "%lld", objId2 );

    getNowStr( myTime );
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlCopyAVUMetadata SQL 3" );
    }
    status =  hs_create_meta_map_by_obj_id(svc, objIdStr2, objIdStr1, myTime, myTime,
                  icss );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlCopyAVUMetadata cmlExecuteNoAnswerSql insert failure %d",
                 status );
        _rollback( "chlCopyAVUMetadata" );
        return ERROR( status, "insert failure" );
    }

    status =  hs_commit(svc,  icss );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlCopyAVUMetadata cmlExecuteNoAnswerSql commit failure %d",
                 status );
        return ERROR( status, "commit failure" );
    }

    return CODE( status );

} // db_copy_avu_metadata_op

irods::error db_mod_access_control_resc_op(
    irods::plugin_context& _ctx,
    const int                    _recursive_flag,
    const char*                  _access_level,
    const char*                  _user_name,
    const char*                  _zone,
    const char*                  _resc_name ) {
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    char myAccessStr[LONG_NAME_LEN];
    char rescIdStr[MAX_NAME_LEN];
    char *myAccessLev = NULL;
    int rmFlag = 0;
    rodsLong_t status;
    const char *myZone;
    rodsLong_t userId;
    char userIdStr[MAX_NAME_LEN];
    char myTime[50];
    rodsLong_t iVal;

    snprintf( myAccessStr, sizeof( myAccessStr ), "%s", _access_level + strlen( MOD_RESC_PREFIX ) );

    if ( strcmp( myAccessStr, AP_NULL ) == 0 ) {
        myAccessLev = ACCESS_NULL;
        rmFlag = 1;
    }
    else if ( strcmp( myAccessStr, AP_READ ) == 0 ) {
        myAccessLev = ACCESS_READ_OBJECT;
    }
    else if ( strcmp( myAccessStr, AP_WRITE ) == 0 ) {
        myAccessLev = ACCESS_MODIFY_OBJECT;
    }
    else if ( strcmp( myAccessStr, AP_OWN ) == 0 ) {
        myAccessLev = ACCESS_OWN;
    }
    else {
        char errMsg[105];
        snprintf( errMsg, 100, "access level '%s' is invalid for a resource",
                  myAccessStr );
        addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
        return ERROR( CAT_INVALID_ARGUMENT, "invalid argument" );
    }

    if ( _ctx.comm()->clientUser.authInfo.authFlag >= LOCAL_PRIV_USER_AUTH ) {
        /* admin, so just get the resc_id */
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModAccessControlResc SQL 1" );
        }
        {
            status = hs_get_int_resc_id_by_name(svc, icss, (void *) _resc_name,
							&iVal);
        }
        if ( status == CAT_NO_ROWS_FOUND ) {
            return ERROR( CAT_UNKNOWN_RESOURCE, "unknown resource" );
        }
        if ( status < 0 ) {
            return ERROR( status, "select resc_id failure" );
        }
        status = iVal;
    }
    else {
        status = cmlCheckResc( _resc_name,
                               _ctx.comm()->clientUser.userName,
                               _ctx.comm()->clientUser.rodsZone,
                               ACCESS_OWN, svc,
                               icss );
        if ( status < 0 ) {
            return ERROR( status, "cmlCheckResc error" );
        }
    }
    snprintf( rescIdStr, MAX_NAME_LEN, "%lld", status );

    /* Check that the receiving user exists and if so get the userId */
    std::string zone;
    ret = getLocalZone( _ctx.prop_map(), icss, zone );
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    myZone = _zone;
    if ( _zone == NULL || strlen( _zone ) == 0 ) {
        myZone = zone.c_str();
    }

    userId = 0;
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlModAccessControlResc SQL 2" );
    }
    {
        status = hs_get_int_user_id(svc, icss, (void *) myZone,(void *) _user_name,
                     &userId  );
    }
    if ( status != 0 ) {
        if ( status == CAT_NO_ROWS_FOUND ) {
            return ERROR( CAT_INVALID_USER, "invalid user" );
        }
        return ERROR( status, "select user_id failure" );
    }

    snprintf( userIdStr, MAX_NAME_LEN, "%lld", userId );

    /* remove any access permissions */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlModAccessControlResc SQL 3" );
    }
    status =  hs_delete_access_by_obj_id_and_user(svc, icss, rescIdStr,userIdStr
                   );
    if ( status != 0 && status != CAT_SUCCESS_BUT_WITH_NO_INFO ) {
        return ERROR( status, "delete failure" );
    }

    /* If not just removing, add the new value */
    if ( rmFlag == 0 ) {
        getNowStr( myTime );
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModAccessControlResc SQL 4" );
        }
        status =  hs_create_access_by_access_type_token(svc, icss, rescIdStr, userIdStr, myAccessLev, myTime, myTime
                       );
        if ( status != 0 ) {
            _rollback( "chlModAccessControlResc" );
            return ERROR( status, "insert failure" );
        }
    }

    status =  hs_commit(svc,  icss );
    if ( status < 0 ) {
        return ERROR( status, "commit failure" );
    }

    return SUCCESS();

} // db_mod_access_control_resc_op

irods::error db_mod_access_control_op(
    irods::plugin_context& _ctx,
    const int                    _recursive_flag,
    const char*                  _access_level,
    const char*                  _user_name,
    const char*                  _zone,
    const char*                  _path_name ) {
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlModAccessControl" );
    }

    if ( strncmp( _access_level, MOD_RESC_PREFIX, strlen( MOD_RESC_PREFIX ) ) == 0 ) {
        ret = db_mod_access_control_resc_op(
                  _ctx,
                  _recursive_flag,
                  _access_level,
                  _user_name,
                  _zone,
                  _path_name );
        return PASS( ret );
    }

    int adminMode = 0;
    char myAccessStr[LONG_NAME_LEN];
    if ( strncmp( _access_level, MOD_ADMIN_MODE_PREFIX,
                  strlen( MOD_ADMIN_MODE_PREFIX ) ) == 0 ) {
        if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
            addRErrorMsg( &_ctx.comm()->rError, 0,
                          "You must be the admin to use the -M admin mode" );
            return ERROR( CAT_NO_ACCESS_PERMISSION, "You must be the admin to use the -M admin mode" );
        }
        snprintf( myAccessStr, sizeof( myAccessStr ), "%s", _access_level + strlen( MOD_ADMIN_MODE_PREFIX ) );
        _access_level = myAccessStr;
        adminMode = 1;
    }

    char *myAccessLev = NULL;
    int rmFlag = 0;
    int inheritFlag = 0;
    if ( strcmp( _access_level, AP_NULL ) == 0 ) {
        myAccessLev = ACCESS_NULL;
        rmFlag = 1;
    }
    else if ( strcmp( _access_level, AP_READ ) == 0 ) {
        myAccessLev = ACCESS_READ_OBJECT;
    }
    else if ( strcmp( _access_level, AP_WRITE ) == 0 ) {
        myAccessLev = ACCESS_MODIFY_OBJECT;
    }
    else if ( strcmp( _access_level, AP_OWN ) == 0 ) {
        myAccessLev = ACCESS_OWN;
    }
    else if ( strcmp( _access_level, ACCESS_INHERIT ) == 0 ) {
        inheritFlag = 1;
    }
    else if ( strcmp( _access_level, ACCESS_NO_INHERIT ) == 0 ) {
        inheritFlag = 2;
    }
    else {
        char errMsg[105];
        snprintf( errMsg, 100, "access level '%s' is invalid",
                  _access_level );
        addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
        return ERROR( CAT_INVALID_ARGUMENT, errMsg );
    }

    if ( !icss_status ) {
        return ERROR( CATALOG_NOT_CONNECTED, "catalog not connected" );
    }

    int status1;
    if ( adminMode ) {
        /* See if the input path is a collection
           and, if so, get the collectionID */
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModAccessControl SQL 14" );
        }
        {
            rodsLong_t iVal = 0;
            status1 = hs_get_int_coll_id_by_name(svc, icss, (void *) _path_name,
                          &iVal );
            if ( status1 == CAT_NO_ROWS_FOUND ) {
                status1 = CAT_UNKNOWN_COLLECTION;
            }
            else if ( status1 == 0 ) {
                status1 = iVal;
            }
        }
    }
    else {
        /* See if the input path is a collection and the user owns it,
           and, if so, get the collectionID */
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModAccessControl SQL 1 " );
        }
        status1 = cmlCheckDir( _path_name,
                               _ctx.comm()->clientUser.userName,
                               _ctx.comm()->clientUser.rodsZone,
                               ACCESS_OWN, svc,
                               icss );
    }
    char collIdStr[MAX_NAME_LEN];
    if ( status1 >= 0 ) {
        snprintf( collIdStr, MAX_NAME_LEN, "%d", status1 );
    }

    if ( status1 < 0 && inheritFlag != 0 ) {
        char errMsg[105];
        snprintf( errMsg, 100, "either the collection does not exist or you do not have sufficient access" );
        addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
        return ERROR( CAT_NO_ACCESS_PERMISSION, errMsg );
    }

    rodsLong_t objId = 0;

    /* Not a collection (with access for non-Admin) */
    if ( status1 < 0 ) {
        char logicalEndName[MAX_NAME_LEN];
        char logicalParentDirName[MAX_NAME_LEN];
        int status2 = splitPathByKey( _path_name,
                                      logicalParentDirName, MAX_NAME_LEN, logicalEndName, MAX_NAME_LEN, '/' );
        if ( strlen( logicalParentDirName ) == 0 ) {
            snprintf( logicalParentDirName, sizeof( logicalParentDirName ), "%s", PATH_SEPARATOR );
            snprintf( logicalEndName, sizeof( logicalEndName ), "%s", _path_name + 1 );
        }
        if ( adminMode ) {
            if ( logSQL != 0 ) {
                rodsLog( LOG_SQL, "chlModAccessControl SQL 15" );
            }
            {
                rodsLong_t iVal = 0;
                status2 = hs_get_int_data_id_by_name(svc, icss, logicalParentDirName,logicalEndName,
                              &iVal  );
                if ( status2 == CAT_NO_ROWS_FOUND ) {
                    status2 = CAT_UNKNOWN_FILE;
                }
                if ( status2 == 0 ) {
                    status2 = iVal;
                }
            }
        }
        else {
            /* Not a collection with access, so see if the input path dataObj
               exists and the user owns it, and, if so, get the objectID */
            if ( logSQL != 0 ) {
                rodsLog( LOG_SQL, "chlModAccessControl SQL 2" );
            }
            status2 = cmlCheckDataObjOnly( logicalParentDirName, logicalEndName,
                                           _ctx.comm()->clientUser.userName,
                                           _ctx.comm()->clientUser.rodsZone,
                                           ACCESS_OWN, svc, icss );
        }
        if ( status2 > 0 ) {
            objId = status2;
        }
        /* If both failed, it doesn't exist or there's no permission */
        else if ( status2 < 0 ) {
            char errMsg[205];

            if ( status1 == CAT_UNKNOWN_COLLECTION && status2 == CAT_UNKNOWN_FILE ) {
                snprintf( errMsg, 200,
                          "Input path is not a collection and not a dataObj: %s",
                          _path_name );
                addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
                return ERROR( CAT_INVALID_ARGUMENT, "unknown collection or file" );
            }
            if ( status1 != CAT_UNKNOWN_COLLECTION ) {
                if ( logSQL != 0 ) {
                    rodsLog( LOG_SQL, "chlModAccessControl SQL 12" );
                }
                int status = cmlCheckDirOwn( _path_name,
                                             _ctx.comm()->clientUser.userName,
                                             _ctx.comm()->clientUser.rodsZone, svc,
                                             icss );
                if ( status < 0 ) {
                    return ERROR( status1, "cmlCheckDirOwn failed" );
                }
                snprintf( collIdStr, MAX_NAME_LEN, "%d", status );
            }
            else {
                if ( status2 == CAT_NO_ACCESS_PERMISSION ) {
                    /* See if this user is the owner (with no access, but still
                       allowed to ichmod) */
                    if ( logSQL != 0 ) {
                        rodsLog( LOG_SQL, "chlModAccessControl SQL 13" );
                    }
                    int status = cmlCheckDataObjOwn( logicalParentDirName, logicalEndName,
                                                     _ctx.comm()->clientUser.userName,
                                                     _ctx.comm()->clientUser.rodsZone, svc,
                                                     icss );
                    if ( status < 0 ) {
                        _rollback( "chlModAccessControl" );
                        return ERROR( status2, "cmlCheckDataObjOwn failed" );
                    }
                    objId = status;
                }
                else {
                    return ERROR( status2, "cmlCheckDataObjOnly failed" );
                }
            }
        }
    }

    /* Doing inheritance */
    if ( inheritFlag != 0 ) {
        int status = _modInheritance( inheritFlag, _recursive_flag, collIdStr, _path_name );
        return ERROR( status, "_modInheritance failed" );
    }

    /* Check that the receiving user exists and if so get the userId */
    std::string zone;
    ret = getLocalZone( _ctx.prop_map(), icss, zone );
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    const char *myZone = ( _zone && strlen( _zone ) != 0 ) ? _zone : zone.c_str();

    rodsLong_t userId = 0;
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlModAccessControl SQL 3" );
    }
    {
        int status = hs_get_int_user_id(svc, icss, (void *) myZone,(void *) _user_name,
                         &userId  );
        if ( status != 0 ) {
            if ( status == CAT_NO_ROWS_FOUND ) {
                return ERROR( CAT_INVALID_USER, "invalid user" );
            }
            return ERROR( status, "select user_id failure" );
        }
    }

    char userIdStr[MAX_NAME_LEN];
    snprintf( userIdStr, sizeof( userIdStr ), "%lld", userId );

    char objIdStr[MAX_NAME_LEN];
    snprintf( objIdStr, sizeof( objIdStr ), "%lld", objId );

    rodsLog( LOG_NOTICE, "recursiveFlag %d", _recursive_flag );

    /* non-Recursive mode */
    if ( _recursive_flag == 0 ) {

        /* doing a dataObj */
        if ( objId ) {
            if ( logSQL != 0 ) {
                rodsLog( LOG_SQL, "chlModAccessControl SQL 4" );
            }
            int status = hs_delete_access_by_obj_id_and_user(svc, icss, objIdStr,userIdStr
                              );
            if ( status != 0 && status != CAT_SUCCESS_BUT_WITH_NO_INFO ) {
                return ERROR( status, "delete failure" );
            }
            if ( rmFlag == 0 ) { /* if not just removing: */
                char myTime[50];
                getNowStr( myTime );
                if ( logSQL != 0 ) {
                    rodsLog( LOG_SQL, "chlModAccessControl SQL 5" );
                }
                int status = hs_create_access_by_access_type_token(svc, icss, objIdStr, userIdStr, myAccessLev, myTime, myTime
                                  );
                if ( status != 0 ) {
                    _rollback( "chlModAccessControl" );
                    return ERROR( status, "insert failure" );
                }
            }

            status =  hs_commit(svc,  icss );
            return ERROR( status, "commit failiure" );
        }

        /* doing a collection, non-recursive */
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModAccessControl SQL 6" );
        }
        int status =  hs_delete_access_by_obj_id_and_user(svc, icss, collIdStr,userIdStr
                           );
        if ( status != 0 && status != CAT_SUCCESS_BUT_WITH_NO_INFO ) {
            _rollback( "chlModAccessControl" );
            return ERROR( status, "delete failure" );
        }
        if ( rmFlag ) { /* just removing */
            status =  hs_commit(svc,  icss );
            return ERROR( status, "commit failure" );
        }

        char myTime[50];
        getNowStr( myTime );
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlModAccessControl SQL 7" );
        }
        status =  hs_create_access_by_access_type_token(svc, icss, collIdStr, userIdStr, myAccessLev, myTime, myTime
                       );

        if ( status != 0 ) {
            _rollback( "chlModAccessControl" );
            return ERROR( status, "insert failure" );
        }
        status =  hs_commit(svc,  icss );
        return ERROR( status, "commit failure" );
    }


    /* Recursive */
    if ( objId ) {
        char errMsg[205];

        snprintf( errMsg, 200,
                  "Input path is not a collection and recursion was requested: %s",
                  _path_name );
        addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
        return ERROR( CAT_INVALID_ARGUMENT, errMsg );
    }


    std::string pathStart = makeEscapedPath( _path_name ) + "/%";
    int status;



    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlModAccessControl SQL 8" );
    }
    status = hs_delete_data_access_by_user_recursive(svc, icss, userIdStr, (void *) _path_name);

    if ( status != 0 && status != CAT_SUCCESS_BUT_WITH_NO_INFO ) {
        _rollback( "chlModAccessControl" );
        return ERROR( status, "delete failure" );
    }

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlModAccessControl SQL 9" );
    }
    status = hs_delete_coll_access_by_user_recursive(svc, icss, userIdStr, (void *) _path_name);
    if ( status != 0 && status != CAT_SUCCESS_BUT_WITH_NO_INFO ) {
        _rollback( "chlModAccessControl" );
        return ERROR( status, "delete failure" );
    }
    if ( rmFlag ) { /* just removing */

        status =  hs_commit(svc,  icss );
        return ERROR( status, "commit failure" );
    }

    char myTime[50];
    getNowStr( myTime );
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlModAccessControl SQL 10" );
    }
    status = hs_create_data_access_recursive(svc, icss, userIdStr, (void *) _path_name, myAccessLev, myTime, myTime);
    if ( status == CAT_SUCCESS_BUT_WITH_NO_INFO ) {
        status = 0;    /* no files, OK */
    }
    if ( status != 0 ) {
        _rollback( "chlModAccessControl" );
        return ERROR( status, "insert failure" );
    }


    /* Now set the collections */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlModAccessControl SQL 11" );
    }
    status =  hs_create_coll_access_recursive(svc, icss, userIdStr, (void *) _path_name, myAccessLev, myTime, myTime );
    if ( status != 0 ) {
        _rollback( "chlModAccessControl" );
        return ERROR( status, "insert failure" );
    }

    status =  hs_commit(svc,  icss );
    if ( status < 0 ) {
        return ERROR( status, "commit failed" );
    }

    return CODE( status );

} // db_mod_access_control_op

irods::error db_rename_object_op(
    irods::plugin_context& _ctx,
    rodsLong_t             _obj_id,
    const char*            _new_name ) {
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    int status;
    rodsLong_t collId;
    rodsLong_t otherDataId;
    rodsLong_t otherCollId;
    char myTime[50];

    char parentCollName[MAX_NAME_LEN] = "";
    char collName[MAX_NAME_LEN] = "";
    char *cVal[3];
    int iVal[3];
    int pLen, cLen, len;
    int isRootDir = 0;
    char objIdString[MAX_NAME_LEN];
    char collIdString[MAX_NAME_LEN];
    char collNameTmp[MAX_NAME_LEN];

    char pLenStr[MAX_NAME_LEN];
    char cLenStr[MAX_NAME_LEN];
    char collNameSlash[MAX_NAME_LEN];
    char collNameSlashLen[20];
    char slashNewName[MAX_NAME_LEN];

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRenameObject" );
    }

    if ( strstr( _new_name, PATH_SEPARATOR ) ) {
        return ERROR( CAT_INVALID_ARGUMENT, "new name invalid" );
    }

    /* See if it's a dataObj and if so get the coll_id
       check the access permission at the same time */
    collId = 0;

    snprintf( objIdString, MAX_NAME_LEN, "%lld", _obj_id );
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRenameObject SQL 1 " );
    }

    {
        status = hs_get_int_coll_id_if_own_access_by_user_zone_and_name(svc, icss,
_ctx.comm()->clientUser.rodsZone,
_ctx.comm()->clientUser.userName,
objIdString,
                     &collId);
    }
    rodsLog( LOG_NOTICE, "status %d", status );

    if ( status == 0 ) { /* it is a dataObj and user has access to it */

        /* check that no other dataObj exists with this name in this collection*/
        snprintf( collIdString, MAX_NAME_LEN, "%lld", collId );
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlRenameObject SQL 2" );
        }
        {
            status = hs_get_int_data_id(svc, icss, collIdString, (void *) _new_name,
                         &otherDataId);
        }
        if ( status != CAT_NO_ROWS_FOUND ) {
            return ERROR( CAT_NAME_EXISTS_AS_DATAOBJ, "select data_id failed" );
        }

        /* check that no subcoll exists in this collection,
           with the _new_name */
        snprintf( collNameTmp, MAX_NAME_LEN, "/%s", _new_name );
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlRenameObject SQL 3" );
        }
        {
            status = hs_get_int_sub_coll_id_by_name(svc, icss, collIdString, (void *) _new_name,
                         &otherCollId);
        }
        if ( status != CAT_NO_ROWS_FOUND ) {
            return ERROR( CAT_NAME_EXISTS_AS_COLLECTION, "select coll_id failed" );
        }

        /* update the tables */
        getNowStr( myTime );
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlRenameObject SQL 4" );
        }
        status =  hs_create_data_name(svc, icss,objIdString,(void *) _new_name
                       );
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlRenameObject cmlExecuteNoAnswerSql update1 failure %d",
                     status );
            _rollback( "chlRenameObject" );
            return ERROR( status, "cmlExecuteNoAnswerSql update1 failure" );
        }

        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlRenameObject SQL 5" );
        }
        status =  hs_create_coll_modify_ts(svc, icss, collIdString, myTime
                       );
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlRenameObject cmlExecuteNoAnswerSql update2 failure %d",
                     status );
            _rollback( "chlRenameObject" );
            return ERROR( status, "cmlExecuteNoAnswerSql update2 failure" );
        }

        return CODE( status );
    }

    /* See if it's a collection, and get the parentCollName and
       collName, and check permission at the same time */

    cVal[1] = parentCollName;
    iVal[1] = MAX_NAME_LEN;
    cVal[0] = collName;
    iVal[0] = MAX_NAME_LEN;

    snprintf( objIdString, MAX_NAME_LEN, "%lld", _obj_id );
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRenameObject SQL 6" );
    }

    {
        status = hs_get_some2_coll_if_own_access_by_user_zone_and_name(svc,  icss, _ctx.comm()->clientUser.rodsZone,_ctx.comm()->clientUser.userName,objIdString,
                     cVal, iVal, 2 );
        if (status > 0) status = 0;
    }
    if ( status == 0 ) {
        /* it is a collection and user has access to it */

        /* check that no other dataObj exists with this name in this collection*/
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlRenameObject SQL 7" );
        }
        {
            status = hs_get_int_data_id_by_name(svc, icss, parentCollName, (void *) _new_name,
                         &otherDataId  );
        }
        if ( status != CAT_NO_ROWS_FOUND ) {
            return ERROR( CAT_NAME_EXISTS_AS_DATAOBJ, "select data_id failed" );
        }

        /* check that no subcoll exists in the parent collection,
           with the _new_name */
        snprintf( collNameTmp, MAX_NAME_LEN, "%s/%s", parentCollName, _new_name );
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlRenameObject SQL 8" );
        }
        {
            status = hs_get_int_coll_id_by_name(svc, icss, collNameTmp,
                         &otherCollId);
        }
        if ( status != CAT_NO_ROWS_FOUND ) {
            return ERROR( CAT_NAME_EXISTS_AS_COLLECTION, "select coll_id failed" );
        }

        /* update the table */
        pLen = strlen( parentCollName );
        cLen = strlen( collName );
        if ( pLen <= 0 || cLen <= 0 ) {
            return ERROR( CAT_INVALID_ARGUMENT, "coll name or parent is invalid" );
        }  /* invalid
                                                                   argument is not really right, but something is really wrong */

        if ( pLen == 1 ) {
            if ( strncmp( parentCollName, PATH_SEPARATOR, 20 ) == 0 ) { /* just to be sure */
                isRootDir = 1; /* need to treat a little special below */
            }
        }

        /* set any collection names that are under this collection to
           the new name, putting the string together from the the old upper
           part, _new_name string, and then (if any for each row) the
           tailing part of the name.
           (In the sql substr function, the index for sql is 1 origin.) */
        snprintf( pLenStr, MAX_NAME_LEN, "%d", pLen ); /* formerly +1 but without is
                                                       correct, makes a difference in Oracle, and works
                                                       in postgres too. */
        snprintf( cLenStr, MAX_NAME_LEN, "%d", cLen + 1 );
        snprintf( collNameSlash, MAX_NAME_LEN, "%s/", collName );
        len = strlen( collNameSlash );
        snprintf( collNameSlashLen, 10, "%d", len );
        snprintf( slashNewName, MAX_NAME_LEN, "%s/%s", parentCollName, _new_name );
        if ( isRootDir ) {
            snprintf( slashNewName, MAX_NAME_LEN, "%s", _new_name );
        }
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlRenameObject SQL 9" );
        }
        status =  hs_create_coll_name_recursive(svc, icss, collName, slashNewName);
        if ( status != 0 && status != CAT_SUCCESS_BUT_WITH_NO_INFO ) {
            rodsLog( LOG_NOTICE,
                     "chlRenameObject cmlExecuteNoAnswerSql update failure %d",
                     status );
            _rollback( "chlRenameObject" );
            return ERROR( status, "cmlExecuteNoAnswerSql update failure" );
        }

        /* like above, but for the parent_coll_name's */
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlRenameObject SQL 10" );
        }
        status =  hs_create_coll_parent_coll_name_recursive(svc, icss, collName, slashNewName);
        if ( status != 0 && status != CAT_SUCCESS_BUT_WITH_NO_INFO ) {
            rodsLog( LOG_NOTICE,
                     "chlRenameObject cmlExecuteNoAnswerSql update failure %d",
                     status );
            _rollback( "chlRenameObject" );
            return ERROR( status, "cmlExecuteNoAnswerSql update failure" );
        }

        /* And now, update the row for this collection */
        getNowStr( myTime );
        snprintf( collNameTmp, MAX_NAME_LEN, "%s/%s", parentCollName, _new_name );
        if ( isRootDir ) {
            snprintf( collNameTmp, MAX_NAME_LEN, "%s%s", parentCollName, _new_name );
        }
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlRenameObject SQL 11" );
        }
        status =  hs_create_coll_name_ts(svc, icss,  objIdString, collNameTmp, myTime
                       );
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlRenameObject cmlExecuteNoAnswerSql update failure %d",
                     status );
            _rollback( "chlRenameObject" );
            return ERROR( status, "cmlExecuteNoAnswerSql update failure" );
        }

        return CODE( status );

    }


    /* Both collection and dataObj failed, go thru the sql in smaller
       steps to return a specific error */

    snprintf( objIdString, MAX_NAME_LEN, "%lld", _obj_id );
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRenameObject SQL 12" );
    }
    {
        status = hs_get_int_coll_id_by_data(svc, icss, objIdString,
                     &otherDataId );
    }
    if ( status == 0 ) {
        /* it IS a data obj, must be permission error */
        return ERROR( CAT_NO_ACCESS_PERMISSION, "select coll_id failed" );
    }

    snprintf( collIdString, MAX_NAME_LEN, "%lld", _obj_id );
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRenameObject SQL 12" );
    }
    {
        status = hs_get_int_coll_id_by_coll(svc, icss, collIdString,
                     &otherDataId );
    }
    if ( status == 0 ) {
        /* it IS a collection, must be permission error */
        return ERROR( CAT_NO_ACCESS_PERMISSION, "select coll_id failed" );
    }

    return ERROR( CAT_NOT_A_DATAOBJ_AND_NOT_A_COLLECTION, "not a collection" );

} // db_rename_object_op

irods::error db_move_object_op(
    irods::plugin_context& _ctx,
    rodsLong_t             _obj_id,
    rodsLong_t             _target_coll_id ) {
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    int status = 0;
    rodsLong_t collId;
    rodsLong_t otherDataId;
    rodsLong_t otherCollId;
    char myTime[50];

    char dataObjName[MAX_NAME_LEN] = "";
    char *cVal[3];
    int iVal[3];

    char parentCollName[MAX_NAME_LEN] = "";
    char oldCollName[MAX_NAME_LEN] = "";
    char endCollName[MAX_NAME_LEN] = "";  /* for example: d1 portion of
                                           /tempZone/home/d1  */

    char targetCollName[MAX_NAME_LEN] = "";
    char parentTargetCollName[MAX_NAME_LEN] = "";
    char newCollName[MAX_NAME_LEN] = "";
    int pLen, ocLen;
    int i, OK, len;
    char *cp;
    char objIdString[MAX_NAME_LEN];
    char collIdString[MAX_NAME_LEN];
    char nameTmp[MAX_NAME_LEN];
    char ocLenStr[MAX_NAME_LEN];
    char collNameSlash[MAX_NAME_LEN];
    char collNameSlashLen[20];

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlMoveObject" );
    }

    /* check that the target collection exists and user has write
       permission, and get the names while at it */
    cVal[0] = targetCollName;
    iVal[0] = MAX_NAME_LEN;
    cVal[1] = parentTargetCollName;
    iVal[1] = MAX_NAME_LEN;
    snprintf( objIdString, MAX_NAME_LEN, "%lld", _target_coll_id );
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlMoveObject SQL 1 " );
    }
    {
      printf("******************************db_move_object_op 0, %d, %s, %s, %s\n", status, _ctx.comm()->clientUser.rodsZone,_ctx.comm()->clientUser.userName, objIdString);
        status = hs_get_some2_coll_if_modify_object_access_by_user_zone_and_name(svc, icss, _ctx.comm()->clientUser.rodsZone,_ctx.comm()->clientUser.userName, objIdString,
                     cVal, iVal, 2);
                     printf("******************************db_move_object_op 1, %d\n", status);
                     if (status > 0 ) status = 0;

    }
    snprintf( collIdString, MAX_NAME_LEN, "%lld", _target_coll_id );
    if ( status != 0 ) {
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlMoveObject SQL 2" );
        }
        {
          printf("******************************db_move_object_op 2, %d\n", status);

            status = hs_get_int_coll_id_by_coll_id(svc, icss, collIdString,
                         &collId);
                         printf("******************************db_move_object_op 3, %d\n", status);
        }
        if ( status == 0 ) {
            return  ERROR( CAT_NO_ACCESS_PERMISSION, "permission error" );  /* does exist, must be
                                                   permission error */
        }
        return ERROR( CAT_UNKNOWN_COLLECTION, "target isnt a collection" );      /* isn't a coll */
    }


    /* See if we're moving a dataObj and if so get the data_name;
       and at the same time check the access permission */
    snprintf( objIdString, MAX_NAME_LEN, "%lld", _obj_id );
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlMoveObject SQL 3" );
    }
    {
      printf("******************************db_move_object_op 4, %d\n", status);
        status = hs_get_data_name_by_id_and_access(svc, icss, objIdString, _ctx.comm()->clientUser.rodsZone, _ctx.comm()->clientUser.userName,
                     dataObjName, MAX_NAME_LEN  );
                     printf("******************************db_move_object_op 5, %d\n", status);
    }
    snprintf( collIdString, MAX_NAME_LEN, "%lld", _target_coll_id );
    if ( status == 0 ) { /* it is a dataObj and user has access to it */

        /* check that no other dataObj exists with the ObjName in the
           target collection */
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlMoveObject SQL 4" );
        }
        {
            status = hs_get_int_data_id(svc, icss, collIdString, dataObjName,
                         &otherDataId  );
        }
        if ( status != CAT_NO_ROWS_FOUND ) {
            return ERROR( CAT_NAME_EXISTS_AS_DATAOBJ, "select data_id failed" );
        }

        /* check that no subcoll exists in the target collection, with
           the name of the object */
        /* //not needed, I think   snprintf(collIdString, MAX_NAME_LEN, "%d", collId); */
        snprintf( nameTmp, MAX_NAME_LEN, "/%s", dataObjName );
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlMoveObject SQL 5" );
        }
        {
            status = hs_get_int_sub_coll_id_by_name(svc, icss, collIdString, dataObjName,
                         &otherCollId);
        }
        if ( status != CAT_NO_ROWS_FOUND ) {
            return ERROR( CAT_NAME_EXISTS_AS_COLLECTION, "select coll_id failed" );
        }

        /* update the table */
        getNowStr( myTime );
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlMoveObject SQL 6" );
        }
        status =  hs_create_data_coll_id_ts(svc, icss, objIdString,collIdString, myTime
                       );
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlMoveObject cmlExecuteNoAnswerSql update1 failure %d",
                     status );
            _rollback( "chlMoveObject" );
            return ERROR( status, "cmlExecuteNoAnswerSql update1 failure" );
        }


        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlMoveObject SQL 7" );
        }
        status =  hs_create_coll_modify_ts(svc, icss, collIdString,myTime
                       );
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlMoveObject cmlExecuteNoAnswerSql update2 failure %d",
                     status );
            _rollback( "chlMoveObject" );
            return ERROR( status, "cmlExecuteNoAnswerSql update2 failure" );
        }

        return CODE( status );
    }

    /* See if it's a collection, and get the parentCollName and
       oldCollName, and check permission at the same time */
    cVal[0] = oldCollName;
    iVal[0] = MAX_NAME_LEN;
    cVal[1] = parentCollName;
    iVal[1] = MAX_NAME_LEN;

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlMoveObject SQL 8" );
    }
    {
        status = hs_get_some2_coll_if_own_access_by_user_zone_and_name(svc, icss, _ctx.comm()->clientUser.rodsZone, _ctx.comm()->clientUser.userName, objIdString,
                             cVal, iVal, 2);
                             if (status > 0) status = 0;
    }
    if ( status == 0 ) {
        /* it is a collection and user has access to it */

        pLen = strlen( parentCollName );

        ocLen = strlen( oldCollName );
        if ( pLen <= 0 || ocLen <= 0 ) {
            return ERROR( CAT_INVALID_ARGUMENT, "parent or coll name null" );
        }  /* invalid
                                                                    argument is not really the right error code, but something
                                                                    is really wrong */
        OK = 0;
        for ( i = ocLen; i > 0; i-- ) {
            if ( oldCollName[i] == '/' ) {
                OK = 1;
                snprintf( endCollName, sizeof( endCollName ), "%s", ( char* )&oldCollName[i + 1] );
                break;
            }
        }
        if ( OK == 0 ) {
            return ERROR( CAT_INVALID_ARGUMENT, "OK == 0" );    /* not really, but...*/
        }

        /* check that the user has write access to the source collection */
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlMoveObject SQL 9" );
        }
        status = cmlCheckDir( parentCollName,  _ctx.comm()->clientUser.userName,
                              _ctx.comm()->clientUser.rodsZone,
                              ACCESS_MODIFY_OBJECT, svc, icss );
        if ( status < 0 ) {
            return ERROR( status, "cmlCheckDir failed" );
        }

        /* check that no other dataObj exists with the ObjName in the
           target collection */
        snprintf( collIdString, MAX_NAME_LEN, "%lld", _target_coll_id );
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlMoveObject SQL 10" );
        }
        {
            status = hs_get_int_data_id(svc, icss, collIdString,endCollName, &otherDataId);
        }
        if ( status != CAT_NO_ROWS_FOUND ) {
            return ERROR( CAT_NAME_EXISTS_AS_DATAOBJ, "select data_id failed" );
        }

        /* check that no subcoll exists in the target collection, with
           the name of the object */
        snprintf( newCollName, sizeof( newCollName ), "%s", targetCollName );
        strncat( newCollName, PATH_SEPARATOR, strlen(PATH_SEPARATOR) );
        strncat( newCollName, endCollName, strlen(endCollName) );

        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlMoveObject SQL 11" );
        }
        {
            status = hs_get_int_coll_id_by_name(svc, icss, newCollName, &otherCollId);
        }
        if ( status != CAT_NO_ROWS_FOUND ) {
            return ERROR( CAT_NAME_EXISTS_AS_COLLECTION, "select coll_id failed" );
        }


        /* Check that we're not moving the coll down into it's own
           subtree (which would create a recursive loop) */
        cp = strstr( targetCollName, oldCollName );
        if ( cp == targetCollName &&
                ( targetCollName[strlen( oldCollName )] == '/' ||
                  targetCollName[strlen( oldCollName )] == '\0' ) ) {
            return ERROR( CAT_RECURSIVE_MOVE, "moving coll into own subtree" );
        }


        /* Update the table */

        /* First, set the collection name and parent collection to the
        new strings, and update the modify-time */
        getNowStr( myTime );
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlMoveObject SQL 12" );
        }
        status =  hs_create_coll_name(svc, icss, objIdString, newCollName, targetCollName, myTime
                       );
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlMoveObject cmlExecuteNoAnswerSql update failure %d",
                     status );
            _rollback( "chlMoveObject" );
            return ERROR( status, "cmlExecuteNoAnswerSql update failure" );
        }

        /* Now set any collection names that are under this collection to
           the new name, putting the string together from the the new upper
           part, endCollName string, and then (if any for each row) the
           tailing part of the name.
           (In the sql substr function, the index for sql is 1 origin.) */
        snprintf( ocLenStr, MAX_NAME_LEN, "%d", ocLen + 1 );
        snprintf( collNameSlash, MAX_NAME_LEN, "%s/", oldCollName );
        len = strlen( collNameSlash );
        snprintf( collNameSlashLen, 10, "%d", len );
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlMoveObject SQL 13" );
        }
        status =  hs_create_coll_name_recursive(svc, icss, oldCollName, newCollName);
        if ( status == CAT_SUCCESS_BUT_WITH_NO_INFO ) {
            status = 0;
        }
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlMoveObject cmlExecuteNoAnswerSql update failure %d",
                     status );
            _rollback( "chlMoveObject" );
            return ERROR( status, "cmlExecuteNoAnswerSql update failure" );
        }

        return CODE( status );
    }


    /* Both collection and dataObj failed, go thru the sql in smaller
       steps to return a specific error */
    snprintf( objIdString, MAX_NAME_LEN, "%lld", _obj_id );
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlMoveObject SQL 14" );
    }
    {
        status = hs_get_int_coll_id_by_data(svc, icss, objIdString,
                     &otherDataId  );
    }
    if ( status == 0 ) {
        /* it IS a data obj, must be permission error */
        return ERROR( CAT_NO_ACCESS_PERMISSION, "select coll_id failed" );
    }

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlMoveObject SQL 15" );
    }
    {
        status = hs_get_int_coll_id_by_coll(svc, icss, objIdString,
                     &otherDataId );
    }
    if ( status == 0 ) {
        /* it IS a collection, must be permission error */
        return  ERROR( CAT_NO_ACCESS_PERMISSION, "select coll_id failed" );
    }

    return ERROR( CAT_NOT_A_DATAOBJ_AND_NOT_A_COLLECTION, "invalid object or collection" );

} // db_move_object_op

irods::error db_reg_token_op(
    irods::plugin_context& _ctx,
    const char*            _name_space,
    const char*            _name,
    const char*            _value,
    const char*            _value2,
    const char*            _value3,
    const char*            _comment ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    int status;
    rodsLong_t objId;
    const char *myValue1;
    const char *myValue2;
    const char *myValue3;
    const char *myComment;
    char myTime[50];
    rodsLong_t seqNum;
    char errMsg[205];
    char seqNumStr[MAX_NAME_LEN];

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegToken" );
    }

    if ( _name_space == NULL || strlen( _name_space ) == 0 ) {
        return ERROR( CAT_INVALID_ARGUMENT, "namespace null or 0 len" );
    }
    if ( _name == NULL || strlen( _name ) == 0 ) {
        return ERROR( CAT_INVALID_ARGUMENT, "name null or 0 len" );
    }

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegToken SQL 1 " );
    }
    {
        status = hs_get_int_token_id(svc, icss, (void *) "token_namespace", (void *) _name_space,
                     &objId);
    }
    if ( status != 0 ) {
        snprintf( errMsg, 200,
                  "Token namespace '%s' does not exist",
                  _name_space );
        addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
        return  ERROR( CAT_INVALID_ARGUMENT, "namespace does not exist" );
    }

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegToken SQL 2" );
    }
    {
        status = hs_get_int_token_id(svc, icss, (void *) _name_space, (void *) _name,
                     &objId);
    }
    if ( status == 0 ) {
        snprintf( errMsg, 200,
                  "Token '%s' already exists in namespace '%s'",
                  _name, _name_space );
        addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
        return ERROR( CAT_INVALID_ARGUMENT, "token is already in namespace" );
    }

    myValue1 = _value;
    if ( myValue1 == NULL ) {
        myValue1 = "";
    }
    myValue2 = _value2;
    if ( myValue2 == NULL ) {
        myValue2 = "";
    }
    myValue3 = _value3;
    if ( myValue3 == NULL ) {
        myValue3 = "";
    }
    myComment = _comment;
    if ( myComment == NULL ) {
        myComment = "";
    }

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegToken SQL 3" );
    }
    status = hs_get_int_next_id(svc,  icss, &seqNum );
    if ( status < 0 ) {
        rodsLog( LOG_NOTICE, "chlRegToken cmlGetNextSeqVal failure %d",
                 status );
        return ERROR( status, "chlRegToken cmlGetNextSeqVal failure" );
    }

    getNowStr( myTime );
    snprintf( seqNumStr, sizeof seqNumStr, "%lld", seqNum );
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegToken SQL 4" );
    }
    status =  hs_create_token(svc, icss, seqNumStr, (void *) _name_space, (void *) _name, (void *) myValue1, (void *) myValue2, (void *) myValue3, (void *) myComment, myTime, myTime
                   );
    if ( status != 0 ) {
        _rollback( "chlRegToken" );
        return ERROR( status, "insert failure" );
    }

    status =  hs_commit(svc,  icss );
    if ( status < 0 ) {
        return ERROR( status, "commit error" );
    }
    else {
        return CODE( status );
    }

} // db_reg_token_op

irods::error db_del_token_op(
    irods::plugin_context& _ctx,
    const char*            _name_space,
    const char*            _name ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );


    int status;
    rodsLong_t objId;
    char errMsg[205];
    // char objIdStr[60];

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlDelToken" );
    }

    if ( _name_space == NULL || strlen( _name_space ) == 0 ) {
        return ERROR( CAT_INVALID_ARGUMENT, "namespace is null or 0 len" );
    }
    if ( _name == NULL || strlen( _name ) == 0 ) {
        return ERROR( CAT_INVALID_ARGUMENT, "name is null or 0 len" );
    }

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlDelToken SQL 1 " );
    }
    {
        status = hs_get_int_token_id(svc, icss, (void *) _name_space, (void *) _name,
                     &objId);
    }
    if ( status != 0 ) {
        snprintf( errMsg, 200,
                  "Token '%s' does not exist in namespace '%s'",
                  _name, _name_space );
        addRErrorMsg( &_ctx.comm()->rError, 0, errMsg );
        return  ERROR( CAT_INVALID_ARGUMENT, "token is not in namespace" );
    }

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlDelToken SQL 2" );
    }
    status =  hs_delete_token_by_namespace_and_name(svc, icss, (void *) _name_space, (void *) _name
                   );
    if ( status != 0 ) {
        _rollback( "chlDelToken" );
        return ERROR( status, "delete failure" );
    }

    status =  hs_commit(svc,  icss );
    if ( status < 0 ) {
        return ERROR( status, "commit failed" );
    }
    else {
        return SUCCESS();
    }

} // db_del_token_op

irods::error db_reg_server_load_op(
    irods::plugin_context& _ctx,
    const char*            _host_name,
    const char*            _resc_name,
    const char*            _cpu_used,
    const char*            _mem_used,
    const char*            _swap_used,
    const char*            _run_q_load,
    const char*            _disk_space,
    const char*            _net_input,
    const char*            _net_output ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    char myTime[50];
    int status;
    // int i;

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegServerLoad" );
    }

    if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
    }

    if ( !icss_status ) {
        return ERROR( CATALOG_NOT_CONNECTED, "catalog not connected" );
    }

    getNowStr( myTime );

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegServerLoad SQL 1" );
    }
    status =  hs_create_server_load(svc, icss,(void *) _host_name,(void *) _resc_name,(void *) _cpu_used,(void *) _mem_used,(void *) _swap_used,(void *) _run_q_load,(void *) _disk_space,(void *) _net_input,(void *) _net_output,myTime
                   );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlRegServerLoad cmlExecuteNoAnswerSql failure %d", status );
        _rollback( "chlRegServerLoad" );
        return ERROR( status, "cmlExecuteNoAnswerSql failure" );
    }

    status =  hs_commit(svc,  icss );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlRegServerLoad cmlExecuteNoAnswerSql commit failure %d",
                 status );
        return ERROR( status, "commit failure" );
    }

    return SUCCESS();

} // db_reg_server_load_op

irods::error db_purge_server_load_op(
    irods::plugin_context& _ctx,
    const char*            _seconds_ago ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );

    // =-=-=-=-=-=-=-
    // delete from R_LOAD_SERVER where (%i -exe_time) > %i
    int status;
    char nowStr[50];
    static char thenStr[50];
    time_t nowTime;
    time_t thenTime;
    time_t secondsAgoTime;

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlPurgeServerLoad" );
    }

    if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
    }

    getNowStr( nowStr );
    nowTime = atoll( nowStr );
    secondsAgoTime = atoll( _seconds_ago );
    thenTime = nowTime - secondsAgoTime;
    snprintf( thenStr, sizeof thenStr, "%011d", ( uint ) thenTime );

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlPurgeServerLoad SQL 1" );
    }

    status =  hs_delete_server_load_by_create_ts(svc, icss, thenStr);
    if ( status != 0 ) {
        _rollback( "chlPurgeServerLoad" );
        return ERROR( status, "delete failed" );
    }

    status =  hs_commit(svc,  icss );
    if ( status < 0 ) {
        return ERROR( status, "commit failed" );
    }
    else {
        return SUCCESS();
    }

} // db_purge_server_load_op

irods::error db_reg_server_load_digest_op(
    irods::plugin_context& _ctx,
    const char*            _resc_name,
    const char*            _load_factor ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    char myTime[50];
    int status;
    // int i;

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegServerLoadDigest" );
    }

    if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
    }

    if ( !icss_status ) {
        return ERROR( CATALOG_NOT_CONNECTED, "catalog not connected" );
    }

    getNowStr( myTime );

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlRegServerLoadDigest SQL 1" );
    }
    status =  hs_create_server_load_digest(svc, icss,
(void *) _resc_name,
(void *) _load_factor,
myTime
                   );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlRegServerLoadDigest cmlExecuteNoAnswerSql failure %d", status );
        _rollback( "chlRegServerLoadDigest" );
        return ERROR( status, "cmlExecuteNoAnswerSql failure" );
    }

    status =  hs_commit(svc,  icss );
    if ( status != 0 ) {
        rodsLog( LOG_NOTICE,
                 "chlRegServerLoadDigest cmlExecuteNoAnswerSql commit failure %d",
                 status );
        return ERROR( status, "commit failure" );
    }

    return SUCCESS();

} // db_reg_server_load_digest_op

irods::error db_purge_server_load_digest_op(
    irods::plugin_context& _ctx,
    const char*            _seconds_ago ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    // =-=-=-=-=-=-=-
    /* delete from R_SERVER_LOAD_DIGEST where (%i -exe_time) > %i */
    int status;
    char nowStr[50];
    static char thenStr[50];
    time_t nowTime;
    time_t thenTime;
    time_t secondsAgoTime;

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlPurgeServerLoadDigest" );
    }

    if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
    }

    getNowStr( nowStr );
    nowTime = atoll( nowStr );
    secondsAgoTime = atoll( _seconds_ago );
    thenTime = nowTime - secondsAgoTime;
    snprintf( thenStr, sizeof thenStr, "%011d", ( uint ) thenTime );

    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlPurgeServerLoadDigest SQL 1" );
    }

    status =  hs_delete_server_load_digest_by_create_ts(svc, icss,thenStr );
    if ( status != 0 ) {
        _rollback( "chlPurgeServerLoadDigest" );
        return ERROR( status, "delete failed" );
    }

    status =  hs_commit(svc,  icss );
    if ( status < 0 ) {
        return ERROR( status, "commit failed" );
    }
    else {
        return SUCCESS();
    }

} // db_purge_server_load_digest_op

irods::error db_calc_usage_and_quota_op(
    irods::plugin_context& _ctx ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    int status;
    char myTime[50];

    status = 0;
    if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
    }

    rodsLog( LOG_NOTICE,
             "chlCalcUsageAndQuota called" );


    getNowStr( myTime );

    /* Delete the old rows from R_QUOTA_USAGE */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlCalcUsageAndQuota SQL 1" );
    }
    status =  hs_delete_quota_usage_by_modify_ts(svc, icss, myTime);
    if ( status != 0 && status != CAT_SUCCESS_BUT_WITH_NO_INFO ) {
        _rollback( "chlCalcUsageAndQuota" );
        return ERROR( status, "delete failed" );
    }

    /* Add a row to R_QUOTA_USAGE for each user's usage on each resource */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlCalcUsageAndQuota SQL 2" );
    }
    status =  hs_create_quota_usage(svc, icss, myTime
                   );
    if ( status == CAT_SUCCESS_BUT_WITH_NO_INFO ) {
        status = 0;    /* no files, OK */
    }
    if ( status != 0 ) {
        _rollback( "chlCalcUsageAndQuota" );
        return ERROR( status, "insert failed" );
    }

    /* Set the over_quota flags where appropriate */
    status = setOverQuota( _ctx.comm() );
    if ( status != 0 ) {
        _rollback( "chlCalcUsageAndQuota" );
        return ERROR( status, "setOverQuota failed" );
    }

    status =  hs_commit(svc,  icss );
    if ( status < 0 ) {
        return ERROR( status, "commit failed" );
    }
    else {
        return SUCCESS();
    }

} // db_calc_usage_and_quota_op

irods::error db_set_quota_op(
    irods::plugin_context& _ctx,
    const char*            _type,
    const char*            _name,
    const char*            _resc_name,
    const char*            _limit ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    int status;
    rodsLong_t rescId;
    rodsLong_t userId;
    char userZone[NAME_LEN];
    char userName[NAME_LEN];
    char rescIdStr[60];
    char userIdStr[60];
    char myTime[50];
    int itype = 0;

    if ( strncmp( _type, "user", 4 ) == 0 ) {
        itype = 1;
    }
    if ( strncmp( _type, "group", 5 ) == 0 ) {
        itype = 2;
    }
    if ( itype == 0 ) {
        return ERROR( CAT_INVALID_ARGUMENT, _type );
    }

    std::string zone;
    ret = getLocalZone( _ctx.prop_map(), icss, zone );
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    /* Get the resource id; use rescId=0 for 'total' */
    rescId = 0;
    if ( strncmp( _resc_name, "total", 5 ) != 0 ) {
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlSetQuota SQL 1" );
        }
        {
            status = hs_get_int_resc_id(svc, icss, (void *) zone.c_str(),(void *)_resc_name,
                         &rescId);
        }
        if ( status != 0 ) {
            if ( status == CAT_NO_ROWS_FOUND ) {
                return ERROR( CAT_INVALID_RESOURCE, _resc_name );
            }
            _rollback( "chlSetQuota" );
            return ERROR( status, "select resc_id failed" );
        }
    }


    status = validateAndParseUserName( _name, userName, userZone );
    if ( status ) {
        return ERROR( status, "Invalid username format" );
    }
    if ( userZone[0] == '\0' ) {
        snprintf( userZone, sizeof( userZone ), "%s", zone.c_str() );
    }

    if ( itype == 1 ) {
        userId = 0;
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlSetQuota SQL 2" );
        }
        {
            status = hs_get_int_user_id(svc, icss, userZone,userName,
                         &userId);
        }
        if ( status != 0 ) {
            if ( status == CAT_NO_ROWS_FOUND ) {
                return ERROR( CAT_INVALID_USER, userName );
            }
            _rollback( "chlSetQuota" );
            return ERROR( status, "select user_id failed" );
        }
    }
    else {
        userId = 0;
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlSetQuota SQL 3" );
        }
        {
            status = hs_get_int_group_id(svc, icss, userZone, userName,
                         &userId);
        }
        if ( status != 0 ) {
            if ( status == CAT_NO_ROWS_FOUND ) {
                return ERROR( CAT_INVALID_GROUP, "invalid group" );
            }
            _rollback( "chlSetQuota" );
            return ERROR( status, "select failure" );
        }
    }

    snprintf( userIdStr, sizeof userIdStr, "%lld", userId );
    snprintf( rescIdStr, sizeof rescIdStr, "%lld", rescId );

    /* first delete previous one, if any */
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlSetQuota SQL 4" );
    }
    status =  hs_delete_quota_by_user_id_and_resc_id(svc, icss, userIdStr, rescIdStr
                   );
    if ( status != 0 ) {
        rodsLog( LOG_DEBUG,
                 "chlSetQuota cmlExecuteNoAnswerSql delete failure %d",
                 status );
    }
    if ( atol( _limit ) > 0 ) {
        getNowStr( myTime );
        if ( logSQL != 0 ) {
            rodsLog( LOG_SQL, "chlSetQuota SQL 5" );
        }
        status =  hs_create_quota(svc, userIdStr, rescIdStr, (void *) _limit, myTime,
                      icss );
        if ( status != 0 ) {
            rodsLog( LOG_NOTICE,
                     "chlSetQuota cmlExecuteNoAnswerSql insert failure %d",
                     status );
            _rollback( "chlSetQuota" );
            return ERROR( status, "cmlExecuteNoAnswerSql insert failure" );
        }
    }

    /* Reset the over_quota flags based on previous usage info.  The
       usage info may take a while to set, but setting the OverQuota
       should be quick.  */
    status = setOverQuota( _ctx.comm() );
    if ( status != 0 ) {
        _rollback( "chlSetQuota" );
        return ERROR( status, "setOverQuota failed" );
    }

    status =  hs_commit(svc,  icss );
    if ( status < 0 ) {
        return ERROR( status, "commit failure" );
    }
    else {
        return SUCCESS();
    }

} // db_set_quota_op

irods::error db_check_quota_op(
    irods::plugin_context& _ctx,
    const char*            _user_name,
    const char*            _resc_name,
    rodsLong_t*            _user_quota,
    int*                   _quota_status ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    /*
       Check on a user's quota status, returning the most-over or
       nearest-over value.

       A single query is done which gets the four possible types of quotas
       for this user on this resource (and ordered so the first row is the
       result).  The types of quotas are: user per-resource, user global,
       group per-resource, and group global.
    */
    int status;
    // int statementNum;

    *_user_quota = 0;
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlCheckQuota SQL 1" );
    }

char* resultValue[4];
    status = hs_get_quota_by_user_name_and_resc_name(svc, icss, (void *) _user_name, (void *) _resc_name,
 resultValue, 4);

    if ( status == CAT_SUCCESS_BUT_WITH_NO_INFO ) {
        rodsLog( LOG_NOTICE,
                 "chlCheckQuota - CAT_SUCCESS_BUT_WITH_NO_INFO" );
        *_quota_status = QUOTA_UNRESTRICTED;
        return SUCCESS();
    }

    if ( status == CAT_NO_ROWS_FOUND ) {
        rodsLog( LOG_NOTICE,
                 "chlCheckQuota - CAT_NO_ROWS_FOUND" );
        *_quota_status = QUOTA_UNRESTRICTED;
        return SUCCESS();
    }

    if ( status != 0 ) {
        return ERROR( status, "check quota failed" );
    }

    /* For now, log it */
    rodsLog( LOG_NOTICE, "checkQuota: inUser:%s inResc:%s RescId:%s Quota:%s",
             _user_name, _resc_name,
             resultValue[1],  /* resc_id column */
             resultValue[3] ); /* quota_over column */

    *_user_quota = atoll( resultValue[3] );
    if ( atoi( resultValue[1] ) == 0 ) {
        *_quota_status = QUOTA_GLOBAL;
    }
    else {
        *_quota_status = QUOTA_RESOURCE;
    }
	for(int i=0;i<4;i++) {
		free(resultValue[i]);
	}

    return SUCCESS();

} // db_check_quota_op

irods::error db_del_unused_avus_op(
    irods::plugin_context& _ctx ) {
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    /*
       Remove any AVUs that are currently not associated with any object.
       This is done as a separate operation for efficiency.  See
       'iadmin h rum'.
    */
    const int remove_status = removeAVUs();
    int commit_status = 0;

    if ( remove_status == CAT_SUCCESS_BUT_WITH_NO_INFO
            || remove_status == 0 ) {
        commit_status = hs_commit(svc,  icss );
    }
    else {
        return ERROR( remove_status, "removeAVUs failed" );
    }

    if ( commit_status == CAT_SUCCESS_BUT_WITH_NO_INFO
            || commit_status == 0 ) {
        return SUCCESS();
    }
    else {
        return ERROR( commit_status, "commit failed" );
    }

} // db_del_unused_avus_op

irods::error db_ins_rule_table_op(
    irods::plugin_context& _ctx,
    const char*            _base_name,
    const char*            _map_priority_str,
    const char*            _rule_name,
    const char*            _rule_head,
    const char*            _rule_condition,
    const char*            _rule_action,
    const char*            _rule_recovery,
    const char*            _rule_id_str,
    const char*            _my_time ) {

    return SUCCESS();

} // db_ins_rule_table_op

irods::error db_ins_dvm_table_op(
    irods::plugin_context& _ctx,
    const char*            _base_name,
    const char*            _var_name,
    const char*            _action,
    const char*            _var_2_cmap,
    const char*            _my_time ) {

    return SUCCESS();

} // db_ins_dvm_table_op

irods::error db_ins_fnm_table_op(
    irods::plugin_context& _ctx,
    const char*            _base_name,
    const char*            _func_name,
    const char*            _func_2_cmap,
    const char*            _my_time ) {

    return SUCCESS();

} // db_ins_fnm_table_op

irods::error db_ins_msrvc_table_op(
    irods::plugin_context& _ctx,
    const char*            _module_name,
    const char*            _msrvc_name,
    const char*            _msrvc_signature,
    const char*            _msrvc_version,
    const char*            _msrvc_host,
    const char*            _msrvc_location,
    const char*            _msrvc_language,
    const char*            _msrvc_type_name,
    const char*            _msrvc_status,
    const char*            _my_time ) {
    return SUCCESS();

} // db_ins_msrvc_table_op

irods::error db_version_rule_base_op(
    irods::plugin_context& _ctx,
    const char*            _base_name,
    const char*            _my_time ) {
    return SUCCESS();

} // db_version_rule_base_op

irods::error db_version_dvm_base_op(
    irods::plugin_context& _ctx,
    const char*            _base_name,
    const char*            _my_time ) {
    return SUCCESS();

} // db_version_dvm_base_op

irods::error db_version_fnm_base_op(
    irods::plugin_context& _ctx,
    const char*            _base_name,
    const char*            _my_time ) {

    return SUCCESS();

} // db_version_fnm_base_op

irods::error db_add_specific_query_op(
    irods::plugin_context& _ctx,
    const char*            _sql,
    const char*            _alias ) {
    return ERROR( -1, "insert failure" );

} // db_add_specific_query_op

irods::error db_del_specific_query_op(
    irods::plugin_context& _ctx,
    const char*            _sql_or_alias ) {
    return ERROR( -1, "delete failure" );

} // db_del_specific_query_op

#define MINIMUM_COL_SIZE 50
irods::error db_specific_query_op(
    irods::plugin_context& _ctx,
    specificQueryInp_t*    _spec_query_inp,
    genQueryOut_t*         _result ) {
    return ERROR(-1, "unsupported operation");

} // db_specific_query_op

irods::error db_substitute_resource_hierarchies_op(
    irods::plugin_context& _ctx,
    const char*            _old_hier,
    const char*            _new_hier ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    int status = 0;
    char old_hier_partial[MAX_NAME_LEN];
    irods::sql_logger logger( "chlSubstituteResourceHierarchies", logSQL );

    logger.log();

    // =-=-=-=-=-=-=-
    // Sanity and permission checks
    if ( !icss_status ) {
        return ERROR( CATALOG_NOT_CONNECTED, "catalog not connected" );
    }
    if ( !_old_hier || !_new_hier ) {
        return ERROR( SYS_INTERNAL_NULL_INPUT_ERR, "null parameter" );
    }
    if ( _ctx.comm()->clientUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH || _ctx.comm()->proxyUser.authInfo.authFlag < LOCAL_PRIV_USER_AUTH ) {
        return ERROR( CAT_INSUFFICIENT_PRIVILEGE_LEVEL, "insufficient privilege" );
    }

    // =-=-=-=-=-=-=-
    // String to match partial hierarchies
    snprintf( old_hier_partial, MAX_NAME_LEN, "%s%s%%", _old_hier, irods::hierarchy_parser::delimiter().c_str() );

    // =-=-=-=-=-=-=-
    // Update r_data_main
    status = hs_create_data_resc_hier_recursive(svc, icss, (void *) irods::hierarchy_parser::delimiter().c_str(), ( char* )_old_hier,( char* )_new_hier);

    // Nothing was modified
    if ( status == CAT_SUCCESS_BUT_WITH_NO_INFO ) {
        return SUCCESS();
    }

    // =-=-=-=-=-=-=-
    // Roll back if error
    if ( status < 0 ) {
        std::stringstream ss;
        ss << "chlSubstituteResourceHierarchies: cmlExecuteNoAnswerSql update failure " << status;
        irods::log( LOG_NOTICE, ss.str() );
        _rollback( "chlSubstituteResourceHierarchies" );
        return ERROR( status, "update failure" );
    }

    return SUCCESS();

} // db_substitute_resource_hierarchies_op

irods::error db_get_distinct_data_obj_count_on_resource_op(
    irods::plugin_context& _ctx,
    const char*            _resc_name,
    long long*             _count ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check incoming pointers
    if ( !_resc_name ||
            !_count ) {
        return ERROR(
                   SYS_INVALID_INPUT_PARAM,
                   "null input param" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    // =-=-=-=-=-=-=-

	int status = hs_get_int_num_data_by_resc_recursive(svc, icss, (void *) _resc_name, _count);
    if ( status != 0 ) {
        return ERROR( status, "cmlGetFirstRowFromSql failed" );
    }

    return SUCCESS();

} // db_get_distinct_data_obj_count_on_resource_op

irods::error db_get_distinct_data_objs_missing_from_child_given_parent_op(
    irods::plugin_context& _ctx,
    const std::string*     _parent,
    const std::string*     _child,
    int                    _limit,
    dist_child_result_t*   _results ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check incoming pointers
    if ( !_parent    ||
            !_child     ||
            _limit <= 0 ||
            !_results ) {
        return ERROR(
                   SYS_INVALID_INPUT_PARAM,
                   "null or invalid input param" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );

char *resultValue = (char *) malloc(MAX_NAME_LEN * _limit);
        int status = 0;
	    status = hs_get_some_data_by_resc_not_in_child(svc, icss,(void *) _parent, (void *) _child, resultValue, MAX_NAME_LEN, _limit);
        if ( status < 0 ) {
            return ERROR( status, "failed to get a row" );
        }
    for ( int i = 0; i<status; i++ ) {

        _results->push_back( atoi( resultValue+(i * MAX_NAME_LEN) ) );

    } // for i

free(resultValue);

    return SUCCESS();

} // db_get_distinct_data_objs_missing_from_child_given_parent_op

irods::error db_get_repl_list_for_leaf_bundles_op(
    irods::plugin_context&      _ctx,
    rodsLong_t                  _count,
    size_t                      _child_index,
    std::vector<leaf_bundle_t>* _bundles,
    dist_child_result_t*        _results ) {

    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check incoming pointers
    if( _count <= 0      ||
        _bundles->empty() ||
        !_results ) {
        return ERROR(
                   SYS_INVALID_INPUT_PARAM,
                   "null or invalid input param" );
    }

    // capture list of child resc ids
    std::stringstream child_array_stream;
    child_array_stream << "{";
    for( auto id : (*_bundles)[_child_index] ) {
        child_array_stream << id << ",";
    }

    if(child_array_stream.str().empty()) {
        return ERROR(
                  SYS_INVALID_INPUT_PARAM,
                  "leaf arry is empty");
    }

    std::string child_array = child_array_stream.str();
    child_array.pop_back(); // trim last ','
    child_array += "}";

    std::stringstream not_child_stream;
    not_child_stream << "{";
    for( size_t idx = 0; idx < _bundles->size(); ++idx ) {
        if( idx == _child_index ) {
            continue;
        }
        for( auto id : (*_bundles)[idx] ) {
            not_child_stream << id << ",";
        }
    } // for idx

    std::string not_child_array = not_child_stream.str();
    not_child_array.pop_back(); // trim last ','
    not_child_array += "}";

	char **resultValue;
  int len;
        int status = 0;
char *not_ca = (char *) not_child_array.c_str();
char *ca = (char *) child_array.c_str(); 

            status = hs_get_all_data_by_resc_list(svc,
				icss, not_ca, ca, &resultValue, &len
			);

        if ( status < 0 ) {
            return ERROR( status, "failed to get a row" );
        }

    // =-=-=-=-=-=-=-
    // iterate over resulting rows
    for ( int i = 0; i < status; i++ ) {
        _results->push_back( atoi( resultValue[i] ) );
free(resultValue[i]);

    } // for i

	free(resultValue);

    return ERROR(CAT_NO_ROWS_FOUND, ""); // see replication plugin

} // db_get_repl_list_for_leaf_bundles_op

irods::error db_get_hierarchy_for_resc_op(
    irods::plugin_context& _ctx,
    const std::string*     _resc_name,
    const std::string*     _zone_name,
    std::string*           _hierarchy ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check incoming pointers
    if ( !_resc_name    ||
            !_zone_name    ||
            !_hierarchy ) {
        return ERROR(
                   SYS_INVALID_INPUT_PARAM,
                   "null or invalid input param" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );

    char *current_node;
    char parent[MAX_NAME_LEN];
    int status;


    irods::sql_logger logger( "chlGetHierarchyForResc", logSQL );
    logger.log();

    if ( !icss_status ) {
        return ERROR( CATALOG_NOT_CONNECTED, "catalog not connected" );
    }

    ( *_hierarchy ) = ( *_resc_name ); // Initialize hierarchy string with resource

    current_node = ( char * )_resc_name->c_str();
    while ( current_node ) {
        {
            // Ask for parent of current node
            status = hs_get_resc_parent_by_zone_and_name(svc,  icss, (void *) _zone_name->c_str(), current_node,
                                               parent, MAX_NAME_LEN);
        }
        if ( status == CAT_NO_ROWS_FOUND ) { // Resource doesn't exist
            // =-=-=-=-=-=-=-
            // quick check to see if the resource actually exists
            char type_name[ 250 ] = "";
            {
                status = hs_get_resc_type_name_by_zone_and_name(svc, icss,
				                (void *) _zone_name->c_str(),
         								current_node,
                        type_name, 250);
            }
            if ( status < 0 ) {
                return ERROR( CAT_UNKNOWN_RESOURCE, "resource does not exist" );
            }
            else {
                ( *_hierarchy ) = "";
                return SUCCESS();
            }
        }

        if ( status < 0 ) { // Other error
            return ERROR( status, "failed to get string" );
        }

        if ( strlen( parent ) ) {
            ( *_hierarchy ) = parent + irods::hierarchy_parser::delimiter() + ( *_hierarchy );    // Add parent to hierarchy string
            current_node = parent;
        }
        else {
            current_node = NULL;
        }
    }

    return SUCCESS();

} // db_get_hierarchy_for_resc_op

irods::error db_mod_ticket_op(
    irods::plugin_context& _ctx,
    const char*            _op_name,
    const char*            _ticket_string,
    const char*            _arg3,
    const char*            _arg4,
    const char*            _arg5 ) {

    return ERROR( -1, "unsupported operation" );

} // db_mod_ticket_op

irods::error db_get_icss_op(
    irods::plugin_context& _ctx,
    void**    _icss ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check incoming pointers
    if ( !_icss ) {
        return ERROR(
                   SYS_INVALID_INPUT_PARAM,
                   "null or invalid input param" );
    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    if ( logSQL != 0 ) {
        rodsLog( LOG_SQL, "chlGetRcs" );
    }
    if ( icss_status != 1 ) {
        ( *_icss ) = 0;
        return ERROR( icss_status, "catalog not connected" );
    }

    ( *_icss ) = icss;
    return SUCCESS();

} // db_get_icss_op

// =-=-=-=-=-=-=-
// from general_query.cpp ::
int chl_gen_query_impl( void *,void *, genQueryInp_t, genQueryOut_t* );

irods::error db_gen_query_op(
    irods::plugin_context& _ctx,
    genQueryInp_t*         _gen_query_inp,
    genQueryOut_t*         _result ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if ( !_gen_query_inp
       ) {
        return ERROR( CAT_INVALID_ARGUMENT, "null parameter" );

    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    int status = chl_gen_query_impl(
      svc,
                     icss,
                     *_gen_query_inp,
                     _result );
//         if( status < 0 ) {
//             return ERROR( status, "chl_gen_query_impl failed" );
//         } else {
//             return SUCCESS();
//         }
    return CODE( status );

} // db_gen_query_op

// =-=-=-=-=-=-=-
// from general_query.cpp ::
int chl_gen_query_access_control_setup_impl( const char*, const char*, const char*, int, int );

irods::error db_gen_query_access_control_setup_op(
    irods::plugin_context& _ctx,
    const char*            _user,
    const char*            _zone,
    const char*            _host,
    int                    _priv,
    int                    _control_flag ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    //if ( ) {
    //    return ERROR( CAT_INVALID_ARGUMENT, "null parameter" );
    //
    //}

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    int status = chl_gen_query_access_control_setup_impl(
                     _user,
                     _zone,
                     _host,
                     _priv,
                     _control_flag );
    if ( status < 0 ) {
        return ERROR( status, "chl_gen_query_access_control_setup_impl failed" );
    }
    else {
        return CODE( status );
    }

} // db_gen_query_access_control_setup_op

// =-=-=-=-=-=-=-
// from general_query.cpp ::
int chl_gen_query_ticket_setup_impl( const char*, const char* );

irods::error db_gen_query_ticket_setup_op(
    irods::plugin_context& _ctx,
    const char*            _ticket,
    const char*            _client_addr ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if ( !_ticket ||
            !_client_addr ) {
        return ERROR( CAT_INVALID_ARGUMENT, "null parameter" );

    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    int status = chl_gen_query_ticket_setup_impl(
                     _ticket,
                     _client_addr );
    if ( status < 0 ) {
        return ERROR( status, "chl_gen_query_ticket_setup_impl failed" );
    }
    else {
        return SUCCESS();
    }

} // db_gen_query_ticket_setup_op

// =-=-=-=-=-=-=-
// from general_query.cpp ::
int chl_general_update_impl( generalUpdateInp_t );

irods::error db_general_update_op(
    irods::plugin_context& _ctx,
    generalUpdateInp_t*    _update_inp ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if ( !_update_inp ) {
        return ERROR( CAT_INVALID_ARGUMENT, "null parameter" );

    }

    // =-=-=-=-=-=-=-
    // get a postgres object from the context
    /*irods::postgres_object_ptr pg;
    ret = make_db_ptr( _ctx.fco(), pg );
    if ( !ret.ok() ) {
        return PASS( ret );

    }*/

    // =-=-=-=-=-=-=-
    // extract the icss property
//        icatSessionStruct icss;
//        _ctx.prop_map().get< icatSessionStruct >( ICSS_PROP, icss );
    int status = chl_general_update_impl(
                     *_update_inp );
    if ( status < 0 ) {
        return ERROR( status, "chl_general_update_impl( failed" );
    }
    else {
        return SUCCESS();
    }

} // db_general_update_op

// =-=-=-=-=-=-=-
//
irods::error db_start_operation( irods::plugin_property_map& _props ) {

    return SUCCESS();

} // db_start_operation


// =-=-=-=-=-=-=-
// derive a new tcp network plugin from
// the network plugin base class for handling
// tcp communications
class postgres_database_plugin : public irods::database {
    public:
        postgres_database_plugin(
            const std::string& _nm,
            const std::string& _ctx ) :
            irods::database(
                _nm,
                _ctx ) {
            // =-=-=-=-=-=-=-
            // create a property for the icat session
            // which will manage the lifetime of the db
            // connection - use a copy ctor to init
            void *icss = NULL;
            properties_.set< void * >( ICSS_PROP, icss );

            set_start_operation( db_start_operation );

        } // ctor

        ~postgres_database_plugin() {
        }

}; // class postgres_database_plugin

// =-=-=-=-=-=-=-
// factory function to provide instance of the plugin
extern "C"
irods::database* plugin_factory(
    const std::string& _inst_name,
    const std::string& _context ) {
    // =-=-=-=-=-=-=-
    // create a postgres database plugin instance
    postgres_database_plugin* pg = new postgres_database_plugin(
        _inst_name,
        _context );

    // =-=-=-=-=-=-=-
    // fill in the operation table mapping call
    // names to function na,mes
    using namespace irods;
    using namespace std;
    pg->add_operation(
        DATABASE_OP_START,
        function<error(plugin_context&)>(
            db_start_op ) );
    pg->add_operation<const char*>(
        DATABASE_OP_DEBUG,
        function<error(plugin_context&, const char*)>(
            db_debug_op ) );
    pg->add_operation(
        DATABASE_OP_OPEN,
        function<error(plugin_context&)>(
            db_open_op ) );
    pg->add_operation(
        DATABASE_OP_CLOSE,
        function<error(plugin_context&)>(
            db_close_op ) );
    pg->add_operation<std::string*>(
        DATABASE_OP_GET_LOCAL_ZONE,
        function<error(plugin_context&,std::string*)>(
            db_get_local_zone_op ) );
    pg->add_operation<const std::string*, int>(
        DATABASE_OP_UPDATE_RESC_OBJ_COUNT,
        function<error(plugin_context&,const std::string*, int)>(
            db_update_resc_obj_count_op ) );
    pg->add_operation<dataObjInfo_t*,keyValPair_t*>(
        DATABASE_OP_MOD_DATA_OBJ_META,
        function<error(plugin_context&,dataObjInfo_t*,keyValPair_t*)>(
            db_mod_data_obj_meta_op ) );
    pg->add_operation<dataObjInfo_t*>(
        DATABASE_OP_REG_DATA_OBJ,
        function<error(plugin_context&,dataObjInfo_t*)>(
            db_reg_data_obj_op ) );
    pg->add_operation<dataObjInfo_t*,dataObjInfo_t*,keyValPair_t*>(
        DATABASE_OP_REG_REPLICA,
        function<error(plugin_context&,dataObjInfo_t*,dataObjInfo_t*,keyValPair_t*)>(
            db_reg_replica_op ) );
    pg->add_operation<dataObjInfo_t*,keyValPair_t*>(
        DATABASE_OP_UNREG_REPLICA,
        function<error(plugin_context&,dataObjInfo_t*,keyValPair_t*)>(
            db_unreg_replica_op ) );
    pg->add_operation<ruleExecSubmitInp_t*>(
        DATABASE_OP_REG_RULE_EXEC,
        function<error(plugin_context&,ruleExecSubmitInp_t*)>(
            db_reg_rule_exec_op ) );
    pg->add_operation<const char*,keyValPair_t*>(
        DATABASE_OP_MOD_RULE_EXEC,
        function<error(plugin_context&,const char*,keyValPair_t*)>(
            db_mod_rule_exec_op ) );
    pg->add_operation<const char*>(
        DATABASE_OP_DEL_RULE_EXEC,
        function<error(plugin_context&,const char*)>(
            db_del_rule_exec_op ) );
    pg->add_operation<map<string, string>*>(
        DATABASE_OP_ADD_CHILD_RESC,
        function<error(plugin_context&,map<string,string>*)>(
            db_add_child_resc_op ) );
    pg->add_operation<map<string, string>*>(
        DATABASE_OP_REG_RESC,
        function<error(plugin_context&,map<string, string>*)>(
            db_reg_resc_op ) );
    pg->add_operation<map<string,string>*>(
        DATABASE_OP_DEL_CHILD_RESC,
        function<error(plugin_context&,map<string,string>*)>(
            db_del_child_resc_op ) );
    pg->add_operation<const char*,int>(
        DATABASE_OP_DEL_RESC,
        function<error(plugin_context&,const char*,int)>(
            db_del_resc_op ) );
    pg->add_operation(
        DATABASE_OP_ROLLBACK,
        function<error(plugin_context&)>(
            db_rollback_op ) );
    pg->add_operation(
        DATABASE_OP_COMMIT,
        function<error(plugin_context&)>(
            db_commit_op ) );
    pg->add_operation<userInfo_t*>(
        DATABASE_OP_DEL_USER_RE,
        function<error(plugin_context&,userInfo_t*)>(
            db_del_user_re_op ) );
    pg->add_operation<collInfo_t*>(
        DATABASE_OP_REG_COLL_BY_ADMIN,
        function<error(plugin_context&,collInfo_t*)>(
            db_reg_coll_by_admin_op ) );
    pg->add_operation<collInfo_t*>(
        DATABASE_OP_REG_COLL,
        function<error(plugin_context&,collInfo_t*)>(
            db_reg_coll_op ) );
    pg->add_operation<collInfo_t*>(
        DATABASE_OP_MOD_COLL,
        function<error(plugin_context&,collInfo_t*)>(
            db_mod_coll_op ) );
    pg->add_operation<const char*,const char*,const char*,const char*>(
        DATABASE_OP_REG_ZONE,
        function<error(plugin_context&,const char*,const char*,const char*,const char*)>(
            db_reg_zone_op ) );
    pg->add_operation<const char*,const char*,const char*>(
        DATABASE_OP_MOD_ZONE,
        function<error(plugin_context&,const char*,const char*,const char*)>(
            db_mod_zone_op ) );
    pg->add_operation<const char*,const char*>(
        DATABASE_OP_RENAME_COLL,
        function<error(plugin_context&,const char*,const char*)>(
            db_rename_coll_op ) );
    pg->add_operation<const char*,const char*,const char*>(
        DATABASE_OP_MOD_ZONE_COLL_ACL,
        function<error(plugin_context&,const char*,const char*,const char*)>(
            db_mod_zone_coll_acl_op ) );
    pg->add_operation<const char*,const char*>(
        DATABASE_OP_RENAME_LOCAL_ZONE,
        function<error(plugin_context&,const char*,const char*)>(
            db_rename_local_zone_op ) );
    pg->add_operation<const char*>(
        DATABASE_OP_DEL_ZONE,
        function<error(plugin_context&,const char*)>(
            db_del_zone_op ) );
    pg->add_operation<const char*,const char*,const char*,const char*,const char*,int,int*,char*,int>(
        DATABASE_OP_SIMPLE_QUERY,
        function<error(plugin_context&,const char*,const char*,const char*,const char*,const char*,int,int*,char*,int)>(
            db_simple_query_op ) );
    pg->add_operation<collInfo_t*>(
        DATABASE_OP_DEL_COLL_BY_ADMIN,
        function<error(plugin_context&,collInfo_t*)>(
            db_del_coll_by_admin_op ) );
    pg->add_operation<collInfo_t*>(
        DATABASE_OP_DEL_COLL,
        function<error(plugin_context&,collInfo_t*)>(
            db_del_coll_op ) );
    pg->add_operation<const char*,const char*,const char*,const char*,int*,int*>(
        DATABASE_OP_CHECK_AUTH,
        function<error(plugin_context&,const char*,const char*,const char*,const char*,int*,int*)>(
            db_check_auth_op ) );
    pg->add_operation<char*,const char*>(
        DATABASE_OP_MAKE_TEMP_PW,
        function<error(plugin_context&,char*, const char*)>(
            db_make_temp_pw_op ) );
    pg->add_operation<const char*,int,const char*,char**>(
        DATABASE_OP_UPDATE_PAM_PASSWORD,
        function<error(plugin_context&,const char*,int,const char*,char**)>(
            db_update_pam_password_op ) );
    pg->add_operation<const char*,const char*,const char*>(
        DATABASE_OP_MOD_USER,
        function<error(plugin_context&,const char*,const char*,const char*)>(
            db_mod_user_op ) );
    pg->add_operation<int,char*>(
        DATABASE_OP_MAKE_LIMITED_PW,
        function<error(plugin_context&,int,char*)>(
            db_make_limited_pw_op ) );
    pg->add_operation<const char*,const char*,const char*,const char*>(
        DATABASE_OP_MOD_GROUP,
        function<error(plugin_context&,const char*,const char*,const char*,const char*)>(
            db_mod_group_op ) );
    pg->add_operation<const char*,const char*,const char*>(
        DATABASE_OP_MOD_RESC,
        function<error(plugin_context&,const char*,const char*,const char*)>(
            db_mod_resc_op ) );
    pg->add_operation<const char*,const char*,const char*,const char*>(
        DATABASE_OP_MOD_RESC_DATA_PATHS,
        function<error(plugin_context&,const char*,const char*,const char*,const char*)>(
            db_mod_resc_data_paths_op ) );
    pg->add_operation<const char*,int>(
        DATABASE_OP_MOD_RESC_FREESPACE,
        function<error(plugin_context&,const char*,int)>(
            db_mod_resc_freespace_op ) );
    pg->add_operation<userInfo_t*>(
        DATABASE_OP_REG_USER_RE,
        function<error(plugin_context&,userInfo_t*)>(
            db_reg_user_re_op ) );
    pg->add_operation<const char*,const char*,const char*,const char*,const char*>(
        DATABASE_OP_SET_AVU_METADATA,
        function<error(plugin_context&,const char*,const char*,const char*,const char*,const char*)>(
            db_set_avu_metadata_op ) );
    pg->add_operation<int,const char*,const char*,const char*,const char*,const char*>(
        DATABASE_OP_ADD_AVU_METADATA_WILD,
        function<error(plugin_context&,int,const char*,const char*,const char*,const char*,const char*)>(
            db_add_avu_metadata_wild_op ) );
    pg->add_operation<int,const char*,const char*,const char*,const char*,const char*>(
        DATABASE_OP_ADD_AVU_METADATA,
        function<error(plugin_context&,int,const char*,const char*,const char*,const char*,const char*)>(
            db_add_avu_metadata_op ) );
    pg->add_operation<const char*,const char*,const char*,const char*,const char*,const char*,const char*,const char*>(
        DATABASE_OP_MOD_AVU_METADATA,
        function<error(plugin_context&,const char*,const char*,const char*,const char*,const char*,const char*,const char*,const char*)>(
            db_mod_avu_metadata_op ) );
    pg->add_operation<int,const char*,const char*,const char*,const char*,const char*,int>(
        DATABASE_OP_DEL_AVU_METADATA,
        function<error(plugin_context&,int,const char*,const char*,const char*,const char*,const char*,int)>(
            db_del_avu_metadata_op ) );
    pg->add_operation<const char*,const char*,const char*,const char*>(
        DATABASE_OP_COPY_AVU_METADATA,
        function<error(plugin_context&,const char*,const char*,const char*,const char*)>(
            db_copy_avu_metadata_op ) );
    pg->add_operation<int,const char*,const char*,const char*,const char*>(
        DATABASE_OP_MOD_ACCESS_CONTROL_RESC,
        function<error(plugin_context&,int,const char*,const char*,const char*,const char*)>(
            db_mod_access_control_resc_op ) );
    pg->add_operation<int,const char*,const char*,const char*,const char*>(
        DATABASE_OP_MOD_ACCESS_CONTROL,
        function<error(plugin_context&,int,const char*,const char*,const char*,const char*)>(
            db_mod_access_control_op ) );
    pg->add_operation<rodsLong_t,const char*>(
        DATABASE_OP_RENAME_OBJECT,
        function<error(plugin_context&,rodsLong_t,const char*)>(
            db_rename_object_op ) );
    pg->add_operation<rodsLong_t,rodsLong_t>(
        DATABASE_OP_MOVE_OBJECT,
        function<error(plugin_context&,rodsLong_t,rodsLong_t)>(
            db_move_object_op ) );
    pg->add_operation<const char*,const char*,const char*,const char*,const char*,const char*>(
        DATABASE_OP_REG_TOKEN,
        function<error(plugin_context&,const char*,const char*,const char*,const char*,const char*,const char*)>(
            db_reg_token_op ) );
    pg->add_operation<const char*,const char*>(
        DATABASE_OP_DEL_TOKEN,
        function<error(plugin_context&,const char*,const char*)>(
            db_del_token_op ) );
    pg->add_operation<const char*,const char*,const char*,const char*,const char*,const char*,const char*,const char*,const char*>(
        DATABASE_OP_REG_SERVER_LOAD,
        function<error(plugin_context&,const char*,const char*,const char*,const char*,const char*,const char*,const char*,const char*,const char*)>(
            db_reg_server_load_op ) );
    pg->add_operation<const char*>(
        DATABASE_OP_PURGE_SERVER_LOAD,
        function<error(plugin_context&,const char*)>(
            db_purge_server_load_op ) );
    pg->add_operation<const char*,const char*>(
        DATABASE_OP_REG_SERVER_LOAD_DIGEST,
        function<error(plugin_context&,const char*,const char*)>(
            db_reg_server_load_digest_op ) );
    pg->add_operation<const char*>(
        DATABASE_OP_PURGE_SERVER_LOAD_DIGEST,
        function<error(plugin_context&,const char*)>(
            db_purge_server_load_digest_op ) );
    pg->add_operation(
        DATABASE_OP_CALC_USAGE_AND_QUOTA,
        function<error(plugin_context&)>(
            db_calc_usage_and_quota_op ) );
    pg->add_operation<const char*,const char*,const char*,const char*>(
        DATABASE_OP_SET_QUOTA,
        function<error(plugin_context&,const char*,const char*,const char*,const char*)>(
            db_set_quota_op ) );
    pg->add_operation<const char*,const char*,rodsLong_t*,int*>(
        DATABASE_OP_CHECK_QUOTA,
        function<error(plugin_context&,const char*,const char*,rodsLong_t*,int*)>(
            db_check_quota_op ) );
    pg->add_operation(
        DATABASE_OP_DEL_UNUSED_AVUS,
        function<error(plugin_context&)>(
            db_del_unused_avus_op ) );
    pg->add_operation<const char*,const char*,const char*,const char*,const char*,const char*,const char*,const char*,const char*>(
        DATABASE_OP_INS_RULE_TABLE,
        function<error(plugin_context&,const char*,const char*,const char*,const char*,const char*,const char*,const char*,const char*,const char*)>(
            db_ins_rule_table_op ) );
    pg->add_operation<const char*,const char*,const char*,const char*,const char*>(
        DATABASE_OP_INS_DVM_TABLE,
        function<error(plugin_context&,const char*,const char*,const char*,const char*,const char*)>(
            db_ins_dvm_table_op ) );
    pg->add_operation<const char*,const char*,const char*,const char*>(
        DATABASE_OP_INS_FNM_TABLE,
        function<error(plugin_context&,const char*,const char*,const char*,const char*)>(
            db_ins_fnm_table_op ) );
    pg->add_operation<const char*,const char*,const char*,const char*,const char*,const char*,const char*,const char*,const char*,const char*>(
        DATABASE_OP_INS_MSRVC_TABLE,
        function<error(plugin_context&,const char*,const char*,const char*,const char*,const char*,const char*,const char*,const char*,const char*,const char*)>(
            db_ins_msrvc_table_op ) );
    pg->add_operation<const char*,const char*>(
        DATABASE_OP_VERSION_RULE_BASE,
        function<error(plugin_context&,const char*,const char*)>(
            db_version_rule_base_op ) );
    pg->add_operation<const char*,const char*>(
        DATABASE_OP_VERSION_DVM_BASE,
        function<error(plugin_context&,const char*,const char*)>(
            db_version_dvm_base_op ) );
    pg->add_operation<const char*,const char*>(
        DATABASE_OP_VERSION_FNM_BASE,
        function<error(plugin_context&,const char*,const char*)>(
            db_version_fnm_base_op ) );
    pg->add_operation<const char*,const char*>(
        DATABASE_OP_ADD_SPECIFIC_QUERY,
        function<error(plugin_context&,const char*,const char*)>(
            db_add_specific_query_op ) );
    pg->add_operation<const char*>(
        DATABASE_OP_DEL_SPECIFIC_QUERY,
        function<error(plugin_context&,const char*)>(
            db_del_specific_query_op ) );
    pg->add_operation<specificQueryInp_t*,genQueryOut_t*>(
        DATABASE_OP_SPECIFIC_QUERY,
        function<error(plugin_context&,specificQueryInp_t*,genQueryOut_t*)>(
            db_specific_query_op ) );
    pg->add_operation<const string*, const string*,std::string*>(
        DATABASE_OP_GET_HIERARCHY_FOR_RESC,
        function<error(plugin_context&,const string*, const string*,std::string*)>(
            db_get_hierarchy_for_resc_op ) );
    pg->add_operation<const char*,const char*,const char*,const char*,const char*>(
        DATABASE_OP_MOD_TICKET,
        function<error(plugin_context&,const char*,const char*,const char*,const char*,const char*)>(
            db_mod_ticket_op ) );
    pg->add_operation<const char*,const char*,const char*>(
        DATABASE_OP_CHECK_AND_GET_OBJ_ID,
        function<error(plugin_context&,const char*,const char*,const char*)>(
            db_check_and_get_object_id_op ) );
    pg->add_operation<void**>(
        DATABASE_OP_GET_RCS,
        function<error(plugin_context&,void**)>(
            db_get_icss_op ) );
    pg->add_operation<genQueryInp_t*,genQueryOut_t*>(
        DATABASE_OP_GEN_QUERY,
        function<error(plugin_context&,genQueryInp_t*,genQueryOut_t*)>(
            db_gen_query_op ) );
    pg->add_operation<generalUpdateInp_t*>(
        DATABASE_OP_GENERAL_UPDATE,
        function<error(plugin_context&,generalUpdateInp_t*)>(
            db_general_update_op ) );
    pg->add_operation<const char*,const char*,const char*,int,int>(
        DATABASE_OP_GEN_QUERY_ACCESS_CONTROL_SETUP,
        function<error(plugin_context&,const char*,const char*,const char*,int,int)>(
            db_gen_query_access_control_setup_op ) );
    pg->add_operation<const char*,const char*>(
        DATABASE_OP_GEN_QUERY_TICKET_SETUP,
        function<error(plugin_context&,const char*,const char*)>(
            db_gen_query_ticket_setup_op ) );
    pg->add_operation<const char*,const char*>(
        DATABASE_OP_SUBSTITUTE_RESOURCE_HIERARCHIES,
        function<error(plugin_context&,const char*,const char*)>(
            db_substitute_resource_hierarchies_op ) );
    pg->add_operation<const char*,long long*>(
        DATABASE_OP_GET_DISTINCT_DATA_OBJ_COUNT_ON_RESOURCE,
        function<error(plugin_context&,const char*,long long*)>(
            db_get_distinct_data_obj_count_on_resource_op ) );
    pg->add_operation<const string*, const string*, int, dist_child_result_t*>(
        DATABASE_OP_GET_DISTINCT_DATA_OBJS_MISSING_FROM_CHILD_GIVEN_PARENT,
        function<error(plugin_context&,const string*, const string*, int, dist_child_result_t*)>(
            db_get_distinct_data_objs_missing_from_child_given_parent_op ) );

    pg->add_operation<rodsLong_t,size_t,std::vector<leaf_bundle_t>*,dist_child_result_t*>(
        DATABASE_OP_GET_REPL_LIST_FOR_LEAF_BUNDLES,
        function<error(plugin_context&,rodsLong_t,size_t,std::vector<leaf_bundle_t>*,dist_child_result_t*)>(
            db_get_repl_list_for_leaf_bundles_op));
    return pg;

} // plugin_factory

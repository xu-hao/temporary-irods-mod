/*** Copyright (c), The Regents of the University of California            ***
 *** For more information please refer to files in the COPYRIGHT directory ***/
/**************************************************************************

  This file contains midLevel functions that can be used to get
  information from the ICAT database. These functions should not have
  any intelligence encoded in them.  These functions use the 'internal
  interface to database' library calls to access the ICAT
  database. Hence these functions can be viewed as providing higher
  level calls to the other routines. These functions do not call any
  of the high-level functions, but do sometimes call each other as well
  as the low-level functions.

**************************************************************************/


#include "mid_level.hpp"
#include "low_level.hpp"
#include "irods_stacktrace.hpp"
#include "irods_log.hpp"

#include "rcMisc.h"

#include <vector>
#include <string>
#include "Plugin_stub.h"
#include "PluginGen_stub.h"


extern int logSQL_CML;

int checkObjIdByTicket( const char *dataId, const char *accessLevel,
                        const char *ticketStr, const char *ticketHost,
                        const char *userName, const char *userZone,
                        void *icss );

/*
  Convert the intput arrays to a string and add bind variables
*/
char *cmlArraysToStrWithBind( char*         str,
                              const char*   preStr,
                              const char*   arr[],
                              const char*   arr2[],
                              int           arrLen,
                              const char*   sep,
                              const char*   sep2,
                              int           maxLen ) {
    int i;

    rstrcpy( str, preStr, maxLen );

    for ( i = 0; i < arrLen; i++ ) {
        if ( i > 0 ) {
            rstrcat( str, sep2, maxLen );
        }
        rstrcat( str, arr[i], maxLen );
        rstrcat( str, sep, maxLen );
        rstrcat( str, "?", maxLen );
        cllBindVars[cllBindVarCount++] = arr2[i];
    }

    return str;

}

int cmlDebug( int mode ) {
    logSQL_CML = mode;
    return 0;
}




int cmlCheckNameToken( const char *nameSpace, const char *tokenName, void *icss ) {

    rodsLong_t iVal;
    int status;

    if ( logSQL_CML != 0 ) {
        rodsLog( LOG_SQL, "cmlCheckNameToken SQL 1 " );
    }
    status = hs_get_int_token_id(icss, (void *) nameSpace, (void *) tokenName, &iVal);
    return status;

}


/* modifed for various tests */
int cmlTest( void *icss ) {

    return 0;

}

/*
  Check that a resource exists and user has 'accessLevel' permission.
  Return code is either an iRODS error code (< 0) or the collectionId.
*/
rodsLong_t
cmlCheckResc( const char *rescName, const char *userName, const char *userZone, const char *accessLevel,
              void *icss ) {
    int status;
    rodsLong_t iVal;

    if ( logSQL_CML != 0 ) {
        rodsLog( LOG_SQL, "cmlCheckResc SQL 1 " );
    }

    status = hs_get_int_resc_id_by_access(&icss, (void *) rescName, (void *) userZone, (void *) userName, (void *) accessLevel, &iVal);
    if ( status ) {
        /* There was an error, so do another sql to see which
           of the two likely cases is problem. */

        if ( logSQL_CML != 0 ) {
            rodsLog( LOG_SQL, "cmlCheckResc SQL 2 " );
        }

        status = hs_get_int_resc_id_by_name(icss, (void *) rescName, &iVal);
        if ( status ) {
            return CAT_UNKNOWN_RESOURCE;
        }
        return CAT_NO_ACCESS_PERMISSION;
    }

    return iVal;

}


/*
  Check that a collection exists and user has 'accessLevel' permission.
  Return code is either an iRODS error code (< 0) or the collectionId.
*/
rodsLong_t
cmlCheckDir( const char *dirName, const char *userName, const char *userZone, const char *accessLevel,
             void *icss ) {
    int status;
    rodsLong_t iVal;

    if ( logSQL_CML != 0 ) {
        rodsLog( LOG_SQL, "cmlCheckDir SQL 1 " );
    }

    status = hs_get_int_coll_id_by_access(icss,(void *) dirName, (void *) userZone, (void *) userName, (void *) accessLevel, &iVal);
    if ( status ) {
        /* There was an error, so do another sql to see which
           of the two likely cases is problem. */

        if ( logSQL_CML != 0 ) {
            rodsLog( LOG_SQL, "cmlCheckDir SQL 2 " );
        }

        status = hs_get_int_coll_id_by_name(icss, (void *) dirName, &iVal);
        if ( status ) {
            return CAT_UNKNOWN_COLLECTION;
        }
        return CAT_NO_ACCESS_PERMISSION;
    }

    return iVal;

}


/*
  Check that a collection exists and user has 'accessLevel' permission.
  Return code is either an iRODS error code (< 0) or the collectionId.
  While at it, get the inheritance flag.
*/
rodsLong_t
cmlCheckDirAndGetInheritFlag( const char *dirName, const char *userName, const char *userZone,
                              const char *accessLevel, int *inheritFlag,
                              const char *ticketStr, const char *ticketHost,
                              void *icss ) {
    int status;
    rodsLong_t iVal = 0;

    int cValSize[2];
    char *cVal[3];
    char cValStr1[MAX_INTEGER_SIZE + 10];
    char cValStr2[MAX_INTEGER_SIZE + 10];

    cVal[0] = cValStr1;
    cVal[1] = cValStr2;
    cValSize[0] = MAX_INTEGER_SIZE;
    cValSize[1] = MAX_INTEGER_SIZE;

    *inheritFlag = 0;

    if ( ticketStr != NULL && *ticketStr != '\0' ) {
        if ( logSQL_CML != 0 ) {
            rodsLog( LOG_SQL, "cmlCheckDirAndGetInheritFlag SQL 1 " );
        }
        /* std::vector<std::string> bindVars;
        bindVars.push_back( dirName );
        bindVars.push_back( ticketStr );
        status = cmlGetOneRowFromSqlBV( "select coll_id, coll_inheritance from R_COLL_MAIN CM, R_TICKET_MAIN TM where CM.coll_name=? and TM.ticket_string=? and TM.ticket_type = 'write' and TM.object_id = CM.coll_id",
                                        cVal, cValSize, 2, bindVars, icss ); */
        return -1;
    }
    else {
        if ( logSQL_CML != 0 ) {
            rodsLog( LOG_SQL, "cmlCheckDirAndGetInheritFlag SQL 2 " );
        }
        status = hs_get_some2_coll_id_and_inheritance_by_access(icss,(void *) dirName,(void *) userZone,(void *) userName,(void *) accessLevel,
                                        cVal, cValSize, 2) * 2;
    }
    if ( status == 2 ) {
        if ( *cVal[0] == '\0' ) {
            return CAT_NO_ROWS_FOUND;
        }
        iVal = strtoll( *cVal, NULL, 0 );
        if ( cValStr2[0] == '1' ) {
            *inheritFlag = 1;
        }
        status = 0;
    }

    if ( status ) {
        /* There was an error, so do another sql to see which
           of the two likely cases is problem. */

        if ( logSQL_CML != 0 ) {
            rodsLog( LOG_SQL, "cmlCheckDirAndGetInheritFlag SQL 3 " );
        }

        status = hs_get_int_coll_id_by_name(icss, (void *) dirName, &iVal);
        if ( status ) {
            return CAT_UNKNOWN_COLLECTION;
        }
        return CAT_NO_ACCESS_PERMISSION;
    }

    /*
     Also check the other aspects ticket at this point.
     */
    if ( ticketStr != NULL && *ticketStr != '\0' ) {
        status = checkObjIdByTicket( cValStr1, accessLevel, ticketStr,
                                     ticketHost, userName, userZone,
                                     icss );
        if ( status != 0 ) {
            return status;
        }
    }

    return iVal;

}


/*
  Check that a collection exists and user has 'accessLevel' permission.
  Return code is either an iRODS error code (< 0) or the collectionId.
*/
rodsLong_t
cmlCheckDirId( const char *dirId, const char *userName, const char *userZone,
               const char *accessLevel, void *icss ) {
    int status;
    rodsLong_t iVal;

    if ( logSQL_CML != 0 ) {
        rodsLog( LOG_SQL, "cmlCheckDirId S-Q-L 1 " );
    }

    status = hs_get_int_coll_id_by_coll_id_and_access(icss, (void *) dirId, (void *) userZone, (void *) userName, (void *) accessLevel, &iVal);
    if ( status ) {
        /* There was an error, so do another sql to see which
           of the two likely cases is problem. */

        if ( logSQL_CML != 0 ) {
            rodsLog( LOG_SQL, "cmlCheckDirId S-Q-L 2 " );
        }

        status = hs_get_int_coll_id_by_name(      icss, (void *) dirId, &iVal);
        if ( status ) {
            return CAT_UNKNOWN_COLLECTION;
        }
        return CAT_NO_ACCESS_PERMISSION;
    }

    return 0;
}

/*
  Check that a collection exists and user owns it
*/
rodsLong_t
cmlCheckDirOwn( const char *dirName, const char *userName, const char *userZone,
                void *icss ) {
    int status;
    rodsLong_t iVal;

    if ( logSQL_CML != 0 ) {
        rodsLog( LOG_SQL, "cmlCheckDirOwn SQL 1 " );
    }

    status = hs_get_int_coll_id_by_own(icss, (void *) dirName, (void *) userZone, (void *) userName,  &iVal);
    if ( status < 0 ) {
        return status;
    }
    return iVal;
}


/*
  Check that a dataObj (iRODS file) exists and user has specified permission
  (but don't check the collection access, only its existance).
  Return code is either an iRODS error code (< 0) or the dataId.
*/
rodsLong_t
cmlCheckDataObjOnly( const char *dirName, const char *dataName,
                     const char *userName, const char *userZone,
                     const char *accessLevel, void *icss ) {
    int status;
    rodsLong_t iVal;

    if ( logSQL_CML != 0 ) {
        rodsLog( LOG_SQL, "cmlCheckDataObjOnly SQL 1 " );
    }

    status = hs_get_int_data_id_by_access(icss,    (void *) dirName, (void *) dataName, (void *) userZone, (void *) userName, (void *) accessLevel, &iVal);

    if ( status ) {
        /* There was an error, so do another sql to see which
           of the two likely cases is problem. */
        if ( logSQL_CML != 0 ) {
            rodsLog( LOG_SQL, "cmlCheckDataObjOnly SQL 2 " );
        }

        status = hs_get_int_data_id(  icss,      (void *) dirName, (void *) dataName, &iVal);
        if ( status ) {
            return CAT_UNKNOWN_FILE;
        }
        return CAT_NO_ACCESS_PERMISSION;
    }

    return iVal;

}

/*
  Check that a dataObj (iRODS file) exists and user owns it
*/
rodsLong_t
cmlCheckDataObjOwn( const char *dirName, const char *dataName, const char *userName,
                    const char *userZone, void *icss ) {
    int status;
    rodsLong_t iVal, collId;
    char collIdStr[MAX_NAME_LEN];

    if ( logSQL_CML != 0 ) {
        rodsLog( LOG_SQL, "cmlCheckDataObjOwn SQL 1 " );
    }
    status = hs_get_int_coll_id_by_name(icss,(void *) dirName, &iVal);
    if ( status < 0 ) {
        return status;
    }
    collId = iVal;
    snprintf( collIdStr, MAX_NAME_LEN, "%lld", collId );

    if ( logSQL_CML != 0 ) {
        rodsLog( LOG_SQL, "cmlCheckDataObjOwn SQL 2 " );
    }
    status = hs_get_int_data_id_by_own(icss,  collIdStr, (void *) dataName, (void *) userZone, (void *) userName, &iVal);

    if ( status ) {
        return status;
    }
    return iVal;
}


int cmlCheckUserInGroup( const char *userName, const char *userZone,
                         const char *groupName, void *icss ) {
    int status;
    char sVal[MAX_NAME_LEN];
    rodsLong_t iVal;

    if ( logSQL_CML != 0 ) {
        rodsLog( LOG_SQL, "cmlCheckUserInGroup SQL 1 " );
    }

    status = hs_get_non_group_user_id(icss,(void *) userZone,(void *) userName,sVal, MAX_NAME_LEN);
    if ( status == CAT_NO_ROWS_FOUND ) {
        return CAT_INVALID_USER;
    }
    if ( status ) {
        return status;
    }

    if ( logSQL_CML != 0 ) {
        rodsLog( LOG_SQL, "cmlCheckUserInGroup SQL 2 " );
    }

    status = hs_get_int_group_id_by_user(icss, sVal, (void *) groupName,&iVal);
    if ( status ) {
        return status;
    }
    return 0;
}

/* check on additional restrictions on a ticket, return error if not
 * allowed */
int
cmlCheckTicketRestrictions( const char *ticketId, const char *ticketHost,
                            const char *userName, const char *userZone,
                            void *icss ) {
    return 0;
}

/* Check access via a Ticket to a data-object or collection */
int checkObjIdByTicket( const char *dataId, const char *accessLevel,
                        const char *ticketStr, const char *ticketHost,
                        const char *userName, const char *userZone,
                        void *icss ) {
    return 0;
}

int
cmlTicketUpdateWriteBytes( const char *ticketStr,
                           const char *dataSize, const char *objectId,
                           void *icss ) {
    return 0;
}


/*
  Check that a user has the specified permission or better to a dataObj.
  Return value is either an iRODS error code (< 0) or success (0).
  TicketStr is an optional ticket for ticket-based access,
  TicketHost is an optional host (the connected client IP) for ticket checks.
*/
int cmlCheckDataObjId( const char *dataId, const char *userName,  const char *zoneName,
                       const char *accessLevel, const char *ticketStr, const char *ticketHost,
                       void *icss ) {
    int status;
    rodsLong_t iVal;

    iVal = 0;
    if ( ticketStr != NULL && *ticketStr != '\0' ) {
        status = checkObjIdByTicket( dataId, accessLevel, ticketStr,
                                     ticketHost, userName, zoneName,
                                     icss );
        if ( status != 0 ) {
            return status;
        }
    }
    else {
        if ( logSQL_CML != 0 ) {
            rodsLog( LOG_SQL, "cmlCheckDataObjId SQL 1 " );
        }
        status = hs_get_int_data_id_by_data_id_and_access(icss, (void *) dataId, (void *) zoneName, (void *) userName, (void *) accessLevel, &iVal
                      );
        if ( iVal == 0 ) {
            return CAT_NO_ACCESS_PERMISSION;
        }
    }
    if ( status != 0 ) {
        return CAT_NO_ACCESS_PERMISSION;
    }
    return status;
}

/*
 * Check that the user has group-admin permission.  The user must be of
 * type 'groupadmin' and in some cases a member of the specified group.
 */
int cmlCheckGroupAdminAccess( const char *userName, const char *userZone,
                              const char *groupName, void *icss ) {
    int status;
    char sVal[MAX_NAME_LEN];
    rodsLong_t iVal;

    if ( logSQL_CML != 0 ) {
        rodsLog( LOG_SQL, "cmlCheckGroupAdminAccess SQL 1 " );
    }

    status = hs_get_group_admin_user_id(icss, (void *) userZone, (void *) userName, sVal, MAX_NAME_LEN);
    if ( status == CAT_NO_ROWS_FOUND ) {
        return CAT_INSUFFICIENT_PRIVILEGE_LEVEL;
    }
    if ( status ) {
        return status;
    }

    // =-=-=-=-=-=-=-
    // JMC - backport 4772
    if ( groupName == NULL ) {
        return CAT_INSUFFICIENT_PRIVILEGE_LEVEL;
    }
    if ( *groupName == '\0' ) {
        return ( 0 );  /* caller is requesting no check for a particular group,
             so if the above check passed, the user is OK */
    }
    // =-=-=-=-=-=-=-
    if ( logSQL_CML != 0 ) {
        rodsLog( LOG_SQL, "cmlCheckGroupAdminAccess SQL 2 " );
    }

    status = hs_get_int_group_id_by_user(icss, sVal, (void *) groupName, &iVal);
    if ( status == CAT_NO_ROWS_FOUND ) {
        return CAT_INSUFFICIENT_PRIVILEGE_LEVEL;
    }
    if ( status ) {
        return status;
    }
    return 0;
}

/*
 Get the number of users who are members of a user group.
 This is used in some groupadmin access checks.
 */
int cmlGetGroupMemberCount( const char *groupName, void *icss ) {

    rodsLong_t iVal;
    int status;
    status = hs_get_group_member_count(icss, (void *) groupName, &iVal);
    if ( status == 0 ) {
        status = iVal;
    }
    return status;
}

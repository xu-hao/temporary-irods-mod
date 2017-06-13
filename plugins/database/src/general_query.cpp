/*** Copyright (c), The Regents of the University of California            ***
 *** For more information please refer to files in the COPYRIGHT directory ***/
/*

 These routines are the genSql routines, which are used to convert the
 generalQuery arguments into SQL select strings.  The generalQuery
 arguments are any arbitrary set of columns in the various tables, so
 these routines have to generate SQL that can link any table.column to
 any other.

 Also see the fklinks.c routine which calls fklink (below) to
 initialize the table table.

 At the core, is an algorithm to find a spanning tree in our graph set
 up by fklink.  This does not need to find the minimal spanning tree,
 just THE spanning tree, as there should be only one.  Thus there are
 no weights on the arcs of this tree either.  But complicating this is
 the fact that there are nodes that can create cycles in the
 semi-tree, but these are flagged so the code can stop when
 encountering these.

 There is also a routine that checks for cycles, tCycleChk, which can
 be called when the tables change to make sure there are no cycles.

 */
 // code based on code from Ben Keller
#include "rodsClient.h"
#include "icatHighLevelRoutines.hpp"
#include "mid_level.hpp"
#include "low_level.hpp"
#include "rodsGenQueryNames.h"
#include "GenQuery_stub.h"
#include "PluginGen_stub.h"

char accessControlUserName[MAX_NAME_LEN];
char accessControlZone[MAX_NAME_LEN];
int accessControlPriv;
int accessControlControlFlag = 0;
char sessionTicket[MAX_NAME_LEN] = "";
char sessionClientAddr[MAX_NAME_LEN] = "";


/*
 Perform a check based on the condInput parameters;
 Verify that the user has access to the dataObj at the requested level.
 If continueFlag is non-zero this is a continuation (more rows), so if
 the dataId is the same, can skip the check to the db.
 */
int
checkCondInputAccess( genQueryInp_t genQueryInp, char **resultValue,
                      void *svc, void *icss, int continueFlag ) {
    int i, nCols;
    int userIx = -1, zoneIx = -1, accessIx = -1, dataIx = -1, collIx = -1;
    int status;
    std::string zoneName;

    static char prevDataId[LONG_NAME_LEN];
    static char prevUser[LONG_NAME_LEN];
    static char prevAccess[LONG_NAME_LEN];
    static int prevStatus;

    if ( getValByKey( &genQueryInp.condInput, ADMIN_KW ) ) {
        return 0;
    }

    for ( i = 0; i < genQueryInp.condInput.len; i++ ) {
        if ( strcmp( genQueryInp.condInput.keyWord[i],
                     USER_NAME_CLIENT_KW ) == 0 ) {
            userIx = i;
        }
        if ( strcmp( genQueryInp.condInput.keyWord[i],
                     RODS_ZONE_CLIENT_KW ) == 0 ) {
            zoneIx = i;
        }
        if ( strcmp( genQueryInp.condInput.keyWord[i],
                     ACCESS_PERMISSION_KW ) == 0 ) {
            accessIx = i;
        }
        if ( strcmp( genQueryInp.condInput.keyWord[i],
                     TICKET_KW ) == 0 ) {
            /* for now, log it but the one used is the session ticket */
            rodsLog( LOG_NOTICE, "ticket input, value: %s",
                     genQueryInp.condInput.value[i] );
        }
    }
    if ( genQueryInp.condInput.len == 1 &&
            strcmp( genQueryInp.condInput.keyWord[0], ZONE_KW ) == 0 ) {
        return 0;
    }

    if ( userIx < 0 || zoneIx < 0 || accessIx < 0 ) {
        // this function will get called if any condInput is available.  we now have a
        // case where this kvp is the only option so consider that a success
        char* disable_acl = getValByKey( &genQueryInp.condInput, DISABLE_STRICT_ACL_KW );
        if ( disable_acl ) {
            return 0;
        }
        return CAT_INVALID_ARGUMENT;
    }

    /* Try to find the dataId and/or collID in the output */
    nCols = genQueryInp.selectInp.len;
    for ( i = 0; i < nCols; i++ ) {
        if ( genQueryInp.selectInp.inx[i] == COL_D_DATA_ID ) {
            dataIx = i;
        }
        if ( genQueryInp.selectInp.inx[i] == COL_COLL_ID ) {
            collIx = i;
        }
    }
    if ( dataIx < 0 && collIx < 0 ) {
        return CAT_INVALID_ARGUMENT;
    }

    if ( dataIx >= 0 ) {
        if ( continueFlag == 1 ) {
            if ( strcmp( prevDataId,
                         resultValue[dataIx] ) == 0 ) {
                if ( strcmp( prevUser, genQueryInp.condInput.value[userIx] ) == 0 ) {
                    if ( strcmp( prevAccess,
                                 genQueryInp.condInput.value[accessIx] ) == 0 ) {
                        return prevStatus;
                    }
                }
            }
        }

        snprintf( prevDataId, sizeof( prevDataId ), "%s", resultValue[dataIx] );
        snprintf( prevUser, sizeof( prevUser ), "%s", genQueryInp.condInput.value[userIx] );
        snprintf( prevAccess, sizeof( prevAccess ), "%s", genQueryInp.condInput.value[accessIx] );
        prevStatus = 0;

        if ( strlen( genQueryInp.condInput.value[zoneIx] ) == 0 ) {
            if ( !chlGetLocalZone( zoneName ) ) {
            }
        }
        else {
            zoneName = genQueryInp.condInput.value[zoneIx];
        }
        status = cmlCheckDataObjId(
                     resultValue[dataIx],
                     genQueryInp.condInput.value[userIx],
                     ( char* )zoneName.c_str(),
                     genQueryInp.condInput.value[accessIx],
                     /*                  sessionTicket, accessControlHost, icss); */
                     sessionTicket, sessionClientAddr, svc, icss );
        prevStatus = status;
        return status;
    }

    if ( collIx >= 0 ) {
        if ( strlen( genQueryInp.condInput.value[zoneIx] ) == 0 ) {
            if ( !chlGetLocalZone( zoneName ) ) {
            }
        }
        else {
            zoneName = genQueryInp.condInput.value[zoneIx];
        }
        status = cmlCheckDirId(
                     resultValue[collIx],
                     genQueryInp.condInput.value[userIx],
                     ( char* )zoneName.c_str(),
                     genQueryInp.condInput.value[accessIx], svc, icss );
    }
    return 0;
}

/* Save some pre-provided parameters if msiAclPolicy is STRICT.
   Called with user == NULL to set the controlFlag, else with the
   user info.
 */

int chl_gen_query_access_control_setup_impl(
    const char *user,
    const char *zone,
    const char *host,
    int priv,
    int controlFlag ) {
    if ( user != NULL ) {
        if ( !rstrcpy( accessControlUserName, user, MAX_NAME_LEN ) ) {
            return USER_STRLEN_TOOLONG;
        }
        if ( !rstrcpy( accessControlZone, zone, MAX_NAME_LEN ) ) {
            return USER_STRLEN_TOOLONG;
        }
//      if(!rstrcpy(accessControlHost, host, MAX_NAME_LEN);
        accessControlPriv = priv;
    } else {
        accessControlUserName[0] = '\0';
        accessControlZone[0] = '\0';
        accessControlPriv = NO_USER_AUTH;
    }

    // =-=-=-=-=-=-=-
    // add the >= 0 to allow for repave of strict acl due to
    // issue with file create vs file open in rsDataObjCreate
    int old_flag = accessControlControlFlag;
    if ( controlFlag >= 0 ) {
        /*
        If the caller is making this STRICT, then allow the change as
               this will be an initial acAclPolicy call which is setup in
               core.re.  But don't let users override this admin setting
               via their own calls to the msiAclPolicy; once it is STRICT,
               it stays strict.
             */
        accessControlControlFlag = controlFlag;
    }

    return old_flag;
}

int chl_gen_query_ticket_setup_impl(
    const char* ticket,
    const char* clientAddr ) {
    if ( !rstrcpy( sessionTicket, ticket, sizeof( sessionTicket ) ) ) {
        return USER_STRLEN_TOOLONG;
    }
    if ( !rstrcpy( sessionClientAddr, clientAddr, sizeof( sessionClientAddr ) ) ) {
        return USER_STRLEN_TOOLONG;
    }
    rodsLog( LOG_NOTICE, "session ticket setup, value: %s", ticket );
    return 0;
}


std::string
getColName(int j) {
    const int n = sizeof(columnNames)/sizeof(columnNames[0]);
    for (int i=0; i<n; ++i) {
        if (columnNames[i].columnId == j) {
            return std::string(columnNames[i].columnName);
        }
    }

    std::stringstream ss;
    ss << j;
    return std::string("COLUMN_NAME_NOT_FOUND_") + ss.str();
}

struct option_element{
    int key;
    const char* cpp_macro;
    const char* token;
};

option_element queryWideOptionsMap[] = {
    {RETURN_TOTAL_ROW_COUNT, "RETURN_TOTAL_ROW_COUNT", "return_total_row_count"},
    {NO_DISTINCT,            "NO_DISTINCT",            "no_distinct"},
    {QUOTA_QUERY,            "QUOTA_QUERY",            "quota_query"},
    {AUTO_CLOSE,             "AUTO_CLOSE",             "auto_close"},
    {UPPER_CASE_WHERE,       "UPPER_CASE_WHERE",       "upper_case_where"}
};

option_element selectInpOptionsMap[] = {
    {ORDER_BY,      "ORDER_BY",      "order"},
    {ORDER_BY_DESC, "ORDER_BY_DESC", "order_desc"}
};

option_element selectInpFunctionMap[] = {
    {SELECT_MIN,   "SELECT_MIN",   "min"},
    {SELECT_MAX,   "SELECT_MAX",   "max"},
    {SELECT_SUM,   "SELECT_SUM",   "sum"},
    {SELECT_AVG,   "SELECT_AVG",   "avg"},
    {SELECT_COUNT, "SELECT_COUNT", "count"}
};


std::string
formatSelectedColumn(int columnIndex, int columnOption) {
    std::string ret = getColName(columnIndex);
    if (columnOption == 0 || columnOption == 1) {
        return ret;
    }

    for (size_t i=0; i<sizeof(selectInpOptionsMap)/sizeof(selectInpOptionsMap[0]); ++i) {
        if (columnOption == selectInpOptionsMap[i].key) {
            ret = std::string(selectInpOptionsMap[i].token) + "(" + ret + ")";
            return ret;
        }
    }
    for (size_t i=0; i<sizeof(selectInpFunctionMap)/sizeof(selectInpFunctionMap[0]); ++i) {
        if (columnOption == selectInpFunctionMap[i].key) {
            ret = std::string(selectInpFunctionMap[i].token) + "(" + ret + ")";
            return ret;
        }
    }

    std::stringstream ss;
    ss << columnOption;
    ret = std::string("combo_func_") + ss.str() + "(" + ret + ")";
    return ret;
}

int
genThatQuery( const genQueryInp_t *q, std::string &qu ) {
    // TODO: handle queryWideOptionsMap
    std::stringstream f;

    f << "select ";
    {
        const int n = q->selectInp.len;
        if (n<=0) {
            return -1;
        }

        f << formatSelectedColumn(q->selectInp.inx[0], q->selectInp.value[0]);
        for (int i=1; i<n; ++i) {
            f << ", ";
            f << formatSelectedColumn(q->selectInp.inx[i], q->selectInp.value[i]);
        }
    }

    {
        const int n = q->sqlCondInp.len;
        if (n>0) {
            f << " where ";

            f << getColName(q->sqlCondInp.inx[0]) << " " << q->sqlCondInp.value[0];
            for (int i=1; i<n; ++i) {
                f << " and ";
                f << getColName(q->sqlCondInp.inx[i]) << " " << q->sqlCondInp.value[i];
            }
        }
    }
    qu = f.str();
    return 0;
}

void freeStringArray(char **arr, int len) {
    for(int i = 0; i < len; i++) {
        free(arr[i]);
    }
    free(arr);

}

int maxLen(char **out, int n) {
  int max = 0;
  for(int i = 0; i< n;i++) {
      int len = strlen(out[i]);
      if(len > max) {
        max = len;
      }
  }
  return max;
}

int fillResultInGenQueryOut(void *svc, void* icss, char **out, int col, int row, int *attrinx, genQueryInp_t &genQueryInp, genQueryOut_t *result) {
    int status;
    int len = maxLen(out, col * row) + 1;
    for(int i = 0; i < col; i++) {
        result->sqlResult[i].attriInx = attrinx[i];
        result->sqlResult[i].len = len;
        result->sqlResult[i].value = (char *) malloc(row * len);
        memset(result->sqlResult[i].value, 0, row * len);
    }
        for(int j = 0; j< row; j++ ) {
            if ( genQueryInp.condInput.len > 0 ) {                                                                                                                                                                                   
                status = checkCondInputAccess( genQueryInp, out + col * j, svc, icss, 0 );                                                                                                                                                 
                // need to free the rest of out
                if ( status != 0 ) {                                                                                                                                                                                                 
                    return status;                                                                                                                                                                                                   
                }                                                                                                                                                                                                                    
            }
          for(int i = 0; i < col; i++) {
            snprintf(result->sqlResult[i].value + j * len, len, "%s", out[col * j + i]);
          }                                                                                                                                                                                                                        
        }
    

    result->rowCnt = row;
    result->totalRowCount = row;
    result->attriCnt = col;
    result->continueInx = 0;
    return 0;
}

/*
 This is used for the QUOTA_QUERY option where a specific query is
 needed for efficiency, handling all the quota types in a single query
 (the group and individual quotas, per-resource and total-usage).
 I decided to make this part of General-Query, since it is so similar but
 it is really a specific-query (as an option to general-query).
 The caller specifies a user (COL_USER_NAME) and optionally a resource
 (COL_R_RESC_NAME).  Rows are returned with the quotas that apply,
 most severe (most over or closest to going over) first, if any.  For
 global-usage quotas, the returned RESC_ID is 0.  If the user is a
 member of a group with a quota (per-resource or total-usage) a row is
 returned for that quota too.  All in the appropriate order.
 */
int
generateSpecialQuery( void *svc, void *session, genQueryInp_t &genQueryInp, genQueryOut_t *result ) {
    static char rescName[LONG_NAME_LEN] = "";
    static char userName[NAME_LEN] = "";
    static char userZone[NAME_LEN] = "";

//    char quotaQuery1[] = "( select distinct QM.user_id, RM.resc_name, QM.quota_limit, QM.quota_over, QM.resc_id from R_QUOTA_MAIN QM, R_RESC_MAIN RM, R_USER_GROUP UG, R_USER_MAIN UM2 where QM.resc_id = RM.resc_id AND (QM.user_id = UG.group_user_id and UM2.user_name = ? and UM2.zone_name = ? and UG.user_id = UM2.user_id )) UNION ( select distinct QM.user_id, RM.resc_name, QM.quota_limit, QM.quota_over, QM.resc_id from R_QUOTA_MAIN QM, R_USER_GROUP UG, R_USER_MAIN UM2, R_RESC_MAIN RM where QM.resc_id = '0' AND (QM.user_id = UG.group_user_id and UM2.user_name = ? and UM2.zone_name = ? and UG.user_id = UM2.user_id)) UNION ( select distinct QM.user_id, RM.resc_name, QM.quota_limit, QM.quota_over, QM.resc_id from R_QUOTA_MAIN QM, R_USER_MAIN UM, R_RESC_MAIN RM WHERE (QM.resc_id = RM.resc_id or QM.resc_id = '0') AND (QM.user_id = UM.user_id and UM.user_name = ? and UM.zone_name = ? )) order by quota_over DESC";

//    char quotaQuery2[] = "( select distinct QM.user_id, RM.resc_name, QM.quota_limit, QM.quota_over, QM.resc_id from R_QUOTA_MAIN QM, R_RESC_MAIN RM, R_USER_GROUP UG, R_USER_MAIN UM2 where QM.resc_id = RM.resc_id AND RM.resc_name = ? AND (QM.user_id = UG.group_user_id and UM2.user_name = ? and UM2.zone_name = ? and UG.user_id = UM2.user_id )) UNION ( select distinct QM.user_id, RM.resc_name, QM.quota_limit, QM.quota_over, QM.resc_id from R_QUOTA_MAIN QM, R_USER_GROUP UG, R_USER_MAIN UM2, R_RESC_MAIN RM where QM.resc_id = '0' AND RM.resc_name = ? AND (QM.user_id = UG.group_user_id and UM2.user_name = ? and UM2.zone_name = ? and UG.user_id = UM2.user_id)) UNION ( select distinct QM.user_id, RM.resc_name, QM.quota_limit, QM.quota_over, QM.resc_id from R_QUOTA_MAIN QM, R_USER_MAIN UM, R_RESC_MAIN RM WHERE (QM.resc_id = RM.resc_id or QM.resc_id = '0') AND RM.resc_name = ? AND (QM.user_id = UM.user_id and UM.user_name = ? and UM.zone_name = ? )) order by quota_over DESC";

    int i, valid = 0;

    for ( i = 0; i < genQueryInp.sqlCondInp.len; i++ ) {
        if ( genQueryInp.sqlCondInp.inx[i] == COL_USER_NAME ) {
            int status = parseUserName( genQueryInp.sqlCondInp.value[i], userName,
                                        userZone );
            if ( status ) {
                rodsLog( LOG_ERROR, "parseUserName failed in generateSpecialQuery on %s with status %d.",
                         genQueryInp.sqlCondInp.value[i], status );
                return status;
            }
            if ( userZone[0] == '\0' ) {
                std::string zoneName;
                if ( !chlGetLocalZone( zoneName ) ) {

                }

                snprintf( userZone, sizeof( userZone ), "%s", zoneName.c_str() );
                rodsLog( LOG_ERROR, "userZone1=:%s:\n", userZone );
            }
            valid = 1;
        }
        if ( genQueryInp.sqlCondInp.inx[i] == COL_R_RESC_NAME ) {
            snprintf( rescName, sizeof( rescName ), "%s", genQueryInp.sqlCondInp.value[i] );
        }
    }
    if ( valid == 0 ) {
        return CAT_INVALID_ARGUMENT;
    }
    int nvals;
    char **vals;
    int status;
    if (rescName[0] == '\0') {
        status = hs_get_all_quota_by_user_zone_and_name(svc, session, userZone, userName, &vals, &nvals);
    } else {
        status = hs_get_all_quota_by_user_zone_and_name_and_resc_name(svc, session, userZone, userName, rescName, &vals, &nvals);
    }
    if(status < 0) {
        return status;
    }
    int attrinx[] = {COL_QUOTA_USER_ID,COL_R_RESC_NAME,COL_QUOTA_LIMIT,COL_QUOTA_OVER,COL_QUOTA_RESC_ID};
    status = fillResultInGenQueryOut(svc, session, vals, 5, nvals/5, attrinx, genQueryInp, result);
    freeStringArray(vals, nvals);
    if(status >= 0) {
        status = 0;
    }
    return status;
}

/* General Query */
 int chl_gen_query_impl(
    void* svc,
    void* icss,
    genQueryInp_t  genQueryInp,
    genQueryOut_t* result ) {


if ( genQueryInp.options & QUOTA_QUERY ) {
            return generateSpecialQuery( svc, icss, genQueryInp, result );
        }
else {
    std::string qu;
    genThatQuery(&genQueryInp, qu);
    int doCheck = 0;

    if ( accessControlPriv == LOCAL_PRIV_USER_AUTH ) {
        doCheck = -1; // -1 indicates list all tickets
    } else {

        if ( accessControlControlFlag > 1 ) {
            doCheck = 1;
        }

        if ( doCheck == 0 ) {
            if ( strncmp( accessControlUserName, ANONYMOUS_USER, MAX_NAME_LEN ) == 0 ) {
                doCheck = 1;
            }
        }
    }

    int distinct = ! (genQueryInp.options & NO_DISTINCT);
    char **out = NULL;
    int col = 0;
    int row = 0;
    char *qucstr = (char *) qu.c_str();
    int status = hs_gen_query(svc, icss, distinct, doCheck, accessControlZone, accessControlUserName, sessionTicket, (void *)  qucstr, &out, &col, &row);
    if (status < 0) {
        return status;
    }

    status = fillResultInGenQueryOut(svc, icss, out, col, row, genQueryInp.selectInp.inx, genQueryInp, result);
    freeStringArray(out, row * col);
    if (status >= 0) {
        status = 0;
    }
    return status;
}
}


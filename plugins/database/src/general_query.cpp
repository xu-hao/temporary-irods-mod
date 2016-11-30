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
    return 0;
}

 int chl_gen_query_ticket_setup_impl(
    const char* ticket,
    const char* clientAddr ) {
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

/* General Query */
 int chl_gen_query_impl(
    void* svc,
    void* icss,
    genQueryInp_t  genQueryInp,
    genQueryOut_t* result ) {

    std::string qu;
    genThatQuery(&genQueryInp, qu);
    char **out = NULL;
    int col = 0;
    int row = 0;
    int status = hs_gen_query(svc, icss, (void *)  qu.c_str(), &out, &col, &row);
    if (status < 0) {
        return status;
    }

    int len = maxLen(out, col * row) + 1;
    for(int i = 0; i < genQueryInp.selectInp.len; i++) {
        result->sqlResult[i].attriInx = genQueryInp.selectInp.inx[i];
        result->sqlResult[i].len = len;
        result->sqlResult[i].value = (char *) malloc(row * len);
        memset(result->sqlResult[i].value, 0, row * len);
        for(int j = 0; j< row; j++ ) {
            snprintf(result->sqlResult[i].value + j * len, len, "%s", out[col * j + i]);
            free(out[col * j + i]);
        }
    }
    free(out);

    result->rowCnt = row;
    result->totalRowCount = row;
    result->attriCnt = col;
    result->continueInx = 0;

    return 0;

}

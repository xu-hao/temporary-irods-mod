/*** Copyright (c), The Regents of the University of California            ***
 *** For more information please refer to files in the COPYRIGHT directory ***/
/*

 These routines provide the generalInsert and Delete row capabilities.
 Admins (for now) are allowed to call these functions to add or remove
 specified rows into certain tables.  The arguments are similar to the
 generalQuery in that columns are specified via the COL_ #defines.

 Currently, updates can only be done on a single table at a time.

 Initially, this was developed for use with a notification service which
 was postponed and now may be used with rule tables.
*/
#include "rodsGeneralUpdate.h"

#include "rodsClient.h"
#include "icatHighLevelRoutines.hpp"
#include "mid_level.hpp"
#include "low_level.hpp"




/* General Update */
int chl_general_update_impl(
    generalUpdateInp_t generalUpdateInp ) {
    return 0;
}

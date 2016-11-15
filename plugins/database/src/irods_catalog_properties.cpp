/*
 * catalog_properties.c
 *
 *  Created on: Oct 9, 2013
 *      Author: adt
 */

// =-=-=-=-=-=-=-
#include "irods_error.hpp"
#include "irods_exception.hpp"
#include "irods_catalog_properties.hpp"

// =-=-=-=-=-=-=-
// irods includes
#include "icatHighLevelRoutines.hpp"
#include "mid_level.hpp"

namespace irods {

    catalog_properties& catalog_properties::instance() {
        static catalog_properties singleton;
        return singleton;
    }

} // namespace irods

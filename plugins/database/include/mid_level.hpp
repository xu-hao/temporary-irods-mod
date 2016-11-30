/*** Copyright (c), The Regents of the University of California            ***
 *** For more information please refer to files in the COPYRIGHT directory ***/
/*******************************************************************************

   This file contains all the  externs of globals used by ICAT

*******************************************************************************/

#ifndef ICAT_MIDLEVEL_ROUTINES_HPP
#define ICAT_MIDLEVEL_ROUTINES_HPP

#include "rodsType.h"
#include "icatStructs.hpp"

#include <vector>
#include <string>


int cmlCheckNameToken( const char *nameSpace,
                       const char *tokenName,
                       void *svc,
                       void *icss );


rodsLong_t cmlCheckDir( const char *dirName, const char *userName, const char *userZone,
                        const char *accessLevel,
                        void *svc,
                        void *icss );

rodsLong_t cmlCheckResc( const char *rescName, const char *userName, const char *userZone,
                         const char *accessLevel,
                         void *svc,
                         void *icss );

rodsLong_t cmlCheckDirAndGetInheritFlag( const char *dirName, const char *userName,
        const char *userZone, const char *accessLevel,
        int *inheritFlag, const char *ticketStr, const char *ticketHost,
        void *svc,
        void *icss );

rodsLong_t cmlCheckDirId( const char *dirId, const char *userName, const char *userZone,
                          const char *accessLevel,
                          void *svc,
                          void *icss );

rodsLong_t cmlCheckDirOwn( const char *dirName, const char *userName, const char *userZone,
  void *svc,
                           void *icss );

int cmlCheckDataObjId( const char *dataId, const char *userName, const char *zoneName,
                       const char *accessLevel, const char *ticketStr,
                       const char *ticketHost,
                       void *svc,
                       void *icss );

int cmlTicketUpdateWriteBytes( const char *ticketStr,
                               const char *dataSize, const char *objectId,
                               void *svc,
                               void *icss );

rodsLong_t cmlCheckDataObjOnly( const char *dirName, const char *dataName, const char *userName,
                                const char *userZone, const char *accessLevel,
                                void *svc,
                                 void *icss );

rodsLong_t cmlCheckDataObjOwn( const char *dirName, const char *dataName, const char *userName,
                               const char *userZone,
                               void *svc,
                                void *icss );

int cmlCheckGroupAdminAccess( const char *userName, const char *userZone,
                              const char *groupName,
                              void *svc,
                               void *icss );

int cmlGetGroupMemberCount( const char *groupName,
  void *svc,
  void *icss );

int cmlDebug( int mode );

#endif /* ICAT_MIDLEVEL_ROUTINES_H */

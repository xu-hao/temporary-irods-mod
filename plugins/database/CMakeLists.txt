if (NOT ODBC_LIBRARY)
  find_library(ODBC_LIBRARY odbc)
  if (ODBC_LIBRARY)
    message(STATUS "Found odbc: ${ODBC_LIBRARY}")
  else()
    message(FATAL_ERROR "odbc library not found")
  endif()
endif()

set(
  IRODS_DATABASE_PLUGIN_COMPILE_DEFINITIONS_postgres
  )
set(
  IRODS_DATABASE_PLUGIN_COMPILE_DEFINITIONS_mysql
  MY_ICAT
  )
set(
  IRODS_DATABASE_PLUGIN_COMPILE_DEFINITIONS_oracle
  ORA_ICAT
  )

set(
  IRODS_DATABASE_PLUGINS
  postgres
  mysql
  oracle
  )

set(
    GHC_INSTALL_DIR
    /home/xuh/.stack/programs/x86_64-linux/ghc-8.0.1/lib/ghc-8.0.1
)
set(
  GHC_CXX_FLAGS
  "-fno-stack-protector -DTABLES_NEXT_TO_CODE -D__GLASGOW_HASKELL__=800 -include ${GHC_INSTALL_DIR}/include/ghcversion.h"
  )

set(
  GHC_LINKER_FLAGS
  "--hash-size=31 --reduce-memory-overheads --no-as-needed --gc-sections -u ghczmprim_GHCziTypes_Izh_static_info -u ghczmprim_GHCziTypes_Czh_static_info -u ghczmprim_GHCziTypes_Fzh_static_info -u ghczmprim_GHCziTypes_Dzh_static_info -u base_GHCziPtr_Ptr_static_info -u ghczmprim_GHCziTypes_Wzh_static_info -u base_GHCziInt_I8zh_static_info -u base_GHCziInt_I16zh_static_info -u base_GHCziInt_I32zh_static_info -u base_GHCziInt_I64zh_static_info -u base_GHCziWord_W8zh_static_info -u base_GHCziWord_W16zh_static_info -u base_GHCziWord_W32zh_static_info -u base_GHCziWord_W64zh_static_info -u base_GHCziStable_StablePtr_static_info -u ghczmprim_GHCziTypes_Izh_con_info -u ghczmprim_GHCziTypes_Czh_con_info -u ghczmprim_GHCziTypes_Fzh_con_info -u ghczmprim_GHCziTypes_Dzh_con_info -u base_GHCziPtr_Ptr_con_info -u base_GHCziPtr_FunPtr_con_info -u base_GHCziStable_StablePtr_con_info -u ghczmprim_GHCziTypes_False_closure -u ghczmprim_GHCziTypes_True_closure -u base_GHCziPack_unpackCString_closure -u base_GHCziIOziException_stackOverflow_closure -u base_GHCziIOziException_heapOverflow_closure -u base_ControlziExceptionziBase_nonTermination_closure -u base_GHCziIOziException_blockedIndefinitelyOnMVar_closure -u base_GHCziIOziException_blockedIndefinitelyOnSTM_closure -u base_GHCziIOziException_allocationLimitExceeded_closure -u base_ControlziExceptionziBase_nestedAtomically_closure -u base_GHCziEventziThread_blockedOnBadFD_closure -u base_GHCziWeak_runFinalizzerBatch_closure -u base_GHCziTopHandler_flushStdHandles_closure -u base_GHCziTopHandler_runIO_closure -u base_GHCziTopHandler_runNonIO_closure -u base_GHCziConcziIO_ensureIOManagerIsRunning_closure -u base_GHCziConcziIO_ioManagerCapabilitiesChanged_closure -u base_GHCziConcziSync_runSparks_closure -u base_GHCziConcziSignal_runHandlersPtr_closure"
  )

file(GLOB GHC_LINK_DIR ${GHC_INSTALL_DIR}/*)

set(
  GHC_LIBRARY
  HSrts-ghc8.0.1
  HSQueryArrow-ffi-c-0.2-ghc8.0.1
  HSQueryArrow-ffi-c-local-0.2-ghc8.0.1
  # HSQueryArrow-ffi-c-tcp-0.2-ghc8.0.1 HSQueryArrow-ffi-c-unixdomainsocket-0.2-ghc8.0.1
  HSQueryArrow-ffi-c-genquery-0.2-ghc8.0.1
)

set(
  GHC_INCLUDE
  ${GHC_INSTALL_DIR}/include
)

set(
  CMAKE_CXX_FLAGS
  "${CMAKE_CXX_FLAGS} ${GHC_CXX_FLAGS}"
  )

set(
  CMAKE_EXE_LINKER_FLAGS
  "${CMAKE_EXE_LINKER_FLAGS} ${GHC_LINKER_FLAGS}"
  )

link_directories(
  ${GHC_LINK_DIR}
)

foreach(PLUGIN ${IRODS_DATABASE_PLUGINS})
  string(TOUPPER ${PLUGIN} PLUGIN_UPPERCASE)

  add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/icatSysTables_${PLUGIN}.sql
    COMMAND cpp -E -P -D${PLUGIN} ${CMAKE_SOURCE_DIR}/plugins/database/src/icatSysTables.sql.pp ${CMAKE_BINARY_DIR}/icatSysTables_${PLUGIN}.sql
    MAIN_DEPENDENCY ${CMAKE_SOURCE_DIR}/plugins/database/src/icatSysTables.sql.pp
    )
  add_custom_target(IRODS_PHONY_TARGET_icatSysTables_${PLUGIN}.sql ALL DEPENDS ${CMAKE_BINARY_DIR}/icatSysTables_${PLUGIN}.sql) # Forces execution of custom_command

  add_library(
    ${PLUGIN}
    MODULE
    ${CMAKE_SOURCE_DIR}/plugins/database/src/db_plugin.cpp
    ${CMAKE_SOURCE_DIR}/plugins/database/src/general_query.cpp
    ${CMAKE_SOURCE_DIR}/plugins/database/src/general_query_setup.cpp
    ${CMAKE_SOURCE_DIR}/plugins/database/src/general_update.cpp
    ${CMAKE_SOURCE_DIR}/plugins/database/src/irods_catalog_properties.cpp
    ${CMAKE_SOURCE_DIR}/plugins/database/src/irods_sql_logger.cpp
    ${CMAKE_SOURCE_DIR}/plugins/database/src/low_level_odbc.cpp
    ${CMAKE_SOURCE_DIR}/plugins/database/src/mid_level_routines.cpp
    )

  target_include_directories(
    ${PLUGIN}
    PRIVATE
    ${CMAKE_BINARY_DIR}/lib/core/include
    ${CMAKE_SOURCE_DIR}/lib/core/include
    ${CMAKE_SOURCE_DIR}/lib/api/include
    ${CMAKE_SOURCE_DIR}/lib/hasher/include
    ${CMAKE_SOURCE_DIR}/server/core/include
    ${CMAKE_SOURCE_DIR}/server/icat/include
    ${CMAKE_SOURCE_DIR}/server/re/include
    ${CMAKE_SOURCE_DIR}/plugins/database/include
    ${IRODS_EXTERNALS_FULLPATH_BOOST}/include
    ${IRODS_EXTERNALS_FULLPATH_JANSSON}/include
    ${GHC_INCLUDE}
    )

  target_link_libraries(
    ${PLUGIN}
    PRIVATE
    irods_server
    irods_plugin_dependencies
    irods_common
    ${IRODS_EXTERNALS_FULLPATH_BOOST}/lib/libboost_system.so
    ${IRODS_EXTERNALS_FULLPATH_BOOST}/lib/libboost_regex.so
    ${ODBC_LIBRARY}
    ${GHC_LIBRARY}
    )

  target_compile_definitions(${PLUGIN} PRIVATE ENABLE_RE ${IRODS_DATABASE_PLUGIN_COMPILE_DEFINITIONS_${PLUGIN}} ${IRODS_COMPILE_DEFINITIONS} BOOST_SYSTEM_NO_DEPRECATED)
  target_compile_options(${PLUGIN} PRIVATE -Wno-write-strings)
  set_property(TARGET ${PLUGIN} PROPERTY CXX_STANDARD ${IRODS_CXX_STANDARD})

  install(
    TARGETS
    ${PLUGIN}
    LIBRARY
    DESTINATION ${IRODS_PLUGINS_DIRECTORY}/database
    COMPONENT ${IRODS_PACKAGE_COMPONENT_${PLUGIN_UPPERCASE}_NAME}
    )

  install(
    FILES
    ${CMAKE_SOURCE_DIR}/plugins/database/${PLUGIN}/upgrade-3.3.xto4.0.0.sql
    DESTINATION ${IRODS_HOME_DIRECTORY}/packaging
    COMPONENT ${IRODS_PACKAGE_COMPONENT_${PLUGIN_UPPERCASE}_NAME}
    )

  install(
    FILES
    ${CMAKE_SOURCE_DIR}/plugins/database/packaging/localhost_setup_${PLUGIN}.input
    DESTINATION ${IRODS_HOME_DIRECTORY}/packaging
    COMPONENT ${IRODS_PACKAGE_COMPONENT_${PLUGIN_UPPERCASE}_NAME}
    PERMISSIONS OWNER_READ GROUP_READ WORLD_READ
    )

  install(
    FILES
    ${CMAKE_SOURCE_DIR}/plugins/database/src/icatDropSysTables.sql
    ${CMAKE_SOURCE_DIR}/plugins/database/src/icatSysInserts.sql
    ${CMAKE_SOURCE_DIR}/plugins/database/src/icatPurgeRecycleBin.sql
    DESTINATION ${IRODS_HOME_DIRECTORY}/packaging/sql
    COMPONENT ${IRODS_PACKAGE_COMPONENT_${PLUGIN_UPPERCASE}_NAME}
    PERMISSIONS OWNER_READ GROUP_READ WORLD_READ
    )

  install(
    FILES
    ${CMAKE_BINARY_DIR}/icatSysTables_${PLUGIN}.sql
    DESTINATION ${IRODS_HOME_DIRECTORY}/packaging/sql
    COMPONENT ${IRODS_PACKAGE_COMPONENT_${PLUGIN_UPPERCASE}_NAME}
    RENAME icatSysTables.sql
    PERMISSIONS OWNER_READ GROUP_READ WORLD_READ
    )

endforeach()

install(
  FILES
  ${CMAKE_SOURCE_DIR}/plugins/database/src/mysql_functions.sql
  DESTINATION ${IRODS_HOME_DIRECTORY}/packaging/sql
  COMPONENT ${IRODS_PACKAGE_COMPONENT_MYSQL_NAME}
  )

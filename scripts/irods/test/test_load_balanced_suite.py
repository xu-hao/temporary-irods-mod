from __future__ import print_function

import contextlib
import time
import sys
import shutil
import os
import socket
import datetime
import imp
if sys.version_info >= (2, 7):
    import unittest
else:
    import unittest2 as unittest

from .. import test
from . import settings
from . import resource_suite
from . import session
from .. import lib
from ..configuration import IrodsConfig
from .. import database_connect


class Test_LoadBalanced_Resource(resource_suite.ResourceBase, unittest.TestCase):

    def setUp(self):
        with session.make_session_for_existing_admin() as admin_session:
            context_prefix = lib.get_hostname() + ':' + IrodsConfig().irods_directory
            admin_session.assert_icommand('iadmin modresc demoResc name origResc', 'STDOUT_SINGLELINE', 'rename', input='yes\n')
            admin_session.assert_icommand('iadmin mkresc demoResc load_balanced', 'STDOUT_SINGLELINE', 'load_balanced')
            admin_session.assert_icommand('iadmin mkresc rescA "unixfilesystem" ' + context_prefix + '/rescAVault', 'STDOUT_SINGLELINE', 'unixfilesystem')
            admin_session.assert_icommand('iadmin mkresc rescB "unixfilesystem" ' + context_prefix + '/rescBVault', 'STDOUT_SINGLELINE', 'unixfilesystem')
            admin_session.assert_icommand('iadmin mkresc rescC "unixfilesystem" ' + context_prefix + '/rescCVault', 'STDOUT_SINGLELINE', 'unixfilesystem')
            admin_session.assert_icommand('iadmin addchildtoresc demoResc rescA')
            admin_session.assert_icommand('iadmin addchildtoresc demoResc rescB')
            admin_session.assert_icommand('iadmin addchildtoresc demoResc rescC')
        super(Test_LoadBalanced_Resource, self).setUp()

    def tearDown(self):
        super(Test_LoadBalanced_Resource, self).tearDown()
        with session.make_session_for_existing_admin() as admin_session:
            admin_session.assert_icommand("iadmin rmchildfromresc demoResc rescA")
            admin_session.assert_icommand("iadmin rmchildfromresc demoResc rescB")
            admin_session.assert_icommand("iadmin rmchildfromresc demoResc rescC")
            admin_session.assert_icommand("iadmin rmresc rescA")
            admin_session.assert_icommand("iadmin rmresc rescB")
            admin_session.assert_icommand("iadmin rmresc rescC")
            admin_session.assert_icommand("iadmin rmresc demoResc")
            admin_session.assert_icommand("iadmin modresc origResc name demoResc", 'STDOUT_SINGLELINE', 'rename', input='yes\n')
        irods_config = IrodsConfig()
        shutil.rmtree(irods_config.irods_directory + "/rescAVault", ignore_errors=True)
        shutil.rmtree(irods_config.irods_directory + "/rescBVault", ignore_errors=True)
        shutil.rmtree(irods_config.irods_directory + "/rescCVault", ignore_errors=True)

    @unittest.skipIf(test.settings.TOPOLOGY_FROM_RESOURCE_SERVER, "Skip for topology testing from resource server")
    def test_load_balanced(self):
        # =-=-=-=-=-=-=-
        # read server_config.json and .odbc.ini
        cfg = IrodsConfig()

        if cfg.catalog_database_type == "postgres":
            # =-=-=-=-=-=-=-
            # seed load table with fake values - rescA should win
            from .. import database_connect
            with contextlib.closing(database_connect.get_database_connection(cfg)) as connection:
                with contextlib.closing(connection.cursor()) as cursor:
                    secs = int(time.time())
                    database_connect.execute_queryarrow_statement("insert SERVER_LOAD_DIGEST_OBJ(\"rescA\", 50, \"{0}\") SERVER_LOAD_DIGEST_OBJ(\"rescB\", 75, \"{0}\") SERVER_LOAD_DIGEST_OBJ(\"rescC\", 95, \"{0}\")".format(secs))

                    # Make a local file to put
                    local_filepath = os.path.join(self.admin.local_session_dir, 'things.txt')
                    lib.make_file(local_filepath, 500, 'arbitrary')

                    # =-=-=-=-=-=-=-
                    # build a logical path for putting a file
                    test_file = self.admin.session_collection + "/test_file.txt"

                    # =-=-=-=-=-=-=-
                    # put a test_file.txt - should be on rescA given load table values
                    self.admin.assert_icommand("iput -f %s %s" % (local_filepath, test_file))
                    self.admin.assert_icommand("ils -L " + test_file, 'STDOUT_SINGLELINE', "rescA")
                    self.admin.assert_icommand("irm -f " + test_file)

                    # =-=-=-=-=-=-=-
                    # drop rescC to a load of 15 - this should now win
                    database_connect.execute_queryarrow_statement("SERVER_LOAD_DIGEST_OBJ(\"rescC\",load_factor,cts) delete SERVER_LOAD_DIGEST_OBJ(\"rescC\", load_factor, cts) insert SERVER_LOAD_DIGEST_OBJ(\"rescC\", 15, cts)")

                    # =-=-=-=-=-=-=-
                    # put a test_file.txt - should be on rescC given load table values
                    self.admin.assert_icommand("iput -f %s %s" % (local_filepath, test_file))
                    self.admin.assert_icommand("ils -L " + test_file, 'STDOUT_SINGLELINE', "rescC")
                    self.admin.assert_icommand("irm -f " + test_file)

                    # =-=-=-=-=-=-=-
                    # clean up our alterations to the load table
                    database_connect.execute_queryarrow_statement("SERVER_LOAD_DIGEST_OBJ(\"rescA\",load_factora,ctsa) (delete SERVER_LOAD_DIGEST_OBJ(\"rescA\", load_factora, ctsa)) SERVER_LOAD_DIGEST_OBJ(\"rescB\",load_factorb,ctsb) (delete SERVER_LOAD_DIGEST_OBJ(\"rescB\", load_factorb, ctsb)) SERVER_LOAD_DIGEST_OBJ(\"rescC\",load_factorc,ctsc) (delete SERVER_LOAD_DIGEST_OBJ(\"rescC\", load_factorc, ctsc)) ")
        else:
            print('skipping test_load_balanced due to unsupported database for this test.')

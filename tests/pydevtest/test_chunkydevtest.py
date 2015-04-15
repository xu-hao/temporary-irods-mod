if sys.version_info >= (2, 7):
from resource_suite import ResourceBase
import lib
        username = self.admin.username
        irodszone = self.admin.zone_name
        testuser1 = self.user0.username
        irodshome = self.admin.session_collection
        irodsdefresource = self.admin.default_resource
        self.admin.assert_icommand("imkdir " + irodshome + "/icmdtest")
        self.admin.assert_icommand("iinit -l", 'STDOUT', self.admin.username)
        self.admin.assert_icommand("iinit -l", 'STDOUT', self.admin.zone_name)
        self.admin.assert_icommand("iinit -l", 'STDOUT', self.admin.default_resource)
        res = self.admin.run_icommand(['ils', '-V'])
        assert res[1].count('irods_host') == 1
        assert res[1].count('irods_port') == 1
        assert res[1].count('irods_default_resource') == 1
        self.admin.assert_icommand("ilsresc", 'STDOUT', self.testresc)
        self.admin.assert_icommand("ilsresc -l", 'STDOUT', self.testresc)
        self.admin.assert_icommand("imiscsvrinfo", 'STDOUT', ["relVersion"])
        self.admin.assert_icommand("iuserinfo", 'STDOUT', "name: " + username)
        self.admin.assert_icommand("ienv", 'STDOUT', "irods_zone")
        self.admin.assert_icommand("ipwd", 'STDOUT', "home")
        self.admin.assert_icommand("ihelp ils", 'STDOUT', "ils")
        self.admin.assert_icommand("ierror -14000", 'STDOUT', "SYS_API_INPUT_ERR")
        self.admin.assert_icommand("iexecmd hello", 'STDOUT', "Hello world")
        self.admin.assert_icommand("ips -v", 'STDOUT', "ips")
        self.admin.assert_icommand("iqstat", 'STDOUT', "No delayed rules pending for user " + self.admin.username)
        self.admin.assert_icommand("ils -AL", 'STDOUT', "home")  # debug
        self.admin.assert_icommand("iput -K --wlock " + progname + " " + irodshome + "/icmdtest/foo1")
        self.admin.assert_icommand("ichksum -f " + irodshome + "/icmdtest/foo1", 'STDOUT', "performed = 1")
        self.admin.assert_icommand("iput -kf " + progname + " " + irodshome + "/icmdtest/foo1")
        self.admin.assert_icommand("ils " + irodshome + "/icmdtest/foo1", 'STDOUT', "foo1")
        self.admin.assert_icommand("ils -l " + irodshome + "/icmdtest/foo1", 'STDOUT', ["foo1", myssize])
        self.admin.assert_icommand("iadmin ls " + irodshome + "/icmdtest", 'STDOUT', "foo1")
        self.admin.assert_icommand("ils -A " + irodshome + "/icmdtest/foo1",
                   'STDOUT', username + "#" + irodszone + ":own")
        self.admin.assert_icommand("ichmod read " + testuser1 + " " + irodshome + "/icmdtest/foo1")
        self.admin.assert_icommand("ils -A " + irodshome + "/icmdtest/foo1",
                   'STDOUT', testuser1 + "#" + irodszone + ":read")
        self.admin.assert_icommand("irepl -B -R " + self.testresc + " --rlock " + irodshome + "/icmdtest/foo1")
        self.admin.assert_icommand("ils -l " + irodshome + "/icmdtest/foo1", 'STDOUT', self.testresc)
        self.admin.assert_icommand("itrim -S " + irodsdefresource + " -N1 " + irodshome + "/icmdtest/foo1")
        self.admin.assert_icommand_fail("ils -L " + irodshome + "/icmdtest/foo1", 'STDOUT', [irodsdefresource])
        self.admin.assert_icommand("iphymv -R " + irodsdefresource + " " + irodshome + "/icmdtest/foo1")
        self.admin.assert_icommand("ils -l " + irodshome + "/icmdtest/foo1", 'STDOUT', irodsdefresource[0:19])
        self.admin.assert_icommand("imeta add -d " + irodshome + "/icmdtest/foo1 testmeta1 180 cm")
        self.admin.assert_icommand("imeta ls -d " + irodshome + "/icmdtest/foo1", 'STDOUT', ["testmeta1"])
        self.admin.assert_icommand("imeta ls -d " + irodshome + "/icmdtest/foo1", 'STDOUT', ["180"])
        self.admin.assert_icommand("imeta ls -d " + irodshome + "/icmdtest/foo1", 'STDOUT', ["cm"])
        self.admin.assert_icommand("icp -K -R " + self.testresc + " " +
        self.admin.assert_icommand("iget -fK --rlock " + irodshome + "/icmdtest/foo2 /tmp/")
        self.admin.assert_icommand("ils " + irodshome + "/icmdtest/foo2", 'STDOUT', "foo2")
        self.admin.assert_icommand("imv " + irodshome + "/icmdtest/foo2 " + irodshome + "/icmdtest/foo4")
        self.admin.assert_icommand("ils -l " + irodshome + "/icmdtest/foo4", 'STDOUT', "foo4")
        self.admin.assert_icommand("imv " + irodshome + "/icmdtest/foo4 " + irodshome + "/icmdtest/foo2")
        self.admin.assert_icommand("ils -l " + irodshome + "/icmdtest/foo2", 'STDOUT', "foo2")
        self.admin.assert_icommand("ichksum " + irodshome + "/icmdtest/foo2", 'STDOUT', "foo2")
        self.admin.assert_icommand("imeta add -d " + irodshome + "/icmdtest/foo2 testmeta1 180 cm")
        self.admin.assert_icommand("imeta add -d " + irodshome + "/icmdtest/foo1 testmeta2 hello")
        self.admin.assert_icommand("imeta ls -d " + irodshome + "/icmdtest/foo1", 'STDOUT', ["testmeta1"])
        self.admin.assert_icommand("imeta ls -d " + irodshome + "/icmdtest/foo1", 'STDOUT', ["hello"])
        self.admin.assert_icommand("imeta qu -d testmeta1 = 180", 'STDOUT', "foo1")
        self.admin.assert_icommand("imeta qu -d testmeta2 = hello", 'STDOUT', "dataObj: foo1")
        self.admin.assert_icommand("iget -f -K --rlock " + irodshome + "/icmdtest/foo2 " + dir_w)
        with lib.make_session_for_existing_admin() as rods_admin:
            rods_admin.assert_icommand(['ichmod', 'own', self.admin.username, '/' + self.admin.zone_name])

        username = self.admin.username
        irodszone = self.admin.zone_name
        testuser1 = self.user0.username
        irodshome = self.admin.session_collection
        irodsdefresource = self.admin.default_resource
        self.admin.assert_icommand("imkdir icmdtest")
        self.admin.assert_icommand("iput -K --wlock " + progname + " " + irodshome + "/icmdtest/foo1")
        self.admin.assert_icommand("icp -K -R " + self.testresc + " " +
        self.admin.assert_icommand("irepl -B -R " + self.testresc + " " + irodshome + "/icmdtest/foo1")
        self.admin.assert_icommand("iput -kfR " + irodsdefresource + " " + sfile2 + " " + irodshome + "/icmdtest/foo1")
        self.admin.assert_icommand("ils -l " + irodshome + "/icmdtest/foo1", 'STDOUT', ["foo1", myssize])
        self.admin.assert_icommand("ils -l " + irodshome + "/icmdtest/foo1",
                   'STDOUT', ["foo1", str(os.stat(sfile2).st_size)])
        self.admin.assert_icommand("irepl -U " + irodshome + "/icmdtest/foo1")
        self.admin.assert_icommand_fail("ils -l " + irodshome + "/icmdtest/foo1", 'STDOUT', myssize)
        self.admin.assert_icommand("itrim -S " + irodsdefresource + " " + irodshome + "/icmdtest/foo1")
        self.admin.assert_icommand("iput -bIvPKr " + mysdir + " " + irodshome + "/icmdtest", 'STDOUT', "Bulk upload")
        self.admin.assert_icommand("iput -PkITr -X " + rsfile + " --retries 10 " +
                   mysdir + " " + irodshome + "/icmdtestw", 'STDOUT', "Processing")
        self.admin.assert_icommand("imv " + irodshome + "/icmdtestw " + irodshome + "/icmdtestw1")
        self.admin.assert_icommand("ils -lr " + irodshome + "/icmdtestw1", 'STDOUT', "sfile10")
        self.admin.assert_icommand("ils -Ar " + irodshome + "/icmdtestw1", 'STDOUT', "sfile10")
        self.admin.assert_icommand("irm -rvf " + irodshome + "/icmdtestw1", 'STDOUT', "num files done")
        self.admin.assert_icommand("iget -vIKPfr -X rsfile --retries 10 " +
                   irodshome + "/icmdtest " + dir_w + "/testx", 'STDOUT', "opened")
        self.admin.assert_icommand("iput " + dir_w + "/testx.tar " + irodshome + "/icmdtestx.tar")
        self.admin.assert_icommand("ibun -x " + irodshome + "/icmdtestx.tar " + irodshome + "/icmdtestx")
        self.admin.assert_icommand("ils -lr " + irodshome + "/icmdtestx", 'STDOUT', ["foo2"])
        self.admin.assert_icommand("ils -lr " + irodshome + "/icmdtestx", 'STDOUT', ["sfile10"])
        self.admin.assert_icommand("ibun -cDtar " + irodshome + "/icmdtestx1.tar " + irodshome + "/icmdtestx")
        self.admin.assert_icommand("ils -l " + irodshome + "/icmdtestx1.tar", 'STDOUT', "testx1.tar")
        self.admin.assert_icommand("iget " + irodshome + "/icmdtestx1.tar " + dir_w + "/testx1.tar")
        self.admin.assert_icommand("ibun -cDgzip " + irodshome + "/icmdtestx1.tar.gz " + irodshome + "/icmdtestx")
        self.admin.assert_icommand("ibun -x " + irodshome + "/icmdtestx1.tar.gz " + irodshome + "/icmdtestgz")
        self.admin.assert_icommand("iget -vr " + irodshome + "/icmdtestgz " + dir_w + "", 'STDOUT', "icmdtestgz")
        self.admin.assert_icommand("ibun --add " + irodshome + "/icmdtestx1.tar.gz " + irodshome + "/icmdtestgz")
        self.admin.assert_icommand("irm -rf " + irodshome + "/icmdtestx1.tar.gz " + irodshome + "/icmdtestgz")
        self.admin.assert_icommand("ibun -cDbzip2 " + irodshome + "/icmdtestx1.tar.bz2 " + irodshome + "/icmdtestx")
        self.admin.assert_icommand("ibun -xb " + irodshome + "/icmdtestx1.tar.bz2 " + irodshome + "/icmdtestbz2")
        self.admin.assert_icommand("iget -vr " + irodshome + "/icmdtestbz2 " + dir_w + "", 'STDOUT', "icmdtestbz2")
        self.admin.assert_icommand("irm -rf " + irodshome + "/icmdtestx1.tar.bz2")
        self.admin.assert_icommand("iphybun -R " + self.anotherresc + " -Dbzip2 " + irodshome + "/icmdtestbz2")
        self.admin.assert_icommand("itrim -N1 -S " + self.testresc + " -r " + irodshome + "/icmdtestbz2", 'STDOUT', "Total size trimmed")
        self.admin.assert_icommand("itrim -N1 -S " + irodsdefresource + " -r " + irodshome + "/icmdtestbz2", 'STDOUT', "Total size trimmed")
        self.admin.assert_icommand("ils --bundle " + bunfile, 'STDOUT', "Subfiles")
        self.admin.assert_icommand("irm -rf " + irodshome + "/icmdtestbz2")
        self.admin.assert_icommand("irm -f --empty " + bunfile)
        username = self.admin.username
        irodszone = self.admin.zone_name
        testuser1 = self.user0.username
        irodshome = self.admin.session_collection
        irodsdefresource = self.admin.default_resource
        self.admin.assert_icommand("imkdir icmdtest")
        self.admin.assert_icommand("ireg -KR " + self.testresc + " /tmp/sfile2 " + irodshome + "/foo5")
        self.admin.assert_icommand("ireg -KR " + self.anotherresc + " --repl /tmp/sfile2r  " + irodshome + "/foo5")
        self.admin.assert_icommand("iget -fK " + irodshome + "/foo5 " + dir_w + "/foo5")
        self.admin.assert_icommand("ireg -KCR " + self.testresc + " " + mysdir + " " + irodshome + "/icmdtesta")
        self.admin.assert_icommand("iget -fvrK " + irodshome + "/icmdtesta " + dir_w + "/testa", 'STDOUT', "testa")
        testuser2home = "/" + irodszone + "/home/" + self.user1.username
        self.user1.assert_icommand("ireg -KR " + self.testresc + " /tmp/sfile2c " +
                   testuser2home + "/foo5", 'STDERR', "PATH_REG_NOT_ALLOWED")
        self.user1.assert_icommand("iput -R " + self.testresc + " /tmp/sfile2c " + testuser2home + "/foo5")
        self.user1.assert_icommand("irm -f " + testuser2home + "/foo5")
        username = self.admin.username
        irodszone = self.admin.zone_name
        testuser1 = self.user0.username
        irodshome = self.admin.session_collection
        irodsdefresource = self.admin.default_resource
        self.admin.assert_icommand("imkdir icmdtest")
        self.admin.assert_icommand("iput -K --wlock " + progname + " " + irodshome + "/icmdtest/foo1")
        self.admin.assert_icommand("icp -K -R " + self.testresc + " " +
        self.admin.assert_icommand("ireg -KCR " + self.testresc + " " + mysdir + " " + irodshome + "/icmdtesta")
        self.admin.assert_icommand("imcoll -m link " + irodshome + "/icmdtesta " + irodshome + "/icmdtestb")
        self.admin.assert_icommand("ils -lr " + irodshome + "/icmdtestb", 'STDOUT', "icmdtestb")
        self.admin.assert_icommand("iget -fvrK " + irodshome + "/icmdtestb " + dir_w + "/testb", 'STDOUT', "testb")
        self.admin.assert_icommand("imcoll -U " + irodshome + "/icmdtestb")
        self.admin.assert_icommand("irm -rf " + irodshome + "/icmdtestb")
        self.admin.assert_icommand("imkdir " + irodshome + "/icmdtestm")
        self.admin.assert_icommand("imcoll -m filesystem -R " +
        self.admin.assert_icommand("imkdir " + irodshome + "/icmdtestm/testmm")
        self.admin.assert_icommand("iput " + progname + " " + irodshome + "/icmdtestm/testmm/foo1")
        self.admin.assert_icommand("iput " + progname + " " + irodshome + "/icmdtestm/testmm/foo11")
        self.admin.assert_icommand("imv " + irodshome +
        self.admin.assert_icommand("imv " + irodshome + "/icmdtestm/testmm " + irodshome + "/icmdtestm/testmm1")
        self.admin.assert_icommand("imv " + irodshome + "/icmdtestm/testmm1/foo2 " + irodshome + "/icmdtest/foo100")
        self.admin.assert_icommand("ils -l " + irodshome + "/icmdtest/foo100", 'STDOUT', "foo100")
        self.admin.assert_icommand("imv " + irodshome + "/icmdtestm/testmm1 " + irodshome + "/icmdtest/testmm1")
        self.admin.assert_icommand("ils -lr " + irodshome + "/icmdtest/testmm1", 'STDOUT', "foo11")
        self.admin.assert_icommand("irm -rf " + irodshome + "/icmdtest/testmm1 " + irodshome + "/icmdtest/foo100")
        self.admin.assert_icommand("iget -fvrK " + irodshome + "/icmdtesta " + dir_w + "/testm", 'STDOUT', "testm")
        self.admin.assert_icommand("imcoll -U " + irodshome + "/icmdtestm")
        self.admin.assert_icommand("irm -rf " + irodshome + "/icmdtestm")
        self.admin.assert_icommand("imkdir " + irodshome + "/icmdtestt_mcol")
        self.admin.assert_icommand("ibun -c " + irodshome + "/icmdtestx.tar " + irodshome + "/icmdtest")
        self.admin.assert_icommand("imcoll -m tar " + irodshome + "/icmdtestx.tar " + irodshome + "/icmdtestt_mcol")
        self.admin.assert_icommand("ils -lr " + irodshome + "/icmdtestt_mcol", 'STDOUT', ["foo2"])
        self.admin.assert_icommand("ils -lr " + irodshome + "/icmdtestt_mcol", 'STDOUT', ["foo1"])
        self.admin.assert_icommand("iget -vr " + irodshome + "/icmdtest  " + dir_w + "/testx", 'STDOUT', "testx")
        self.admin.assert_icommand("iget -vr " + irodshome +
                   "/icmdtestt_mcol/icmdtest  " + dir_w + "/testt", 'STDOUT', "testt")
        self.admin.assert_icommand("imkdir " + irodshome + "/icmdtestt_mcol/mydirtt")
        self.admin.assert_icommand("iput " + progname + " " + irodshome + "/icmdtestt_mcol/mydirtt/foo1mt")
        self.admin.assert_icommand("imv " + irodshome + "/icmdtestt_mcol/mydirtt/foo1mt " +
        self.admin.assert_icommand("imcoll -U " + irodshome + "/icmdtestt_mcol")
        username = self.admin.username
        irodszone = self.admin.zone_name
        testuser1 = self.user0.username
        irodshome = self.admin.session_collection
        irodsdefresource = self.admin.default_resource
        self.admin.assert_icommand("imkdir icmdtest")
        self.admin.assert_icommand("iput -K --wlock " + progname + " " + irodshome + "/icmdtest/foo1")
        self.admin.assert_icommand("icp -K -R " + self.testresc + " " +
        self.admin.assert_icommand("ibun -c " + irodshome + "/icmdtestx.tar " + irodshome + "/icmdtest")
        self.admin.assert_icommand("imkdir " + irodshome + "/icmdtestt_large")
        self.admin.assert_icommand("imcoll -m tar " + irodshome + "/icmdtestx.tar " + irodshome + "/icmdtestt_large")
        self.admin.assert_icommand("imkdir " + irodshome + "/icmdtestt_large/mydirtt")
        self.admin.assert_icommand("iput " + myldir + "/lfile1 " + irodshome + "/icmdtestt_large/mydirtt")
        self.admin.assert_icommand("iget " + irodshome + "/icmdtestt_large/mydirtt/lfile1 " + dir_w + "/testt")
        self.admin.assert_icommand("irm -rf " + irodshome + "/icmdtestt_large/mydirtt")
        self.admin.assert_icommand("imcoll -s " + irodshome + "/icmdtestt_large")
        self.admin.assert_icommand("imcoll -p " + irodshome + "/icmdtestt_large")
        self.admin.assert_icommand("imcoll -U " + irodshome + "/icmdtestt_large")
        self.admin.assert_icommand("irm -rf " + irodshome + "/icmdtestt_large")
        with lib.make_session_for_existing_admin() as rods_admin:
            rods_admin.run_icommand(['ichmod', 'own', self.admin.username, '/' + self.admin.zone_name])

        username = self.admin.username
        irodszone = self.admin.zone_name
        testuser1 = self.user0.username
        irodshome = self.admin.session_collection
        irodsdefresource = self.admin.default_resource
        self.admin.assert_icommand("imkdir icmdtest")
        self.admin.assert_icommand("iput -rR " + self.testresc + " " + mysdir + " " + irodshome + "/icmdtestp")
        self.admin.assert_icommand("iphybun -KR " + self.anotherresc + " " + irodshome + "/icmdtestp")
        self.admin.assert_icommand("itrim -rS " + self.testresc + " -N1 " +
                   irodshome + "/icmdtestp", 'STDOUT', "files trimmed")
        self.admin.assert_icommand("irepl --purgec -R " + self.anotherresc + " " + bunfile)
        self.admin.assert_icommand("itrim -rS " + self.testresc + " -N1 " +
                   irodshome + "/icmdtestp", 'STDOUT', "files trimmed")
        self.admin.assert_icommand("irm -f --empty " + bunfile)
        self.admin.assert_icommand("ils " + bunfile, 'STDOUT', bunfile)
        self.admin.assert_icommand("irm -rvf " + irodshome + "/icmdtestp", 'STDOUT', "num files done")
        self.admin.assert_icommand("irm -f --empty " + bunfile)
        username = self.admin.username
        irodszone = self.admin.zone_name
        testuser1 = self.user0.username
        irodshome = self.admin.session_collection
        irodsdefresource = self.admin.default_resource
        self.admin.assert_icommand("imkdir icmdtest")
        self.admin.assert_icommand("irsync " + progname + " i:" + irodshome + "/icmdtest/foo100")
        self.admin.assert_icommand("irsync i:" + irodshome + "/icmdtest/foo100 " + dir_w + "/foo100")
        self.admin.assert_icommand("irsync i:" + irodshome + "/icmdtest/foo100 i:" + irodshome + "/icmdtest/foo200")
        self.admin.assert_icommand("irm -f " + irodshome + "/icmdtest/foo100 " + irodshome + "/icmdtest/foo200")
        self.admin.assert_icommand("iput -R " + self.testresc + " " + progname + " " + irodshome + "/icmdtest/foo100")
        self.admin.assert_icommand("irsync " + progname + " i:" + irodshome + "/icmdtest/foo100")
        self.admin.assert_icommand("iput -R " + self.testresc + " " + progname + " " + irodshome + "/icmdtest/foo200")
        self.admin.assert_icommand("irsync i:" + irodshome + "/icmdtest/foo100 i:" + irodshome + "/icmdtest/foo200")
        username = self.admin.username
        irodszone = self.admin.zone_name
        testuser1 = self.user0.username
        irodshome = self.admin.session_collection
        irodsdefresource = self.admin.default_resource
        self.admin.assert_icommand("imkdir icmdtest")
        self.admin.assert_icommand("ilsresc", 'STDOUT', self.testresc)
        self.admin.assert_icommand("imiscsvrinfo", 'STDOUT', "relVersion")
        self.admin.assert_icommand("iuserinfo", 'STDOUT', "name: " + username)
        self.admin.assert_icommand("ienv", 'STDOUT', "Release Version")
        self.admin.assert_icommand("icd " + irodshome)
        self.admin.assert_icommand("ipwd", 'STDOUT', "home")
        self.admin.assert_icommand("ihelp ils", 'STDOUT', "ils")
        self.admin.assert_icommand("ierror -14000", 'STDOUT', "SYS_API_INPUT_ERR")
        self.admin.assert_icommand("iexecmd hello", 'STDOUT', "Hello world")
        self.admin.assert_icommand("ips -v", 'STDOUT', "ips")
        self.admin.assert_icommand("iqstat", 'STDOUT', "No delayed rules")
        self.admin.assert_icommand("imkdir " + irodshome + "/icmdtest1")
        self.admin.assert_icommand("iput -kf  " + progname + "  " + irodshome + "/icmdtest1/foo1")
        self.admin.assert_icommand("ils -l " + irodshome + "/icmdtest1/foo1", 'STDOUT', ["foo1", myssize])
        self.admin.assert_icommand("iadmin ls " + irodshome + "/icmdtest1", 'STDOUT', "foo1")
        self.admin.assert_icommand("ichmod read " + self.user0.username + " " + irodshome + "/icmdtest1/foo1")
        self.admin.assert_icommand("ils -A " + irodshome + "/icmdtest1/foo1",
                   'STDOUT', self.user0.username + "#" + irodszone + ":read")
        self.admin.assert_icommand("irepl -B -R " + self.testresc + " " + irodshome + "/icmdtest1/foo1")
        self.admin.assert_icommand("itrim -S  " + irodsdefresource + " -N1 " + irodshome + "/icmdtest1/foo1")
        self.admin.assert_icommand("iphymv -R  " + irodsdefresource + " " + irodshome + "/icmdtest1/foo1")
        self.admin.assert_icommand("imeta add -d " + irodshome + "/icmdtest1/foo1 testmeta1 180 cm")
        self.admin.assert_icommand("imeta ls -d " + irodshome + "/icmdtest1/foo1", 'STDOUT', "testmeta1")
        self.admin.assert_icommand("imeta ls -d " + irodshome + "/icmdtest1/foo1", 'STDOUT', "180")
        self.admin.assert_icommand("imeta ls -d " + irodshome + "/icmdtest1/foo1", 'STDOUT', "cm")
        self.admin.assert_icommand("icp -K -R " + self.testresc + " " +
        self.admin.assert_icommand("imv " + irodshome + "/icmdtest1/foo2 " + irodshome + "/icmdtest1/foo4")
        self.admin.assert_icommand("imv " + irodshome + "/icmdtest1/foo4 " + irodshome + "/icmdtest1/foo2")
        self.admin.assert_icommand("ichksum -K " + irodshome + "/icmdtest1/foo2", 'STDOUT', "foo2")
        self.admin.assert_icommand("iget -f -K " + irodshome + "/icmdtest1/foo2 " + dir_w)
        self.admin.assert_icommand("irsync " + progname + " i:" + irodshome + "/icmdtest1/foo1")
        self.admin.assert_icommand("irsync i:" + irodshome + "/icmdtest1/foo1 /tmp/foo1")
        self.admin.assert_icommand("irsync i:" + irodshome + "/icmdtest1/foo1 i:" + irodshome + "/icmdtest1/foo2")
        username = self.admin.username
        irodszone = self.admin.zone_name
        testuser1 = self.user0.username
        irodshome = self.admin.session_collection
        irodsdefresource = self.admin.default_resource
        self.admin.assert_icommand("imkdir icmdtest")
        self.admin.assert_icommand("iput -vbPKr --retries 10 --wlock -X " + rsfile + " --lfrestart " +
                   lrsfile + " -N 2 " + myldir + " " + irodshome + "/icmdtest/testy", 'STDOUT', "New restartFile")
        self.admin.assert_icommand("ichksum -rK " + irodshome + "/icmdtest/testy", 'STDOUT', "Total checksum performed")
        self.admin.assert_icommand("irepl -BvrPT -R " + self.testresc + " --rlock " +
                   irodshome + "/icmdtest/testy", 'STDOUT', "icmdtest/testy")
        self.admin.assert_icommand("itrim -vrS " + irodsdefresource + " --dryrun --age 1 -N 1 " +
                   irodshome + "/icmdtest/testy", 'STDOUT', "This is a DRYRUN")
        self.admin.assert_icommand("itrim -vrS " + irodsdefresource + " -N 1 " +
                   irodshome + "/icmdtest/testy", 'STDOUT', "a copy trimmed")
        self.admin.assert_icommand("icp -vKPTr -N 2 " + irodshome + "/icmdtest/testy " +
                   irodshome + "/icmdtest/testz", 'STDOUT', "Processing lfile1")
        self.admin.assert_icommand("irsync -r i:" + irodshome + "/icmdtest/testy i:" + irodshome + "/icmdtest/testz")
        self.admin.assert_icommand("irm -vrf " + irodshome + "/icmdtest/testy")
        self.admin.assert_icommand("iphymv -vrS " + irodsdefresource + " -R " +
                   self.testresc + " " + irodshome + "/icmdtest/testz", 'STDOUT', "icmdtest/testz")
        self.admin.assert_icommand("iget -vPKr --retries 10 -X " + rsfile + " --lfrestart " + lrsfile +
                   " --rlock -N 2 " + irodshome + "/icmdtest/testz " + dir_w + "/testz", 'STDOUT', "testz")
        self.admin.assert_icommand("irsync -r " + dir_w + "/testz i:" + irodshome + "/icmdtest/testz")
        self.admin.assert_icommand("irsync -r i:" + irodshome + "/icmdtest/testz " + dir_w + "/testz")
        self.admin.assert_icommand("iput -N0 -R " + self.testresc + " " +
        self.admin.assert_icommand("iget -N0 " + irodshome + "/icmdtest/testz/lfoo100 " + dir_w + "/lfoo100")
        self.admin.assert_icommand("irm -vrf " + irodshome + "/icmdtest/testz")
        username = self.admin.username
        irodszone = self.admin.zone_name
        testuser1 = self.user0.username
        irodshome = self.admin.session_collection
        irodsdefresource = self.admin.default_resource
        self.admin.assert_icommand("imkdir icmdtest")
        self.admin.assert_icommand("iput -vQPKr --retries 10 -X " + rsfile + " --lfrestart " +
                   lrsfile + " " + myldir + " " + irodshome + "/icmdtest/testy", 'STDOUT', "icmdtest/testy")
        self.admin.assert_icommand("irepl -BQvrPT -R " + self.testresc + " " +
                   irodshome + "/icmdtest/testy", 'STDOUT', "icmdtest/testy")
        self.admin.assert_icommand("itrim -vrS " + irodsdefresource + " -N 1 " +
                   irodshome + "/icmdtest/testy", 'STDOUT', "a copy trimmed")
        self.admin.assert_icommand("icp -vQKPTr " + irodshome + "/icmdtest/testy " +
                   irodshome + "/icmdtest/testz", 'STDOUT', "Processing sfile1")
        self.admin.assert_icommand("irm -vrf " + irodshome + "/icmdtest/testy")
        self.admin.assert_icommand("iget -vQPKr --retries 10 -X " + rsfile + " --lfrestart " + lrsfile +
                   " " + irodshome + "/icmdtest/testz " + dir_w + "/testz", 'STDOUT', "Processing sfile2")
        self.admin.assert_icommand("irm -vrf " + irodshome + "/icmdtest/testz")
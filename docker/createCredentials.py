import sys
import os
import stat
from irods.password_obfuscation import encode

filename = sys.argv[1]
admin_password = sys.argv[2]
uid = int(sys.argv[3])

with open(filename, "w") as f:
    f.write(encode(admin_password, uid=uid))

os.chmod(filename, stat.S_IREAD | stat.S_IWRITE)
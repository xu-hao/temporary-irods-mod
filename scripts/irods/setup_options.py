from . import start_options
import json

def add_options(parser):
    parser.add_option('--database',
                      dest='database', action="store_true",
                      help='Setup database.')

    parser.add_option('--provider',
                      dest='provider', action="store_true",
                      help='Setup provider.')

    parser.add_option('--consumer',
                      dest='consumer', action="store_true",
                      help='Setup consumer.')

    parser.add_option('--resc',
                      dest='resc', action="store_true",
                      help='Setup resource.')

    parser.add_option('-d', '--database_type',
                      dest='database_type', metavar='DB_TYPE',
                      help='The type of database to be used by the iRODS '
                      'Catalog (ICAT) server. Valid values are '
                      '\'postgres\', \'mysql\', and \'oracle\'. This option '
                      'is required to set up an ICAT server and ignored for '
                      'a resource server.')

    parser.add_option('--ld_library_path',
                      dest='ld_library_path', metavar='ENV_VAR',
                      help='The LD_LIBRARY_PATH to use when running iRODS. '
                      'This is primarily useful for linking ODBC libraries '
                      'installed in non-standard locations. Since \'sudo\' '
                      'scrubs all LD_* environment variables and the irods '
                      'user has no guarantee of inheriting your environment '
                      'at all, this may need to be set even if you have '
                      'LD_LIBRARY_PATH configured in your environment.')

    parser.add_option('--json_configuration_file',
            dest='json_configuration_file', metavar='FILE_PATH',
            help='The json file to use when setting up the server. '
            'This option will ingest a json file instead of '
            'prompting the user for input to perform server '
            'configuration.')

    start_options.add_options(parser)

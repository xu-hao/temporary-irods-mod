FROM irods:4.3.0

COPY irods_config_resc.json ./

RUN python /var/lib/irods/scripts/setup_irods.py --consumer --json_configuration_file=irods_config_resc.json

EXPOSE 1247
EXPOSE 1248
EXPOSE 20000-20199

WORKDIR /usr/sbin

ENTRYPOINT [ "irodsServer", "-u" ]


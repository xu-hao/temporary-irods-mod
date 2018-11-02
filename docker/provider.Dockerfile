FROM irods:4.3.0

COPY irods_config.json ./

RUN python /var/lib/irods/scripts/setup_irods.py --provider --json_configuration_file=irods_config.json

EXPOSE 1247
EXPOSE 1248
EXPOSE 20000-20199

WORKDIR /usr/sbin

ENTRYPOINT [ "irodsServer", "-u" ]


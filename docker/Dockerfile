FROM ubuntu:16.04

RUN apt-get update && apt-get install -y wget lsb-release apt-transport-https
RUN wget -qO - https://packages.irods.org/irods-signing-key.asc | apt-key add -
RUN echo "deb [arch=amd64] https://packages.irods.org/apt/ $(lsb_release -sc) main" | tee /etc/apt/sources.list.d/renci-irods.list
RUN apt-get update
# RUN apt-get install irods-externals-* 

COPY irods-server_4.3.0~xenial_amd64.deb irods-database-plugin-postgres_4.3.0~xenial_amd64.deb irods-dev_4.3.0~xenial_amd64.deb irods-runtime_4.3.0~xenial_amd64.deb irods-icommands_4.3.0~xenial_amd64.deb ./

RUN dpkg -i irods-server_4.3.0~xenial_amd64.deb irods-database-plugin-postgres_4.3.0~xenial_amd64.deb irods-dev_4.3.0~xenial_amd64.deb irods-runtime_4.3.0~xenial_amd64.deb irods-icommands_4.3.0~xenial_amd64.deb || true && \
    apt install -fy

RUN apt-get install -y libxml2

ENTRYPOINT [ "python", "/var/lib/irods/scripts/setup_irods.py" ]
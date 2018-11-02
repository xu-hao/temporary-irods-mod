## building images
Copy the following to current directory:
```
irods-database-plugin-mysql_4.3.0~xenial_amd64.deb
irods-database-plugin-oracle_4.3.0~xenial_amd64.deb
irods-database-plugin-postgres_4.3.0~xenial_amd64.deb
irods-dev_4.3.0~xenial_amd64.deb
irods-icommands_4.3.0~xenial_amd64.deb
irods-runtime_4.3.0~xenial_amd64.deb
irods-server_4.3.0~xenial_amd64.deb
```
Build image:
```
docker build -t irods:4.3.0 .
```

### setup provider

Edit the following in current directory:
```
irods_config.json
```
Build image:
```
docker build -t irods-provider:4.3.0 -f provider.Dockerfile .
```

### setup consumer

Edit the following in current directory:
```
irods_config_resc.json
```
Build image:
```
docker build -t irods-consumer:4.3.0 -f consumer.Dockerfile .
```


## deployment
### setup network

Create a docker network
```
docker network create -d bridge irods-network
```

### setup database
Start postgresql server:
```
docker run --rm --net irods-network --name icat -e POSTGRES_PASSWORD=root -d postgres
```

Setup ICAT 

Set up database as instructed by iRODS documentation
```
docker exec icat psql -U postgres -c "create database \"ICAT\""
docker exec icat psql -U postgres -c "create user irods with password 'testpassword'"
docker exec icat psql -U postgres -c "grant all on database \"ICAT\" to irods"
```

Edit the following in current directory:
```
irods_db_config.json
```
`fish`
```
docker run -it --rm --net irods-network --mount type=bind,source=(pwd)/irods_db_config.json,target=/irods_db_config.json,readonly --hostname irods-provider irods:4.3.0 --database --json_configuration_file irods_db_config.json
```


### setup resource
Edit the following in current directory:
```
irods_db_config_resc.json
```
`fish`
```
docker run -it --rm --net irods-network --mount type=bind,source=(pwd)/irods_db_config_resc.json,target=/irods_db_config_resc.json,readonly irods:4.3.0 --resc --json_configuration_file irods_db_config_resc.json
```


### run provider

Install Python irods client
```
pip install python-irodsclient
```
Initialize container with admin password and uid. The following example uses "rods" and 0.
```
python createCredentials.py irodsA rods 0
```

```
docker run --net irods-network -d --rm --name irods-provider --hostname irods-provider --init --shm-size=1g --mount type=bind,source=(pwd)/irodsA,target=/root/.irods/.irodsA,readonly irods-provider:4.3.0
```
### run consumer wip
```
docker run --net irods-network -d --rm --name irods-consumer --hostname irods-consumer --init --shm-size=1g --mount type=bind,source=(pwd)/irodsA,target=/root/.irods/.irodsA,readonly irods-consumer:4.3.0
```

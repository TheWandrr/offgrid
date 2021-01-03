LDFLAGS = -lwiringPi -lpthread -lsystemd -lmosquitto -lsqlite3

DB_EXISTS := $(or $(and $(wildcard /usr/local/lib/mqtt.db),1),0)

#.PHONY: all clean

all : offgrid-daemon

offgrid-daemon : offgrid-daemon.o
	${CXX} $^ -o $@ ${LDFLAGS}

#parse_message.o : parse_message.c
#	${CXX} -c $^ -o $@ ${CFLAGS}

#mqtt_manager.o : mqtt_manager.cpp
#	${CXX} -c $^ -o $@ ${CFLAGS}

#io_controller.o : io_controller.cpp
#	${CXX} -c $^ -o $@ ${CFLAGS}

clean :
	@-rm -f *.o offgrid-daemon

install : all
ifeq ($(DB_EXISTS), 0)
	@sqlite3 /usr/local/lib/mqtt.db < mqtt_db_schema.sql
endif
	@-systemctl stop offgrid-daemon
	@chown root:root ./offgrid-daemon.service ./offgrid-daemon
	@chmod 664 ./offgrid-daemon.service
	@cp ./offgrid-daemon.service /etc/systemd/system/
	@cp ./offgrid-daemon /usr/local/lib/
	@systemctl daemon-reload
	@systemctl enable offgrid-daemon
	@systemctl restart offgrid-daemon

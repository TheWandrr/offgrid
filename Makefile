TARGET = offgrid-daemon
LIBS = -lm -lwiringPi -lpthread -lsystemd -lmosquitto -lsqlite3
CC = gcc
#CFLAGS = -Wall -fvisibility=hidden # Release
CFLAGS = -g -O0 -fvisibility=hidden # Debugging

DB_EXISTS := $(or $(and $(wildcard /usr/local/lib/mqtt.db),1),0)

.PHONY: default all clean

default: $(TARGET)
all: default

OBJECTS = $(patsubst %.c, %.o, $(wildcard *.c))
HEADERS = $(wildcard *.h)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

.PRECIOUS: $(TARGET) $(OBJECTS)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -Wall $(LIBS) -o $@

#offgrid-daemon : offgrid-daemon.o
#	${CXX} $^ -o $@ ${LDFLAGS}

clean:
	@-rm -f *.o
	@-rm -f $(TARGET)

install: all
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

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <pthread.h>

#include <systemd/sd-daemon.h>

#include <sqlite3.h>
#include <mosquitto.h>
#include <wiringSerial.h>
//#include <argp.h>

#include "offgrid_constants.h"

// TODO: Implement control over how much information is output to logs
// TODO: Make software battery monitor process optional with switch

//const char *argp_program_version = "offgrid-daemon 0.0.1";
//const char *argp_program_bug_address = "";

//static char doc[] = "Offgrid hardware daemon -- UART to MQTT bridge";

void ParseMessage(char *msg_buf);
void PublishRequestReturn(unsigned int address, long data);
void LogToDatabase(const char *topic, const long data, const uint64_t now);

sqlite3 *db;
const char *DB_FILENAME = "/usr/local/lib/mqtt.db";

enum ReceiveState {
        GET_STX,
        GET_DATA,
};

const int ADDR_INTERNAL_ONLY = -1;

// Only used for memory mapped data from UART-connected hardware
struct BridgeMap {
    const int address;
    const unsigned int bytes;
    const char *topic;
    const bool readable;
    const bool writable;
    const char *format;
    const double multiplier;
    long data;
    uint64_t data_timestamp;
};

static volatile int running = 1;
pthread_t process_rx_thread;
pthread_t process_tx_thread;
pthread_t process_sunrise;

double house_battery_amps = 0;
double house_battery_volts = 0;
double state_of_charge = 0;

const char *mqtt_host = "localhost";
const unsigned int mqtt_port = 1883;

// Used to translate between UART-connected hardware memory-mapped variables and MQTT topics
// Fixed point data is shifted into an integer value for serial transmission, database storage, and most recent cached value
struct BridgeMap lookup_map[] = {
    // Switches
    { 0x10, 4, "og/setting/broadcast_period_ms", 1, 1, "%0.0f", 1, 0, 0 },

    // Firmware battery monitor variables (RAM)
    { 0x20, 2, "og/batmon/bank0/volts", 1, 0, "%0.2f", 0.01, 0, 0 },
    { 0x21, 2, "og/batmon/bank0/amps", 1, 0, "%0.1f", 0.1, 0, 0 },
    { 0x22, 2, "og/batmon/bank0/ah", 1, 1, "%0.1f", 0.1, 0, 0 },
    { 0x23, 2, "og/batmon/bank0/soc", 1, 1, "%0.1f", 0.01, 0, 0 },

    // Firmware battery monitor constants (EEPROM)
    { 0x24, 4, "og/batmon/bank0/amps_multiplier", 1, 1, "%0.6f", 0.000001, 0, 0 },
    { 0x25, 4, "og/batmon/bank0/volts_multiplier", 1, 1, "%0.6f", 0.000001, 0, 0 },
    { 0x26, 2, "og/batmon/bank0/amphours_capacity", 1, 1, "%0.1f", 0.1, 0, 0 },
    { 0x27, 2, "og/batmon/bank0/volts_charged", 1, 1, "%0.3f", 0.001, 0, 0 },
    { 0x28, 2, "og/batmon/bank0/minutes_charged_detection_time", 1, 1, "%0.1f", 0.1, 0, 0 },
    { 0x29, 4, "og/batmon/bank0/current_threshold", 1, 1, "%0.6f", .000001, 0, 0 },
    { 0x2A, 1, "og/batmon/bank0/tail_current_factor", 1, 1, "%0.2f", .01, 0, 0 },
    { 0x2B, 1, "og/batmon/bank0/peukert_factor", 1, 1, "%0.2f", .01, 0, 0 },
    { 0x2C, 1, "og/batmon/bank0/charge_efficiency_factor", 1, 1, "%0.2f", .01, 0, 0 },

//    { 0x30, 2, "og/batmon/bank1/volts", 1, 0, "%0.2f", 0.01 },
//    { 0x31, 2, "og/batmon/bank1/amps", 1, 0, "%0.1f", 0.1 },
//    { 0x32, 2, "og/batmon/bank1/ah", 1, 1, "%0.1f", 0.1 },
//    { 0x33, 2, "og/batmon/bank1/soc", 1, 1, "%0.1f", 0.01 },

    // PWM outputs
    { 0xA0, 1, "og/house/light/ceiling", 1, 1, "%0.0f", 1, 0, 0 },
    //{ 0xA1, 1, "", 1, 1, "", (1) },
    //{ 0xA2, 1, "", 1, 1, "", (1) },
    //{ 0xA3, 1, "", 1, 1, "", (1) },
    //{ 0xA4, 1, "", 1, 1, "", (1) },
    //{ 0xA5, 1, "", 1, 1, "", (1) },

    // ON/OFF outputs
    { 0xA6, 1, "og/house/light/ceiling_encoder", 1, 1, "%0.0f", 1, 0, 0 },
    //{ 0xA7, 1, "", 1, 1, "", (1) },

    // Differential ADC inputs, 75mV shunt ** SIGNED 16 BIT
    { 0xB0, 2, "og/house/battery/amps", 1, 0, "%0.1f", (0.0000078125 * 500.0 / 0.050 * 1.031), 0, 0 }, // Calibrated at 2.47A
    { 0xB1, 2, "og/house/fuse_panel/amps", 1, 0, "%0.1f", (0.0000078125 * 50.0 / 0.075), 0, 0 },
    { 0xB2, 2, "og/house/vehicle_in/amps", 1, 0, "%0.1f", (0.0000078125 * 200.0 / 0.075), 0, 0 },
    { 0xB3, 2, "og/house/inverter/amps", 1, 0, "%0.1f", (0.0000078125 * 200.0 / 0.075), 0, 0 },

    // Single ended ADC inputs, 12V
    // !! RAW values no longer used !!
//    { 0xB4, 2, "og/house/battery/volts", 1, 0, "%0.2f", (0.000125 * 4 * 1.05350), 0, 0 }, // Calibrated @ 13.85 V
//    { 0xB5, 2, "og/vehicle/battery/volts", 1, 0, "%0.2f", (0.000125 * 4 * 1.04390), 0, 0 }, // Calibrated at 14.42 V
    //{ 0xB6, 2, "", 1, 1, "", (1) },
    //{ 0xB7, 2, "", 1, 1, "", (1) },
};

// Returns epoch time in whole centiseconds (seconds * 0.01)
uint64_t timestamp(void) {
    struct timespec spec;

    clock_gettime(CLOCK_REALTIME, &spec);

    return ( spec.tv_sec + (spec.tv_nsec / 1.0e9) ) * 100ull;
}

int AddressToTopic(const unsigned int address) {
    int i;

    for (i = 0; i < ( sizeof(lookup_map) / sizeof(lookup_map[0]) ); i++) {
        if (lookup_map[i].address == address) {
            return i;
        }
    }

    return -1;
}

int TopicToAddress(const char *topic) {
    int i;

    for (i = 0; i < ( sizeof(lookup_map) / sizeof(lookup_map[0]) ); i++) {
        if ( !strcmp(lookup_map[i].topic, topic) ) {

            //printf("TopicToAddress MATCHED: %s at i=%d\r\n", topic, i);
            return i;
        }
    }

    return -1;
}


// TODO: Eventually move these to classes
int fd; // File descriptor for serial port
// TODO: Circular Buffer rx_uart
// TODO: Circular Buffer tx_uart
static char message_buffer[32] = {0}; // KEEP INCOMING MESSAGES SMALL!
struct mosquitto *mqtt;

void SignalHandler(int signum)
{
	running = 0;
}

// TODO: Change this to a variadic function so we can use format strings and arguments
//void send_message(const char *message) {
//	serialPutchar(fd, '\x02');
//	serialPrintf(fd, message);
//	serialPutchar(fd, '\x03');
//}

void *ProcessReceiveThread(void *param) {
	static enum ReceiveState receive_state = GET_STX;
	char c;

	while(running) {

		if(serialDataAvail(fd)) {
			switch(receive_state) {
				case GET_STX:
					//received_message = false;
					if( (c = serialGetchar(fd)) == '\x02' ) {
						////Serial.print("<STX>");
						strncpy(message_buffer, "", sizeof(message_buffer));
						receive_state = GET_DATA;
					}
				break;

				case GET_DATA:
					c = serialGetchar(fd);

					if( isprint(c) ) {
						// Valid data, save/buffer it for later
						////Serial.print(c);
						strncat(message_buffer, &c, 1);
					}
					else if( c == '\x03' ) {
						////Serial.println("<ETX>");
						ParseMessage(message_buffer);
						receive_state = GET_STX;
					}
					else if( c == '\x02' ) {
						// ERROR - expected ETX before STX
						////Serial.print("   <<<   MISSING ETX\r\n<RECOVER-STX>");
						printf("<UART> ERROR: New packet began before previous packet finished\r\n");
						strncpy(message_buffer, "", sizeof(message_buffer));
					}
				break;

			}
		}
	}
}

unsigned int asciiHexToInt(char ch) {
  unsigned int num = 0;
  if( (ch >= '0') && (ch <= '9') ) {
    num = ch - '0';
  }
  else {
    switch(ch) {
      case 'A': case 'a': num = 10; break;
      case 'B': case 'b': num = 11; break;
      case 'C': case 'c': num = 12; break;
      case 'D': case 'd': num = 13; break;
      case 'E': case 'e': num = 14; break;
      case 'F': case 'f': num = 15; break;
      default: num = 0;
    }
  }

  return num;
}

void ParseMessage(char *msg_buf) {
  char *p = msg_buf;
  uint8_t count = 0;
  uint16_t address = 0;
  uint32_t value_buffer = 0;
  bool invalid_message = false; // Any invalid message will be ignored.  Reporting anything more isn't useful.

  // LIMITED EMBEDDED IMPLEMENTATION
  //   One address and up to four 32-bit parameters will be accepted.  The remainder will be discarded silently.
  uint32_t arg[4] = {0};
  uint8_t arg_count = 0;

  // Message to be composed of only PRINTABLE ASCII - isprint() !
  // Message format is addr:XX, XX, ...
  //   Where a 4-character ASCII-HEX is used for an address
  //   ... followed by optional colon
  //   ... followed by a comma-separated list of ASCII-HEX or STRING values of variable length
  //   ... string values have an additional marker character pre-pended TBD

  //consumeWhitespace(&p); // TODO: Not working

  // Empty message is invalid.  Parsing loop will be skipped.
  if(*p == '\0') {
    invalid_message = true;
  }

  while ( (*p != '\0') && (!invalid_message) ) {
    // Get up to 4 ASCII-HEX characters for address
    address = 0;
    count = 0;

    if( !isxdigit(*p) ) {
      // ERROR: Address did not start with hex digit
      invalid_message = true;
      break;
    }

    //while ( (*p != '\0') && (*p != ':') && isxdigit(*p) ) {
    while ( isxdigit(*p) ) {
      address <<= 4;
      address += asciiHexToInt(*p);

      count++;
      p++;
    }

    //DEBUG//Serial.print('['); Serial.print(address, HEX); Serial.print(']');

    if (count > 4) {
      // ERROR - address must maximum 4 ASCII-HEX characters
      invalid_message = true;
      break;
    }

    // Colon (continue), end of string (done), else (error)
    if(*p == ':') {
      p++; // Consume colon

      // Get list following colon
      arg_count = 0;
      do {
        if(isxdigit(*p)) {
          // Get ascii-hex digits up to comma or end of string
          count = 0;
          value_buffer = 0;
          while ( isxdigit(*p) ) {
            value_buffer <<= 4;
            value_buffer += asciiHexToInt(*p);

            count++;
            p++;
          }

          if (count > 8) {
            // ERROR - max data size is 4 bytes or 8 ascii-hex characters
            invalid_message = true;
            break;
          }
          else {
            // Looks like we have good data.  Do something with it before it goes away in the next loop iteration.
            arg[arg_count] = value_buffer;
            arg_count++;

            if( arg_count > ( sizeof(arg) / sizeof(arg[0]) ) ) {
              // ERROR: IMPOSED ARBITRARY EMBEDDED LIMITATION
              invalid_message = true;
              break;
            }
          }

          // If we're looking at a the comma following a value, consume it.
          if (*p == ',') {
            p++;
            //DEBUG//Serial.print(';');
          }
          else if (*p != '\0') {
            // ERROR - Only a comma or end of string should follow a value
            invalid_message = true;
            break;
          }
        }
//        else if (*p == '"') {
//          // get everything up to comma or end of string
//        }
        else {
          // ERROR - invalid contents
          invalid_message = true;
          break;
        }
      } while (*p != '\0');

      if(invalid_message) {
        // Something went wrong in the above parsing loop getting the list.  Get out of the outer loop as well.
        break;
      }

    }
    else if (*p != '\0') {
      // ERROR: Something unexpected followed the address
      invalid_message = true;
      break;
    }

    p++;
  }

  if ( !invalid_message ) {

    switch(address) {

	case MSG_KEEP_ALIVE: // Sent by system master (or designate) to ensure bus is operating.  This module will be automatically reset by the watchdog timer if not received in time.
		if(arg_count == 1) {
			//DEBUG//printf(">> <UART> MSG_KEEP_ALIVE: %0.2X\r\n", (uint8_t)arg[0]); fflush(NULL);
			mosquitto_publish(mqtt, NULL, "og/status/tick", 0, "", 0, false);
	        //DEBUG//printf("<< <MQTT> %s = %s\r\n", "og/status/tick", ""); fflush(NULL);
		}
	break;

	case MSG_GET_SET_ERROR:
		if(arg_count == 1) {
			printf(">> <UART> MSG_GET_SET_ERROR: %0.2X\r\n", (uint8_t)arg[0]); fflush(NULL);
		}
	break;

	case MSG_RETURN_8_8:
		if(arg_count == 2) {
			//DEBUG//printf(">> <UART> MSG_RETURN_8_8: %0.2X, %0.2X\r\n", (uint8_t)arg[0], (uint8_t)arg[1]); fflush(NULL);
			PublishRequestReturn( (uint8_t)arg[0], (int8_t)arg[1] );
		}
	break;

	case MSG_RETURN_8_16:
		if(arg_count == 2) {
			//DEBUG//printf(">> <UART> MSG_RETURN_8_16: %0.2X, %0.4X\r\n", (uint8_t)arg[0], (uint16_t)arg[1]); fflush(NULL);
			PublishRequestReturn( (uint8_t)arg[0], (int16_t)arg[1] );
		}
	break;

	case MSG_RETURN_8_32:
		if(arg_count == 2) {
			//DEBUG//printf(">> <UART> MSG_RETURN_8_32: %0.2X, %0.8lX\r\n", (uint8_t)arg[0], (uint32_t)arg[1]); fflush(NULL);
			PublishRequestReturn( (uint8_t)arg[0], (uint32_t)arg[1] );
		}
	break;
    }

  }
  else {
    //DEBUG//Serial.println("  <-- INVALID");
  }
}

// Not strictly necessary but will allow control of message sending frequency
void *ProcessTransmitThread(void *param) {
	while(running) {
		//printf("ProcessTransmitThread()\r\n");
		//fflush(NULL);
		//sleep(1);
	}
}

// TODO: enqueueMessage - add message to transmit queue, use variadic function printf style

//void connect_callback(struct mosquitto *mosq, void *obj, int result) {
//}

void PublishRequestReturn(unsigned int address, long data) {
	char payload[256];
	int payloadlen;
    uint64_t now;
    int i;

    if( (i = AddressToTopic(address)) >= 0 ) {
        now = timestamp();
        payloadlen = sprintf( payload, lookup_map[i].format, data * lookup_map[i].multiplier ) + 1;
        //DEBUG//printf("<< <MQTT> %s = %s\r\n", lookup_map[i].topic, payload); fflush(NULL);

        mosquitto_publish(mqtt, NULL, lookup_map[i].topic, payloadlen, payload, 0, false);

        // Only store to database if it has changed since the last time
        if(lookup_map[i].data != data) {
            LogToDatabase(lookup_map[i].topic, data, now);

            lookup_map[i].data_timestamp = now;
            lookup_map[i].data = data;
        }
    }
}

// Not used right now but maybe in the future
//static int database_callback(void *not_used, int argc, char **argv, char **col_name) {
//    for (int i = 0; i < argc; i++) {
//        printf("%s = %s\r\n", col_name[i], argv[i] ? argv[i] : "NULL");
//    }
//    printf("\n");
//    return 0;
//}

void LogToDatabase(const char *topic, const long data, const uint64_t now) {
    char *err_msg = 0;
    int rc;
    sqlite3_stmt *stmt;
    int row_count;

    // TODO: Check that topic exists first.  Output error and skip insertion if it doesn't.

    if( sqlite3_prepare_v2(db, "INSERT INTO message(payload,timestamp,topic_id) VALUES(?1,?2,(SELECT id FROM topic WHERE name=?3));", -1, &stmt, NULL) ) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
        return;
    }

    rc = sqlite3_bind_int64(stmt, 1, data);
    if(rc != SQLITE_OK) {
        fprintf(stderr, "Failed to bind data: %s\n", sqlite3_errmsg(db));
    }

    rc = sqlite3_bind_int64(stmt, 2, now);
    if(rc != SQLITE_OK) {
        fprintf(stderr, "Failed to bind timestamp: %s\n", sqlite3_errmsg(db));
    }

    rc = sqlite3_bind_text(stmt, 3, topic, -1, SQLITE_TRANSIENT);
    if(rc != SQLITE_OK) {
        fprintf(stderr, "Failed to bind topic: %s\n", sqlite3_errmsg(db));
    }

    if ( sqlite3_step(stmt) != SQLITE_DONE ) {
        fprintf(stderr, "Failed to execute statement: %s\n", sqlite3_errmsg(db));
    }

    sqlite3_finalize(stmt);
}

bool StringHasSuffix(const char *s, const char *suffix) {
    int suffix_len = strlen(suffix);
    int s_len = strlen(s);
    int diff_len = s_len - suffix_len;

    if( (suffix_len > 0) && ( s_len > 0) && (diff_len >= 0) ) {
        for (int i = s_len - 1; i >= (diff_len); i --) {
            if( s[i] != suffix[i - diff_len] ) {
                return false;
            }
        }
        return true;
    }
    return false;
}

void message_callback(struct mosquitto *mosq, void *obj, const struct mosquitto_message *message) {
    int i;
    char *suffix_set = "/set"; // Append to a topic to write a value
    char *suffix_get = "/get"; // Append to a topic to force it to be published immediately
    char topic[255];
    int newlen;
	char payload[16];
	int payloadlen;

	//DEBUG//printf(">> <MQTT> %s = %s\r\n", message->topic, message->payload); fflush(NULL);

    // *** If message ends with "/set", it needs to be handled differently
    if( StringHasSuffix(message->topic, suffix_set) ) {
        //printf("SET message found: %s\r\n", message->topic);

        // Strip suffix and try to match
        newlen = strlen(message->topic) - strlen(suffix_set);
        newlen = ( (newlen > 254) ? 254 : newlen);
        strncpy(topic, message->topic, newlen);
        topic[newlen] = '\0';
        //printf("Stripped Topic: %s\r\n", topic);

        if( (i = TopicToAddress(topic)) >= 0) {
            //printf("SET matched topic: %d, %s\r\n", i, lookup_map[i].topic);
            if( (lookup_map[i].writable == 1) && (lookup_map[i].bytes > 0) ) {

                if(lookup_map[i].address == ADDR_INTERNAL_ONLY) {
                    lookup_map[i].data_timestamp = timestamp();
                    lookup_map[i].data = atof(message->payload) / lookup_map[i].multiplier;
                }
                else {

                    // Don't update the cached data here.  It will happen when it gets confirmed by the MSG_RETURN_X_X.

                    serialPutchar(fd, '\x02');

                    switch(lookup_map[i].bytes) {

                        case 1:
			                printf("<< <UART> MSG_SET_8_8: %0.2X, %0.2X\r\n", (uint8_t)lookup_map[i].address, (uint8_t)( atof(message->payload) / lookup_map[i].multiplier  ) ); fflush(NULL);
		                    serialPrintf( fd, "%0.2X:%0.2X,%0.2X", (uint8_t)MSG_SET_8_8, (uint8_t)lookup_map[i].address, (uint8_t)( atof(message->payload) / lookup_map[i].multiplier ) );
                        break;

                        case 2:
		           		    printf("<< <UART> MSG_SET_8_16: %0.2X, %0.4X\r\n", (uint8_t)lookup_map[i].address, (uint16_t)( atof(message->payload) / lookup_map[i].multiplier ) ); fflush(NULL);
                            serialPrintf( fd, "%0.2X:%0.2X,%0.4X", (uint8_t)MSG_SET_8_16, (uint8_t)lookup_map[i].address, (uint16_t)( atof(message->payload) / lookup_map[i].multiplier ) );
                        break;

//                        case 3:
//		                      serialPrintf( fd, "0.2X:%0.2X,%0.6X", (uint8_t)MSG_SET_8_24, (uint8_t)lookup_map[i].address, (uint32_t)(atoi(message->payload)) );
//                        break;

                        case 4:
			                printf("<< <UART> MSG_SET_8_32: %0.2X, %0.8lX\r\n", (uint8_t)lookup_map[i].address, (uint32_t)( atof(message->payload) / lookup_map[i].multiplier ) ); fflush(NULL);
		                    serialPrintf( fd, "%0.2X:%0.2X,%0.8lX", (uint8_t)MSG_SET_8_32, (uint8_t)lookup_map[i].address, (uint32_t)( atof(message->payload) / lookup_map[i].multiplier ) );
                        break;
                    }

                    serialPutchar(fd, '\x03');

                }
            }
        }
    }
    else if( StringHasSuffix(message->topic, suffix_get) ) {
        //printf("GET message found: %s\r\n", message->topic);

        // Strip suffix and try to match
        newlen = strlen(message->topic) - strlen(suffix_get);
        newlen = ( (newlen > 254) ? 254 : newlen);
        strncpy(topic, message->topic, newlen);
        topic[newlen] = '\0';
        //printf("Stripped Topic: %s\r\n", topic);

        if( (i = TopicToAddress(topic)) >= 0) {
            printf("GET matched topic: %d, %s\r\n", i, lookup_map[i].topic);
            if( (lookup_map[i].readable == 1) && (lookup_map[i].bytes > 0) ) {

                serialPutchar(fd, '\x02');

                switch(lookup_map[i].bytes) {

                    case 1:
                        printf("<< <UART> MSG_GET_8_8: %0.2X\r\n", (uint8_t)lookup_map[i].address ); fflush(NULL);
		                serialPrintf( fd, "%0.2X:%0.2X", (uint8_t)MSG_GET_8_8, (uint8_t)lookup_map[i].address );
                    break;

                    case 2:
		           	    printf("<< <UART> MSG_GET_8_16: %0.2X\r\n", (uint8_t)lookup_map[i].address ); fflush(NULL);
                        serialPrintf( fd, "%0.2X:%0.2X", (uint8_t)MSG_GET_8_16, (uint8_t)lookup_map[i].address );
                    break;

//                    case 3:
//		                  serialPrintf( fd, "0.2X:%0.2X", (uint8_t)MSG_GET_8_24 );
//                    break;

                    case 4:
			            printf("<< <UART> MSG_GET_8_32: %0.2X\r\n", (uint8_t)lookup_map[i].address ); fflush(NULL);
		                serialPrintf( fd, "%0.2X:%0.2X", (uint8_t)MSG_GET_8_32, (uint8_t)lookup_map[i].address );
                    break;

                    default:
                        fprintf(stderr, "Unhandled number of bytes (%d) in %s", lookup_map[i].bytes, lookup_map[i].topic);
                }

                serialPutchar(fd, '\x03');

            }
        }
    }
}

void *ProcessSunrise(void *param) {
    while(running) {

    }
}

int main (int argc, char** argv) {
	char client_id[30];
    int rc;

	signal(SIGINT, SignalHandler);
	signal(SIGHUP, SignalHandler);
	signal(SIGTERM, SignalHandler);

	// SETUP DATABASE
    rc = sqlite3_open(DB_FILENAME, &db);
    if (rc != SQLITE_OK) {
        fprintf (stderr, "Can't open database: %s\n\r", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 1;
    }

	// SETUP UART
    if ((fd = serialOpen ("/dev/ttyS0", 115200)) < 0) {
        fprintf (stderr, "Unable to open serial device: %s\n", strerror(errno));
        return 1;
    }

	// SETUP MQTT
	mosquitto_lib_init();
	snprintf(client_id, sizeof(client_id)-1, "offgrid-daemon-%d", getpid());

	if( (mqtt = mosquitto_new(client_id, true, NULL)) != NULL ) { // TODO: Replace NULL with pointer to data structure that will be passed to callbacks
		mosquitto_message_callback_set(mqtt, message_callback);

		if( (mosquitto_connect(mqtt, mqtt_host, mqtt_port, 15)) == MOSQ_ERR_SUCCESS ) {
			if( (mosquitto_subscribe(mqtt, NULL, "og/#", 0)) == MOSQ_ERR_SUCCESS ) {
			    mosquitto_loop_start(mqtt);
            }
            else {
                fprintf (stderr, "Unable to subscribe to main topic: %s\n", strerror(errno));
                return 1;
            }
		}
		else {
            fprintf (stderr, "Unable to connect with MQTT broker (%s:%s): %s\n", mqtt_host, mqtt_port, strerror(errno));
            return 1;
		}
	}
	else {
        fprintf (stderr, "Unable to create MQTT client: %s\n", strerror(errno));
		return 1;
	}

	// SETUP THREADS
    // TODO: Check return code, exit with error if any of these threads can't be created
	pthread_create(&process_rx_thread, NULL, ProcessReceiveThread, NULL);
	pthread_create(&process_tx_thread, NULL, ProcessTransmitThread, NULL);
    pthread_create(&process_sunrise, NULL, ProcessSunrise, NULL);

	sd_notify (0, "READY=1");

	while(running) {
		sleep(5);
		sd_notify(0, "WATCHDOG=1");
	}


    // CLEANUP CODE ONLY BEYOND THIS POINT

	printf("Waiting for threads to terminate...\r\n"); fflush(NULL);
	pthread_join(process_rx_thread, NULL);
	pthread_join(process_tx_thread, NULL);
    pthread_join(process_sunrise, NULL);
	printf("...threads terminated\r\n"); fflush(NULL);

    printf("Stopping mosquitto client...\r\n"); fflush(NULL);
	mosquitto_loop_stop(mqtt, true);
	mosquitto_destroy(mqtt);
	mosquitto_lib_cleanup();
    printf("...mosquitto client stopped\r\n"); fflush(NULL);

    printf("Terminating database connection...\r\n"); fflush(NULL);
    sqlite3_close(db);
    sqlite3_shutdown();
    printf("...database connection terminated\r\n"); fflush(NULL);

	return (EXIT_SUCCESS);
}



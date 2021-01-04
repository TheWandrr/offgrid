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
void LogToDatabase(const char *topic, const char *payload);

sqlite3 *db;
const char *DB_FILENAME = "/usr/local/lib/mqtt.db";

enum ReceiveState {
        GET_STX,
        GET_DATA,
};

struct BridgeMap {
    const unsigned int address; // Ignored if *storage is not NULL
    const unsigned int bytes; // Ignored if *storage is not NULL
    const char *topic;
    const bool readable;
    const bool writable;
    const char *format; // TODO: Not used at this point
    const double multiplier;
    double *storage; // Points to an internal storage variable to read/write.
};

static volatile int running = 1;
pthread_t process_rx_thread;
pthread_t process_tx_thread;
pthread_t process_sunrise;
pthread_t process_state_of_charge;

double house_battery_amps = 0;
double house_battery_volts = 0;
double state_of_charge = 0;

// TODO: This needs to be initialized from persistent storage - database?
// --OR-- have this calculated in Arduino firmware instead of here
double ah_cumulative = 0; // Goes negative as battery is discharged
const int capacity = 198;

const char *mqtt_host = "localhost";
const unsigned int mqtt_port = 1883;

// Used to translate between memory-mapped variables and MQTT topics
struct BridgeMap lookup_map[] = {
    // Switches
    { 0x10, 4, "og/setting/broadcast_period_ms", 1, 1, "%0.0f", 1, NULL },

    // Firmware battery monitor variables (RAM)
    { 0x20, 2, "og/batmon/bank0/volts", 1, 0, "%0.2f", 0.01, NULL },
    { 0x21, 2, "og/batmon/bank0/amps", 1, 0, "%0.1f", 0.1, NULL },
    { 0x22, 2, "og/batmon/bank0/ah", 1, 1, "%0.1f", 0.1, NULL },
    { 0x23, 2, "og/batmon/bank0/soc", 1, 1, "%0.1f", 0.01, NULL },

    // Firmware battery monitor constants (EEPROM)
    { 0x24, 4, "og/batmon/bank0/amps_multiplier", 1, 1, "%0.6f", 0.000001, NULL },
    { 0x25, 4, "og/batmon/bank0/volts_multiplier", 1, 1, "%0.6f", 0.000001, NULL },
    { 0x26, 2, "og/batmon/bank0/amphours_capacity", 1, 1, "%0.1f", 0.1, NULL },
    { 0x27, 2, "og/batmon/bank0/volts_charged", 1, 1, "%0.3f", 0.001, NULL },
    { 0x28, 2, "og/batmon/bank0/minutes_charged_detection_time", 1, 1, "%0.1f", 0.1, NULL },
    { 0x29, 4, "og/batmon/bank0/current_threshold", 1, 1, "%0.6f", .000001, NULL },
    { 0x2A, 1, "og/batmon/bank0/tail_current_factor", 1, 1, "%0.2f", .01, NULL },
    { 0x2B, 1, "og/batmon/bank0/peukert_factor", 1, 1, "%0.2f", .01, NULL },
    { 0x2C, 1, "og/batmon/bank0/charge_efficiency_factor", 1, 1, "%0.2f", .01, NULL },

//    { 0x30, 2, "og/batmon/bank1/volts", 1, 0, "%0.2f", 0.01 },
//    { 0x31, 2, "og/batmon/bank1/amps", 1, 0, "%0.1f", 0.1 },
//    { 0x32, 2, "og/batmon/bank1/ah", 1, 1, "%0.1f", 0.1 },
//    { 0x33, 2, "og/batmon/bank1/soc", 1, 1, "%0.1f", 0.01 },

    // PWM outputs
    { 0xA0, 1, "og/house/light/ceiling", 1, 1, "%0.0f", 1, NULL },
    //{ 0xA1, 1, "", 1, 1, "", (1) },
    //{ 0xA2, 1, "", 1, 1, "", (1) },
    //{ 0xA3, 1, "", 1, 1, "", (1) },
    //{ 0xA4, 1, "", 1, 1, "", (1) },
    //{ 0xA5, 1, "", 1, 1, "", (1) },

    // ON/OFF outputs
    { 0xA6, 1, "og/house/light/ceiling_encoder", 1, 1, "%0.0f", 1, NULL },
    //{ 0xA7, 1, "", 1, 1, "", (1) },

    // Differential ADC inputs, 75mV shunt ** SIGNED 16 BIT
    { 0xB0, 2, "og/house/battery/amps", 1, 0, "%0.1f", (0.0000078125 * 500.0 / 0.050 * 1.031), NULL }, // Calibrated at 2.47A
    { 0xB1, 2, "og/house/fuse_panel/amps", 1, 0, "%0.1f", (0.0000078125 * 50.0 / 0.075), NULL },
    { 0xB2, 2, "og/house/vehicle_in/amps", 1, 0, "%0.1f", (0.0000078125 * 200.0 / 0.075), NULL },
    { 0xB3, 2, "og/house/inverter/amps", 1, 0, "%0.1f", (0.0000078125 * 200.0 / 0.075), NULL },

    // Single ended ADC inputs, 12V
    { 0xB4, 2, "og/house/battery/volts", 1, 0, "%0.2f", (0.000125 * 4 * 1.05350), NULL }, // Calibrated @ 13.85 V
    { 0xB5, 2, "og/vehicle/battery/volts", 1, 0, "%0.2f", (0.000125 * 4 * 1.04390), NULL }, // Calibrated at 14.42 V
    //{ 0xB6, 2, "", 1, 1, "", (1) },
    //{ 0xB7, 2, "", 1, 1, "", (1) },

    // TODO: Refactor this into two different structures, both of which are checked when messages come in.
    // Software storage only.  No external communication.
//    { 0, 0, "og/house/battery/soc", 1, 1, "%0.1f", 1, &state_of_charge },
    { 0, 0, "og/house/battery/ah", 1, 1, "%0.1f", 1, &ah_cumulative },

};

double timestamp(void) {
    struct timespec spec;

    clock_gettime(CLOCK_REALTIME, &spec);
    return spec.tv_sec + ( spec.tv_nsec / 1.0e9 );
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
    int i;

    if( (i = AddressToTopic(address)) >= 0 ) {
        payloadlen = sprintf( payload, lookup_map[i].format, data * lookup_map[i].multiplier ) + 1;
        //DEBUG//printf("<< <MQTT> %s = %s\r\n", lookup_map[i].topic, payload); fflush(NULL);
		mosquitto_publish(mqtt, NULL, lookup_map[i].topic, payloadlen, payload, 0, false);

        LogToDatabase(lookup_map[i].topic, payload);

        // TODO: Need to know if this has stagnated
        if( !strcmp(lookup_map[i].topic, "og/house/battery/amps") ) {
            house_battery_amps = data * lookup_map[i].multiplier;
        }
        else if( !strcmp(lookup_map[i].topic, "og/house/battery/volts") ) {
            house_battery_volts = data * lookup_map[i].multiplier;
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

void LogToDatabase(const char *topic, const char *payload) {
    char *err_msg = 0;
    int rc;
    sqlite3_stmt *stmt;
    char now[32];
    int row_count;

    sprintf(now, "%0.6f", timestamp());
/*
    // Query the database for the most recent entry if it matches the current topic and payload
    if( sqlite3_prepare_v2(db,  "SELECT * FROM message WHERE topic=?1 AND payload=?2 ORDER BY timestamp DESC LIMIT 1;", -1, &stmt, NULL) ) {
        fprintf(stderr, "Failed to prepare statement: %s\r\n", sqlite3_errmsg(db));
        return;
    }

    if( sqlite3_bind_text(stmt, 1, topic, -1, SQLITE_TRANSIENT) ) {
        fprintf(stderr, "Failed to bind topic: %s\n", sqlite3_errmsg(db));
        return;
    }

    if( sqlite3_bind_text(stmt, 2, payload, -1, SQLITE_TRANSIENT) ) {
        fprintf(stderr, "Failed to bind payload: %s\n", sqlite3_errmsg(db));
        return;
    }

    row_count = 0;
    while( sqlite3_step(stmt) != SQLITE_DONE ) {
        row_count++;  // Should never exceed 1, but small chance there are two.  We only care about 0 vs !0
    }

    sqlite3_finalize(stmt);
*/
    // If no rows matched, then the most recent DB entry is different than the current so go ahead and add it.
//    if (row_count == 0) {


//---------------------------------------
//---------------------------------------
// Upon insertion to database, also store the topic/payload internally.  Before insertion, compare 
// to previous and only insert if payload for topic has changed by a certain amount (or a certain amount of time has passed?)
//
//    Maybe use 'storage' in BridgeMap?
//---------------------------------------
//---------------------------------------


    if (1) {

        if( sqlite3_prepare_v2(db, "INSERT INTO message(topic,payload,timestamp) VALUES(?,?,?);", -1, &stmt, NULL) ) {
            fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
            return;
        }

        rc = sqlite3_bind_text(stmt, 1, topic, -1, SQLITE_TRANSIENT);
        if(rc != SQLITE_OK) {
            fprintf(stderr, "Failed to bind topic: %s\n", sqlite3_errmsg(db));
        }

        rc = sqlite3_bind_text(stmt, 2, payload, -1, SQLITE_TRANSIENT);
        if(rc != SQLITE_OK) {
            fprintf(stderr, "Failed to bind payload: %s\n", sqlite3_errmsg(db));
        }

        rc = sqlite3_bind_text(stmt, 3, now, -1, SQLITE_TRANSIENT);
        if(rc != SQLITE_OK) {
            fprintf(stderr, "Failed to bind timestamp: %s\n", sqlite3_errmsg(db));
        }

        if ( sqlite3_step(stmt) != SQLITE_DONE ) {
            fprintf(stderr, "Failed to execute statement: %s\n", sqlite3_errmsg(db));
        }

        sqlite3_finalize(stmt);
    }
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
            if( ( (lookup_map[i].writable == 1) && (lookup_map[i].bytes > 0) ) ||
                  (lookup_map[i].writable == 1) && (lookup_map[i].storage != NULL) ) {

                if(lookup_map[i].storage != NULL) {
                    //printf("Writing internal variable for '%s'\r\n", topic);
                    *(lookup_map[i].storage) = atof(message->payload);
                }
                else {

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
            if( ( (lookup_map[i].readable == 1) && (lookup_map[i].bytes > 0) ) ||
                  (lookup_map[i].readable == 1) && (lookup_map[i].storage != NULL) ) {

                if(lookup_map[i].storage != NULL) {
                    printf("Requesting internal variable for '%s'\r\n", topic);
                    payloadlen = sprintf( payload, lookup_map[i].format, *(lookup_map[i].storage) ) + 1;     
	                printf(">> <MQTT> %s = %s\r\n", lookup_map[i].topic, payload); fflush(NULL);
		            mosquitto_publish(mqtt, NULL, lookup_map[i].topic, payloadlen, payload, 0, false);
                }
                else {

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

//                        case 3:
//		                      serialPrintf( fd, "0.2X:%0.2X", (uint8_t)MSG_GET_8_24 );
//                        break;

                        case 4:
			                printf("<< <UART> MSG_GET_8_32: %0.2X\r\n", (uint8_t)lookup_map[i].address ); fflush(NULL);
		                    serialPrintf( fd, "%0.2X:%0.2X", (uint8_t)MSG_GET_8_32, (uint8_t)lookup_map[i].address );
                        break;
                    }

                    serialPutchar(fd, '\x03');

                }
            }
        }
    }
    else {
        // Log everything except /get and /set, which are handled as special cases
        LogToDatabase(message->topic, message->payload);
    }

}

void *ProcessStateOfCharge(void *param) {
    const int sample_period_us = 1000000;
    const double charged_voltage = 14.4;
    const double tail_current_percent = 0.04;
    const double peukert_exponent = 1.05;
    const double current_threshold = 0.1;
    double copy_house_battery_amps;
    double copy_house_battery_volts;
    const int charge_efficiency = 99;
    const int charged_detect_time_m = 3;
    bool sync_pending = false;
    time_t sample_start_time, sample_end_time, sync_pending_begin_time;
    double ah_change = 0;

    enum ChargeState {
        CS_DISCHARGING,
        CS_CHARGING,
        CS_CHARGED,
        CS_SYNC_PENDING,
    } charge_state = CS_DISCHARGING;

	char payload[16];
	int payloadlen;

    time(&sample_start_time);

    while(running) {

        usleep(sample_period_us);

        // TODO: MUTEX? >>
        copy_house_battery_amps = house_battery_amps;
        copy_house_battery_volts = house_battery_volts;
        // << MUTEX?

        time(&sample_end_time);

        ah_change = difftime(sample_end_time, sample_start_time) / 3600 * copy_house_battery_amps;

        //printf("## delta_T: %0.4f / AH Change: %0.6f\r\n", difftime(sample_end_time, sample_start_time), ah_change);

        //if(charge_state == CS_CHARGING) {
        //    ah_change *= (charge_efficiency / 100);
        //}

        // TODO: MUTEX? >>
        ah_cumulative += ah_change;
        // << MUTEX?

        // If this hits 100 but the current is still high, we could flag that it's out of sync
        state_of_charge =  ( (capacity + ah_cumulative) / capacity) * 100;
        //if(state_of_charge > 100) state_of_charge = 100; // no min function
/*
        // >>> MQTT
        payloadlen = sprintf( payload, "%0.1f", state_of_charge ) + 1;
	    printf(">> <MQTT> %s = %s\r\n", "og/house/battery/soc", payload); fflush(NULL);
		mosquitto_publish(mqtt, NULL, "og/house/battery/soc", payloadlen, payload, 0, false);

        payloadlen = sprintf( payload, "%0.1f", ah_cumulative ) + 1;
	    printf(">> <MQTT> %s = %s\r\n", "og/house/battery/ah", payload); fflush(NULL);
		mosquitto_publish(mqtt, NULL, "og/house/battery/ah", payloadlen, payload, 0, false);

        payloadlen = sprintf( payload, "%d", charge_state ) + 1;
	    printf(">> <MQTT> %s = %s\r\n", "og/house/battery/charge_state", payload); fflush(NULL);
		mosquitto_publish(mqtt, NULL, "og/house/battery/charge_state", payloadlen, payload, 0, false);
*/
        switch(charge_state) {

            case CS_CHARGING:
                if(ah_change < 0) {
                    charge_state = CS_DISCHARGING;
                }
                else if (ah_change > 0) {
                    if( (house_battery_volts >= charged_voltage) &&
                        (house_battery_amps <= tail_current_percent) ) {

                        // Set a timestamp, time_since_sinc_pending_started
                        time(&sync_pending_begin_time);
                        sync_pending  = true;

                        if ( (sync_pending) && 
                             (difftime(sync_pending_begin_time, sample_end_time) >= charged_detect_time_m) ) {
                            state_of_charge = 100;
                            ah_cumulative = 0;
                            charge_state = CS_CHARGED;
                        }
                    }
                }
            break;

            case CS_DISCHARGING:
                if(ah_change > 0) {
                    charge_state = CS_CHARGING;
                }
            break;

            case CS_CHARGED:
                if(ah_change < 0) {
                    charge_state = CS_DISCHARGING;
                }
                else if(ah_change > 0) {
                    charge_state = CS_CHARGING;
                }
            break;

        }

        sample_start_time = sample_end_time;
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
    pthread_create(&process_state_of_charge, NULL, ProcessStateOfCharge, NULL);

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
    pthread_join(process_state_of_charge, NULL);
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



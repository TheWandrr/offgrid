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

#include "offgrid_constants.h"
#include "ds18b20.h"

// TODO: Improve CPU usage
// TODO: Implement control over how much information is output to logs

enum ReceiveState {
    GET_STX,
    GET_DATA,
};

enum InterfaceAccessMask {
    AM_NONE =       0b00000000,
    AM_READ =       0b00000001,
    AM_WRITE =      0b00000010,
    AM_READWRITE =  0b00000011,
};

// This structure is shared with firmware and defines topic interfaces
// In this module, it is the data contained within a linked list where nodes are added as the device reports/exposes them
struct Interface {
    uint16_t address;
    uint8_t bytes;
    int8_t exponent;
    enum InterfaceAccessMask access_mask;
    uint8_t enable_logging;
    char name[255];
    char unit[15];
    long data; // Not part of firmware interface definition
    uint64_t data_timestamp; // Not part of firmware interface definition
};

struct Node {
    struct Interface *interface;
    struct Node *next;
};

struct Node *interface_root = NULL;

static volatile int running = 1;
pthread_t process_rx_thread;
pthread_t process_tx_thread;
pthread_t process_sunrise_thread;
pthread_t process_local_thread;

struct mosquitto *mqtt;
const char *mqtt_host = "localhost";
const unsigned int mqtt_port = 1883;

int fd;
static char message_buffer[1023] = {0};

sqlite3 *db;
const char *DB_FILENAME = "/usr/local/lib/mqtt.db"; // TODO: Make this configurable via config file and/or command line

// Locally attached DS18B20 temperature sensors
char **sensorNames;
int sensorNamesCount;
SensorList *sensorList;


void ParseMessage(const char *msg_buf);
void PublishRequestReturn(unsigned int address, long data);
void LogToDatabase(struct Interface *interface, const long data, const uint64_t now);

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


void FreeInterfaces(struct Node *node) {
    if(node == NULL) {
        return;
    }

    FreeInterfaces(node->next);

    if(node->interface != NULL) {
        // DEBUG
        //printf("Free interface: %s\r\n", node->interface->name); fflush(NULL);
        // DEBUG

        free(node->interface);
    }
    free(node);
}

void AddInterface(struct Node **node, struct Interface *interface) {
    struct Node *new_node;

    new_node = (struct Node*)malloc(sizeof(struct Node));
    new_node->interface = interface;
    new_node->next = (*node);
    (*node) = new_node;

    // DEBUG //
    //printf("Add interface: name: %s\r\n\taccess_mask: %d\r\n\tenable_logging: %d\r\n",
    //       (*node)->interface->name,
    //       (*node)->interface->access_mask,
    //       (*node)->interface->enable_logging);
    //       fflush(NULL);
    // DEBUG //
}

// Search for topic by name or address (but not both!)
struct Node* FindInterface(struct Node *node, const char *name, uint16_t address) {
    struct Node *current = node;

    while (current != NULL) {
        if( current->interface !=  NULL ) {
            if(  ( name != NULL ) && ( !strcmp(current->interface->name, name) )  ) {
                //DEBUG//printf("FindInterface: %s ==> address %0.4X\r\n", name, current->interface->address);
                return current;
            }
            else if(  ( address != 0 ) && ( current->interface->address == address)  ) {
                //DEBUG//printf("FindInterface: address %0.4X ==> %s\r\n", address, current->interface->name);
                return current;
            }
        }
        current = current->next;
    }

    return NULL;
}

struct Interface * NewInterface(uint16_t address, uint8_t bytes, int8_t exponent, enum InterfaceAccessMask access_mask, uint8_t enable_logging, const char *name, const char *unit) {
    struct Interface *interface = NULL;

    interface = (struct Interface *)malloc(sizeof(struct Interface));

    if( interface != NULL ) {
        interface->address = address;
        interface->bytes = bytes;
        interface->exponent = exponent;
        interface->access_mask = access_mask;
        interface->enable_logging = enable_logging;
        strcpy(interface->name, name);
        strcpy(interface->unit, unit);

        // DEBUG //
        //printf("NewInterface(): %0.4x, %d, %d, %d, %d, %s, %s\r\n", address, bytes, exponent, access_mask, enable_logging, name, unit); fflush(NULL);
        // DEBUG //
    }
    else {
        printf("Could not allocate memory for new interface\r\n");
        fflush(NULL);
    }

    return interface;
}

// Based on a negative or positive power of ten, create a format string with the same decimal places
//void MakeFormatString(char *fmt, int8_t exponent) {
unsigned int FormatPayload(char *buff, float n, int8_t exp) {
    char fmt[15];
    double val;
    unsigned int len;

    val = (double)n * pow(10, exp);

    if (exp >= 0) {
        len = sprintf(buff, "%d", (long)trunc(val));
    }
    else if (exp < 0) {
        sprintf(fmt, "%%0.%df", abs(exp));
        len = sprintf(buff, fmt, val);
    }

    return len;
}

// Returns epoch time in whole centiseconds (seconds * 0.01)
uint64_t timestamp(void) {
    struct timespec spec;

    clock_gettime(CLOCK_REALTIME, &spec);

    return ( spec.tv_sec + (spec.tv_nsec / 1.0e9) ) * 100ull;
}

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
        else {
            usleep(1000);
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
        case 'A':
        case 'a':
            num = 10;
            break;
        case 'B':
        case 'b':
            num = 11;
            break;
        case 'C':
        case 'c':
            num = 12;
            break;
        case 'D':
        case 'd':
            num = 13;
            break;
        case 'E':
        case 'e':
            num = 14;
            break;
        case 'F':
        case 'f':
            num = 15;
            break;
        default:
            num = 0;
        }
    }

    return num;
}

// TODO: Add trimming white space from beginning and end of arguments
void ParseMessage(const char *msg_buf) {
    unsigned int arg_count = 0;
    char sep;
    const char *a;
    const char *b;
    char arg[16][255];

    if ( (msg_buf == NULL) || (msg_buf == '\0') ) {
        return;
    }

    // DEBUG //
    //printf("ParseMessage(\"%s\")\n", msg_buf); fflush(NULL);
    // DEBUG //

    // Move through the string with two pointers, locating the start and end of each token
    a = msg_buf;
    b = msg_buf;

    while ( *b != '\0' ) {
        sep = (arg_count == 0) ? ':' : ','; // First time through separator is colon, the rest a comma

        while( (*b != sep) && (*b != '\0') ) {
            b++;    // Advance end pointer to next separator
        }

        strncpy(arg[arg_count], a, b - a);
        arg[arg_count][b-a] = '\0';

        arg_count++;

        if ( *b == '\0' ) {
            break;
        }
        else {
            b++; // One past separator
            a = b;
        }
    };

    // DEBUG
    //for( unsigned int i = 0; i < arg_count; i++ ) {
    //    printf("{%d} %s | ", i, arg[i]);
    //}
    //printf("\r\n");
    // DEBUG

    // ====== Now do something with the message and arguments  ======

    // NOTE: Message is included in arg_count, stored in arg[0]!
    switch( (uint8_t)strtoul(arg[0], NULL, 16) ) {

//    	case MSG_KEEP_ALIVE: // Sent by system master (or designate) to ensure bus is operating.  This module will be automatically reset by the watchdog timer if not received in time.
//    		if(arg_count == 2) {
//    			//DEBUG//printf(">> <UART> MSG_KEEP_ALIVE: %0.2X\r\n", (uint8_t)arg[0]); fflush(NULL);
//    			mosquitto_publish(mqtt, NULL, "og/status/tick", 0, "", 0, false);
//    	        //DEBUG//printf("<< <MQTT> %s = %s\r\n", "og/status/tick", ""); fflush(NULL);
//    		}
//    	break;

    case MSG_GET_SET_ERROR:
        if(arg_count == 2) {
            printf(">> <UART> MSG_GET_SET_ERROR: %0.2X\r\n", (uint8_t)strtoul(arg[1], NULL, 16));
            fflush(NULL);
        }
        break;

    case MSG_RETURN_8_8:
        if(arg_count == 3) {
            PublishRequestReturn( (uint8_t)strtoul(arg[1], NULL, 16), (int8_t)strtol(arg[2], NULL, 16) );
        }
        break;

    case MSG_RETURN_8_16:
        if(arg_count == 3) {
            PublishRequestReturn( (uint8_t)strtoul(arg[1], NULL, 16), (int16_t)strtol(arg[2], NULL, 16) );
        }
        break;

    case MSG_RETURN_8_32:
        if(arg_count == 3) {
            PublishRequestReturn( (uint8_t)strtoul(arg[1], NULL, 16), (int32_t)strtoll(arg[2], NULL, 16) );
        }
        break;

    case MSG_RETURN_INTERFACE:
        if(arg_count == 8) {
            // TODO: Replace with cleaner quote trimming function
            // Trim first and last characters of string (that really should be the double quotes)
            unsigned int len;
            if(  ( len = strlen(arg[6]) ) >= 2 ) {
                memmove(arg[6], arg[6]+1, len-2);
                *(arg[6]+len-2) = '\0';
            }
            if(  ( len = strlen(arg[7]) ) >= 2 ) {
                memmove(arg[7], arg[7]+1, len-2);
                *(arg[7]+len-2) = '\0';
            }

            AddInterface( &interface_root, NewInterface((uint16_t)strtoul(arg[1], NULL, 16), (uint8_t)strtoul(arg[2], NULL, 16),
                          (int8_t)strtol(arg[3], NULL, 16), (uint8_t)strtoul(arg[4], NULL, 16), (uint8_t)strtoul(arg[5], NULL, 16), arg[6], arg[7]) );
        }
        break;

    case MSG_DEBUG_STRING:
        if(arg_count == 2) {
            // Trim first and last characters of string (that really should be the double quotes)
            unsigned int len;
            if(  ( len = strlen(arg[1]) ) >= 2 ) {
                memmove(arg[1], arg[1]+1, len-2);
                *(arg[1]+len-2) = '\0';
            }

            //printf("<<< DEBUG [%016llX] >>> %s\n", timestamp(), arg[1]);
            printf("<<< DEBUG >>> %s\n", arg[1]);
            fflush(NULL);
        }
        break;

    }

}

// Not strictly necessary but will allow control of message sending frequency
void *ProcessTransmitThread(void *param) {
    while(running) {
        //if(tx_ready) {
        //}
        //else {
        //    usleep(100);
        //}
    }
}

// TODO: enqueueMessage - add message to transmit queue, use variadic function printf style

//void connect_callback(struct mosquitto *mosq, void *obj, int result) {
//}

void PublishRequestReturn(unsigned int address, long data) {
    char payload[256];
    int payloadlen;
    uint64_t now;
    struct Node *node;
    struct Interface *interface;

    // DEBUG //
    //printf("PublishRequestReturn(): %0.4X, %d\r\n", address, data);
    // DEBUG //

    if( (node = FindInterface(interface_root, NULL, address)) != NULL ) {
        //DEBUG//printf("PublishRequestReturn(): address %0.4X found\r\n", address); fflush(NULL);

        interface = node->interface;

        now = timestamp();
        payloadlen = FormatPayload(payload, data, interface->exponent);
        //MakeFormatString(fmt, interface->exponent),
        //payloadlen = sprintf( payload, fmt,  (double)data * pow(10, interface->exponent) );

        // DEBUG //
        //printf("<PUT-MQTT> %s = %s\r\n", interface->name, payload); fflush(NULL);
        // DEBUG //

        mosquitto_publish(mqtt, NULL, interface->name, payloadlen, payload, 0, false);

        // Only store to database if it has changed since the last time and it is flagged to be logged
        if( (interface->data != data) && (interface->enable_logging != 0) ) {
            LogToDatabase(interface, data, now);
            interface->data_timestamp = now;
            interface->data = data;
        }
    }
    else {
        // Silently ignore if we don't have an interface for this.  Could maybe re-query the device, but seems liek a waste.
        //DEBUG//printf("PublishRequestReturn(): address %0.4X NOT found\r\n", address); fflush(NULL);
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

void LogToDatabase(struct Interface *interface, const long data, const uint64_t now) {
    char *err_msg = 0;
    int rc;
    sqlite3_stmt *stmt;
    int row_count;
    //struct Node *node;
    bool failed;

    // Inhibit logging if it's disabled or the last written value is the same as current
    //if ( (interface->enable_logging == 0) || (interface->data == data) ) {
    //  return;
    //}

    failed = false;

    // TODO: Look up topic.  If it doesn't exist, add it before inserting the message.
    //if( (node = FindInterface(interface_root, topic, 0)) != NULL ) {
    if( sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO interface (name,unit,exponent) VALUES(?1,?2,?3);", -1, &stmt, NULL) == SQLITE_OK ) {
        if(   (  ( sqlite3_bind_text(stmt, 1, interface->name, -1, SQLITE_TRANSIENT) ) == SQLITE_OK  ) &&
                (  ( sqlite3_bind_text(stmt, 2, interface->unit, -1, SQLITE_TRANSIENT) ) == SQLITE_OK  ) &&
                (  ( sqlite3_bind_int(stmt, 3, interface->exponent) ) == SQLITE_OK  )   )
        {
            if ( sqlite3_step(stmt) == SQLITE_DONE ) {
                // Successfully added new row for interface
                // Note: If the "IGNORE" is executed, it still succeeds and we don't know when this gets added or not.  But it does seem to work.
                //DEBUG//printf("Added new interface to database: %s, %s, %d\r\n", node->interface->name, node->interface->unit, node->interface->exponent);
            }
            else {
                fprintf(stderr, "Failed to execute INSERT statement: %s\n", sqlite3_errmsg(db));
                failed = true;
            }
        }
        else {
            fprintf(stderr, "Failed to bind INSERT arguments: %s\n", sqlite3_errmsg(db));
            failed = true;
        }
    }
    else {
        fprintf(stderr, "Failed to prepare INSERT statement: %s\n", sqlite3_errmsg(db));
        failed = true;
    }

    if(failed) {
        fprintf(stderr, "Error while finding or creating interface in database\n");
        return;
    }
    //}




    if( sqlite3_prepare_v2(db, "INSERT INTO message(payload,timestamp,interface_id) VALUES(?1,?2,(SELECT id FROM interface WHERE name=?3));", -1, &stmt, NULL) ) {
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

    rc = sqlite3_bind_text(stmt, 3, interface->name, -1, SQLITE_TRANSIENT);
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
//    int i;
    char *suffix_set = "/set"; // Append to a topic to write a value
    char *suffix_get = "/get"; // Append to a topic to force it to be published immediately
    // TODO: Add more suffixes to get metadata like broadcast period, precision, units, read/write access
    char topic[255];
    int newlen;
    char payload[16];
    int payloadlen;
    struct Node *node;

    //DEBUG//printf("<GET-MQTT> %s = %s\r\n", message->topic, message->payload); fflush(NULL);

    // *** If message ends with "/set", it needs to be handled differently
    if( StringHasSuffix(message->topic, suffix_set) ) {
        //printf("SET message found: %s\r\n", message->topic);

        // Strip suffix and try to match
        newlen = strlen(message->topic) - strlen(suffix_set);
        newlen = ( (newlen > 254) ? 254 : newlen);
        strncpy(topic, message->topic, newlen);
        topic[newlen] = '\0';
        //printf("Stripped Topic: %s\r\n", topic);

        if( (node = FindInterface(interface_root, topic, 0)) != NULL ) {
            //printf("SET matched topic: %d, %s\r\n", i, lookup_map[i].topic);
            if( (node->interface->access_mask & AM_WRITE) && (node->interface->bytes > 0) ) {

                // Don't update the cached data here.  It will happen when it gets confirmed by the MSG_RETURN_X_X.

                serialPutchar(fd, '\x02');

                switch(node->interface->bytes) {

                case 1:
                    //DEBUG//printf("<< <UART> MSG_SET_8_8: %0.2X, %0.2X\r\n", (uint8_t)node->interface->address, (uint8_t)( atof(message->payload) / pow(10, node->interface->exponent) ) ); fflush(NULL);
                    serialPrintf( fd, "%0.2X:%0.2X,%0.2X", (uint8_t)MSG_SET_8_8, (uint8_t)node->interface->address, (int8_t)( atof(message->payload) / pow(10, node->interface->exponent) ) );
                    break;

                case 2:
                    //DEBUG//printf("<< <UART> MSG_SET_8_16: %0.2X, %0.4X\r\n", (uint8_t)node->interface->address, (uint16_t)( atof(message->payload) / pow(10, node->interface->exponent) ) ); fflush(NULL);
                    serialPrintf( fd, "%0.2X:%0.2X,%0.4X", (uint8_t)MSG_SET_8_16, (uint8_t)node->interface->address, (int16_t)( atof(message->payload) / pow(10, node->interface->exponent) ) );
                    break;

//                    case 3:
//	                      serialPrintf( fd, "0.2X:%0.2X,%0.6X", (uint8_t)MSG_SET_8_24, (uint8_t)lookup_map[i].address, (uint32_t)(atoi(message->payload)) );
//                    break;

                case 4:
                    //DEBUG//printf("<< <UART> MSG_SET_8_32: %0.2X, %0.8lX\r\n", (uint8_t)node->interface->address, (uint32_t)( atof(message->payload) / pow(10, node->interface->exponent) ) ); fflush(NULL);
                    serialPrintf( fd, "%0.2X:%0.2X,%0.8lX", (uint8_t)MSG_SET_8_32, (uint8_t)node->interface->address, (int32_t)( atof(message->payload) / pow(10, node->interface->exponent) ) );
                    break;

                default:
                    fprintf(stderr, "Unhandled number of bytes (%d) in %s", node->interface->bytes, node->interface->name);
                }

                serialPutchar(fd, '\x03');

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

        if( (node = FindInterface(interface_root, topic, 0)) != NULL ) {
            //printf("GET matched topic: %d, %s\r\n", i, node->interface->);
            if( (node->interface->access_mask & AM_WRITE) && (node->interface->bytes > 0) ) {

                serialPutchar(fd, '\x02');

                switch(node->interface->bytes) {

                case 1:
                    //DEBUG//printf("<< <UART> MSG_GET_8_8: %0.2X\r\n", (uint8_t)node->interface->address ); fflush(NULL);
                    serialPrintf( fd, "%0.2X:%0.2X", (uint8_t)MSG_GET_8_8, (uint8_t)node->interface->address );
                    break;

                case 2:
                    //DEBUG//printf("<< <UART> MSG_GET_8_16: %0.2X\r\n", (uint8_t)node->interface->address ); fflush(NULL);
                    serialPrintf( fd, "%0.2X:%0.2X", (uint8_t)MSG_GET_8_16, (uint8_t)node->interface->address );
                    break;

//                    case 3:
//		                  serialPrintf( fd, "0.2X:%0.2X", (uint8_t)MSG_GET_8_24 );
//                    break;

                case 4:
                    //DEBUG//printf("<< <UART> MSG_GET_8_32: %0.2X\r\n", (uint8_t)node->interface->address ); fflush(NULL);
                    serialPrintf( fd, "%0.2X:%0.2X", (uint8_t)MSG_GET_8_32, (uint8_t)node->interface->address );
                    break;

                default:
                    fprintf(stderr, "Unhandled number of bytes (%d) in %s", node->interface->bytes, node->interface->name);
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

void *ProcessLocal(void *param) {
    float temperature;
    uint64_t now;
    char now_str[64];

    while(running) {

        // Send 64-bit timestamp, high 32 bits first then low 32 bits
        now = timestamp() / 10; // Convert to milliseconds
        //serialPutchar(fd, '\x02');
        //serialPrintf( fd, "%0.2X:%0.2X,%0.8lX", (uint8_t)MSG_SET_8_32, (uint8_t)0xF0, (int32_t)( now >> 32 ) );
        //serialPrintf( fd, "%0.2X:%0.2X,%0.8lX", (uint8_t)MSG_SET_8_32, (uint8_t)0xF1, (int32_t)( now ) );
        //serialPutchar(fd, '\x03');

        sprintf(now_str, "%ul", now >> 32);
        mosquitto_publish(mqtt, NULL, "og/system_clock_h/set", strlen(now_str), now_str, 0, false);
        sprintf(now_str, "%ul", now);
        mosquitto_publish(mqtt, NULL, "og/system_clock_l/set", strlen(now_str), now_str, 0, false);

        for(int i = 0; i < sensorList->SensorCount; i++) {
            temperature = ReadTemperature(sensorList->Sensors[i]);
            // TODO: Replace hard-coded address for temperature sensors with...?
            serialPutchar(fd, '\x02');
            serialPrintf( fd, "%0.2X:%0.2X,%0.4X", (uint8_t)MSG_SET_8_16, (uint8_t)0xF0 + i, (int16_t)( temperature * 10 ) );
            serialPutchar(fd, '\x03');
        }

        sleep(5);
    }
}

// Sends a message to the device requesting that it output all of its available interfaces
void RequestInterfaces(void) {
    serialPutchar(fd, '\x02');
    //printf("<< <UART> MSG_GET_INTERFACE\r\n"); fflush(NULL);
    serialPrintf( fd, "%0.2X", (uint8_t)MSG_GET_INTERFACE );
    serialPutchar(fd, '\x03');
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
    // TODO: This needs to be made into a configuration setting
    // TODO: It seems to change on its own.  Auto detect?

    if ((fd = serialOpen ("/dev/ttyACM0", 115200)) >= 0) {
        fprintf(stdout, "Connect on /dev/ttyACM0\n");
    }
    else if ((fd = serialOpen ("/dev/ttyACM1", 115200)) >= 0) {
        fprintf(stdout, "Connect on /dev/ttyACM1\n");
    }
    else if ((fd = serialOpen ("/dev/ttyS0", 115200)) >= 0) {
        fprintf(stdout, "Connect on /dev/ttyS0\n");
    }
    else if ((fd = serialOpen ("/dev/ttyAMA0", 115200)) >= 0) {
        fprintf(stdout, "Connect on /dev/ttyAMA0\n");
    }
    else {
        fprintf (stderr, "Unable to open serial device: %s\n", strerror(errno));
        return 1;
    }

    fflush(NULL);

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

    // SETUP DIRECTLY CONNECTED DEVICES
    sensorNames = NULL;
    sensorNamesCount = 0;

    sensorList = GetSensors(sensorNames, sensorNamesCount);

//    if (sensorList->SensorCount == 0) {
//        fprintf (stderr, "No temperature sensors found");
//    }
//    else {
//        // Add local interfaces to database storage
//        for(int i = 0; i < sensorList->SensorCount; i++) {
//            char name[20];
//            snprintf(name, sizeof(name), "og/temperature/%d", i);
//            AddInterface( &interface_root, NewInterface(0xFF00 + i, 2, -1, AM_READ, true, name, "°C" ) );
//        }
//    }

    // SETUP THREADS
    // TODO: Check return code, exit with error if any of these threads can't be created
    pthread_create(&process_rx_thread, NULL, ProcessReceiveThread, NULL);
    //pthread_create(&process_tx_thread, NULL, ProcessTransmitThread, NULL);
    pthread_create(&process_sunrise_thread, NULL, ProcessSunrise, NULL);
    pthread_create(&process_local_thread, NULL, ProcessLocal, NULL);

    sd_notify (0, "READY=1");

    RequestInterfaces();

    while(running) {
        sleep(5);
        sd_notify(0, "WATCHDOG=1");
    }


    // CLEANUP CODE ONLY BEYOND THIS POINT

    FreeInterfaces(interface_root);

    printf("Waiting for threads to terminate...\r\n");
    fflush(NULL);
    pthread_join(process_rx_thread, NULL);
    //pthread_join(process_tx_thread, NULL);
    pthread_join(process_sunrise_thread, NULL);
    pthread_join(process_local_thread, NULL);
    printf("...threads terminated\r\n");
    fflush(NULL);

    printf("Stopping mosquitto client...\r\n");
    fflush(NULL);
    mosquitto_loop_stop(mqtt, true);
    mosquitto_destroy(mqtt);
    mosquitto_lib_cleanup();
    printf("...mosquitto client stopped\r\n");
    fflush(NULL);

    printf("Terminating database connection...\r\n");
    fflush(NULL);
    sqlite3_close(db);
    sqlite3_shutdown();
    printf("...database connection terminated\r\n");
    fflush(NULL);

    return (EXIT_SUCCESS);
}



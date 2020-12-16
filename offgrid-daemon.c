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

#include <wiringSerial.h>
//#include <argp.h>

#include "offgrid_constants.h"

//const char *argp_program_version = "offgrid-daemon 0.0.1";
//const char *argp_program_bug_address = "";

//static char doc[] = "Offgrid hardware daemon -- UART to MQTT bridge";

void ParseMessage(char *msg_buf);

enum ReceiveState {
        GET_STX = 1,
        GET_DATA = 2,
};

static volatile int running = 1;
pthread_t process_rx_thread;
pthread_t process_tx_thread;

// TODO: Eventually move these to classes
int fd; // File descriptor for serial port
// TODO: Circular Buffer rx_uart
// TODO: Circular Buffer tx_uart
static char message_buffer[32] = {0}; // KEEP INCOMING MESSAGES SMALL!

void SignalHandler(int signum)
{
	running = 0;

	printf("Waiting for threads to stop...\r\n");
	fflush(NULL);

	pthread_join(process_rx_thread, NULL);
	pthread_join(process_tx_thread, NULL);

	printf("...Threads stopped\r\n");
	fflush(NULL);

	exit(signum);
}

// TODO: Change this to a variadic function so we can use format strings and arguments
void send_message(const char *message) {
	serialPutchar(fd, '\x02');
	serialPrintf(fd, message);
	serialPutchar(fd, '\x03');
}

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
						//receive_state = GET_STX;
						//received_message = true;
						//printf("%s\r\n", message_buffer);
						//fflush(NULL);
					}
					else if( c == '\x02' ) {
						// ERROR - expected ETX before STX
						////Serial.print("   <<<   MISSING ETX\r\n<RECOVER-STX>");

						// TODO: Clear any buffered data
						// TODO: Set things up the same as though we've receiv$

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
  bool invalid_message = false; // TODO: Replace with descriptive error codes later?

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
      // TODO - ERROR: Address did not start with hex digit
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
      // TODO: ERROR - address must maximum 4 ASCII-HEX characters
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
          //while ( (*p != '\n') && (*p != ',') && isxdigit(*p) && (count < 8) ) {
          while ( isxdigit(*p) ) {
            value_buffer <<= 4;
            value_buffer += asciiHexToInt(*p);

            count++;
            p++;
          }

          if (count > 8) {
            // TODO: ERROR - max data size is 4 bytes or 8 ascii-hex characters
            invalid_message = true;
            break;
          }
          else {
            // Looks like we have good data.  Do something with it before it goes away in the next loop iteration.
            //DEBUG//Serial.print(value_buffer, HEX);
            arg[arg_count] = value_buffer;
            arg_count++;

            if( arg_count > ( sizeof(arg) / sizeof(arg[0]) ) ) {
              // TODO - ERROR: IMPOSED EMBEDDED LIMITATION
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
            // TODO: ERROR - Only a comma or end of string should follow a value
            invalid_message = true;
            break;
          }
        }
//        else if (*p == '"') {
//          // get everything up to comma or end of string
//        }
        else {
          // TODO: ERROR - invalid contents
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
      // TODO - ERROR: Something unexpected followed the address
      invalid_message = true;
      break;
    }

    //p += sizeof(char);
    p++;
  }

  if ( !invalid_message ) {
    //DEBUG//Serial.println("  <-- VALID");

    switch(address) {

      case MSG_KEEP_ALIVE: // Sent by system master (or designate) to ensure bus is operating.  This module will be automatically reset by the watchdog timer if not received in time.
        if(arg_count == 1) {
          printf("Received keep alive message\r\n");
	  fflush(NULL);
        }
        break;

	case MSG_RETURN_8_8:
		if(arg_count == 2) {
			printf("MSG_RETURN_8_8 >>>  %0.2X, %0.2X\r\n", (uint8_t)arg[0], (uint8_t)arg[1]);
			fflush(NULL);
		}
	break;

	case MSG_RETURN_8_16:
		if(arg_count == 2) {
			printf("MSG_RETURN_8_16 >>>  %0.2X, %0.4X\r\n", (uint8_t)arg[0], (uint16_t)arg[1]);
			fflush(NULL);
		}
	break;

	case MSG_RETURN_8_32:
		if(arg_count == 2) {
			printf("MSG_RETURN_8_32 >>>   %0.2X, %0.8X\r\n", (uint8_t)arg[0], (uint32_t)arg[1]);
			fflush(NULL);
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

int main (int argc, char** argv) {

	signal(SIGINT, SignalHandler);
	signal(SIGHUP, SignalHandler);
	signal(SIGINT, SignalHandler);

        if ((fd = serialOpen ("/dev/ttyS0", 115200)) < 0) {
                fprintf (stderr, "Unable to open serial device: %s\n", strerror (errno));
                return 1;
        }

	pthread_create(&process_rx_thread, NULL, ProcessReceiveThread, NULL);
	pthread_create(&process_tx_thread, NULL, ProcessTransmitThread, NULL);

	sd_notify (0, "READY=1"); // Tell systemd we're ready

	while(running) {
		sleep(5);
		sd_notify(0, "WATCHDOG=1"); // Tells systemd to reset it's watchdog timer
	}

	return (EXIT_SUCCESS);
}

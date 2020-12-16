#ifndef __OFFGRID_CONSTANTS_H
#define __OFFGRID_CONSTANTS_H

 /* RESPOND TO COMMAND MESSAGES FROM THIS ID */
//#define MASTER_CAN_ID 0x01

/* MESSAGE CODES SHARED ACROSS NODES */
#define MSG_RESERVED_00		0x00
#define MSG_RESERVED_01		0x01
#define MSG_RESERVED_02		0x02
#define MSG_RESERVED_03		0x03
#define MSG_RESERVED_04		0x04
#define MSG_RESERVED_05		0x05

/* Consider grouping these under a MSG_CONTROL with sub messages (CTRLMSG_KEEP_ALIVE, etc) in the second byte.  This way we don't use up the rest unnecessarily. */

#define MSG_KEEP_ALIVE		0x06 /* (DLC = 1) Sent by master (or designate) every 1 second to ensure bus is operating.  Modules will time-out if this is not received by a predetermined maximum time */
								 /* (DLC = 2) Module acknowledges by sending MSG_KEEP_ALIVE with a second data byte being a status code.  "0" = OK, others are module dependent. */

#define MSG_POWER_ON		0x07 /* (DLC = 1) Sent by a module when it has completed initialization */
								 /* (DLC = 2) Master acknowledges by sending MSG_POWER_ON with a second data byte being a status code.    "0" = OK, others are to be defined. */

#define MSG_CAN_STATUS		0x08 /* Every second each module sends this with CAN status details */

#define MSG_GET_MEMMAP      0x09 /* Not sure yet how this is going to work, but it should allow the master to dynamically obtain writeable/readable addresses from the module */
#define MSG_RETURN_MEMMAP   0x0A

#define MSG_RESERVED_10     0x10 
#define MSG_GET_SET_ERROR   0x11 /* Sent by a module in response to a get/set request that could not be satisfied */

#define MSG_SET_8_8         0x12 /* Sent by a master requesting a module to set address to supplied data.  1 byte address 1 byte data */
#define MSG_GET_8_8         0x13 /* Sent by a master requesting a module to return the data at supplied address.   1 byte address 1 byte data */
#define MSG_RETURN_8_8      0x14 /* Sent by a module in response to a master's request for data.    1 byte address 1 byte data */

#define MSG_SET_8_16        0x15
#define MSG_GET_8_16        0x16
#define MSG_RETURN_8_16     0x17

#define MSG_SET_8_32        0x18
#define MSG_GET_8_32        0x19
#define MSG_RETURN_8_32     0x1A

#define MSG_RESERVED_1B     0x1B
#define MSG_RESERVED_1C     0x1C
#define MSG_RESERVED_1D     0x1D
#define MSG_RESERVED_1E		0x1E
#define MSG_RESERVED_1F		0x1F



#define MSG_BROADCAST_00	0x40 /* Used for each module to send predefined periodic data, maybe dynamic in the future */
#define MSG_BROADCAST_01	0x41
#define MSG_BROADCAST_02	0x42
#define MSG_BROADCAST_03	0x43
#define MSG_BROADCAST_04	0x44
#define MSG_BROADCAST_05	0x45
#define MSG_BROADCAST_06	0x46
#define MSG_BROADCAST_07	0x47
#define MSG_BROADCAST_08	0x48
#define MSG_BROADCAST_09	0x49
#define MSG_BROADCAST_0A	0x4A
#define MSG_BROADCAST_0B	0x4B
#define MSG_BROADCAST_0C	0x4C
#define MSG_BROADCAST_0D	0x4D
#define MSG_BROADCAST_0E	0x4E
#define MSG_BROADCAST_0F	0x4F

// Fatal error flash codes - make sure none of these are ambiguous when flashing as two hex nybbles
// 0x0? is acceptable but not 0x?0
// Maybe avoid 1's if possible
// Also avoid high numbers - maybe keep them under 8?
#define ERR_FATAL_WDT_TIMEOUT						0x03
#define ERR_FATAL_DEBUG_4							0x04

#define ERR_FATAL_UNHANDLED_INTERRUPT_1				0x11
#define ERR_FATAL_UNHANDLED_INTERRUPT_2				0x12
#define ERR_FATAL_UNHANDLED_INTERRUPT_3				0x13
#define ERR_FATAL_UNHANDLED_INTERRUPT_4				0x14

#define ERR_FATAL_RX_QUEUE_OVERFLOW					0x21
#define ERR_FATAL_TX_QUEUE_OVERFLOW					0x22

// DEBUGGING - EEPROM addresses to store CAN status on WDT interrupt prior to reset
#define addr_last_mcp_eflg 0x60
#define addr_last_mcp_rec 0x61
#define addr_last_mcp_tec 0x62
#define addr_last_rx_queue_count 0x63
#define addr_last_rx_queue_count_max 0x64

#endif /* __OFFGRID_CONSTANTS_H */

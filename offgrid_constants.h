#ifndef __OFFGRID_CONSTANTS_H
#define __OFFGRID_CONSTANTS_H

/* MESSAGE CODES SHARED ACROSS NODES */
#define MSG_RESERVED_00		0x00
#define MSG_RESERVED_01		0x01
#define MSG_RESERVED_02		0x02
#define MSG_RESERVED_03		0x03
#define MSG_RESERVED_04		0x04
#define MSG_RESERVED_05		0x05

#define MSG_KEEP_ALIVE		0x06
#define MSG_POWER_ON		0x07

#define MSG_GET_INTERFACE       0x08 /* Query for available memory mapped topics, accessible with MSG_SET_X_X/MSG_GET_X_X.  No data requests that the device return all available interfaces.  Data=address returns only the interface associated with that address, or error/nothing */
#define MSG_RETURN_INTERFACE    0x09 /* Return one message for each available interface when queried by MSG_GET_INTERFACES */
#define MSG_GET_INTERFACE_ERROR 0x0A

#define MSG_RESERVED_10     0x10
#define MSG_GET_SET_ERROR   0x11

#define MSG_SET_8_8         0x12
#define MSG_GET_8_8         0x13
#define MSG_RETURN_8_8      0x14

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

#endif /* __OFFGRID_CONSTANTS_H */

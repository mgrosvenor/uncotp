/*
 * Copyright (c) 2016, All rights reserved.
 * See LICENSE.txt for full details. 
 * 
 *  Created:   29 Mar 2016
 *  File name: packets.h
 *  Description:
 *  <INSERT DESCRIPTION HERE> 
 */
#ifndef SRC_PACKETS_H_
#define SRC_PACKETS_H_

typedef struct __attribute__((packed)){
    //Note 1: Client and server times cannot be compared unless there is some kind of time synchronisation (eg PTP)
    //Note 2: Hardware cycle counts and software times cannot be compared unless there is time synchronisation and conversion
    //Note 3: To save space, some times are 32bits only, which assumes that time differences will be <4 seconds.
    //Note 4: Assumption 3 can be checked using swRxTimeNs - swTxTimeNs which is the absolute unix time.
    uint64_t swTxTimeNs;   //Client Unix time in ns, used for RTT time estimation
    uint64_t hwTxTimeNs;   //Client hardware used for network time estimation
    uint64_t hwRxTimeNs;   //Server hardware used for network time estimation
    uint64_t swRxTimeNs;   //Server Unix time in ns, used for host processing estimation
} etcpTime_t;

typedef struct __attribute__((packed)){
    uint32_t offset; //Offset from the base seq number, max 4 billion
    uint32_t count;  //Number of ack's in this range max, 4 billion
    etcpTime_t time;
} etcpSackField_t;

_Static_assert(sizeof(etcpSackField_t) == 5 * sizeof(uint64_t) , "Don't let this grow too big, 40B is big enough!");

#define UNCO_MAX_SACK 12 //This is arbitrarily set, to get a nice number of sacks, but make the packet not too big (~512B)
typedef struct __attribute__((packed)){
    i64 sackBaseSeq;
    uint64_t sackCount    : 8;  //Max 256 sack fields in a packet (256*32B = 8192B ~= 1 jumbo frame)
    uint64_t reserved     : 24; //Not in use right now
    uint64_t rxWindowSegs : 32; //Max 4B segment buffers in the rx window
} etcpMsgSackHdr_t;

typedef struct __attribute__((packed)){
    uint64_t seqNum;
    uint32_t datLen;     //Max 4GB per message
    uint32_t txAttempts; //Max 4 billion retires
} etcpMsgDatHdr_t;

//Enum describing the types of etcp packets.
typedef enum {
    ETCP_ERR = 0x00, //Not a valid message, something wrong

    //Uses the the data message packet format
    ETCP_CON = 0x01, //The first packet on this connection
    ETCP_FIN = 0x02, //Is the last packet on this connection
    ETCP_DAT = 0x03, //Just another data packet

    //Uses the acknowledgement packet format
    ETCP_ACK = 0x04, //Contains acknowledgement fields
    ETCP_DEN = 0x05, //Connection denied
} etcpMsgType_t;


//Assumes a fast layer 2 network (10G plus), with built in check summing and reasonable latency. In this case, sending
//more bits, is better than sending many more packets at higher latency. Keep this generic header pretty minimal, but use nice
//large types without too many range restrictions.
typedef struct __attribute__((packed)){
    union{
        struct{
            uint64_t magic     :32;  //ASCII encoded "ETCP" -- To make hexdumps easier to parse
            uint64_t ver       :8;  //Protocol version. Max 255 versions
            uint64_t type      :24; //16M different message types for each protocol version. How could we run out...?
        };
        uint64_t fulltype; //A full type is the magic string, version number and message type in one 64bit into make it easy to parse
    };

    uint32_t srcPort;    //"port" on the tx side, max 4 billion ports
    uint32_t dstPort;    //"port" on the rx side, max 4 billion ports

    etcpTime_t ts; //Timing info for estimation
} etcpMsgHead_t;


_Static_assert(sizeof(etcpMsgHead_t) == 6 * sizeof(uint64_t) , "Don't let this grow too big, 48B is big enough!");

#define ETCP_MAGIC 0x45544350ULL //"ETCP" in ASCII
#define ETCP_V1 0x1ULL
#define ETCP_V1_FULLHEAD(MSG) ((ETCP_MAGIC << 32) + (ETCP_V1 << 24) + (MSG) )

#endif /* SRC_PACKETS_H_ */

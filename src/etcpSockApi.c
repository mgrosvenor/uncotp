/*
 * Copyright (c) 2016, All rights reserved.
 * See LICENSE.txt for full details. 
 * 
 *  Created:   31 Mar 2016
 *  File name: etcpSockApi.c
 *  Description:
 *  <INSERT DESCRIPTION HERE> 
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "etcpSockApi.h"
#include "etcp.h"
#include "HashTable.h"
#include "debug.h"
#include "utils.h"
#include "etcpState.h"



typedef enum {
    ETCPSOCK_UK, //Unknown, not yet bound/connected
    ETCPSOCK_LA, //For listening/accepting
    ETCPSOCK_SR, //For sending/receiving
} socketType_t;


struct etcpSocket_s;
typedef struct etcpSocket_s
{
    etcpState_t* etcpState; //Back reference to the etcp global state

    socketType_t type;
    union{
        etcpLAMap_t* la;  //For listening/acepting new connections
        etcpSRConns_t sr; //For sending/receving on existing connections
    };
} etcpSocket_t;


//Delete a socket
void etcpSockeDelete(etcpSocket_t* const sock)
{
    //What?
    if_unlikely(!sock){
        WARN("Supplied an empty socket structure to delete?\n");
        return;
    }

    //Deal with socket internal members
    switch(sock->type){
        case ETCPSOCK_SR:
            if(sock->sr.sendConn){
                etcpConnDelete(sock->sr.sendConn);
            }
            if(sock->sr.recvConn){
                etcpConnDelete(sock->sr.recvConn);
            }
            break;
        case ETCPSOCK_LA:
            if(sock->la){
                srcsMapDelete(sock->la);
            }
            break;

        case ETCPSOCK_UK:
            //Nothing to do here, we don't know what socket type it is
            break;

        //defualt intentionally no default case so that extra values in the enum will be picked up

    }

    //Done with the socket now too.
    free(sock);
}


//Make a new "socket" for either listening on or writing to
//A socket is a generic container that holds either a pair of connection read/write queues, or a pair of listening/accepted queues
etcpSocket_t* etcpSocketNew(etcpState_t* const etcpState)
{
    etcpSocket_t* const sock = (etcpSocket_t* const)calloc(1,sizeof(etcpSocket_t));
    if_unlikely(!sock){
        return NULL;
    }
    sock->etcpState = etcpState;
    sock->type      = ETCPSOCK_UK;
    return sock;

}

etcpError_t remConnMapping(etcpSocket_t* const sock, const uint64_t srcAddr, const uint32_t srcPort, const uint64_t dstAddr, const uint32_t dstPort)
{

    if_unlikely(!sock){
        ERR("Why are you trying to remove from an empty socket?\n");
        return etcpERANGE;
    }

    //Uniqye con mappings are only found in in send/recieve sockets
    if_unlikely(sock->type != ETCPSOCK_SR){
        WARN("Wrong socket type supplied\n");
        return etcpEWRONGSOCK;
    }

    etcpState_t* const state = sock->etcpState;
    ht_t* const dstMap = state->dstMap;

    htKey_t dstKey = {.keyHi = dstAddr, .keyLo = dstPort }; //Flip the SRC/DST around so they match the listener side
    etcpLAMap_t* srcsMap = NULL;
    //First check if this destination is in our destinations map, if not, add a new sources map, based on this destination
    htError_t htErr = htGet(dstMap,&dstKey,(void**)&srcsMap);
    if_unlikely(htErr == htENOTFOUND){
        ERR("Why are you trying to remove a connection that's not there?\n");
        return etcpEHTERR;
    }

    //This should probably be made thread safe??
    etcpConn_t* conn = NULL;
    ht_t* const srcsTable = srcsMap->table;
    htKey_t srcKey = {.keyHi = srcAddr, .keyLo = srcPort }; //Flip the SRC/DST around so they match the listener side    ;
    htErr = htGet(srcsTable,&srcKey,(void**)&conn);
    if_unlikely(htErr == htENOEROR){
        ERR("Why are you trying to remove a connection that's not there?\n");
        return etcpENOTCONN;
    }

    //Delete the conenction
    etcpConnDelete(conn);

    //If the srcmap is empty (except for this entry), get rid of it too
    if_eqlikely(srcsMap->listenQ->readable == 1){
        srcsMapDelete(srcsMap);

        //And remove it from the destination map
        htRem(dstMap,&dstKey);
    }


    return etcpENOERR;
}







etcpError_t addConnMapping(etcpSocket_t* const sock, const uint32_t windowSizeLog2, const uint32_t buffSize, const uint64_t srcAddr, const uint32_t srcPort, const uint64_t dstAddr, const uint32_t dstPort, const bool isSender, const i64 vlan, const i64 prioirty)
{
    if_unlikely(sock->type != ETCPSOCK_SR){
        WARN("Wrong socket type supplied\n");
        return etcpEWRONGSOCK;
    }

    etcpState_t* const state = sock->etcpState;
    ht_t* const dstMap = state->dstMap;

    htKey_t dstKey = {.keyHi = dstAddr, .keyLo = dstPort };
    etcpLAMap_t* srcsMap = NULL;
    //First check if this destination is in our destinations map, if not, add a new sources map, based on this destination
    htError_t htErr = htGet(dstMap,&dstKey,(void**)&srcsMap);
    if_eqlikely(htErr == htENOTFOUND){
        srcsMap = srcsMapNew(0,0,vlan,prioirty); //Listen window/buff size = 0 because we are not listening.
        if_unlikely(!srcsMap){
            WARN("Ran out of memory making new sources connections container\n");
            return etcpENOMEM;
        }

        htErr = htAddNew(dstMap,&dstKey,srcsMap);
        if_unlikely(htErr != htENOEROR){
            ERR("Failed on hash table with error=%li\n", htErr);
            return etcpENOTCONN;
        }
    }

    //We have a sources map for this destination. Make a new connection structure
    etcpConn_t* const conn = etcpConnNew(sock->etcpState, windowSizeLog2,buffSize,srcAddr,srcPort, dstAddr,dstPort, vlan, prioirty);
    if_unlikely(conn == NULL){
        WARN("Ran out of memory trying to make a new connection\n");
        return etcpENOMEM;
    }

    //Now add the connection into the sources map
    //This should probably be made thread safe??
    ht_t* const srcsTable = srcsMap->table;
    htKey_t srcKey = {.keyHi = srcAddr, .keyLo = srcPort };
    htErr = htAddNew(srcsTable,&srcKey,conn);
    if_unlikely(htErr != htENOEROR){
        WARN("Trying to setup an existing connection\n");
        //We're already connected using the same source and destination ports!
        //Clean up the mess
        etcpConnDelete(conn);
        return etcpEALREADY;
    }

    //All mapped, now put the result into the right socket and return
    if_eqlikely(isSender){
        sock->sr.sendConn = conn;
    }
    else{
        sock->sr.recvConn = conn;
    }

    return etcpENOERR;

}


//Set the socket to have an outbound address. Etcp does not have a connection setup phase, so you can immediately send/recv directly on this socket
etcpError_t etcpConnect(etcpSocket_t* const sock, const uint32_t windowSizeLog2, const uint64_t buffSize, const uint64_t srcAddr, const uint64_t srcPort, const uint64_t dstAddr, const uint64_t dstPort, const bool doReturn, const i64 vlan, const i64 priority)
{
    if(sock->type != ETCPSOCK_UK){
        WARN("Wrong socket type, expected %li but got %li\n", ETCPSOCK_UK, sock->type);
        return etcpEWRONGSOCK;
    }

    sock->type = ETCPSOCK_SR; //Set this socket to be a new send/recv socket

    addConnMapping(sock,windowSizeLog2,buffSize,srcAddr,srcPort,dstAddr,dstPort,true, vlan, priority);


    //Most comms are two way, so return seems likely
    if_likely(doReturn){
        //Set up a mapping for the return connection (recv)
        addConnMapping(sock,windowSizeLog2,buffSize,dstAddr,dstPort,srcAddr, srcPort,false, vlan, priority);
    }


    return etcpENOERR;
}

//Set the socket to have an inbound address.
etcpError_t etcpBind(etcpSocket_t* const sock, const uint32_t windowSizeLog2, const uint32_t buffSize, const uint64_t dstAddr, const uint32_t dstPort, const i64 vlan, const i64 priority)
{

    if(sock->type != ETCPSOCK_UK){
        WARN("Wrong socket type, expected %li but got %li\n", ETCPSOCK_UK, sock->type);
        return etcpEWRONGSOCK;
    }

    DBG("Setting up a new listening address with window size %li\n", windowSizeLog2);
    etcpLAMap_t* const srcsMap = srcsMapNew(windowSizeLog2,buffSize,vlan,priority);
    if_unlikely(!srcsMap){
        WARN("Ran out of memory making new sources connections container\n");
        return etcpENOMEM;
    }

    etcpState_t* const state = sock->etcpState;
    ht_t* const dstMap = state->dstMap;
    htKey_t dstKey = {.keyHi = dstAddr, .keyLo = dstPort };
    htError_t htErr = htAddNew(dstMap,&dstKey,srcsMap);
    if(htErr == htEALREADY){
        ERR("Trying to bind to an address that is already in use address=%li, port=%li\n", dstAddr, dstPort);
        //TODO could add a SO_REUSEADDR idea here, but that could get messy. Skip for now.
        return etcpEALREADY;
    }

    sock->type = ETCPSOCK_LA;
    sock->la   = srcsMap;

    return etcpENOERR;
}


//Tell the socket to start accepting connections. Backlog sets the length of the queue for unaccepted connections
//Maximum backlog 4 billion...
etcpError_t etcpListen(etcpSocket_t* const sock, uint32_t backlogLog2)
{

    if_unlikely(sock->type != ETCPSOCK_LA){
        WARN("Wrong socket type, expected %li but got %li\n", ETCPSOCK_UK, sock->type);
        return etcpEWRONGSOCK;
    }

    sock->la->listenQ = cqNew(sizeof(etcpConn_t*), backlogLog2);
    if_unlikely(!sock->la->listenQ){
        WARN("Ran out of memory trying to allocate new listener queue\n");
        return etcpENOMEM;
    }

    return etcpENOERR;
}


//Dequeue new connections from the listen queue
etcpError_t etcpAccept(etcpSocket_t* const listenSock, etcpSocket_t** const acceptSock_o)
{
    if_unlikely(listenSock->type != ETCPSOCK_LA){
        WARN("Wrong socket type, expected %li but got %li\n", ETCPSOCK_LA, listenSock->type);
        return etcpEWRONGSOCK;
    }

    //if there is anything in the listen queue, then make a new socket and return it
    cq_t* listenQ = listenSock->la->listenQ;
    if_unlikely(listenQ == NULL){
        WARN("You need to do a listen on this socket before you can do an accept\n");
        return etcpEWRONGSOCK; //
    }

    //If RX is event triggered then do it now, this is the event!
    if_eqlikely(listenSock->etcpState->eventTriggeredRx){
        doEtcpNetRx(listenSock->etcpState); //This is a generic RX function
    }

    if_unlikely(listenQ->readable == 0){
        return etcpETRYAGAIN; //Nothing here to be collected. Come back another time
    }

    i64 len = sizeof(etcpConn_t*);
    i64 idx = -1;
    cqError_t err = cqPullNext(listenSock->la->listenQ,acceptSock_o,&len,&idx);
    if_unlikely(err != cqENOERR){
        ERR("Unexpected error on cq %s\n", cqError2Str(err));
        return etcpECQERR;
    }

    //Ok we're done with the entry in the cq
    err = cqReleaseSlot(listenQ,idx);
    if_unlikely(err != cqENOERR){
        ERR("Unexpected error on cq %s\n", cqError2Str(err));
        return etcpECQERR;
    }

    return etcpENOERR;
}


//Send on an etcpSocket
etcpError_t etcpSend(etcpSocket_t* const sock, const void* const toSendData, i64* const toSendLen_io)
{
    if_unlikely(sock->type != ETCPSOCK_SR){
        WARN("Wrong socket type, expected %li but got %li\n", ETCPSOCK_SR, sock->type);
        return etcpEWRONGSOCK;
    }

    if(toSendData != NULL && *toSendLen_io > 0){
        //DBG("Triggering user TX with packet of legth %li\n", *toSendLen_io);
        doEtcpUserTx(sock->sr.sendConn,toSendData,toSendLen_io);
    }

    bool ackFirst = true;
    i64 maxAck = -1;
    i64 maxDat = -1;

    cq_t* sendTxQ = sock->sr.sendConn ? sock->sr.sendConn->txQ : NULL; //Outbound DAT queue
    cq_t* sendRxQ = sock->sr.sendConn ? sock->sr.sendConn->rxQ : NULL; //Inbound ACK queue
    cq_t* recvTxQ = sock->sr.recvConn ? sock->sr.recvConn->txQ : NULL; //Outbound ACK queue
    cq_t* recvRxQ = sock->sr.recvConn ? sock->sr.recvConn->rxQ : NULL; //Inboud DAT queue

    //If TX is event triggered then do it now, this is the event!
    //DBG("Running TX traffic control\n");
    if(sock->etcpState->eventTriggeredTx){
        sock->etcpState->etcpTxTc(
                sock->etcpState->etcpTxTcState,
                sendTxQ,
                sendRxQ,
                recvTxQ,
                recvRxQ,
                &ackFirst,
                &maxAck,
                &maxDat);
    }

    maxAck = maxAck < 0 ? INT64_MAX : maxAck;
    maxDat = maxDat < 0 ? INT64_MAX : maxDat;

    DBG("Running %li acks and %li dats, with ackfirst=%li\n",maxAck, maxDat,ackFirst ? 1 : 0);
    if(maxAck > 0 || maxDat > 0){
        if_eqlikely(ackFirst){
            if_eqlikely(sock->sr.recvConn){
                doEtcpNetTx(sock->sr.recvConn->txQ,sock->etcpState,maxAck);
            }
            if_eqlikely(sock->sr.sendConn){
                doEtcpNetTx(sock->sr.sendConn->txQ,sock->etcpState,maxDat);
            }
        }
        else{
            if_eqlikely(sock->sr.sendConn){
                doEtcpNetTx(sock->sr.sendConn->txQ,sock->etcpState,maxDat);
            }
            if_eqlikely(sock->sr.recvConn){
                doEtcpNetTx(sock->sr.recvConn->txQ,sock->etcpState,maxAck);
            }
        }
    }


    return etcpENOERR;
}


//Recv on an etcpSocket
etcpError_t etcpRecv(etcpSocket_t* const sock, void* const data, i64* const len_io)
{
    if_unlikely(sock->type != ETCPSOCK_SR){
        WARN("Wrong socket type, expected %li but got %li\n", ETCPSOCK_SR, sock->type);
        return etcpEWRONGSOCK;
    }

    //If RX is event triggered then do it now, this is the event!
    if(sock->sr.recvConn->rxQ->available == 0){ //There's no more RX slot, don't even bother
        WARN("No RX slots available, not trying to RX\n");
        return etcpETRYAGAIN;
    }

    i64 rxPackets = 0;
    if_eqlikely(sock->etcpState->eventTriggeredRx){
        rxPackets = doEtcpNetRx(sock->etcpState); //It doesn't matter how much we receive here
    }

    if_eqlikely(data == NULL || len_io == NULL || *len_io == 0){
        //Not trying to RX anything, just triggering a HW rx in case, but check if there is something for the user
        return etcpENOERR;
    }

    if(rxPackets == 0 && sock->sr.recvConn->rxQ->readable == 0){
        //We didn't get anything new, and there's nothing waiting, so there's nothing to do
        return etcpETRYAGAIN;
    }


    i64 maxAckPkts      = 0;
    i64 maxAckSlots     = 0;
    i64 maxStaleSlots   = 0;
    i64 maxStaleAckPkts = 0;
    sock->etcpState->etcpRxTc(sock->etcpState->etcpRxTcState, sock->sr.recvConn->rxQ, sock->sr.recvConn->staleQ, sock->sr.recvConn->txQ, &maxAckSlots, &maxAckPkts, &maxStaleSlots, &maxStaleAckPkts);

    maxAckPkts = maxAckPkts < 0 ? sock->sr.recvConn->rxQ->__slotCount : maxAckPkts; //1 packet per slot is the maximum
    maxAckSlots = maxAckSlots < 0 ? sock->sr.recvConn->rxQ->__slotCount : maxAckSlots;
    maxStaleAckPkts = maxStaleAckPkts < 0 ? sock->sr.recvConn->staleQ->slotCount : maxStaleAckPkts;
    maxStaleSlots = maxStaleSlots < 0 ? sock->sr.recvConn->staleQ->slotCount : maxStaleSlots;

    if_eqlikely(maxAckPkts > 0 && maxAckSlots > 0){
        generateAcks(sock->sr.recvConn,maxAckPkts, maxAckSlots);
    }

    if_eqlikely(maxStaleAckPkts > 0 && maxStaleSlots > 0){
        generateStaleAcks(sock->sr.recvConn,maxStaleAckPkts, maxStaleSlots);
    }


    return doEtcpUserRx(sock->sr.recvConn,data,len_io);
}



//Close down the socket and free resources
void etcpClose(etcpSocket_t* const sock)
{
    if(!sock){
        return;
    }

    //Should do some shutdown things here, send a FIN message, clear out the buffers etc.
    //...

    etcpSockeDelete(sock);
}


//This function gets triggered on an incoming packet that has not been recognised as beloging to an active connection.
//It's job is to make a new socket structure and place that socket structure into the listening queue, ready for an accept to be called.
etcpError_t addNewConn(etcpState_t* const state, etcpLAMap_t* const srcsMap, const etcpFlowId_t* const flowId, bool noRet, etcpConn_t** const connRecv_o, etcpConn_t** const connSend_o )
{
    //The connection has not been established with this source
    //Check if there's space in the listening queue for another connection
    cqSlot_t* slot = NULL;
    i64 slotIdx = -1;
    cqError_t cqErr = cqGetNextWr(srcsMap->listenQ, &slot,&slotIdx);
    if_unlikely(cqErr == cqENOSLOT){
        //No space for this connection, ignore it.
        return etcpEREJCONN;
    }
    else if_unlikely(cqErr != cqENOERR){
        ERR("Unexpected error getting a listening slot: %s\n", cqError2Str(cqErr));
        return etcpEHTERR;
    }

    etcpError_t result = etcpENOERR;

    //We've reserved a slot in the listen queue, so make a new read/write socket structure. This will become an accept socket in the future
    etcpSocket_t* acceptSock = etcpSocketNew(state);
    if_unlikely(acceptSock == NULL){
        WARN("Ran out of memory making new socket\n");
        result = etcpENOMEM;
        goto failReleaseWr;
    }


    acceptSock->type = ETCPSOCK_SR;
    etcpError_t err = addConnMapping(acceptSock,srcsMap->listenWindowSizeLog2, srcsMap->listenBuffSize, flowId->srcAddr, flowId->srcPort, flowId->dstAddr, flowId->dstPort,false, srcsMap->vlan, srcsMap->priority);
    if(err != etcpENOERR){
        WARN("Could not add recv connection mapping\n");
        result = err;
        goto failDelSock;
    }

    //Does this connection need a return path? If so, set that up as well
    //Only make a return connection if it is desired
    const bool requireReturn = !noRet;
    if_likely(!requireReturn){
        //Flip the source and destination address so we can rcv acks here safely
        etcpError_t err = addConnMapping(acceptSock,srcsMap->listenWindowSizeLog2, srcsMap->listenBuffSize, flowId->dstAddr, flowId->dstPort, flowId->srcAddr, flowId->srcPort,true, -1, 01);
        if(err != etcpENOERR){
            WARN("Could not add connection mapping\n");
            result = err;
            goto failRemRecvConn;
        }
    }

    //We have a new s/r socket fully populated. Add it to the listening queue for accept to pickup
    memcpy(slot->buff, &acceptSock, sizeof(etcpSocket_t*));


    //Commit the new connection to the listening queue.
    cqErr = cqCommitSlot(srcsMap->listenQ, slotIdx, sizeof(etcpConn_t*));
    if_unlikely(cqErr != cqENOERR){
        ERR("Unexpected cq error while trying to commit slot: %s\n", cqError2Str(cqErr));
        result =  etcpECQERR;
        goto failRemSendConn;
    }

    *connRecv_o = acceptSock->sr.recvConn;
    *connSend_o = acceptSock->sr.sendConn;
    return etcpENOERR;

failRemSendConn:
    DBG("Removing send connection\n");
    etcpConnDelete(acceptSock->sr.sendConn);

failRemRecvConn:
    DBG("Removing send connection\n");
    etcpConnDelete(acceptSock->sr.recvConn);

failDelSock:
    DBG("Deleting socket\n");
    etcpSockeDelete(acceptSock);

failReleaseWr:
    DBG("Releasing listen slot\n");

    if_unlikely(cqErr != cqENOERR){
        ERR("Unexpected cq error while trying to exit on ENOMEM: %s\n", cqError2Str(cqErr));
        return etcpECQERR;
    }
    return result;

}




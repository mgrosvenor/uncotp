/*
 * Copyright (c) 2016, All rights reserved.
 * See LICENSE.txt for full details. 
 * 
 *  Created:   30 Mar 2016
 *  File name: etcp.h
 *  Description:
 *  <INSERT DESCRIPTION HERE> 
 */
#ifndef SRC_ETCP_H_
#define SRC_ETCP_H_

#include "types.h"
#include "CircularQueue.h"
#include "etcpState.h"
#include "etcpConn.h"

etcpError_t doEtcpUserTx(etcpConn_t* const conn, const void* const toSendData, i64* const toSendLen_io);
etcpError_t doEtcpUserRx(etcpConn_t* const conn, void* __restrict data, i64* const len_io);

etcpError_t doEtcpNetTx(etcpState_t* state, etcpConn_t* const conn);
void doEtcpNetRx(etcpState_t* state, etcpConn_t* const conn);



#endif /* SRC_ETCP_H_ */

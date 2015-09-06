/* Copyright (C) 2012 Xilinx Inc. */

#ifndef REMOTEPROC_H
#define REMOTEPROC_H

#include <stdint.h>
#include "remoteproc_kernel.h"

/* This is the baremetal application's rpmsg address used for the first
   communication channel announced to linux. It will be incremented for
   each new channel announced to linux.  */
#define APP_ADDR_START 0x50

// define the number of max. available rpmsg channels
#define MAX_RPMSG_CH      5

/* Resource table setup */
//void mmu_resource_table_setup(void);

/* Structure of a message passed between the application and the remoteproc
 * handler */
struct remoteproc_request {
	struct rpmsg_hdr* __hdr;
	unsigned int state;
};

enum rpmsg_ch_state {
    CH_UNUSED = 0,        // channel not announced (not active)
    CH_ANNOUNCED = 1,   // announced channel to other side
    CH_UP = 2          // received at least one msg from other side
};

struct rpmsg_channel;

typedef void (rpmsg_rx_callback)(struct rpmsg_channel* ch, uint8_t* data, uint32_t len);

struct rpmsg_channel {
   uint32_t local_addr;   // address used by this end of the channel (bm)
   uint32_t remote_addr;  // address used by remote end of channel (linux)
   enum rpmsg_ch_state state;
   char name[RPMSG_NAME_SIZE];
   rpmsg_rx_callback* cb;   // callback for received data
};


// Init function, has to be call before anything else.
void remoteproc_init();

// poll function processes data, has to be called periodically
int rpmsg_poll();

// announce a new channel to linux
struct rpmsg_channel* rpmsg_create_ch (const char* name,
        rpmsg_rx_callback* cb);

// send data to remote side (linux) using channel ch (has to be created in advance)
void rpmsg_send(struct rpmsg_channel* ch, const void* data, int len);

// copy the trace buffer settings to d
void rpmsg_get_trace_buf_settings (struct fw_rsc_trace* d);

#endif /* REMOTEPROC_H */

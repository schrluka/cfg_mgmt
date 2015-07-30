/* Copyright (C) 2012 Xilinx Inc. */

/****************************************************************
*
* compared to regular Vring implementations (virtualization) we
* rpmsg exchanges the roles of host and guest. So the bare metal
* application is the 'host', i.e. uses the used_elem->idx while
* the rpmsg kernel module acts as 'guest' in the sens of traditional
* virtio.
*/

#include <stdlib.h>
#include <stdio.h>
#include "xil_printf.h"
#include "xil_cache.h"
#include "xil_cache_l.h"
#include "xil_mmu.h"
#include <xscugic.h>

//#include "mutex.h"
#include "remoteproc_kernel.h"
#include "remoteproc.h"
#include "virtio_ring.h"


// enable debug message printing
//#define DBG_MSG

/* Linux host needs to know what resources are required by the FreeRTOS
 * firmware.
 *
 * This table is accessed by the kernel during initialisation of the remoteproc
 * driver in order to setup the system for AMP.
 */
struct resource_table {
	unsigned int version;
	unsigned int num;
	unsigned int reserved[2];
	unsigned int offset[NO_RESOURCE_ENTRIES];
	/* text carveout entry */
	struct fw_rsc_carveout text_cout;
	/* rpmsg vdev entry */
	struct fw_rsc_vdev rpmsg_vdev;
	struct fw_rsc_vdev_vring rpmsg_vring0;
	struct fw_rsc_vdev_vring rpmsg_vring1;
	/* trace entry */
	struct fw_rsc_trace trace;
	// describe the HW we need, this will be enabled in the TLB
	//struct fw_rsc_mmu slcr;
	struct fw_rsc_mmu uart0;
	struct fw_rsc_mmu scu;
	struct fw_rsc_mmu leds;
};

struct resource_table __resource resources = {
	1, /* we're the first version that implements this */
	6, /* number of entries in the table */
	{ 0, 0, }, /* reserved, must be zero */
	/* offsets to entries */
	{
		offsetof(struct resource_table, text_cout),
		offsetof(struct resource_table, rpmsg_vdev),
		offsetof(struct resource_table, trace),
		//offsetof(struct resource_table, slcr),
		offsetof(struct resource_table, uart0),
		offsetof(struct resource_table, scu),
		offsetof(struct resource_table, leds),
	},

	/* End of ELF file */
	//{ TYPE_CARVEOUT, 0, 0, ELF_END, 0, 0, "TEXT/DATA", },
	{ TYPE_CARVEOUT, ELF_START, ELF_START, (ELF_LEN), 0, 0, "TEXT/DATA", },

	/* rpmsg vdev entry */
	{ TYPE_VDEV, VIRTIO_ID_RPMSG, 0, RPMSG_IPU_C0_FEATURES, 0, 0, 0, 2,
			{ 0, 0 }, /* no config data */ },

	/* the two vrings */
	{ 0, 0x1000, VRING_SIZE, 1, 0 },
	{ 0, 0x1000, VRING_SIZE, 2, 0 },

	/* Trace buffer */
	{ TYPE_TRACE, TRACE_BUFFER_START, TRACE_BUFFER_SIZE, 0, "trace_buffer", },

	/* Peripherals */
	//{ TYPE_MMU, 0, TTC_BASEADDR, 0, 0xc02, "ttc", },
	{ TYPE_MMU, 1, STDOUT_BASEADDRESS, 0, 0xc02, "uart", },
	{ TYPE_MMU, 2, XPS_SCU_PERIPH_BASE, 0, 0xc02, "scu", },
	{ TYPE_MMU, 3, 0x41210000, 0, 0xc02, "leds", },
};

// allcate memory for the channels
struct rpmsg_channel channels[MAX_RPMSG_CH];


uint32_t next_rpmsg_addr = APP_ADDR_START;

volatile unsigned int txvring_kicks = 0;
volatile unsigned int rxvring_kicks = 0;

// vring resources as used by virtio_ring.c
static struct vring tx_vring;
static struct vring rx_vring;

// interrupt controller driver data structure is defined in main file
extern XScuGic IntcInst;



void block_send_message(u32 src, u32 dst, const void *data, u32 len);
void read_message(void);

static void txvring_task();
static void rxvring_task();


// implement a critical section by disabling IRQs
#define vPortEnterCritical()    Xil_ExceptionDisable()
#define vPortExitCritical()     Xil_ExceptionEnable()



/* This has to be called periodically by the main loop to perform data processing */
void rpmsg_poll()
{
    Xil_L1DCacheFlush();
    txvring_task();
    rxvring_task();
}


void kick_linux()
{
    XScuGic_SoftwareIntr(&IntcInst, NOTIFY_LINUX_IRQ, 1);
    //xil_printf("kicked linux\n");
}


void txvring_irq2(void *data)
{
	txvring_kicks++;
}

static void txvring_task( )
{
    int kicked = 0;

    // Enter a critical section, for atomicity (IRQ)
    vPortEnterCritical();
    if (txvring_kicks) {
        txvring_kicks--;
        kicked = 1;
    }
    vPortExitCritical();

    if (kicked) {
        // the kernel has sent us something, invalidate our L1 cache to get new data
        //Xil_L1DCacheFlushRange((uint32_t)&tx_vring, tx_vring.vring_len);
        Xil_L1DCacheFlush();
    }
}

void rxvring_irq3(void *data)
{
	/* Linux kick since it has put data to the RX ring */
	rxvring_kicks++;
}

static void rxvring_task()
{
	int kicked = 0;

    /* Enter a critical section, for atomicity */
    vPortEnterCritical();
    if (rxvring_kicks) {
        rxvring_kicks--;
        kicked = 1;
    }
    vPortExitCritical();

    if (kicked) {
        // the kernel has sent us data, invalidate our L1 cache to get new data
        // theoretically the cache is turned off so this should not be required
        //Xil_L1DCacheFlush();

        //xil_printf("reading data from rx vring\n");
        // process all messages
        while (vring_available(&rx_vring))
            read_message();
    }
}

/* -------------------------------------------------------------------------- */

/*
 * Function to send messages to Linux through txvring. It prepends the data
 *   with the required rpmsg header
 * @para:
 *  src: source address of the remote processor message
 *  dst: destination address of the remote processor message
 *  data: data of the message
 *  len: length of the data, will be truncated to DATA_LEN_MAX
 * @return:
 *  0: succeeded
 *  1: failed
 */
 int __send_message(u32 src, u32 dst, const void *data, u32 len)
 {
    int32_t idx;

    // try to get a buffer for the message
    idx = vring_get_buf(&tx_vring);

    if (idx < 0)
        return 1;   // no buffer available right now

    // ok, we have a descriptor, lets look at its content
#ifdef DBG_MSG
    xil_printf("TX: using buffer at x%08x with flagsx%04x\n", tx_vring.desc[idx].addr, tx_vring.desc[idx].flags);
#endif
    // create the rpmsg header and add payload data
    struct rpmsg_hdr *hdr = (struct rpmsg_hdr *)(tx_vring.desc[idx].addr);
    hdr->src = src;
	hdr->dst = dst;
	hdr->reserved = 0;
	hdr->flags = 0;
	if (len > DATA_LEN_MAX)
	{
        xil_printf("rpmsg __send_message: len=%d is too long, truncating, ie data loss\n", len);
        len = DATA_LEN_MAX;
	}
	hdr->len = (unsigned short)len; // data len
	memcpy(&(hdr->data), data, hdr->len);
	//int clr_len = DATA_LEN_MAX - len;
	// clear space not used by message
	//memset(&(hdr->data)+len, 0, clr_len);

    // tell linux that we have a message for it
    // Note: necessary memory barriers are done in this function
    vring_publish_buf(&tx_vring, (uint16_t)idx, PACKET_LEN_MAX, 1);

    return 0;
}


/*
 * Function to send messages to Linux through txvring.
 * It will not return until it sends successfully.
 * @para:
 *  src: source address of the remote processor message
 *  dst: destination address of the remote processor message
 *  data: data of the message
 *  len: length of the data
 */
void block_send_message(u32 src, u32 dst, const void *data, u32 len)
{
	while(__send_message(src, dst, data , len))
	{
        // wait until a buffer becomes available
        // send cpu to sleep, we wake when automatically on an interrupt
        //__asm__ __volatile__ ("wfe" ::: "memory");
        txvring_task(); // this checks for kicks from the kernel and
	}
}


void read_message(void)
{
    int32_t index = vring_get_buf(&rx_vring);

    if (index < 0)
    {
        //xil_printf("read_message: no buffer available in rx_vring\n");
        return;
    }

    // load address of the buffer associated with this descriptor
    struct rpmsg_hdr *hdr = (struct rpmsg_hdr *)(rx_vring.desc[index].addr);

     // make sure no old data is in our local L1 cache
    Xil_L1DCacheFlushRange((uint32_t)hdr, PACKET_LEN_MAX);

#ifdef DBG_MSG
//    xil_printf("ring_rx_used at %08x ", ring_rx_used);
//	xil_printf("ring_rx is at %08x\n", ring_rx);
//    xil_printf("ring_rx_used: idx=%d\n", ring_rx_used->idx);
    xil_printf("RX: mem=x%08x, src=x%x, dst=x%x, flags=x%08x, len=%d\n", (u32)hdr, hdr->src, hdr->dst, hdr->flags, hdr->len);
    int i;
    for (i=0; (i<hdr->len) && (i<32); i++)
        xil_printf(" %02x",hdr->data[i]);
    xil_printf("\n");
#endif

	// search which channel this message belongs to
    for (int i=0; i<MAX_RPMSG_CH; i++)
    {
        if ((channels[i].state == CH_ANNOUNCED) || (channels[i].state == CH_UP))
        {
            // ok, this channel structure is valid
            if (hdr->dst == channels[i].local_addr)
            {
                struct rpmsg_channel* ch = channels+i;
                // this channel is address, deliver message
                ch->remote_addr = hdr->src;    // remember link partner's address
                ch->state = CH_UP;
                // check if we have a valid callback and call it
                rpmsg_rx_callback* cb = channels[i].cb;
                if (cb != NULL)
                {
                    cb((void*)ch, hdr->data, hdr->len);
                    break;
                }
            }
        }
    }

   	// return the buffer to linux (recycling) (don't know what we should put at len)
   	vring_publish_buf(&rx_vring, index, PACKET_LEN_MAX, 1);
	return;
}


// create a new rpmsg communication channel
// name: name which will be announce to linux, hard length limit defined
//      by the protocol (32 bytes)
// cb: function pointer for callback function which get called for
//      received data
// returns a pointer to a global struct which holds all relevant data,
// don't mess with it. Returns NULL if no resources are available
struct rpmsg_channel* rpmsg_create_ch (const char* name,
        rpmsg_rx_callback* cb)
{
    struct rpmsg_channel* ch = NULL;

    for (int i=0; i<MAX_RPMSG_CH; i++)
    {
        if (channels[i].state == CH_UNUSED)
        {
            ch = &(channels[i]);
            break;
        }
    }
    if (ch == NULL)
    {
        xil_printf("Can't create channel: no channel struct available.\n");
        return ch;
    }

    // use incrementing addresses as our address
    ch->local_addr = next_rpmsg_addr;
    next_rpmsg_addr++;
    // we don't yet know the remote address, so we send broadcasts
    // FIXME: this has the potential for big troubles, debugging required
    ch->remote_addr = RPMSG_ADDR_ANY;
    // remember the channels name (just in case...)
    strncpy(ch->name, name, RPMSG_NAME_SIZE);
    ch->state = CH_ANNOUNCED;
    ch->cb = cb;

    xil_printf("announcing channel '%s' with addr x%x\n", name, ch->local_addr);

    // send announcement message to linux
    struct rpmsg_ns_msg ns_msg;
    memset(&ns_msg, 0, sizeof(ns_msg));
    ns_msg.addr = ch->local_addr;
    ns_msg.flags = RPMSG_NS_CREATE;
    strncpy(ns_msg.name, name, RPMSG_NAME_SIZE);

    block_send_message(ch->local_addr,
            LINUX_SERVICE_ANNOUNCEMENT_ADDR, &ns_msg, sizeof(ns_msg));

    return ch;
}


// transmit a message to Linux using the given channel
// this blocks until the message is sent.
void rpmsg_send(struct rpmsg_channel* ch, const void* data, int len)
{
    if (ch == NULL)
        return;

    if ((ch->state != CH_ANNOUNCED) && (ch->state != CH_UP))
        return;

    block_send_message(ch->local_addr, ch->remote_addr, data, len);
}


void remoteproc_init()
{
	memset(channels, 0, sizeof(channels)*MAX_RPMSG_CH);

    // load pointers to vring elements allocated by the kernel
    // this is the element defined by the vring protocol
    uint32_t addr = resources.rpmsg_vring0.da;
    vring_init(&tx_vring, addr, &kick_linux);
    addr = resources.rpmsg_vring1.da;
    vring_init(&rx_vring, addr, &kick_linux);
    //rx_vring.dbg_print = 1; // enable debug print messages

	xil_printf("resoucre table:\n");
	xil_printf("tx vring: desc=%08x avail=%08x used=x%08x len=x%04x\n", (uint32_t)tx_vring.desc, (uint32_t)tx_vring.avail, (uint32_t)tx_vring.used, tx_vring.vring_len);
	xil_printf("rx vring: desc=%08x avail=%08x used=x%08x\n", (uint32_t)rx_vring.desc, (uint32_t)rx_vring.avail, (uint32_t)rx_vring.used);

	// disable L1 data cache on vrings (this silently assumes that the actual data buffers are covered by the same 1MB region)
	// However, this is pretty save as the kernel assigns the buffer in a continuous block outside our memory region
	// This seemed to cause some problems, so use a cache flush in virtio_ring.c instead
	//Xil_SetTlbAttributes(resources.rpmsg_vring0.da & 0xFFF00000, 0x04de2);  // S=b0 TEX=b100 AP=b11, Domain=b1111, C=b0, B=b0
    //Xil_EnableMMU();

    //xil_printf("Setup IRQ handlers for remoteproc\n");
	XScuGic_Connect(&IntcInst, TXVRING_IRQ, &txvring_irq2, NULL);
	XScuGic_Enable(&IntcInst, TXVRING_IRQ);
	XScuGic_Connect(&IntcInst, RXVRING_IRQ, &rxvring_irq3, NULL);
	XScuGic_Enable(&IntcInst, RXVRING_IRQ);
}




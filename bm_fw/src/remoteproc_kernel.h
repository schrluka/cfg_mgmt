/* Copyright (C) 2012 Xilinx Inc. */

/*
 * This header defines a number of structs, macros and variables that are used
 * for communication with the Linux Kernel.
 *
 * - RPMSG primitives, and macros
 * - ELF file section definitions
 * - Resource Table types and value macros
 */

#ifndef REMOTEPROC_KERNEL_H
#define REMOTEPROC_KERNEL_H

#include <stdint.h>
#include <sys/cdefs.h>


// somehow the headers don't define u8, u16, ...
typedef uint8_t     u8;
typedef uint16_t    u16;
typedef uint32_t    u32;


/**********************************************************************************************************
*   I N T E R R U P T   C O N F I G
* This has to match the kernel's device tree entry
*/
/* IRQ to notify Linux */
#define NOTIFY_LINUX_IRQ			8
/* Tx Vring IRQ from Linux */
#define TXVRING_IRQ					9
/* Rx Vring IRQ from Linux */
#define RXVRING_IRQ					10


/* Just load all symbols from Linker script */

extern char *_vector_table;
#define ELF_START               (unsigned int)&_vector_table

extern char *__elf_end;
#define ELF_END					(unsigned int)&__elf_end

extern char *__elf_len;
#define ELF_LEN                 (unsigned int)&__elf_len

extern char *__trace_buffer_start;
#define TRACE_BUFFER_START		(unsigned int)&__trace_buffer_start
extern char *__trace_buffer_end;
#define TRACE_BUFFER_END		(unsigned int)&__trace_buffer_end

/* This value should be shared with Linker script */
#define TRACE_BUFFER_SIZE		0x8000

/* section helpers */
#define __to_section(S)			__attribute__((__section__(#S)))
#define __resource				__to_section(.resource_table)

/* virtio ids: keep in sync with the linux "include/linux/virtio_ids.h" */
#define VIRTIO_ID_CONSOLE		3 /* virtio console */
#define VIRTIO_ID_RPMSG			7 /* virtio remote processor messaging */

/* Indices of rpmsg virtio features we support */
#define VIRTIO_RPMSG_F_NS		0 /* RP supports name service notifications */


/* flip up bits whose indices represent features we support */
#define RPMSG_IPU_C0_FEATURES	(1<<VIRTIO_RPMSG_F_NS)

/* Resource info: Must match include/linux/remoteproc.h: */
#define TYPE_CARVEOUT			0
#define TYPE_DEVMEM				1
#define TYPE_TRACE				2
#define TYPE_VDEV				3
#define TYPE_MMU				4

// define max. number of resource entries
#define NO_RESOURCE_ENTRIES		13




/* Max length of channel name in service announcement messages */
#define RPMSG_NAME_SIZE             32

/* Linux address to receive service announcement */
#define LINUX_SERVICE_ANNOUNCEMENT_ADDR 0x35


struct fw_rsc_mmu {
	unsigned int type;
	unsigned int id;
	unsigned int da;
	unsigned int len; /* unused now */
	unsigned int flags;
	char name[32];
};

struct fw_rsc_carveout {
	unsigned int type;
	unsigned int da;
	unsigned int pa;
	unsigned int len;
	unsigned int flags;
	unsigned int reserved;
	char name[32];
};

struct fw_rsc_devmem {
	unsigned int type;
	unsigned int da;
	unsigned int pa;
	unsigned int len;
	unsigned int flags;
	unsigned int reserved;
	char name[32];
};

struct fw_rsc_trace {
	unsigned int type;
	unsigned int da;
	unsigned int len;
	unsigned int reserved;
	char name[32];
};

struct fw_rsc_vdev_vring {
	unsigned int da; /* address (physical) of the ring structure */
	unsigned int align;
	unsigned int num;
	unsigned int notifyid;
	unsigned int reserved;
};

struct fw_rsc_vdev {
	unsigned int type;
	unsigned int id;
	unsigned int notifyid;
	unsigned int dfeatures;
	unsigned int gfeatures;
	unsigned int config_len;
	char status;
	char num_of_vrings;
	char reserved[2];
};

/* original from xilinx project
struct rpmsg_hdr {
	unsigned int src;
	unsigned int dst;
	unsigned int reserved;
	unsigned short len;
	unsigned short flags;
	unsigned char data[0];
} __packed; */

/*************************************************************************************
* Taken from Linux 3.18 (Xilinx)
*/

/**
 * struct rpmsg_hdr - common header for all rpmsg messages
 * @src: source address
 * @dst: destination address
 * @reserved: reserved for future use
 * @len: length of payload (in bytes)
 * @flags: message flags
 * @data: @len bytes of message payload data
 *
 * Every message sent(/received) on the rpmsg bus begins with this header.
 */

struct rpmsg_hdr {
	u32 src;
	u32 dst;
	u32 reserved;
	u16 len;
	u16 flags;
	u8 data[0];
} __attribute__((packed));

/**
 * struct rpmsg_ns_msg - dynamic name service announcement message
 * @name: name of remote service that is published
 * @addr: address of remote service that is published
 * @flags: indicates whether service is created or destroyed
 *
 * This message is sent across to publish a new service, or announce
 * about its removal. When we receive these messages, an appropriate
 * rpmsg channel (i.e device) is created/destroyed. In turn, the ->probe()
 * or ->remove() handler of the appropriate rpmsg driver will be invoked
 * (if/as-soon-as one is registered).
 */
struct rpmsg_ns_msg  {
	char name[RPMSG_NAME_SIZE];
	u32 addr;
	u32 flags;
} __attribute__((packed));

/**
 * enum rpmsg_ns_flags - dynamic name service announcement flags
 *
 * @RPMSG_NS_CREATE: a new remote service was just created
 * @RPMSG_NS_DESTROY: a known remote service was just destroyed
 */
enum rpmsg_ns_flags {
	RPMSG_NS_CREATE		= 0,
	RPMSG_NS_DESTROY	= 1,
};

#define RPMSG_ADDR_ANY		0xFFFFFFFF



/* vring data buffer max length including the header */
#define PACKET_LEN_MAX				512
#define DATA_LEN_MAX				(PACKET_LEN_MAX - sizeof(struct rpmsg_hdr))

#endif /* REMOTEPROC_KERNEL_H */


/******************************************************************************************************************************
*
*   RPMSG Implementation for Bare Metal Applications
*
*   (c) 2015
*   Lukas Schrittwieser
*
*******************************************************************************************************************************
*
*  Header for virtio_ring.c
*
******************************************************************************************************************************/

#ifndef __VIRTIO_RING__
#define __VIRTIO_RING__


/******************************************************************************************************************************
*   C O N F I G                                                                                                              */

// number of buffers in the vring, must match kernal and must be a power of 2 (this should probably not be define...)
#define VRING_SIZE					256



/******************************************************************************************************************************
*   S T R U C T S                                                                                                            */

// the following structures represent the memory layout which is shared with kernel, so these have to match the kernel!

// Virtio ring descriptors: 16 bytes.  These can chain together via "next"
struct vring_desc {
	uint32_t addr;      // Address (physical).
	uint32_t addr_hi;   // for 64 bit systems, unused by us (32 bit only)
	uint32_t len;       // Length.
	uint16_t flags;     // The flags as indicated above.
	uint16_t next;      // The kernel chains descriptors via this index (of struct vring->desc)
} __attribute__((packed));

// unsigned int is used here for ids for padding reasons.
struct vring_used_elem {
	uint32_t id;    // Index (of struct vring->desc) of head of used descriptor chain.
	uint32_t len;   // Total length of the descriptor chain which was used (written to)
} __attribute__((packed));

// ring buffer of descriptor (desc) indices we (baremetal) want to transmit to the kernel
struct vring_used {
	uint16_t flags;
	uint16_t idx;
	struct vring_used_elem ring[VRING_SIZE];
} __attribute__((packed));

// ring of buffer descriptor (desc) indices linux wants to send to us (baremetal)
struct vring_avail {
    uint16_t avail_flags;
    uint16_t avail_idx;
    uint16_t ring[VRING_SIZE];
    uint16_t used_event_idx;
} __attribute__((packed));


// this struct is a complete description of a vring's resources.
// It does _NOT_ reflect the memory layout which is required for correct operation
// It exists for easier coding, the pointers are set during initialization.
struct vring {
    // pointers to the actual rings use for communication with the kernel
    // buffer descriptor heads (an array of VRING_SIZE elements)
    volatile struct vring_desc* desc;
    // ring buffer kernel->baremetal
    volatile struct vring_avail* avail;
    // ring buffer baremetal->kernel
    volatile struct vring_used* used;

    // private stuff which is used only by this code
    uint16_t avail_tail;    // tail index of available ring buffer

    uint16_t dbg_print;     // debug messages will be printed if this is not 0

    // handle of function which gets called when we want to kick linux (send an interrupt to it)
    void (*notify)();

    uint32_t vring_len;     // number of bytes occupied by memory shared with kernel (incl padding)
                            // this is needed for cache invalidation and is set in init functions
};


// comment taken from the kernel:
/* The standard layout for the ring is a continuous chunk of memory which looks
 * like this.  We assume num is a power of 2.
 *
 * struct vring
 * {
 *      // The actual descriptors (16 bytes each)
 *      struct vring_desc desc[num];
 *
 *      // A ring of available descriptor heads with free-running index.
 *      __u16 avail_flags;
 *      __u16 avail_idx;
 *      __u16 available[num];
 *      __u16 used_event_idx;
 *
 *      // Padding to the next align boundary.
 *      char pad[];
 *
 *      // A ring of used descriptor heads with free-running index.
 *      __u16 used_flags;
 *      __u16 used_idx;
 *      struct vring_used_elem used[num];
 *      __u16 avail_event_idx;
 * }; */




/******************************************************************************************************************************
*   P R O T O T Y P E S                                                                                                      */

// see C file for descriptions

void vring_init(struct vring* vr, uint32_t addr, void (*notify)());

int32_t vring_get_buf(struct vring* vr);

void vring_publish_buf(struct vring* vr, uint16_t idx, uint32_t len, int kick);

int vring_available(struct vring* vr);


#endif // __VIRTIO_RING__

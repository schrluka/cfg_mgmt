/******************************************************************************************************************************
*
*   RPMSG Implementation for Bare Metal Applications
*
*   (c) 2015
*   Lukas Schrittwieser
*
*******************************************************************************************************************************
*
*   This is the dual part to the kernel's code in kernel_root/drivers/virtio/virtio_ring.c
*
*   However, only the virtqueue part required by the RPMSG bus is implemented.
*   The basic idea behind virtio are two ring buffers called available and used. The available ring transfers buffers from the
*   kernel to the bare metal app (us). The used ring transfers the buffers back from us to the kernel. This is true for tx
*   (bare metal to linux) and the rx (linux to baremetal) vring.
*
******************************************************************************************************************************/

#include <stdint.h>
#include "virtio_ring.h"
#include "xil_printf.h"
#include "xpseudo_asm_gcc.h"
#include "xil_cache_l.h"


/******************************************************************************************************************************
*   D E F I N E S                                                                                                            */

/* This marks a buffer as continuing via the next field. */
#define VRING_DESC_F_NEXT	1
/* This marks a buffer as write-only (otherwise read-only). */
#define VRING_DESC_F_WRITE	2
/* This means the buffer contains a list of buffer descriptors. */
#define VRING_DESC_F_INDIRECT	4

/* The Host uses this in used->flags to advise the Guest: don't kick me when
 * you add a buffer.  It's unreliable, so it's simply an optimization.  Guest
 * will still kick if it's out of buffers. */
#define VRING_USED_F_NO_NOTIFY	1
/* The Guest uses this in avail->flags to advise the Host: don't interrupt me
 * when you consume a buffer.  It's unreliable, so it's simply an
 * optimization.  */
#define VRING_AVAIL_F_NO_INTERRUPT	1



/******************************************************************************************************************************
*   I M P L E M E N T A T I O N                                                                                              */


// initialize a vring, all data structures have to be configured by the other side and are expected at physical address addr
// vr: the vring to be intialized
// notify: callback function wich gets called when we want to kick linux
void vring_init(struct vring* vr, uint32_t addr, void (*notify)())
{
    vr->desc = (void*)(addr);  // buffer descriptor heads are at the address assigned to us by the kernel
    addr += VRING_SIZE * sizeof(struct vring_desc);
    vr->avail = (void*)(addr); // available ring follows immediatly after descriptor heads
    addr += sizeof(struct vring_avail);
    // padding to next 4kB (12 bit) page boundary is inserted here as required by vring protocol specs
    if ((addr & 0xFFF) != 0)
    {
        addr >>= 12;
        addr++;
        addr <<= 12;
    }
    vr->vring_len = addr+sizeof(struct vring_used) - (uint32_t)(vr->desc);
    // used section follows after padding (padding is inserted to prevent cache invalidations between the two cpus
    vr->used = (void*)addr;
    vr->notify = notify;
    vr->avail_tail = 0; // by convention head (kernel) and tail indices start at 0
    vr->dbg_print = 0;
}


// get the next buffer which was sent to us from the kernel.
// returns the index of the available buffer descriptor (not the buffer itself) or -1 if none is available
int32_t vring_get_buf(struct vring* vr)
{
    // the available index in the vring struct is moved by the linux kernel
    // if it has advanced before us there is a buffer which we can use
    uint16_t krnl_avail_idx = vr->avail->avail_idx;

    if (krnl_avail_idx == vr->avail_tail)
        return -1;    // no buffer available

    if (vr->dbg_print)
        xil_printf("vring_get_buf: krnl_avail_idx: %d  local_avail_idx: %d\n", krnl_avail_idx, vr->avail_tail);

    // ok, there is at least one descriptor (with attached buffer) available, use it
    // NOTE: the indices are free running uint16s, so we shorten them to the ring size here
    uint16_t a_index = (vr->avail_tail) % VRING_SIZE;   // therefore VRING_SIZE has to be a power of 2

    // ok, lets see which buffer is indexed by the available ring at the given available ring's index
    uint16_t available_desc_ind = vr->avail->ring[a_index];
    if (vr->dbg_print)
        xil_printf("   desc nr %d is available.\n", available_desc_ind);

    // consume this entry from the ring buffer of available buffer heads
    vr->avail_tail++;   // this is the ring buffer tail index

    return available_desc_ind;
}

// publish (pass to other side) the buffer described by vr->desc[idx]
// this will render the buffer visible to the other side (linux kernel)
// Note: The required index has to be obtained by rpmsg_get_buf()
// len: is the payload length in bytes
// Linux will be kicked if kick is not 0
void vring_publish_buf(struct vring* vr, uint16_t idx, uint32_t len, int kick)
{
    // load the index at which we will write to the used-ring
    uint16_t used_idx = vr->used->idx;

    // the indices are free running, limit it to array length
    used_idx = used_idx % VRING_SIZE;

    if (idx > VRING_SIZE)
        xil_printf("vring_publish_buf: idx=%d is invalid (too big)\n", idx);

    // make sure the kernel has not set any bogus flags
    vr->desc[idx].flags &= ~VRING_DESC_F_NEXT;
    vr->desc[idx].next = 0;

    if (vr->dbg_print)
        xil_printf("vring: publishing desc %d within used ring entry %d\n", idx, used_idx);
    // place the index of the descriptor we want to publish in the ring.
    vr->used->ring[used_idx].id = idx;
    vr->used->ring[used_idx].len = len;

    //dmb();  // ensure memory writes are ordered.
    __asm__ __volatile__ ("dsb" ::: "memory");

    // tell linux that we have placed something in the used ring
    vr->used->idx++;

    //dsb();  // wait until the CPU has updated the memory
    __asm__  __volatile__  ("dsb" ::: "memory");
    Xil_L1DCacheFlush();    // make sure data arrives at the other end

    if (vr->dbg_print)
        xil_printf("vring: used index is now %d\n", vr->used->idx);

    // kick the kernel (linux) to make it aware of the new data
    if ((kick != 0) && (vr->notify != 0))
        vr->notify();

}

// check if there are buffer available (sent by the other side, ie linux kernel)
// returns 1 of there is atleast one buffer, 0 otherwise
int vring_available(struct vring* vr)
{
    if (vr->avail->avail_idx == vr->avail_tail)
        return 0;    // no buffer available
    return 1;
}

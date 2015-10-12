/***********************************************************************************************************************
*
*   CFG_MGMT  -  Configuration Variable Management for AMP using RPMSG
*
* (c) 2015 Lukas Schrittwieser (LS)
*
*    This program is free software: you can redistribute it and/or modify
*    it under the terms of the GNU General Public License as published by
*    the Free Software Foundation, either version 2 of the License, or
*    (at your option) any later version.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program; if not, write to the Free Software
*    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*    Or see <http://www.gnu.org/licenses/>
*
************************************************************************************************************************
*
* cfg_rpmsg_link.c
*
* Communication with a bare metal application through a rpmsg channel in order to transfer configuration variable
* (meta-) values.
*
************************************************************************************************************************/

#define DEBUG

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/string.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/rpmsg.h>
#include <linux/spinlock.h>
#include <linux/slab.h>

#include "rpmsg_link.h"



// request and response types (codes) for communication with bare metal firmware (type field in  cfgReq_t)
#define REQ_NOP        0       // do nothing
// kernel to BM (requests)
#define REQ_N_VARS  1       // read number of variables (N_VARS)
#define REQ_WR_VAL  2       // write (kernel to BM) request
#define REQ_RD_VAL  3       // read value (BM to kernel) request
#define REQ_RD_MIN  4       // read min limit (BM to kernel) request
#define REQ_RD_MAX  5       // read max limit
#define REQ_NAME    6       // read name of variable with given index (0..N_VARS-1)
#define REQ_DESC    7       // read description text of variable with given index


// BM to kernel (response)
#define RES_OK      128     // requestion done, no further data (e.g. value written)
#define RES_N_VARS  129
#define RES_ID_ERR  130     // request with undefined id
#define RES_RD_VAL  131     // read (BM to kernel) response
#define RES_RD_MIN  132
#define RES_RD_MAX  133
#define RES_NAME    134
#define RES_DESC    135

#define RES_REQ_ERR 255     // unknown request

#define REQ_NONE 	0xffffffff	// invalid type code



// all information about one request, chained in a list of pending transactions
struct transaction {
    u32 msg_seq_nr;         // cfg_mgmt sequence number used in the request
    struct file_io_buf* buf;// where to store the data
    wait_queue_head_t* wq;  // wait queue to be woken when response arrives
    struct list_head list;  // used to chain the transactions
};



/************************************************************************************************************************
*   G L O B A L S
*/

// rpmsg channel for communication with the bare metal application
static struct rpmsg_channel* rpmsg_chnl = NULL;

// number of variables exported via sysfs (after a query to remote side)
int n_vars = -1;

// list of all pending transactions (request sent, no response received yet)
static struct list_head pending_list = LIST_HEAD_INIT(pending_list);
// lits of unsued transaction structs (for recycling)
LIST_HEAD(unused_list);

static spinlock_t pending_list_lock;
static spinlock_t unused_list_lock;
static spinlock_t seq_lock;



/************************************************************************************************************************
*   P R O T O T Y P E S
*/

static u32 get_next_seq_nr(void);

static int alloc_trans_struct(struct transaction** t);

static inline void add_pend_trans(struct transaction* t);




/************************************************************************************************************************
*   I M P L E M E N T A T I O N
*/

// must be called first, channel to be used for communication is passed as argument
int rpmsg_link_init(struct rpmsg_channel *ch)
{
    struct transaction* transactions;
    const int N = 16;

    spin_lock_init(&pending_list_lock);
    spin_lock_init(&unused_list_lock);
    spin_lock_init(&seq_lock);

    // add some transaction structs to the list of unused structs to speed things up on the first transactions
    transactions = kzalloc(sizeof(*transactions)*N, GFP_KERNEL);
    if (!transactions)
        return -ENOMEM;

    rpmsg_chnl = ch;
    return 0;
}


void rpmsg_link_exit()
{
    struct list_head* pos;
    struct list_head* temp;

    // empty transactions list
    spin_lock(&pending_list_lock);
    spin_lock(&unused_list_lock);
    list_for_each_safe(pos, temp, &pending_list) // we delete the object from the list -> use safe version
    {
        // convert list_head pointer to a pointer to the list object
        struct transaction* t = list_entry ( pos, struct transaction , list );

        dev_err(&rpmsg_chnl->dev, "%s: found pending transcation: seq=%d\n", __func__, t->msg_seq_nr);
        list_del(pos);
        kfree(t);
    }
    // free all unused transaction structs
    list_for_each_safe(pos, temp, &unused_list) // we delete the object from the list -> use safe version
    {
        // convert list_head pointer to a pointer to the list object
        struct transaction* t = list_entry ( pos, struct transaction , list );
        list_del(pos);
        kfree(t);
    }
    rpmsg_chnl = NULL;
    spin_unlock(&unused_list_lock);
    spin_unlock(&pending_list_lock);
}


// rpmsg callback function: receives messages from the bare metal application
void cfg_mgmt_rpmsg_cb(struct rpmsg_channel *rpdev, void *data, int len, void *priv, u32 src)
{
    struct list_head* pos;
    struct list_head* temp;
    struct transaction* trans = NULL;
    cfgMsg_t* response = data;

	// check length
	if (len != sizeof(cfgMsg_t))
	{
		dev_info(&rpdev->dev, "CFG_MGMT %s: Message from BM application has wrong length.\n", __func__);
		return;
	}

    // get the correct transaction struct

    spin_lock(&pending_list_lock);
    list_for_each_safe(pos, temp, &pending_list) // we delete the object from the list -> use safe version
    {
        // convert list_head pointer to a pointer to the list object
        struct transaction* t = list_entry ( pos, struct transaction , list );

        if (t->msg_seq_nr == response->seq) {
            // found it, remove it from the list
            trans = t;
            list_del(pos);
            break;
        }
    }
    spin_unlock(&pending_list_lock);

	if (!trans)	{
		dev_err(&rpdev->dev, "%s: cound not find a transaction for response with seq nr %d.\n",
            __func__, response->seq);
		return;
	}

    // We could cross check that response type with the request type, however we don't know it

    switch (response->type) {
    case RES_OK:
        dev_info(&rpmsg_chnl->dev, "%s: received OK responce", __func__);
        if (trans->buf) {
            trans->buf->len = 0;
            trans->buf->valid = true;
        }
        break;

    case RES_N_VARS:
        // copy the number of variables to our global variable, no need to return it
        n_vars = response->val;
        //*((int32_t*)trans->buf->buf) = response->val;
        if (trans->buf) {
            trans->buf->len = 0;        // no data in placed in io buffer
            trans->buf->valid = true;   // we got the anser
        }
        break;
    case RES_RD_VAL:
    case RES_RD_MIN:
    case RES_RD_MAX:
        if (trans->buf) {
            // convert numerical results to a string for communication with the user space
            trans->buf->len =  scnprintf(trans->buf->buf, IO_BUF_SIZE, "%d\n", response->val);
            trans->buf->err = 0;
            trans->buf->valid = true;
        } else {
            dev_err(&rpdev->dev, "%s: no io_buf\n", __func__);
        }
        break;
    case RES_NAME:
    case RES_DESC:
        if (trans->buf) {
            if (response->len  > IO_BUF_SIZE) {
                dev_err(&rpdev->dev, "%s: data part of response too long\n", __func__);
                trans->buf->err = -EINVAL;
                trans->buf->len = scnprintf(trans->buf->buf, IO_BUF_SIZE, "data part of response too long\n");
            } else {
                // copy string response to io buffer
                memcpy(trans->buf->buf, response->data, response->len);
                trans->buf->len = response->len;
                trans->buf->err = 0;
            }
            trans->buf->valid = true;
        } else {
            dev_err(&rpdev->dev, "%s: no io_buf\n", __func__);
        }
        break;

    case RES_ID_ERR:
        if (trans->buf) {
            trans->buf->len = scnprintf(trans->buf->buf, IO_BUF_SIZE,
                "received ID error for id %d in msg nr %d\n", response->ind, response->seq);
            trans->buf->valid = true;
        } else {
            dev_err(&rpdev->dev, "%s: no io_buf\n", __func__);
        }
        break;

    case RES_REQ_ERR:
        if (trans->buf) {
            trans->buf->len = scnprintf(trans->buf->buf, IO_BUF_SIZE,
                "received request error for msg nr %d\n", response->seq);
            trans->buf->valid = true;
        } else {
            dev_err(&rpdev->dev, "%s: no io_buf\n", __func__);
        }
        break;

    default:
        if (trans->buf) {
            trans->buf->len = scnprintf(trans->buf->buf, IO_BUF_SIZE,
                "unknonw type %d in msg nr %d\n", response->ind, response->seq);
            trans->buf->valid = true;
        } else {
            dev_err(&rpdev->dev, "%s: no io_buf\n", __func__);
        }
    }

    // if wait queue was registered wake it such that the process awaiting a result my continue
    if (trans->wq) {
        wake_up_interruptible(trans->wq);
    }

    // recycle transaction struct, iobuf is recycled or freed in cfg_mgmt
    memset(trans, 0, sizeof(*trans));   // clear content, just to be sure
    spin_lock(&unused_list_lock);
    list_add(&(trans->list), &unused_list);
    spin_unlock(&unused_list_lock);
}


// query the number of config variables available at the remote side.
// If wait_q_p points to a wait_queue_head the process will be blocked until the answer from the bare metal
// application has arrived and the number of variables is returned.
// if wait_q_p = 0 the function returns 0 immediatly, the global n_vars will be updated once the response arrives
// neg value indicates an error
int get_n_vars(wait_queue_head_t* wait_q_p)
{
    int ret;
    cfgMsg_t req;
    struct transaction* t;

    if (!rpmsg_chnl)
        return -EINVAL;

	dev_dbg(&rpmsg_chnl->dev, "%s: requesting n_vars\n", __func__);

	// invalidate all request fields
	req.seq = get_next_seq_nr();
	req.ind = -1;
	req.val = 0;
	req.len = 0;
	req.type = REQ_N_VARS;

	// create a structure for this transaction
    ret = alloc_trans_struct(&t);   // get an empty (or new) struct
    if (ret) {
        dev_err(&rpmsg_chnl->dev, "%s: can't allocate transaction struct: %d\n", __func__, ret);
        return ret;
    }
    t->buf = NULL;  // need no IO buffer to read the number of variables (results goes to gobal var)
    t->msg_seq_nr = req.seq;    // rpmsg callback uses this for identification
    t->wq = wait_q_p;

    // add struct to list of pending transactions
    add_pend_trans(t); // contains the necessary locking

	// send the request to the other side,
	ret = rpmsg_send(rpmsg_chnl, (void*)(&req), sizeof(req));
	if (ret) {
        dev_dbg(&rpmsg_chnl->dev, "%s: rpmsg send failed with %d\n", __func__, ret);
        return ret;
	}
	// block calling user context until we receive a reply
	if (wait_q_p) {
        ret = wait_event_interruptible((*wait_q_p), n_vars >= 0);
        if (ret) {	// abort in case we got interrupted
            dev_err(&rpmsg_chnl->dev, "%s: interrupted\n", __func__);
            return ret;
        }
    } else
        return 0;   // no wait queue, so we return immediately

    dev_dbg(&rpmsg_chnl->dev, "%s: n_vars is %d\n", __func__, n_vars);
    return n_vars;
}


// initiate a variable access (either read or write)
int access_var(int index, access_t acc, struct file_io_buf* buf, wait_queue_head_t* wait_q_p)
{
    int ret;
    cfgMsg_t req;
    struct transaction* t;

    if (!rpmsg_chnl)
        return -EINVAL;

    if (!buf) {
        dev_err(&rpmsg_chnl->dev, "%s: no io buffer, abort\n", __func__);
        return -EINVAL;
    }

    // create a structure for this transaction
    ret = alloc_trans_struct(&t);   // get an empty (or new) struct
    if (ret) {
        dev_err(&rpmsg_chnl->dev, "%s: can't allocate transaction struct: %d\n", __func__, ret);
        return ret;
    }

	// set all request fields
	req.ind = index;
	req.val = 0;
	req.len = 0;
	switch(acc) {
	case ACC_VAL:
        if (buf->rnw) {
            req.type = REQ_RD_VAL;
        } else {
            // write
            req.type = REQ_WR_VAL;
            // convert string to integer
            ret = kstrtol(buf->buf, 0, (long int*)&req.val);
            if (ret) {
                dev_err(&rpmsg_chnl->dev, "%s: can't parse string '%s' %d\n", __func__, buf->buf, ret);
                return ret;
            }
            dev_dbg(&rpmsg_chnl->dev, "%s: writing val %ld to index %d\n", __func__, (long int)req.val, index);
        }
        break;
    case ACC_MIN:
        req.type = REQ_RD_MIN;
        break;
	case ACC_MAX:
        req.type = REQ_RD_MAX;
        break;
    case ACC_DESC:
        req.type = REQ_DESC;
        break;
    case ACC_NAME:
        req.type = REQ_NAME;
        break;
    default:
        return -EINVAL;

	}
	req.seq = get_next_seq_nr();

    t->buf = buf;
    t->msg_seq_nr = req.seq;    // rpmsg callback uses this for identification
    t->wq = wait_q_p;

    // add struct to list of pending transactions
    add_pend_trans(t); // contains the necessary locking

	// send the request to the other side,
	ret = rpmsg_send(rpmsg_chnl, (void*)(&req), sizeof(req));
	if (ret) {
        dev_dbg(&rpmsg_chnl->dev, "%s: rpmsg send failed with %d\n", __func__, ret);
        return ret;
	}
	// check if we have a wait queue (ie if we should block)
	if (wait_q_p) {
        // block calling user context until we receive a reply
        ret = wait_event_interruptible((*wait_q_p), buf->valid);
        if (ret) {	// abort in case we got interrupted
            dev_err(&rpmsg_chnl->dev, "%s: interrupted\n", __func__);
            return ret;
        }
        return buf->err;
    }

    return 0;
}


// returns the next message sequence number to be used (protected by in built in spin lock)
static u32 get_next_seq_nr(void)
{
    // message sequence number (for request / response matching) of next request to be sent
    static u32 msg_seq_nr = 0;  // start a zero
    u32 ret;
    spin_lock(&seq_lock);
    ret = msg_seq_nr;
    msg_seq_nr++;
    spin_unlock(&seq_lock);
    return ret;
}


// get a pointer to an empty (unused) transaction struct or allocate a new one if necessary
// returns 0 on success or an error code <0
static int alloc_trans_struct(struct transaction** t)
{
    if (!t)
        return -EINVAL;
    (*t) = NULL;    // default assumption: no struct available
    // try to reuse an empty struct
    spin_lock(&unused_list_lock);
    if (!list_empty(&unused_list)) {
        // list is not empty, so simply grab the first entry
        (*t) = list_first_entry(&unused_list, struct transaction, list);
        // remove it from the list of unused items, we will use it now ;)
        list_del(&((*t)->list));
    }
    spin_unlock(&unused_list_lock);

    if (!(*t)) {
        // nothing found in the list, make a new one
        (*t) = kzalloc(sizeof(struct transaction), GFP_KERNEL);
        // TODO: we could add more than one to reduce the number of malloc calls, is it worth it?
        INIT_LIST_HEAD(&((*t)->list));
    }

    if (!(*t))
        return -ENOMEM; // still no entry, something is wrong

    return 0;
}


static inline void add_pend_trans(struct transaction* t)
{
    spin_lock(&pending_list_lock);
    list_add(&t->list, &pending_list);
    spin_unlock(&pending_list_lock);
}

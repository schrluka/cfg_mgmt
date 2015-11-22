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

static inline void add_pend_trans(struct rpmsg_link_transaction* t);




/************************************************************************************************************************
*   I M P L E M E N T A T I O N
*/

// must be called first, channel to be used for communication is passed as argument
int rpmsg_link_init(struct rpmsg_channel *ch)
{
    struct rpmsg_link_transaction* t;
    const int N = 16;
    int i;

    rpmsg_chnl = ch;

    spin_lock_init(&pending_list_lock);
    spin_lock_init(&unused_list_lock);
    spin_lock_init(&seq_lock);

    // add some transaction structs to the list of unused structs to speed things up on the first transactions
    spin_lock(&pending_list_lock);
    for (i=0; i<N; i++) {
        t = kzalloc(sizeof(*t), GFP_KERNEL);
        if (!t)
            return -ENOMEM;

        dev_dbg(&ch->dev, "%s: creating transaction struct at 0x%08x\n", __func__, (u32)t);
        INIT_LIST_HEAD(&(t->list));
        list_add(&t->list, &unused_list);
    }
    spin_unlock(&pending_list_lock);

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
        struct rpmsg_link_transaction* t = list_entry ( pos, struct rpmsg_link_transaction , list );

        dev_err(&rpmsg_chnl->dev, "%s: found pending transcation: seq=%d\n", __func__, t->msg_seq_nr);
        list_del(pos);
        kfree(t);
    }
    // free all unused transaction structs
    list_for_each_safe(pos, temp, &unused_list) // we delete the object from the list -> use safe version
    {
        // convert list_head pointer to a pointer to the list object
        struct rpmsg_link_transaction* t = list_entry ( pos, struct rpmsg_link_transaction , list );
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
    struct rpmsg_link_transaction* trans = NULL;
    cfgMsg_t* response = data;

    //dev_dbg(&rpdev->dev, "%s: starting\n", __func__);

	// check length
	if (len != sizeof(cfgMsg_t))
	{
		dev_info(&rpdev->dev, "CFG_MGMT %s: Message from BM application has wrong length.\n", __func__);
		return;
	}

    dev_dbg(&rpdev->dev, "%s: processing reply with seq nr %d\n", __func__, response->seq);

    // get the correct transaction struct
    spin_lock(&pending_list_lock);
    list_for_each_safe(pos, temp, &pending_list) // we delete the object from the list -> use safe version
    {
        // convert list_head pointer to a pointer to the list object
        struct rpmsg_link_transaction* t = list_entry (pos, struct rpmsg_link_transaction, list);

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
        trans->len = 0;
        trans->valid = true;
        trans->err = 0;
        break;

    case RES_N_VARS:
        // copy the number of variables to our global variable, no need to return it
        n_vars = response->val;
        trans->len = 0;        // no data in placed in io buffer
        trans->valid = true;   // we got the answer
        break;

    case RES_RD_VAL:
    case RES_RD_MIN:
    case RES_RD_MAX:
        // convert numerical results to a string for communication with the user space
        trans->len =  scnprintf(trans->buf, IO_BUF_SIZE, "%d\n", response->val);
        trans->err = 0;
        trans->valid = true;
        break;

    case RES_NAME:
    case RES_DESC:
        if (response->len  > IO_BUF_SIZE) {
            dev_err(&rpdev->dev, "%s: data part of response too long\n", __func__);
            trans->err = -EINVAL;
            trans->len = scnprintf(trans->buf, IO_BUF_SIZE, "data part of response too long\n");
        } else {
            // copy string response to io buffer
            memcpy(trans->buf, response->data, response->len);
            trans->len = response->len;
            trans->err = 0;
        }
        trans->valid = true;
        break;

    case RES_ID_ERR:
        trans->len = scnprintf(trans->buf, IO_BUF_SIZE,
            "received ID error for id %d in msg nr %d\n", response->ind, response->seq);
        trans->valid = true;
        trans->err = RES_ID_ERR;
        break;

    case RES_REQ_ERR:
        trans->len = scnprintf(trans->buf, IO_BUF_SIZE,
                "received request error for msg nr %d\n", response->seq);
        trans->valid = true;
        trans->err = RES_REQ_ERR;
        break;

    default:
        trans->len = scnprintf(trans->buf, IO_BUF_SIZE,
            "unknonw type %d in msg nr %d\n", response->ind, response->seq);
        trans->valid = true;
        trans->err = -1;
    }

    dev_dbg(&rpdev->dev, "%s: waking waitqueue\n",__func__);

    // wait any sleeping processes, a result is available
    if (trans->wq)
        wake_up_interruptible(trans->wq);
    //wake_up(&(trans->wq));

    dev_dbg(&rpdev->dev, "%s: done\n", __func__);

}


// Query the number of config variables available at the remote side.
// The process will be blocked until the answer from the bare metal application has arrived and the number of variables is returned.
int get_n_vars(wait_queue_head_t* wq)
{
    int ret;
    static cfgMsg_t req;    // keep this static to save stack space
    struct rpmsg_link_transaction* t;

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
    t = rpmsg_link_alloc_trans();   // get an empty (or new) struct
    if (!t) {
        dev_err(&rpmsg_chnl->dev, "%s: can't allocate transaction struct, no memory\n", __func__);
        return -ENOMEM;
    }

    t->msg_seq_nr = req.seq;    // rpmsg callback uses this for identification
    t->wq = wq;

    // add struct to list of pending transactions
    add_pend_trans(t); // contains the necessary locking

	// send the request to the other side,
	ret = rpmsg_send(rpmsg_chnl, (void*)(&req), sizeof(req));
	if (ret) {
        dev_dbg(&rpmsg_chnl->dev, "%s: rpmsg send failed with %d\n", __func__, ret);
        return ret;
	}

	dev_dbg(&rpmsg_chnl->dev, "%s: message sent, waiting for reply\n", __func__);

	// block calling user context until we receive a reply
    ret = wait_event_interruptible((*wq), n_vars >= 0);
    if (ret) {	// abort in case we got interrupted
        dev_err(&rpmsg_chnl->dev, "%s: interrupted\n", __func__);
        return ret;
    }

    //dev_dbg(&rpmsg_chnl->dev, "%s: n_vars is %d\n", __func__, n_vars);
    return n_vars;
}


// initiate a variable access (either read or write)
int access_var(int index, access_t acc, struct rpmsg_link_transaction* t)
{
    int ret;
    static cfgMsg_t req;    // save stack space

    if (!rpmsg_chnl)
        return -EINVAL;

    if (!t) {
        dev_err(&rpmsg_chnl->dev, "%s: no transaction struct, abort\n", __func__);
        return -EINVAL;
    }

  	// set all request fields
	req.ind = index;
	req.val = 0;
	req.len = 0;
	switch(acc) {
	case ACC_VAL:
        if (t->rnw) {
            req.type = REQ_RD_VAL;
        } else {
            // write
            req.type = REQ_WR_VAL;
            // convert string to integer
            ret = kstrtol(t->buf, 0, (long int*)&req.val);
            if (ret) {
                dev_err(&rpmsg_chnl->dev, "%s: can't parse string '%s' %d\n", __func__, t->buf, ret);
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
    t->msg_seq_nr = req.seq;    // rpmsg callback uses this for identification

    // add struct to list of pending transactions
    add_pend_trans(t); // contains the necessary locking

    dev_dbg(&rpmsg_chnl->dev, "%s: sending message nr %d.\n", __func__, req.seq);

	// send the request to the other side,
	ret = rpmsg_send(rpmsg_chnl, (void*)(&req), sizeof(req));
	if (ret) {
        dev_dbg(&rpmsg_chnl->dev, "%s: rpmsg send failed with %d\n", __func__, ret);
        return ret;
	}

    dev_dbg(&rpmsg_chnl->dev, "%s: done\n", __func__);
    return 0;
}


// get a pointer to an empty (unused) transaction struct or allocate a new one if necessary
// returns NULL if no memory is available
struct rpmsg_link_transaction* rpmsg_link_alloc_trans()
{
    struct rpmsg_link_transaction* t = NULL;    // default assumption: no struct available

    // try to reuse an empty struct
    spin_lock(&unused_list_lock);
    if (!list_empty(&unused_list)) {
        // list is not empty, so simply grab the first entry
        t = list_first_entry(&unused_list, struct rpmsg_link_transaction, list);
        // remove it from the list of unused items, we will use it now ;)
        list_del(&(t->list));
    }
    spin_unlock(&unused_list_lock);

    if (!(t)) {
        // nothing found in the list, make a new one
        t = kzalloc(sizeof(*t), GFP_KERNEL);
        INIT_LIST_HEAD(&(t->list));
    }

    memset((void*)t, 0, sizeof(*t));

    return t;
}


void rpmsg_link_return_trans(struct rpmsg_link_transaction* trans)
{
    // recycle transaction struct
    memset(trans, 0, sizeof(*trans));   // clear content, just to be sure
    spin_lock(&unused_list_lock);
    list_add(&(trans->list), &unused_list);
    spin_unlock(&unused_list_lock);
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


static inline void add_pend_trans(struct rpmsg_link_transaction* t)
{
    spin_lock(&pending_list_lock);
    list_add(&t->list, &pending_list);
    spin_unlock(&pending_list_lock);
}

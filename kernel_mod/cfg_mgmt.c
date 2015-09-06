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
* cfg_mgmt.c
*
* Main File of Kernel Module which uses a rpmsg channel to communicate with the bare metal code and exposes
* its config variables to the user via the kernel's confifs
*
************************************************************************************************************************/

#define DEBUG

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/wait.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/ioport.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
//#include <asm/io.h>
#include <linux/rpmsg.h>
//#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/debugfs.h>
//#include <linux/dcache.h>

#include "cfg_mgmt.h"

#define DRIVER_AUTHOR "Lukas Schrittwieser"
#define DRIVER_DESC   "Driver for config variable management over an rpmsg link"


/******************************************************************************************************************
*   D E F I N E S   F O R   B M   I N T E R F A C E
*/

// configure size (max length) of the data field in messages exchanged with BM application
//#define MSG_DATA_SIZE 	(DATA_LEN_MAX-sizeof(cfgMsg_t))
// This is a super uggly hack, I have not found a good solution yet. (Total message length is 512 bytes, leave
// space for headers
#define MSG_DATA_SIZE 	(400)

// define size of general kernel space buffer
// use this only for temporary copy operations, not preserved between func calls
#define TMP_BUF_LEN 		MSG_DATA_SIZE

// file IOs are made to a buffer in kernel space, define its length
// as we read/write up to the max transport capability of the underlying comm channel reserver this amount
#define IO_BUF_SIZE         MSG_DATA_SIZE


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



/******************************************************************************************************************
*   T Y P E S
*/

// struct exchanged with bare metal firmwar for communication (can be a request or a response)
typedef struct __attribute__((packed))       // make sure it has no holes (kernel does the same)
{
    uint32_t    seq;    // message sequence number identifying request and response
    uint32_t    type;   // message type
    int32_t     ind;    // config variable index (<0 means unkown/undefined)
    int32_t     val;    // numerical value (for WR req, RD resp, etc)
    uint32_t    len;    // length of data section (in bytes)
    uint8_t     data[MSG_DATA_SIZE]; // opt. data section, total messages has to fit into TX_BUFFER_SIZEs
} cfgMsg_t;


// collection of all sysfs attributes (files) for one config variable)
/*
struct cfg_var_sysfs_attrs
{
	struct device_attribute	val;	// current value
	struct device_attribute	min;	// minimum (read only)
	struct device_attribute	max;	// maximum (read only)
	struct device_attribute	desc;	// description (read only)
	int index;						// used to identify this on the remote side
}; */

// define an enum which tells the read/write functions what aspect of a var is accessed
typedef enum {ACC_VAL, ACC_MIN, ACC_MAX, ACC_DESC} access_t;

// group the information about a variable access (this is passed to the file io functions
// through the inode private data pointer)
struct var_access_info {
    int index;
    access_t type;
};

// buffer for IO manipulation by user (reading / writing a file changes this)
struct file_io_buf {
    ssize_t len;                // length of string in buf
    char buf[IO_BUF_SIZE];
    bool dirty;                 // true if buffer was modified
};


/******************************************************************************************************************
*   P R O T O T Y P E S
*/

static int cfg_mgmt_probe(struct rpmsg_channel *rpdev);
static void cfg_mgmt_remove(struct rpmsg_channel *rpdev);
static void cfg_mgmt_rpmsg_cb(struct rpmsg_channel *rpdev, void *data, int len, void *priv, u32 src);

static int get_n_vars(void);
static int get_var_name(int index, char* buf, int len);
static int get_val(int index, s32* val);
static int set_val(int index, s32 val);
static int get_min(int index, s32* val);
static int get_max(int index, s32* val);
static int get_desc(int index, char* buf, int len);

static int alloc_mem(void);
static void free_mem(void);

static int debugfs_open_var(struct inode *inod, struct file *filp);
static ssize_t debugfs_read_var(struct file *filp, char *buff, size_t len, loff_t *off);
static ssize_t debugfs_write_var(struct file *filp, const char *buff, size_t len, loff_t *ppos);
static int debugfs_release_var(struct inode *inod, struct file *filp);

static ssize_t debugfs_read_update(struct file *filp, char *buff, size_t len, loff_t *off);



/******************************************************************************************************************
*   G L O B A L S
*/

// table of RPMSG channels names used by this driver
static struct rpmsg_device_id rpmsg_channel_id_table[] = {
	{ .name = "cfg_mgmt" },
	{},
};
MODULE_DEVICE_TABLE(rpmsg, rpmsg_channel_id_table);

// callback functions for RPMSG communication (with bare metal application)
static struct rpmsg_driver cfg_mgmt_rpmsg_drv = {
	.drv.name = "cfg_mgmt_rpmsg",
	.drv.owner = THIS_MODULE,
	.id_table = rpmsg_channel_id_table,
	.probe = &cfg_mgmt_probe,
	.remove = &cfg_mgmt_remove,
	.callback = &cfg_mgmt_rpmsg_cb,
};


// temp buffer for copy operations
static char tmp_buf[TMP_BUF_LEN];

// keep track how many times this was started
//static int  times = 0;

// rpmsg channel for communication with the bare metal application
struct rpmsg_channel* rpmsg_chnl;

// wait queue used to block the user process
wait_queue_head_t usr_wait_q;

// pending request
static cfgMsg_t pending_req;

// response from BM
static cfgMsg_t response;
static int response_valid;

// message sequence number (for request / response matching) of next request to be sent
u32 msg_seq_nr;

// number of variables exported via sysfs (after a query to remote side)
static int n_vars;

// DEBUGFS elements for exporting the variables
// directories
static struct dentry* cfg_mgmt_dir_p;
static struct dentry* val_dir_p;
static struct dentry* min_dir_p;
static struct dentry* max_dir_p;
static struct dentry* desc_dir_p;

static struct dentry* update_file_p;

// array of all variables access associated with the files, n_vars entries each
static struct var_access_info* val_access;
static struct var_access_info* min_access;
static struct var_access_info* max_access;
static struct var_access_info* desc_access;

static struct file_operations fops_var = {
    .owner      = THIS_MODULE,
    .open       = &debugfs_open_var,
    .read       = &debugfs_read_var,
	.write      = &debugfs_write_var,
	.release    = &debugfs_release_var,
};

// file operations for update file
static struct file_operations fops_update = {
    .owner  = THIS_MODULE,
    .read   = &debugfs_read_update,
	//.open = dev_open,
	//.write = debugfs_write_var,   read only
};



/******************************************************************************************************************
*   I M P L E M E N T A T I O N
*/

// init function, called upon module start
static int __init cm_init(void)
{
	printk(KERN_INFO "CFG_MGMT: Loading configuration variable managment module\n");

    cfg_mgmt_dir_p = NULL;
    val_dir_p = NULL;
    min_dir_p = NULL;
    max_dir_p = NULL;
    desc_dir_p = NULL;
    val_access = NULL;
    min_access = NULL;
    max_access = NULL;
    desc_access = NULL;

    // no request pending
	memset(&pending_req, 0, sizeof(pending_req));
	pending_req.type = REQ_NONE;
	pending_req.ind = -1;
	response_valid = 0;

	n_vars  = -1;	// don't know yet

	init_waitqueue_head(&usr_wait_q);

	// register as rpmsg driver module, we will get probed once the other side establishes a connection
	return register_rpmsg_driver(&cfg_mgmt_rpmsg_drv);
}

// when a file is opened query the current value and print it into a string buffer
static int debugfs_open_var(struct inode *inod, struct file *filp)
{
    int ret;
    s32 v;
    struct file_io_buf* iobuf;
    struct var_access_info* acc_p = inod->i_private;

    dev_dbg(&rpmsg_chnl->dev, "CFG_MGMT %s: index %d\n", __func__, acc_p->index);
    // get memory for a local kernel buffer
    iobuf = kzalloc(sizeof(*iobuf), GFP_KERNEL);
    if (!iobuf)
        return -ENOMEM;

    // check if the file is opened for reading
    if (filp->f_mode&FMODE_READ) {
        // query the value and print it to a local (kernel space) buffer
        switch (acc_p->type) {
        case ACC_VAL:
            ret = get_val(acc_p->index, &v);
            if (ret)
                return ret;
            iobuf->len = scnprintf(iobuf->buf, IO_BUF_SIZE, "%d\n", v);
            break;
        case ACC_MIN:
            ret = get_min(acc_p->index, &v);
            if (ret)
                return ret;
            iobuf->len = scnprintf(iobuf->buf, IO_BUF_SIZE, "%d\n", v);
            break;
        case ACC_MAX:
            ret = get_max(acc_p->index, &v);
            if (ret)
                return ret;
            iobuf->len = scnprintf(iobuf->buf, IO_BUF_SIZE, "%d\n", v);
            break;
        case ACC_DESC:
            ret = get_desc(acc_p->index, iobuf->buf, IO_BUF_SIZE);
            if (ret < 0)
                return ret;
            iobuf->len = ret;
            break;
        default:
            dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: unknown access type\n", __func__);
            return -EINVAL;
        }
    }

    // store a pointer to this buffer in the file structure where read/write functions can use it
    filp->private_data = (void*)iobuf;
    return 0;
}

static ssize_t debugfs_read_var(struct file *filp, char *buff, size_t len, loff_t *ppos)
{
    // use a simple helper to allow the user process to read our buffer
    struct file_io_buf* iobuf;
    iobuf = filp->private_data;
    dev_dbg(&rpmsg_chnl->dev, "CFG_MGMT %s: len %d, ppos %lld\n", __func__, len, *ppos);
    return simple_read_from_buffer(buff, len, ppos, iobuf->buf, iobuf->len);
}


static ssize_t debugfs_write_var(struct file *filp, const char *buff, size_t len, loff_t *ppos)
{
    // use a simple helper to allow the user process to write our buffer
    struct file_io_buf* iobuf;
    iobuf = filp->private_data;
    dev_dbg(&rpmsg_chnl->dev, "CFG_MGMT %s: len %d, ppos %lld\n", __func__, len, *ppos);
    iobuf->dirty = true;    // buffer is now modified
    return simple_write_to_buffer(iobuf->buf, IO_BUF_SIZE, ppos, (void*)buff, len);
}


static int debugfs_release_var(struct inode *inod, struct file *filp)
{
    int ret;
    long v;
    struct file_io_buf* iobuf = filp->private_data;
    struct var_access_info* acc_p = inod->i_private;

	dev_dbg(&rpmsg_chnl->dev, "CFG_MGMT %s: private at 0x%08x\n", __func__, (int)(filp->private_data));

    if (!iobuf)
        return -EINVAL; // should never happen

    // write the value back if the file was opened for writing
    if ((filp->f_mode&FMODE_WRITE) && (iobuf->dirty)) {
        if (acc_p->type != ACC_VAL) {
            dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: attempting to write anything other then variable value\n", __func__);
            return -EINVAL;
        }
        // convert string to integer
        ret = kstrtol(iobuf->buf, 0, &v);
        if (ret) {
            dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: can't parse string '%s' %d\n", __func__, iobuf->buf, ret);
            return ret;
        }
        dev_dbg(&rpmsg_chnl->dev, "CFG_MGMT %s: writing val %ld to index %d\n", __func__, v, acc_p->index);
        // write the new value to the BM application
        ret = set_val(acc_p->index, v);
        if (ret) {
            dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: can't set new value: %d\n", __func__, ret);
            return ret;
        }
    }
    // remove the io buffer, no longer need it as the file is closed
    kfree(iobuf);
	return 0;
}


// create the required debugfs directories
static int create_debugfs_dirs(void)
{
    if (!cfg_mgmt_dir_p) {
        dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: no parent directory\n", __func__);
        return -ENOENT;
    }

    val_dir_p = debugfs_create_dir("val", cfg_mgmt_dir_p);
    min_dir_p = debugfs_create_dir("min", cfg_mgmt_dir_p);
    max_dir_p = debugfs_create_dir("max", cfg_mgmt_dir_p);
    desc_dir_p = debugfs_create_dir("desc", cfg_mgmt_dir_p);

    return 0;
}


// callback for read call to update file, this will (re)create the file structure
//static ssize_t show_update(struct device *dev, struct device_attribute *attr, char *buf)
static ssize_t debugfs_read_update(struct file *filp, char *buff, size_t len, loff_t *off)
{
    int ret,i;
    struct device* dev = &rpmsg_chnl->dev;  // abbrevation

    dev_dbg(dev, "CFG_MGMT %s: starting\n", __func__);

    if (n_vars > 0) {
        return copy_to_user(buff, "already done\n", 14);
    }

    n_vars = get_n_vars();

	dev_dbg(dev, "%s: n_vars=%d\n", __func__, n_vars);
	if (n_vars <= 0)
		return -EINVAL;	// nothing todo as there are no vars or we don't know how many there are

    ret = alloc_mem();  // get memory for global arrays
    if (ret) {
        n_vars = -1;
        return ret;
    }

	// allocate directories
	ret = create_debugfs_dirs();
	if (ret < 0) {
        dev_err(dev," CFG_MGMT %s: can't create directories: %d\n", __func__, ret);
        return -ENOENT;
	}

    // fill the data structs
	for (i=0; i<n_vars; i++) {
        val_access[i].index = i; // save it for use by file io functions
        min_access[i].index = i;
        max_access[i].index = i;
        desc_access[i].index = i;
        // specify what kind of access it is
        val_access[i].type = ACC_VAL;
        min_access[i].type = ACC_MIN;
        max_access[i].type = ACC_MAX;
        desc_access[i].type = ACC_DESC;
        // get the variable name
        ret = get_var_name(i, tmp_buf, TMP_BUF_LEN);
		if (ret < 0) {
			dev_err(dev, "CFG_MGMT %s: get var name failed for index %d\n", __func__, i);
			// TODO: cleanup
			return ret;
		}
		// create all files for this variable, save index
        debugfs_create_file(tmp_buf, 0666, val_dir_p, (void*)(&val_access[i]),
				       &fops_var);
        debugfs_create_file(tmp_buf, 0444, min_dir_p, (void*)(&min_access[i]),
				       &fops_var);
        debugfs_create_file(tmp_buf, 0444, max_dir_p, (void*)(&max_access[i]),
				       &fops_var);
        debugfs_create_file(tmp_buf, 0444, desc_dir_p, (void*)(&desc_access[i]),
				       &fops_var);
	}

    dev_dbg(dev, "%s: done\n", __func__);

    return simple_read_from_buffer(buff, len, off, "ok\n", 3);

}


// probe function, called when the remote side establishes a connection with us
static int cfg_mgmt_probe (struct rpmsg_channel *rpdev)
{
	dev_dbg(&rpdev->dev, "%s: starting\n",__func__);

	// no request pending
	memset(&pending_req, 0, sizeof(pending_req));
	pending_req.type = REQ_NONE;
	pending_req.ind = -1;
	response_valid = 0;

	n_vars  = -1;	// don't know yet

	// save the rpmsg channel pointer for use by all other functions
	rpmsg_chnl = rpdev;

	msg_seq_nr = 0;

    // create a new directory in debugfs for our module
    cfg_mgmt_dir_p = debugfs_create_dir("cfg_mgmt", NULL);
    if (!cfg_mgmt_dir_p || (cfg_mgmt_dir_p<0)) {
        dev_err(&rpdev->dev, "CFG_MGMT %s: can't create debugfs directory: %d\n", __func__, (int)cfg_mgmt_dir_p);
        cfg_mgmt_dir_p = NULL;
        return -ENOENT;
    }

    // create the update file, reading it will trigger a generation of the variable files
    update_file_p = debugfs_create_file("update", 0444, cfg_mgmt_dir_p, NULL, &fops_update);

    dev_dbg(&rpdev->dev, "CFG_MGMT %s: done\n", __func__);
	return 0;
}

static int alloc_mem(void)
{
    struct device* dev = &rpmsg_chnl->dev;  // abbrevation
    val_access = kmalloc(sizeof(*val_access)*n_vars, GFP_KERNEL);
    if (!val_access) {
        dev_err(dev, "CFG_MGMT %s: no memory\n", __func__);
        return -ENOMEM;
    }

    min_access = kmalloc(sizeof(*min_access)*n_vars, GFP_KERNEL);
    if (!min_access) {
        dev_err(dev, "CFG_MGMT %s: no memory\n", __func__);
        return -ENOMEM;
    }

    max_access = kmalloc(sizeof(*max_access)*n_vars, GFP_KERNEL);
    if (!max_access) {
        dev_err(dev, "CFG_MGMT %s: no memory\n", __func__);
        return -ENOMEM;
    }

    desc_access = kmalloc(sizeof(*desc_access)*n_vars, GFP_KERNEL);
    if (!desc_access) {
        dev_err(dev, "CFG_MGMT %s: no memory\n", __func__);
        return -ENOMEM;
    }
    return 0;
}


static void free_mem()
{
    printk(KERN_DEBUG "CFG_MGMT %s: freeing mem\n", __func__);

    if (cfg_mgmt_dir_p)
        debugfs_remove_recursive(cfg_mgmt_dir_p);
    cfg_mgmt_dir_p = NULL;

    if (val_access)
        kfree(val_access);
    val_access = NULL;

    if (min_access)
        kfree(min_access);
    min_access = NULL;

    if (max_access)
        kfree(max_access);
    max_access = NULL;

    if (desc_access)
        kfree(desc_access);
    desc_access = NULL;
}


static void cfg_mgmt_remove(struct rpmsg_channel *rpdev)
{
	dev_dbg(&rpdev->dev, "%s: starting\n",__func__);

    if (n_vars > 0) {
        free_mem(); // remove all files and free memory
        n_vars = -1;
    }

	dev_dbg(&rpdev->dev, "%s: done\n",__func__);
}


// rpmsg callback function: receives messages from the bare metal application
static void cfg_mgmt_rpmsg_cb(struct rpmsg_channel *rpdev, void *data, int len, void *priv, u32 src)
{
	// check length
	if (len < sizeof(cfgMsg_t))
	{
		dev_info(&rpdev->dev, "CFG_MGMT %s: Message from BM application is too short.\n", __func__);
		return;
	}

	if (pending_req.type == REQ_NONE)
	{
		printk(KERN_ERR "CFG_MGMT %s: received message via rpmsg but no request pending.\n", __func__);
		return;
	}

	// sanity check length
	if (len > sizeof(response))
	{
		printk(KERN_ERR "CFG_MGMT %s: message received via rpmsg is too long.\n", __func__);
		// create a response with an error
		response.type = RES_REQ_ERR;
	}
	else
	{
		// copy the received data into the kernel buffer
		memcpy((void*)(&response), data, len);
		dev_dbg(&rpdev->dev, "CFG_MGMT %s: received reply: seq=%d, ind=%d, val=%d.\n", __func__, response.seq, response.ind, response.val);
	}

	// wake the sleeping context
	response_valid = 1;
	wake_up_interruptible(&usr_wait_q);
}


static void __exit cm_exit(void)
{
    //free_mem(); // make sure we freed our memory
	printk(KERN_INFO "CFG_MGMT: unloading module\n");
	unregister_rpmsg_driver(&cfg_mgmt_rpmsg_drv);
}


// query the number of config variables available at the remote side
// neg value indicates an error
static int get_n_vars()
{
    int ret;
	// make sure no request is pending
	if (pending_req.type != REQ_NONE)
	{
		dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: request still pending\n", __func__);
		return -EINVAL;
	}

	dev_dbg(&rpmsg_chnl->dev, "CFG_MGMT %s: requesting n_vars\n", __func__);

	// invalidate all request fields
	pending_req.seq = msg_seq_nr++;
	pending_req.ind = -1;
	pending_req.val = 0;
	pending_req.len = 0;
	pending_req.type = REQ_N_VARS;
	response_valid = 0;
	// send the request to the other side
	ret = rpmsg_sendto(rpmsg_chnl, (void*)(&pending_req), sizeof(cfgMsg_t),rpmsg_chnl->dst);
	if (ret) {
        pending_req.type = REQ_NONE;
        return ret;
	}
	// block calling user context until we received a reply
	ret = wait_event_interruptible(usr_wait_q, response_valid==1);
	if (ret) {	// abort in case we got interrupted
        pending_req.type = REQ_NONE;
        dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: interrupted\n", __func__);
		return ret;
	}
	// check the response
	if (response.type == RES_N_VARS)
	{
		// ok, we got the correct answer, relay it to the user process
		pending_req.type = REQ_NONE;
		dev_dbg(&rpmsg_chnl->dev, "CFG_MGMT %s: n_vars is %d\n", __func__, response.val);
		return response.val;
	}
	dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: wrong response to N_VARS: %d\n", __func__, response.type);
	pending_req.type = REQ_NONE;
	return -EINVAL;
}


// query bare metal application to get the name of the variable given by index
// returns number of bytes copied (without \0) on success or a neg error code
static int get_var_name(int index, char* buf, int len)
{
    int ret;
	// make sure no request is pending
	if (pending_req.type != REQ_NONE)
	{
		dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: request still pending\n", __func__);
		return -EINVAL;
	}
	// setup all request fields
	pending_req.seq = msg_seq_nr++;
	pending_req.ind = index;
	pending_req.val = 0;
	pending_req.len = 0;
	pending_req.type = REQ_NAME;
	response_valid = 0;
	// send the request to the other side
	ret = rpmsg_sendto(rpmsg_chnl, (void*)(&pending_req), sizeof(cfgMsg_t),rpmsg_chnl->dst);
	if (ret) {
        pending_req.type = REQ_NONE;
		return ret;
	}
	// block calling user context until we received a reply
	ret = wait_event_interruptible(usr_wait_q, response_valid==1);
	if (ret) {	// abort in case we got interrupted
        pending_req.type = REQ_NONE;
		return ret;
	}
	if (response.type == RES_NAME) {
		len--; // leave space for \0
		if (response.len < (len))	// ensure we don't overfill the buffer and leave space for \0
			len = response.len;
		// got it, copy to buffer
		memcpy(buf, response.data, len);
		buf[len] = '\0';	// make it a proper string
		pending_req.type = REQ_NONE;
		return len;
	}
	if (response.type == RES_ID_ERR) {
		dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: BM app rejected index: %d\n", __func__, pending_req.ind);
		pending_req.type = REQ_NONE;
		return -EINVAL;
	}
	// all other responses should not happen
	dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: unknown response from BM app: %d\n", __func__, response.type);
	pending_req.type = REQ_NONE;
	return -EINVAL;
}


// query a variable value
// returns 0 on success or a neg error code
static int get_val(int index, s32* val)
{
    int ret;
	// make sure no request is pending
	if (pending_req.type != REQ_NONE)
	{
		dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: request still pending\n", __func__);
		return -EINVAL;
	}
	// invalidate all request fields
	pending_req.seq = msg_seq_nr++;
	pending_req.ind = index;
	pending_req.val = 0;
	pending_req.len = 0;
	pending_req.type = REQ_RD_VAL;
	response_valid = 0;
	// send the request to the other side
	ret = rpmsg_sendto(rpmsg_chnl, (void*)(&pending_req), sizeof(cfgMsg_t),rpmsg_chnl->dst);
	if (ret) {	// abort in case we got interrupted
        pending_req.type = REQ_NONE;
		return ret;
	}
	// block calling user context until we received a reply
	ret = wait_event_interruptible(usr_wait_q, response_valid==1);
	if (ret) {	// abort in case we got interrupted
        pending_req.type = REQ_NONE;
		return ret;
	}
	if (response.type == RES_RD_VAL) {
        pending_req.type = REQ_NONE;
		*val = response.val;
		return 0;
	}
	if (response.type == RES_ID_ERR) {
		dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: BM app rejected index: %d\n", __func__, pending_req.ind);
    pending_req.type = REQ_NONE;
		return -EINVAL;
	}
	// all other responses should not happen
	dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: unknown response from BM app: %d\n", __func__, response.type);
	pending_req.type = REQ_NONE;
	return -EINVAL;
}


// sends a new variable value request to the BM application
// returns 0 on success or a neg error code
static int set_val(int index, s32 val)
{
    int ret;
	// make sure no request is pending
	if (pending_req.type != REQ_NONE)
	{
		dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: request still pending\n", __func__);
		return -EINVAL;
	}
	// invalidate all request fields
	pending_req.seq = msg_seq_nr++;
	pending_req.ind = index;
	pending_req.val = val;
	pending_req.len = 0;
	pending_req.type = REQ_WR_VAL;
	response_valid = 0;
	// send the request to the other side
	ret = rpmsg_sendto(rpmsg_chnl, (void*)(&pending_req), sizeof(cfgMsg_t),rpmsg_chnl->dst);
	if (ret) {
        pending_req.type = REQ_NONE;
        return ret;
	}
	// block calling user context until we received a reply
	ret = wait_event_interruptible(usr_wait_q, response_valid==1);
	if (ret) {	// abort in case we got interrupted
        pending_req.type = REQ_NONE;
		return ret;
	}
	if (response.type == RES_OK) {
        pending_req.type = REQ_NONE;
		return 0;
	}
	if (response.type == RES_ID_ERR) {
		dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: BM app rejected index: %d\n", __func__, pending_req.ind);
		return -EINVAL;
	}
	// all other responses should not happen
	dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: unknown response from BM app: %d\n", __func__, response.type);
	return -EINVAL;
}


// query a variable's minimum (lower limit)
// returns 0 on success or a neg error code
static int get_min(int index, s32* val)
{
    int ret;
	// make sure no request is pending
	if (pending_req.type != REQ_NONE)
	{
		dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: request still pending\n", __func__);
		return -EINVAL;
	}
	// invalidate all request fields
	pending_req.seq = msg_seq_nr++;
	pending_req.ind = index;
	pending_req.val = 0;
	pending_req.len = 0;
	pending_req.type = REQ_RD_MIN;
	response_valid = 0;
	// send the request to the other side
	ret = rpmsg_sendto(rpmsg_chnl, (void*)(&pending_req), sizeof(cfgMsg_t),rpmsg_chnl->dst);
	if (ret) {
        pending_req.type = REQ_NONE;
		return ret;
	}
	// block calling user context until we received a reply
	ret = wait_event_interruptible(usr_wait_q, response_valid==1);
	if (ret) {	// abort in case we got interrupted
        pending_req.type = REQ_NONE;
		return ret;
	}
	if (response.type == RES_RD_MIN) {
		*val = response.val;
		pending_req.type = REQ_NONE;
		return 0;
    }
	if (response.type == RES_ID_ERR) {
		dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: BM app rejected index: %d\n", __func__, pending_req.ind);
		pending_req.type = REQ_NONE;
		return -EINVAL;
	}
	// all other responses should not happen
	dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: unknown response from BM app: %d\n", __func__, response.type);
	pending_req.type = REQ_NONE;
	return -EINVAL;
}


// query a variable maximum (upper limit)
// returns 0 on success or a neg error code
static int get_max(int index, s32* val)
{
    int ret;
	// make sure no request is pending
	if (pending_req.type != REQ_NONE)
	{
		dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: request still pending\n", __func__);
		return -EINVAL;
	}
	// invalidate all request fields
	pending_req.seq = msg_seq_nr++;
	pending_req.ind = index;
	pending_req.val = 0;
	pending_req.len = 0;
	pending_req.type = REQ_RD_MAX;
	response_valid = 0;
	// send the request to the other side
	ret = rpmsg_sendto(rpmsg_chnl, (void*)(&pending_req), sizeof(cfgMsg_t),rpmsg_chnl->dst);
	if (ret) {
        pending_req.type = REQ_NONE;
		return ret;
	}
	// block calling user context until we received a reply
	ret = wait_event_interruptible(usr_wait_q, response_valid==1);
	if (ret) {	// abort in case we got interrupted
		pending_req.type = REQ_NONE;
		return ret;
	}

	if (response.type == RES_RD_MAX) {
        pending_req.type = REQ_NONE;
		*val = response.val;
		return 0;
	}

	if (response.type == RES_ID_ERR) {
        pending_req.type = REQ_NONE;
		dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: BM app rejected index: %d\n", __func__, pending_req.ind);
		return -EINVAL;
	}

	// all other responses should not happen
	dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: unknown response from BM app: %d\n", __func__, response.type);
	pending_req.type = REQ_NONE;
	return -EINVAL;
}


// get a variables descrition string (truncated if too long)
// returns number of bytes copied (without \0) on success or a neg error code
static int get_desc(int index, char* buf, int len)
{
    int ret;

	// make sure no request is pending
	if (pending_req.type != REQ_NONE)
	{
		dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: request still pending\n", __func__);
		return -EINVAL;
	}
	// setup all request fields
	pending_req.seq = msg_seq_nr++;
	pending_req.ind = index;
	pending_req.val = 0;
	pending_req.len = 0;
	pending_req.type = REQ_DESC;
	response_valid = 0;
	// send the request to the other side
	ret = rpmsg_sendto(rpmsg_chnl, (void*)(&pending_req), sizeof(cfgMsg_t),rpmsg_chnl->dst);
    if (ret) {
        pending_req.type = REQ_NONE;
		return ret;
	}
	// block calling user context until we received a reply
	ret = wait_event_interruptible(usr_wait_q, response_valid==1);
	if (ret) {	// abort in case we got interrupted
        pending_req.type = REQ_NONE;
		return ret;
	}

	if (response.type == RES_DESC) {
		len -= 2;	// leave space for \n and \0
		if (response.len < len)	// ensure we don't overfill the buffer and leave space for \0
			len = response.len;
		// got it, copy to buffer
		memcpy(buf, response.data, len);
		buf[len++] = '\n';
		buf[len] = '\0';	// make it a proper string
		pending_req.type = REQ_NONE;
		return len;
	}

	if (response.type == RES_ID_ERR) {
		dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: BM app rejected index: %d\n", __func__, pending_req.ind);
		pending_req.type = REQ_NONE;
		return -EINVAL;
	}

	// all other responses should not happen
	dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: unknown response from BM app: %d\n", __func__, response.type);
	pending_req.type = REQ_NONE;
	return -EINVAL;
}


// specify init / exit functions
module_init(cm_init);
module_exit(cm_exit);



/******************************************************************************************************************
*   K E R N E L   D O C
*/
MODULE_LICENSE("GPL v2");

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);

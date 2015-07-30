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
#include <linux/sysfs.h>
#include <linux/string.h>

#include "cfg_mgmt.h"

#define DRIVER_AUTHOR "Lukas Schrittwieser"
#define DRIVER_DESC   "Driver for config variable management over an rpmsg link"


/******************************************************************************************************************
*   D E F I N E S   F O R   B M   I N T E R F A C E
*/

// configure size (max length) of the data field in messages exchanged with BM application
//#define MSG_DATA_SIZE 	(DATA_LEN_MAX-sizeof(cfgMsg_t))
// This is super uggly hack, have not found a good solution yet. (Total message length is 512 bytes, leave
// space for headers
#define MSG_DATA_SIZE 	(400)

// define size of general kernel space buffer
// use this only for temporary copy operations, not preserved between func calls
#define TMP_BUF_LEN 		MSG_DATA_SIZE

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
struct cfg_var_sysfs_attrs
{
	struct device_attribute	val;	// current value
	struct device_attribute	min;	// minimum (read only)
	struct device_attribute	max;	// maximum (read only)
	struct device_attribute	desc;	// description (read only)
	int index;						// used to identify this on the remote side
};





/******************************************************************************************************************
*   P R O T O T Y P E S
*/

static int cfg_mgmt_probe(struct rpmsg_channel *rpdev);
static void cfg_mgmt_remove(struct rpmsg_channel *rpdev);
static void cfg_mgmt_rpmsg_cb(struct rpmsg_channel *rpdev, void *data, int len, void *priv, u32 src);

static int get_n_vars(void);
static int get_var_name(int index, char* buf, int len);
static int get_val(int index, u32* val);
static int set_val(int index, s32 val);
static int get_min(int index, u32* val);
static int get_max(int index, u32* val);
static int get_desc(int index, char* buf, int len);

static void free_mem(void);
static int alloc_mem(void);
static int create_kobjs(struct rpmsg_channel *rpdev);

// sysfs callbacks
static ssize_t show_val(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t show_min(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t show_max(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t show_desc(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t store_val(struct device *dev, struct device_attribute *attr, const char *buf, size_t count);
static ssize_t store_none(struct device *dev, struct device_attribute *attr, const char *buf, size_t count);



/******************************************************************************************************************
*   G L O B A L S
*/

// table of RPMSG channels names used by this driver
static struct rpmsg_device_id rpmsg_channel_id_table[] = {
	{ .name = "cfg_mgmt" },
	{},
};

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


// sysfs data structures

// array (dyn allocation) of all attribute groups, has n_vars entries
struct cfg_var_sysfs_attrs *cfg_var_attrs;

// array of pointers to all value attributes
static struct attribute **attrs_val;
// array of pointers to all minimum attributes
static struct attribute **attrs_min;
// array of pointers to all maximum attributes
static struct attribute **attrs_max;
// array of pointers to all description attributes
static struct attribute **attrs_desc;

// attribute groups, required by sysfs
static struct attribute_group* attr_grp_val_p;
static struct attribute_group* attr_grp_min_p;
static struct attribute_group* attr_grp_max_p;
static struct attribute_group* attr_grp_desc_p;

// kobjects, these represent folders in sysfs
static struct kobject *cfg_mgmt_kobj;
static struct kobject *val_kobj;
static struct kobject *min_kobj;
static struct kobject *max_kobj;
static struct kobject *desc_kobj;



/******************************************************************************************************************
*   I M P L E M E N T A T I O N
*/

// init function, called upon module start
static int __init cm_init(void)
{
	printk(KERN_INFO "CFG_MGMT: Loading configuration variable managment module\n");

    cfg_var_attrs = NULL;
    attrs_val = NULL;
    attrs_min = NULL;
    attrs_max = NULL;
    attrs_desc = NULL;
    attr_grp_val_p = NULL;
    attr_grp_min_p = NULL;
    attr_grp_max_p = NULL;
    attr_grp_desc_p = NULL;
    cfg_mgmt_kobj = NULL;
    val_kobj = NULL;
    min_kobj = NULL;
    max_kobj = NULL;
    desc_kobj = NULL;

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


// helper function to read (kernel to user) a variable's value
static ssize_t show_val (struct device *dev, struct device_attribute *attr, char *buf)
{
	// convert to the config variable struct this attribute belongs to
	struct cfg_var_sysfs_attrs *a = container_of(attr, struct cfg_var_sysfs_attrs, val);
	// query the value from the bare metal application
	int v;
	int ret = get_val(a->index, &v);
	if (ret)
		return scnprintf(buf, PAGE_SIZE, "error: %d\n", ret);

	// return it in human readable form
	return scnprintf(buf, PAGE_SIZE, "%d\n", v);
}


// helper function to read (kernel to user) a variable's minimum
static ssize_t show_min (struct device *dev, struct device_attribute *attr, char *buf)
{
	// convert to the config variable struct this attribute belongs to
	struct cfg_var_sysfs_attrs *a = container_of(attr, struct cfg_var_sysfs_attrs, min);
	// query the value from the bare metal application
	u32 v;
	int ret = get_min(a->index, &v);
	if (ret)
		return scnprintf(buf, PAGE_SIZE, "error: %d\n", ret);

	// return it in human readable form
	return scnprintf(buf, PAGE_SIZE, "%d\n", v);
}


// helper function to read (kernel to user) a variable's minimum
static ssize_t show_max (struct device *dev, struct device_attribute *attr, char *buf)
{
	// convert to the config variable struct this attribute belongs to
	struct cfg_var_sysfs_attrs *a = container_of(attr, struct cfg_var_sysfs_attrs, max);
	// query the value from the bare metal application
	u32 v;
	int ret = get_max(a->index, &v);
	if (ret)
		return scnprintf(buf, PAGE_SIZE, "error: %d\n", ret);

	// return it in human readable form
	return scnprintf(buf, PAGE_SIZE, "%d\n", v);
}


// helper function to read (kernel to user) a variable's description
static ssize_t show_desc(struct device *dev, struct device_attribute *attr, char *buf)
{
	// convert to the config variable struct this attribute belongs to
	struct cfg_var_sysfs_attrs *a = container_of(attr, struct cfg_var_sysfs_attrs, max);
	// query the value from the bare metal application
	int len = PAGE_SIZE;
	int ret = get_desc(a->index, buf, len);
	// return a error message, don't know if that's a good idea. But what else could we do?
	if (ret)
		return scnprintf(buf, PAGE_SIZE, "error: %d\n", ret);
	return len;
}


// helper function for sysfs to write a new variable value
static ssize_t store_val(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    int ret;
    long v;

	// convert to the config variable struct this attribute belongs to
	struct cfg_var_sysfs_attrs *a = container_of(attr, struct cfg_var_sysfs_attrs, max);

	ret = kstrtol(buf, 0, &v);	// parse user's string
	if (ret) {
		dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: can't parse string '%s', %d\n", __func__, buf, ret);
		return 0;
	}
	// trigger a write at the BM application
	ret = set_val(a->index, v);
	if (ret) {
        dev_err(&rpmsg_chnl->dev, "CFG_MGMT %s: set_val failed: %d\n", __func__, ret);
		return 0;	// TODO: what is the supposed error handling here?
	}
	return (count);	// TODO: what should we return here?
}


// sysfs callback function for writing read only values (aka does nothing)
static ssize_t store_none(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	return 0;
}


// probe function, called when the remote side establishes a connection with us
static int cfg_mgmt_probe (struct rpmsg_channel *rpdev)
{
	int ret,i;
	char *str;

	// no request pending
	memset(&pending_req, 0, sizeof(pending_req));
	pending_req.type = REQ_NONE;
	pending_req.ind = -1;
	response_valid = 0;

	n_vars  = -1;	// don't know yet

	// save the rpmsg channel pointer for use by all other functions
	rpmsg_chnl = rpdev;

	msg_seq_nr = 0;

	// create sysfs files
	n_vars = get_n_vars();

	if (n_vars <= 0)
		return 0;	// nothing todo as there are no vars or we don't know how many there are

	// allocate memory for all data structs
	ret = alloc_mem();
	if (ret < 0)
		return ret;

	// fill the data structs
	for (i=0; i<n_vars; i++) {
		cfg_var_attrs[i].index = i;
		// STEP 1: get the variable name
		ret = get_var_name(i, tmp_buf, TMP_BUF_LEN);
		if (ret < 0) {
			// TODO: free all memory we claimed
			return ret;
		}
		// get memory to store the name
		str = kmalloc(ret+1, GFP_KERNEL);
		if (!str) {
			free_mem();
			return -ENOMEM;
		}
		// copy name (strcpy is save as we created the buffer for it)
		strcpy(str, tmp_buf);
		// TODO: is this really possible, the name is not constant!?!?!
		cfg_var_attrs[i].val.attr.name = (void*)str;
		// set the same name for all the sysfs attributes
		cfg_var_attrs[i].min.attr.name = cfg_var_attrs[i].val.attr.name;
		cfg_var_attrs[i].max.attr.name = cfg_var_attrs[i].val.attr.name;
		cfg_var_attrs[i].desc.attr.name = cfg_var_attrs[i].val.attr.name;
		// SETP 2: set owner and access rights
		//cfg_var_attrs[i].val.attr.owner = THIS_MODULE;
		//cfg_var_attrs[i].min.attr.owner = THIS_MODULE;
		//cfg_var_attrs[i].max.attr.owner = THIS_MODULE;
		//cfg_var_attrs[i].desc.attr.owner = THIS_MODULE;
		cfg_var_attrs[i].val.attr.mode = 0666;	// read and write
		cfg_var_attrs[i].min.attr.mode = 0444;  // others: read only
		cfg_var_attrs[i].max.attr.mode = 0444;
		cfg_var_attrs[i].desc.attr.mode = 0444;
		// STEP 3: callbacks
		cfg_var_attrs[i].val.show = &show_val;
		cfg_var_attrs[i].min.show = &show_min;
		cfg_var_attrs[i].max.show = &show_max;
		cfg_var_attrs[i].desc.show = &show_desc;
		cfg_var_attrs[i].val.store = &store_val;
		cfg_var_attrs[i].min.store = &store_none;
		cfg_var_attrs[i].max.store = &store_none;
		cfg_var_attrs[i].desc.store = &store_none;
		// STEP 4: setup entries in pointer arrays for sysfs
		attrs_val[i] = &(cfg_var_attrs[i].val.attr);
		attrs_min[i] = &(cfg_var_attrs[i].min.attr);
		attrs_max[i] = &(cfg_var_attrs[i].max.attr);
		attrs_desc[i] = &(cfg_var_attrs[i].desc.attr);
	}
	// init the sysfs groups
	attr_grp_val_p->attrs = attrs_val;
	attr_grp_min_p->attrs = attrs_min;
	attr_grp_max_p->attrs = attrs_max;
	attr_grp_desc_p->attrs = attrs_desc;

    // create kobj, this will create folders in sysfs and register all attributes
    ret = create_kobjs(rpdev);
    if (ret) {
        free_mem();
        dev_err(&rpdev->dev, "CFG_MGMT %s: could not create the sysfs entries: %d\n", __func__, ret);
        return ret;
    }

    return 0;
}


static void free_mem()
{
    if (desc_kobj)
        kobject_put(desc_kobj);
    desc_kobj = NULL;

    if (max_kobj)
        kobject_put(max_kobj);
    max_kobj = NULL;

    if (min_kobj)
        kobject_put(min_kobj);
    min_kobj = NULL;

    if (val_kobj)
        kobject_put(val_kobj);
    val_kobj = NULL;

    if (cfg_mgmt_kobj)
        kobject_put(cfg_mgmt_kobj);
    cfg_mgmt_kobj = NULL;

    if (attr_grp_val_p)
        kfree(attr_grp_val_p);
	attr_grp_val_p = NULL;
    if (attr_grp_max_p)
        kfree(attr_grp_max_p);
	attr_grp_max_p = NULL;
    if (attr_grp_min_p)
        kfree(attr_grp_min_p);
	attr_grp_min_p = NULL;
    if (attr_grp_val_p)
        kfree(attr_grp_val_p);
	attr_grp_min_p = NULL;
    if (attrs_desc)
        kfree(attrs_desc);
	attrs_desc = NULL;
    if (attrs_max)
        kfree(attrs_max);
	attrs_max = NULL;
    if (attrs_min)
        kfree(attrs_min);
	attrs_min = NULL;
    if (attrs_val)
        kfree(attrs_val);
	attrs_val = NULL;
    if (cfg_var_attrs)
        kfree(cfg_var_attrs);
	cfg_var_attrs = NULL;
}


// allocate required memory
// this is separate function to improve readability
static int alloc_mem()
{
	// get memory for the data structures required by sysfs
	cfg_var_attrs = kzalloc(sizeof(cfg_var_attrs)*n_vars, GFP_KERNEL);
	if (!cfg_var_attrs)
		goto out_nomem;
	// one extra element for the null termination
	attrs_val = kzalloc(sizeof(void*)*(n_vars+1), GFP_KERNEL);
	if (!attrs_val)
		goto out_cfg_var_attrs;
	attrs_min = kzalloc(sizeof(void*)*(n_vars+1), GFP_KERNEL);
	if (!attrs_min)
		goto out_attrs_val;
	attrs_max = kzalloc(sizeof(void*)*(n_vars+1), GFP_KERNEL);
	if (!attrs_max)
		goto out_attrs_min;
	attrs_desc = kzalloc(sizeof(void*)*(n_vars+1), GFP_KERNEL);
	if (!attrs_desc) {
		n_vars = -1;
		goto out_attrs_max;
	}

	attr_grp_val_p = kzalloc(sizeof(*attr_grp_val_p), GFP_KERNEL);
	if (!attr_grp_val_p)
		goto out_attrs_desc;
	attr_grp_min_p = kzalloc(sizeof(*attr_grp_min_p), GFP_KERNEL);
	if (!attr_grp_min_p)
		goto out_grp_val;
	attr_grp_max_p = kzalloc(sizeof(*attr_grp_max_p), GFP_KERNEL);
	if (!attr_grp_max_p)
		goto out_grp_min;
	attr_grp_desc_p = kzalloc(sizeof(*attr_grp_desc_p), GFP_KERNEL);
	if (!attr_grp_desc_p)
		goto out_grp_max;

	// ok, we got all the memory we need
	return 0;

out_grp_max:
	kfree(attr_grp_max_p);
	attr_grp_max_p = NULL;
out_grp_min:
	kfree(attr_grp_min_p);
	attr_grp_min_p = NULL;
out_grp_val:
	kfree(attr_grp_val_p);
	attr_grp_min_p = NULL;
out_attrs_desc:
	kfree(attrs_desc);
	attrs_desc = NULL;
out_attrs_max:
	kfree(attrs_max);
	attrs_max = NULL;
out_attrs_min:
	kfree(attrs_min);
	attrs_min = NULL;
out_attrs_val:
	kfree(attrs_val);
	attrs_val = NULL;
out_cfg_var_attrs:
	kfree(cfg_var_attrs);
	cfg_var_attrs = NULL;
out_nomem:
	n_vars = -1;
	return -ENOMEM;
}


static int create_kobjs(struct rpmsg_channel *rpdev)
{
    int ret;
    cfg_mgmt_kobj = kobject_create_and_add("cfg_mgmt", &(rpdev->dev.kobj));
    if (!cfg_mgmt_kobj)
        return -ENOMEM;

    val_kobj = kobject_create_and_add("val", cfg_mgmt_kobj);
    if (!val_kobj)
        return -ENOMEM;

    min_kobj = kobject_create_and_add("min", cfg_mgmt_kobj);
    if (!min_kobj)
        return -ENOMEM;

    max_kobj = kobject_create_and_add("max", cfg_mgmt_kobj);
    if (!max_kobj)
        return -ENOMEM;

    desc_kobj = kobject_create_and_add("desc", cfg_mgmt_kobj);
    if (!desc_kobj)
        return -ENOMEM;

    // register attributes with sysfs, this will create the files
    ret = sysfs_create_group(val_kobj, attr_grp_val_p);
    if(ret)
        return ret;

    ret = sysfs_create_group(val_kobj, attr_grp_val_p);
    if(ret)
        return ret;

    ret = sysfs_create_group(min_kobj, attr_grp_min_p);
    if(ret)
        return ret;

    ret = sysfs_create_group(max_kobj, attr_grp_max_p);
    if(ret)
        return ret;

    ret = sysfs_create_group(desc_kobj, attr_grp_desc_p);
    if(ret)
        return ret;

    return 0;
}


static void cfg_mgmt_remove(struct rpmsg_channel *rpdev)
{
	// remove sysfs files
}


// rpmsg callback function: receives messages from the bare metal application
static void cfg_mgmt_rpmsg_cb(struct rpmsg_channel *rpdev, void *data, int len, void *priv, u32 src)
{
	// check length
	if (len < sizeof(cfgMsg_t))
	{
		dev_info(&rpdev->dev, "Message from BM application is too short.\n");
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
	}

	// wake the sleeping context
	response_valid = 1;
	wake_up_interruptible(&usr_wait_q);
}


static void __exit cm_exit(void)
{
    free_mem(); // make sure we freed our memory
	printk(KERN_INFO "CFG_MGMT: unloading module\n");
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
		return ret;
	}
	// check the response
	if (response.type == RES_N_VARS)
	{
		// ok, we got the correct answer, relay it to the user process
		pending_req.type = REQ_NONE;
		return response.val;
	}
	dev_err(&rpmsg_chnl->dev, "CFG_MGMT: wrong response to N_VARS: %d\n", response.type);
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
static int get_val(int index, u32* val)
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
static int get_min(int index, u32* val)
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
static int get_max(int index, u32* val)
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
		len--;	// leave space for \0
		if (response.len < len)	// ensure we don't overfill the buffer and leave space for \0
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


// specify init / exit functions
module_init(cm_init);
module_exit(cm_exit);


/******************************************************************************************************************
*   K E R N E L   D O C
*/
MODULE_LICENSE("GPL v2");

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);

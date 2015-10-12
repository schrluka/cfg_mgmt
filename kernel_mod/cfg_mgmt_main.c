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
* cfg_mgmt_main.c
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
#include <linux/rpmsg.h>
#include <linux/string.h>
#include <linux/debugfs.h>

#include "rpmsg_link.h"

#define DRIVER_AUTHOR "Lukas Schrittwieser"
#define DRIVER_DESC   "Driver for config variable management over an rpmsg link"




/******************************************************************************************************************
*   T Y P E S
*/

// group the information about a variable access (this is passed to the file io functions
// through the inode private data pointer)
struct var_access_info {
    int index;
    access_t type;
};



/******************************************************************************************************************
*   P R O T O T Y P E S
*/

static int cfg_mgmt_probe(struct rpmsg_channel *rpdev);
static void cfg_mgmt_remove(struct rpmsg_channel *rpdev);


static int alloc_mem(int n_vars);
static void free_mem(void);

static int debugfs_open_var(struct inode *inod, struct file *filp);
static ssize_t debugfs_read_var(struct file *filp, char *buff, size_t len, loff_t *off);
static ssize_t debugfs_write_var(struct file *filp, const char *buff, size_t len, loff_t *ppos);
static int debugfs_release_var(struct inode *inod, struct file *filp);

static int debugfs_open_ll(struct inode *inod, struct file *filp);



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
	.drv.name   = "cfg_mgmt_rpmsg",
	.drv.owner  = THIS_MODULE,
	.id_table   = rpmsg_channel_id_table,
	.probe      = &cfg_mgmt_probe,
	.remove     = &cfg_mgmt_remove,
	.callback   = &cfg_mgmt_rpmsg_cb,
};

struct rpmsg_channel* rpmsg_chnl;

// wait queue used to block the user process
wait_queue_head_t usr_wait_q;

// DEBUGFS elements for exporting the variables
// directories
static struct dentry* cfg_mgmt_dir_p;
static struct dentry* val_dir_p;
static struct dentry* min_dir_p;
static struct dentry* max_dir_p;
static struct dentry* desc_dir_p;

static struct dentry* ll_file_p;

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
static struct file_operations fops_ll = {
    .owner      = THIS_MODULE,
    .open       = &debugfs_open_ll,
    .read       = &debugfs_read_var,
	.release    = &debugfs_release_var,
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

    init_waitqueue_head(&usr_wait_q);

	// register as rpmsg driver module, we will get probed once the other side establishes a connection
	return register_rpmsg_driver(&cfg_mgmt_rpmsg_drv);
}

// when a file is opened query the current value and print it into a string buffer
static int debugfs_open_var(struct inode *inod, struct file *filp)
{
    int ret;
    struct file_io_buf* iobuf;
    struct var_access_info* acc_p = inod->i_private;

    dev_dbg(&rpmsg_chnl->dev, "CFG_MGMT %s: index %d\n", __func__, acc_p->index);
    // get memory for a local kernel buffer
    iobuf = kzalloc(sizeof(*iobuf), GFP_KERNEL);
    if (!iobuf)
        return -ENOMEM;

    // store a pointer to this buffer in the file structure where read/write functions can use it
    filp->private_data = (void*)iobuf;

    // if the file is opened for reading query the according variable
    if (filp->f_mode & FMODE_READ) {
        iobuf->rnw = true;
        // query the value and print it to a local (kernel space) buffer
        if (filp->f_flags & O_NONBLOCK) {
            // user process requests non-blocking IO, so just start the request
            // and return. Once data arrives it will be put in the iobuf by the rpmsg callback
            ret = access_var(acc_p->index, acc_p->type, iobuf, NULL);
        } else {
            // query variable and block until we have the result
            ret = access_var(acc_p->index, acc_p->type, iobuf, &usr_wait_q);
        }
    }
    if (filp->f_mode & FMODE_WRITE) {
        iobuf->rnw = false;
        iobuf->valid = true;    // file could be opened for reading and writing!
    }
    return 0;
}


static ssize_t debugfs_read_var(struct file *filp, char *buff, size_t len, loff_t *ppos)
{
    int ret;
    // use a simple helper to allow the user process to read our buffer
    struct file_io_buf* iobuf;
    iobuf = filp->private_data;

    dev_dbg(&rpmsg_chnl->dev, "%s: len %d, ppos %lld\n", __func__, len, *ppos);

    if (!iobuf)
        return -EINVAL; // should never happen

        // check that the buffer really has data
    if (!iobuf->valid) {
        // there is no data in the buffer (yet)
        if (filp->f_flags & O_NONBLOCK) {
            // file is opened for async IO, so tell the user process to come back later
            return -EAGAIN;
        }
        // blocking IO, wait until we have the data
        ret = wait_event_interruptible(usr_wait_q, iobuf->valid);
        if (ret) {	// abort in case we got interrupted
            dev_err(&rpmsg_chnl->dev, "%s: interrupted\n", __func__);
            return ret;
        }
    }

    if (iobuf->err < 0) {
        dev_err(&rpmsg_chnl->dev, "%s: config var query failed: %d\n", __func__, iobuf->err);
        return iobuf->err;
    }

    return simple_read_from_buffer(buff, len, ppos, iobuf->buf, iobuf->len);
}


static ssize_t debugfs_write_var(struct file *filp, const char *buff, size_t len, loff_t *ppos)
{
    // use a simple helper to allow the user process to write our buffer
    struct file_io_buf* iobuf;
    iobuf = filp->private_data;

    if (!iobuf)
        return -EINVAL; // should never happen


    dev_dbg(&rpmsg_chnl->dev, "%s: len %d, ppos %lld\n", __func__, len, *ppos);
    iobuf->dirty = true;    // buffer is now modified
    return simple_write_to_buffer(iobuf->buf, IO_BUF_SIZE, ppos, (void*)buff, len);
}


static int debugfs_release_var(struct inode *inod, struct file *filp)
{
    int ret;
    struct file_io_buf* iobuf = filp->private_data;
    struct var_access_info* acc_p = inod->i_private;

	if (!iobuf)
        return -EINVAL; // should never happen

    // write the value back if the file was opened for writing
    if ((filp->f_mode&FMODE_WRITE) && (iobuf->dirty)) {
        if (acc_p->type != ACC_VAL) {
            dev_err(&rpmsg_chnl->dev, "%s: attempting to write anything other then variable value\n", __func__);
            return -EINVAL;
        }

        // write the new value to the BM application
        ret = access_var(acc_p->index, acc_p->type, iobuf, &usr_wait_q);
        if (ret) {
            dev_err(&rpmsg_chnl->dev, "%s: can't set new value: %d\n", __func__, ret);
            return ret;
        }
    }
    // remove the io buffer, no longer need it as the file is closed
    // todo: we could recycle those in a list of unused buffers
    kfree(iobuf);
	return 0;
}


// called when the update file is opened: get all variable names and create the necessary debugfs
// directories and files
static int debugfs_open_ll(struct inode *inod, struct file *filp)
{
    int ret,i;
    int n_vars;
    struct device* dev = &rpmsg_chnl->dev;  // abbrevation
    // used to query the variable names, static to save stack space as this contains a rather large buffer
    static struct file_io_buf query_buf;
    // buffer for the message presented to the user (after it has opened the files which called this function)
    struct file_io_buf* msg_buf;

    dev_dbg(dev, "%s: starting\n", __func__);

    msg_buf = kzalloc(sizeof(*msg_buf), GFP_KERNEL);
    if (!msg_buf)
        return -ENOMEM;
    // keep a pointer to the message buffer in the file pointer where the read function can access it
    // memory will be freed once the file is closed
    filp->private_data = (void*)msg_buf;

    if (val_access) {
        // already initialized, we could re-init here? (not coded yet)
        msg_buf->len = scnprintf(msg_buf->buf, IO_BUF_SIZE,
            "Variables list was already loaded, can't reload (unimplemented)\n");
        msg_buf->valid = true;
        msg_buf->rnw = true;
        return 0;   // Note: return with success, the file is opened, user will read the error text
    }

    // query the number of variables and block until we have a result
    n_vars = get_n_vars(&usr_wait_q);

	if (n_vars <= 0) {
        msg_buf->len = scnprintf(msg_buf->buf, IO_BUF_SIZE,
            "Can't query the number of configuration variables from BM firmware: %d\n", n_vars);
        msg_buf->valid = true;
        msg_buf->rnw = true;
		return 0;	// nothing todo as there are no vars or we don't know how many there are
    }

    ret = alloc_mem(n_vars);  // get memory for global arrays
    if (ret) {
        msg_buf->len = scnprintf(msg_buf->buf, IO_BUF_SIZE,
            "Memory allocation failed: %d\n", ret);
        msg_buf->valid = true;
        msg_buf->rnw = true;
        return 0;
    }

	// allocate directories
    val_dir_p = debugfs_create_dir("val", cfg_mgmt_dir_p);
    if (!val_dir_p) {
        msg_buf->len = scnprintf(msg_buf->buf, IO_BUF_SIZE, "Can't create debugfs dir 'val' %d\n", ret);
        msg_buf->valid = true;
        msg_buf->rnw = true;
        return 0;
    }
    min_dir_p = debugfs_create_dir("min", cfg_mgmt_dir_p);
    if (!min_dir_p) {
        msg_buf->len = scnprintf(msg_buf->buf, IO_BUF_SIZE, "Can't create debugfs dir 'min' %d\n", ret);
        msg_buf->valid = true;
        msg_buf->rnw = true;
        return 0;
    }
    max_dir_p = debugfs_create_dir("max", cfg_mgmt_dir_p);
    if (!max_dir_p) {
        msg_buf->len = scnprintf(msg_buf->buf, IO_BUF_SIZE, "Can't create debugfs dir 'max' %d\n", ret);
        msg_buf->valid = true;
        msg_buf->rnw = true;
        return 0;
    }
    desc_dir_p = debugfs_create_dir("desc", cfg_mgmt_dir_p);
    if (!desc_dir_p) {
        msg_buf->len = scnprintf(msg_buf->buf, IO_BUF_SIZE, "Can't create debugfs dir 'desc' %d\n", ret);
        msg_buf->valid = true;
        msg_buf->rnw = true;
        return 0;
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
        query_buf.dirty = false;
        query_buf.rnw = true;
        query_buf.valid = false;
        query_buf.len = 0;
        // get the variable name
        ret = access_var(i, ACC_NAME, &query_buf, &usr_wait_q);
		if (ret < 0) {
			dev_err(dev, "%s: can't query variable name for index %d\n", __func__, i);
			continue;   // we can keep the other variables and simply create no files for this index
		}
		// create all files for this variable
        debugfs_create_file(query_buf.buf, 0666, val_dir_p, (void*)(&val_access[i]),
				       &fops_var);
        debugfs_create_file(query_buf.buf, 0444, min_dir_p, (void*)(&min_access[i]),
				       &fops_var);
        debugfs_create_file(query_buf.buf, 0444, max_dir_p, (void*)(&max_access[i]),
				       &fops_var);
        debugfs_create_file(query_buf.buf, 0444, desc_dir_p, (void*)(&desc_access[i]),
				       &fops_var);
	}

    // alternatively we could do a 'happy programs don't talk' here.
    msg_buf->len = scnprintf(msg_buf->buf, IO_BUF_SIZE, "ok\n");
    msg_buf->valid = true;
    msg_buf->rnw = true;

    dev_dbg(dev, "%s: done\n", __func__);
    return 0;
}


// probe function, called when the remote side establishes a connection with us
static int cfg_mgmt_probe (struct rpmsg_channel *rpdev)
{
	dev_dbg(&rpdev->dev, "%s: starting\n",__func__);

	// save the rpmsg channel pointer for use by all other functions
	rpmsg_chnl = rpdev;

    // create a new directory in debugfs for our module
    cfg_mgmt_dir_p = debugfs_create_dir("cfg_mgmt", NULL);
    if (!cfg_mgmt_dir_p || (cfg_mgmt_dir_p<0)) {
        dev_err(&rpdev->dev, "%s: can't create debugfs directory: %d\n", __func__, (int)cfg_mgmt_dir_p);
        cfg_mgmt_dir_p = NULL;
        return -ENOENT;
    }

    // init communication logic
    rpmsg_link_init(rpdev);

    // create the update file, reading it will trigger a generation of the variable files
    ll_file_p = debugfs_create_file("load_list", 0444, cfg_mgmt_dir_p, NULL, &fops_ll);

    dev_dbg(&rpdev->dev, "%s: done\n", __func__);
	return 0;
}


static int alloc_mem(int n_vars)
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

    rpmsg_link_exit();
    free_mem(); // remove all files and free memory

	dev_dbg(&rpdev->dev, "%s: done\n",__func__);
}


static void __exit cm_exit(void)
{
    //free_mem(); // make sure we freed our memory
	printk(KERN_INFO "CFG_MGMT: unloading module\n");
	unregister_rpmsg_driver(&cfg_mgmt_rpmsg_drv);
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

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
#define DRIVER_DESC   "Driver for stdio communication between linux and bare metal app over an rpmsg link"


/******************************************************************************************************************
*   D E F I N E S   F O R   B M   I N T E R F A C E
*/




/******************************************************************************************************************
*   T Y P E S
*/




/******************************************************************************************************************
*   P R O T O T Y P E S
*/

static int bm_stdio_probe(struct rpmsg_channel *rpdev);
static void bm_stdio_remove(struct rpmsg_channel *rpdev);
static void bm_stdio_rpmsg_cb(struct rpmsg_channel *rpdev, void *data, int len, void *priv, u32 src);

static loff_t dev_llseek(struct file *filp, loff_t off, int whence);
static int dev_open(struct inode*, struct file*);
static int dev_rls(struct inode*, struct file*);
static ssize_t dev_read(struct file*, char*, size_t, loff_t *);
static ssize_t dev_write(struct file*, const char*, size_t, loff_t *);



/******************************************************************************************************************
*   G L O B A L S
*/

// table of RPMSG channels names used by this driver
static struct rpmsg_device_id rpmsg_channel_id_table[] = {
	{ .name = "bm_stdio" },
	{},
};
MODULE_DEVICE_TABLE(rpmsg, rpmsg_channel_id_table);

// callback functions for RPMSG communication (with bare metal application)
static struct rpmsg_driver cfg_mgmt_rpmsg_drv = {
	.drv.name = "bm_stdio_rpmsg",
	.drv.owner = THIS_MODULE,
	.id_table = rpmsg_channel_id_table,
	.probe = &bm_stdio_probe,
	.remove = &bm_stdio_remove,
	.callback = &bm_stdio_rpmsg_cb,
};


// data buffers
static char bm2lin_buf [BUF_LEN];
static char lin2bm_buf [BUF_LEN];




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
		dev_info(&rpdev->dev, "CFG_MGMT %s: received reply: seq=%d, ind=%d, val=%d.\n", __func__, response.seq, response.ind, response.val);
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



static int dev_open(struct inode *inod, struct file *fil)
{
	times++;
	printk(KERN_INFO "AMP_CTRL: Prog Mem Device opened %d times\n",times);
	return 0;
}

static ssize_t dev_read(struct file *filp, char *buff, size_t len, loff_t *off)
{
    size_t todo;
	//kernel_buf = (char*)kmalloc(len, GFP_KERNEL);

	if(len > PROG_MEM_LEN) {
		len = PROG_MEM_LEN;
	}
	// use the static buffer to transfer data from CPU1 to the user application
	todo = len;
    while (todo > 0)
    {
        size_t l = (todo > KBUF_LEN) ? KBUF_LEN : todo;
        memcpy_fromio(kernel_buf, progMemp, l);
        copy_to_user(buff,kernel_buf,l);
        todo -= l;
    }

	//kfree(kernel_buf);
	return len;
}

static ssize_t dev_write(struct file *filp, const char *buff, size_t len, loff_t *ppos)
{
	printk(KERN_INFO "AMP_CTRL: Prom Mem Write, len %d, file ptr %d, offset %llu\n", len, (int)(filp->f_pos), (*ppos));

	if(len + *ppos > PROG_MEM_LEN) {
		len = PROG_MEM_LEN - *ppos;
	}

	memcpy_toio(progMemp + *ppos, buff, len);

	*ppos = *ppos + len;

	return len;
}

static int dev_rls(struct inode *inod, struct file *fil)
{
	printk(KERN_ALERT "AMP_CTRL: Prog Mem Device Closed\n");
	return 0;
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

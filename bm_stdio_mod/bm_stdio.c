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
* bm_stdio.c
*
* Main File of Kernel module which uses a rpmsg channel to send/receive stdio data of a baremetal appliation running
* on CPU1 and exports this to a char-dev file
*
************************************************************************************************************************/

//#define DEBUG

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/kfifo.h>
#include <asm/uaccess.h>
#include <linux/rpmsg.h>
#include <linux/string.h>


#define DRIVER_AUTHOR "Lukas Schrittwieser"
#define DRIVER_DESC   "Driver for stdio communication between linux and bare metal app over an rpmsg link"


/******************************************************************************************************************
*   D E F I N E S
*/

// use a major file number assigned for experimental use
#define MAJOR_NR 242

#define TX_BUF_SIZE     256



/******************************************************************************************************************
*   T Y P E S
*/




/******************************************************************************************************************
*   P R O T O T Y P E S
*/

static int bm_stdio_probe(struct rpmsg_channel *rpdev);
static void bm_stdio_remove(struct rpmsg_channel *rpdev);
static void bm_stdio_rpmsg_cb(struct rpmsg_channel *rpdev, void *data, int len, void *priv, u32 src);


//static loff_t dev_llseek(struct file *filp, loff_t off, int whence);
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
static struct rpmsg_driver bm_stdio_rpmsg_drv = {
	.drv.name = KBUILD_MODNAME,
	.drv.owner = THIS_MODULE,
	.id_table = rpmsg_channel_id_table,
	.probe = &bm_stdio_probe,
	.remove = &bm_stdio_remove,
	.callback = &bm_stdio_rpmsg_cb,
};

static struct file_operations fops_bm_stdio =
{
	//.llseek = dev_llseek,
	.read = dev_read,
	.open = dev_open,
	.write = dev_write,
	//.unlocked_ioctl = dev_ioctl,
	.release = dev_rls,
};

// rpmsg channel for communication with the bare metal application
struct rpmsg_channel* rpmsg_chnl;

// use two kernel fifos to transfer data to and from BM application
//DEFINE_KFIFO(lin2bm_fifo, u8, 8*1024);
DEFINE_KFIFO(bm2lin_fifo, u8, 64*1024);

static int times = 0;

static u8 tx_buf[TX_BUF_SIZE];



/******************************************************************************************************************
*   I M P L E M E N T A T I O N
*/

// init function, called upon module start
static int __init bm_init(void)
{
    printk(KERN_INFO "bm_stdio: Loading baremetal app stdio message forwarding module\n");

    // register as rpmsg driver module, we will get probed once the other side establishes a connection
	return register_rpmsg_driver(&bm_stdio_rpmsg_drv);
}


// probe function, called when the remote side establishes a connection with us
static int bm_stdio_probe (struct rpmsg_channel *rpdev)
{
    int ret;
	dev_info(&rpdev->dev, "%s: starting\n", __func__);

    rpmsg_chnl = rpdev;

	//INIT_KFIFO(lin2bm_fifo);
	INIT_KFIFO(bm2lin_fifo);

    ret = register_chrdev(MKDEV(MAJOR_NR,0), "bm_stdio", &fops_bm_stdio);
    if (ret) {
        dev_dbg(&rpdev->dev, "%s: register_chrdev failed: %d\n", __func__, ret);
        return ret;
    }
    // we have to send a message to the other side, otherwise the BM app can't know our address
    ret = 0;
    ret = rpmsg_send(rpmsg_chnl, &ret, 1);
    dev_info(&rpdev->dev, "%s: rmpsg_send ret: %d\n", __func__, ret);

    dev_info(&rpdev->dev, "%s: done\n", __func__);
	return 0;
}


static void bm_stdio_remove(struct rpmsg_channel *rpdev)
{
	dev_info(&rpdev->dev, "%s: starting\n",__func__);

    unregister_chrdev(MKDEV(MAJOR_NR,0), "bm_stdio");

	dev_info(&rpdev->dev, "%s: done\n",__func__);
}


// rpmsg callback function: receives messages from the bare metal application
static void bm_stdio_rpmsg_cb(struct rpmsg_channel *rpdev, void *data, int len, void *priv, u32 src)
{
	// do we have to append a \0 to the string here?
	char* d = data;
	d[len] = '\0';   // FIXME: check for buffer overrun here
	dev_dbg(&rpdev->dev, "%s: received data: %s\n", __func__, (char*)data);
	// put data into fifo
	kfifo_in(&bm2lin_fifo, data, len);
}


static void __exit bm_exit(void)
{
    printk(KERN_INFO "bm_stdio: unloading module\n");
	unregister_rpmsg_driver(&bm_stdio_rpmsg_drv);
	printk(KERN_INFO "bm_stdio: unload done\n");
}


static int dev_open(struct inode *inod, struct file *fil)
{
	times++;
	dev_dbg(&rpmsg_chnl->dev, "%s: deviced opened %d times\n", __func__, times);
	return 0;
}


static ssize_t dev_read(struct file *filp, char *buf, size_t len, loff_t *off)
{
    size_t copied;

    int ret = kfifo_to_user(&bm2lin_fifo, buf, len, &copied);

    dev_dbg(&rpmsg_chnl->dev, "%s: kfifo copied %d bytes (ret %d)\n", __func__, copied, ret);

    if (ret < 0) {
        dev_err(&rpmsg_chnl->dev, "%s: can't copy from kfifo to user space: %d\n", __func__, ret);
        return ret;
    }

    return copied;
}


static ssize_t dev_write(struct file *filp, const char *buf, size_t len, loff_t *ppos)
{
	size_t copied = 0;
    int ret;
    size_t n;

    while (copied < len) {
        // copy data in TX_BUF_SIZE long chuncks
        if ((len-copied) > TX_BUF_SIZE)
            n = TX_BUF_SIZE;
        else
            n = len-copied;
        ret = copy_from_user(tx_buf, buf, n);
        if (ret) {
            dev_err(&rpmsg_chnl->dev, "%s: can't read from user space\n", __func__);
            return -1;
        }
        // send message to BM app
        ret = rpmsg_send(rpmsg_chnl, tx_buf, n);
        if (ret) {
            dev_err(&rpmsg_chnl->dev, "%s: can't transmit on RPMSG channel: %d\n", __func__, ret);
            return ret;
        }
        copied += len;
    }

    return copied;
}


static int dev_rls(struct inode *inod, struct file *fil)
{
	return 0;
}


// specify init / exit functions
module_init(bm_init);
module_exit(bm_exit);



/******************************************************************************************************************
*   K E R N E L   D O C
*/
MODULE_LICENSE("GPL v2");

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);

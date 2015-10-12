

#ifndef __RPMSG_LINK__
#define __RPMSG_LINK__

// configure size (max length) of the data field in messages exchanged with BM application
//#define MSG_DATA_SIZE 	(DATA_LEN_MAX-sizeof(cfgMsg_t))
// This is a super uggly hack, I have not found a good solution yet. (Total message length is 512 bytes, leave
// space for headers
#define MSG_DATA_SIZE 	(400)


// file IOs are made to a buffer in kernel space, define its length
// as we read/write up to the max transport capability of the underlying comm channel reserve that amount
#define IO_BUF_SIZE         MSG_DATA_SIZE


// define an enum which tells the read/write functions what aspect of a var is accessed
typedef enum {ACC_NAME, ACC_VAL, ACC_MIN, ACC_MAX, ACC_DESC} access_t;


// struct exchanged with bare metal firmware for communication (can be a request or a response)
typedef struct __attribute__((packed))       // make sure it has no holes (kernel does the same)
{
    uint32_t    seq;    // message sequence number identifying request and response
    uint32_t    type;   // message type
    int32_t     ind;    // config variable index (<0 means unkown/undefined)
    int32_t     val;    // numerical value (for WR req, RD resp, etc)
    uint32_t    len;    // length of data section (in bytes)
    uint8_t     data[MSG_DATA_SIZE]; // opt. data section, total message has to fit into TX_BUFFER_SIZE of rpmsg
} cfgMsg_t;


struct file_io_buf {
    ssize_t len;                // length of string in buf
    char buf[IO_BUF_SIZE];
    bool dirty;                 // true if buffer was modified
    bool valid;                 // true once data has arrived (in case of async io)
    bool rnw;                   // read-not-write flag to determine direction of var access
    int err;                    // error code (neg value) if access failed
};

int rpmsg_link_init(struct rpmsg_channel *ch);

void rpmsg_link_exit(void);

int get_n_vars(wait_queue_head_t* wait_q_p);

int access_var(int index, access_t acc, struct file_io_buf* buf, wait_queue_head_t* wait_q_p);

void cfg_mgmt_rpmsg_cb(struct rpmsg_channel *rpdev, void *data, int len, void *priv, u32 src);

#endif

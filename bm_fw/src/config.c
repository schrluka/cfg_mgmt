/******************************************************************************************************************************
*
*   AMP Configuration Variable Management
*
*   Copyright (c) 2015 Lukas Schrittwieser
*
*   Permission is hereby granted, free of charge, to any person obtaining a copy
*   of this software and associated documentation files (the "Software"), to deal
*   in the Software without restriction, including without limitation the rights
*   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
*   copies of the Software, and to permit persons to whom the Software is
*   furnished to do so, subject to the following conditions:
*
*   The above copyright notice and this permission notice shall be included in
*   all copies or substantial portions of the Software.
*
*   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
*   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
*   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
*   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
*   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
*   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
*   THE SOFTWARE.
*
*******************************************************************************************************************************
*
*   config.c
*
*   Config varaible management functions
*
******************************************************************************************************************************/

/******************************************************************************************************************************
*   I N C L U D E S
*/
#include <xil_printf.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "config_vars.h"
#include "remoteproc.h"



/******************************************************************************************************************************
*   D E F I N E S
*/

// macro to get the total number of configured variables
//#define N_VARS	(sizeof(vars)/sizeof(cfgVar_t))

// request and response types (codes) for communication with kernel (type field in  cfgReq_t)
#define REQ_NOP        0       // do nothing
// kernel to BM (requests)
#define REQ_N_VARS  1       // read number of variables (N_VARS)
#define REQ_WR      2       // write (kernel to BM) request
#define REQ_RD      3       // read value (BM to kernel) request
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


// configure size (max length) of the data field in messages exchanged with BM application
//#define MSG_DATA_SIZE 	(DATA_LEN_MAX-sizeof(cfgMsg_t))
// This is a super uggly hack, I have not found a good solution yet. (Total message length is 512 bytes, leave
// space for headers
#define MSG_DATA_SIZE 	(400)


/******************************************************************************************************************************
*   G L O B A L S
*/




// struct exchanged with kernel for communication (can be a request or a response)
typedef struct __attribute__((packed))       // make sure it has no holes (kernel does the same)
{
    uint32_t    seq;    // message sequence number identifying request and response
    uint32_t    type;   // message type
    int32_t     ind;    // config variable index (<0 means unkown/undefined)
    int32_t     val;    // numerical value (for WR req, RD resp, etc)
    uint32_t    len;    // length of data section (in bytes)
    uint8_t     data[MSG_DATA_SIZE]; // opt. data section, total messages has to fit into TX_BUFFER_SIZEs
} cfgMsg_t;


// allocate a TX buffer to send replies to the kernel
#define CFG_BUF_LEN     DATA_LEN_MAX
uint8_t cfgMsgTxBuf[CFG_BUF_LEN];





/******************************************************************************************************************************
*   P R O T O T Y P E S
*/

// rp message channel used for communication with linux host
struct rpmsg_channel* rpmsg_config = NULL;

// callback function receiving config data from kernel
void config_msg_handler(struct rpmsg_channel* ch, uint8_t* data, uint32_t len);

// set new value for variable at index i in global variable array
int cfgSetInd(int i, int32_t val, bool trigCb);



/******************************************************************************************************************************
*   I M P L E M E N T A T I O N
*/

// creates rpmsg channel for communication with kernel
void cfgInit()
{
    // announce a rpmsg channel for communication with the kernel
    rpmsg_config = rpmsg_create_ch ("cfg_mgmt", &config_msg_handler);

}


void config_msg_handler(struct rpmsg_channel* ch, uint8_t* data, uint32_t len)
{
    cfgMsg_t* req = (cfgMsg_t*)data;  // request message from kernel
    cfgMsg_t* rep = (cfgMsg_t*)cfgMsgTxBuf;   // reply message to kernel

    // copy message sequence number (for request / reply matching) and variable index
    rep->seq = req->seq;
    rep->ind = req->ind;
    rep->len = 0;   // assume no additional data

    //xil_printf("%s: receveid req, seq: %d, index: %d, type: %d\n",__func__, req->seq, req->ind, req->type);
    if (req->type == REQ_NOP)
    {
        // simply reply with an OK message
        rep->type = RES_OK;
        rep->val = 0;   // could send some data here
        rpmsg_send(rpmsg_config, (void*)rep, sizeof(*rep));
        return;
    }

    if (req->type == REQ_N_VARS)
    {
        rep->type = RES_N_VARS;
        rep->val = n_vars;
        rpmsg_send(rpmsg_config, (void*)rep, sizeof(*rep));
        return;
    }

    // all other commands require a clear identification of a variable
    // try to identify the variable requested by the kernel
    int32_t ind = req->ind;
    /*if (ind < 0)
    {
        // no index specified, check for name string in the data section
        if (req->len > 0)
        {
            for (int i=0; i<n_vars; i++)
            {
                if (strncmp((char*)(req->data), vars[i].name, req->len) == 0)
                {
                    ind = i; // found it!
                    break;
                }
            }
        }
    }*/

    // check index, all remaining commands require a valid index
    if ((ind >= n_vars) || (ind < 0))
    {
        rep->type = RES_ID_ERR;
        rpmsg_send(rpmsg_config, (void*)rep, sizeof(*rep));
        return;
    }

    // parse the kernel's request
    switch (req->type)
    {
       case REQ_WR:
            // write request from kernel, set new value
            if (cfgSetInd(ind, req->val, true) == 1)
                rep->type = RES_OK;
            else
                rep->type = RES_ID_ERR;
            break;

        case REQ_RD:
            // read request from kernel, reply with current value
            // trigger read callback if available (do this before we copy the value)
            if (vars[ind].rd_cb != NULL)
                vars[ind].rd_cb(&vars[ind], true, vars[ind].rd_cb_data);
            rep->val = vars[ind].val;
            rep->type = RES_RD_VAL;
            break;

        case REQ_RD_MIN:
            // read request from kernel, reply with min limit value
            rep->val = vars[ind].min;
            rep->type = RES_RD_MIN;
            break;

        case REQ_RD_MAX:
            // read request from kernel, reply with max limit value
            rep->val = vars[ind].max;
            rep->type = RES_RD_MAX;
            break;

        case REQ_NAME:
            // send variable name to kernel
            rep->len = strlen(vars[ind].name);
            if (rep->len > (CFG_BUF_LEN-sizeof(cfgMsg_t)))
                rep->len = (CFG_BUF_LEN-sizeof(cfgMsg_t));
            strncpy((char*)(rep->data), vars[ind].name, rep->len);
            rep->val = vars[ind].val;    // just send the current value as well
            rep->type = RES_NAME;
            break;

        case REQ_DESC:
            // send variable name to kernel
            rep->len = strlen(vars[ind].desc);
            if (rep->len > (CFG_BUF_LEN-sizeof(cfgMsg_t)))
                rep->len = (CFG_BUF_LEN-sizeof(cfgMsg_t));
            strncpy((char*)(rep->data), vars[ind].desc, rep->len);
            rep->val = vars[ind].val;    // just send the current value as well
            rep->type = RES_DESC;
            break;

        default:
            rep->type = RES_REQ_ERR;
            break;

    }
    // send the reply to the server
    rpmsg_send(rpmsg_config, (void*)rep, sizeof(*rep));
}


// read configuration value from variable with given id
// id: id of the config variable to be read
// val: pointer where variable will be stored (unchanged in case of error)
// return: 1 on success, 0 on error
int cfgGetValId(int id, int32_t* val)
{
	if (val == NULL)
		return 0;

	// check all ids
	for (int i=0; i<n_vars; i++)
	{
		if (vars[i].id == id)
		{
			*val = vars[i].val;
			return 1;
		}
	}
	// we could not find this id
	return 0;
}


// read configuration value from variable with given name
// name: pointer ot the variable name
// val: pointer where variable will be stored (unchanged in case of error)
// return: 1 on success, 0 on error
int cfgGetValName(const char* name, int32_t* val)
{
	if (val == NULL)
		return 0;

	// check all ids
	for (int i=0; i<n_vars; i++)
	{
		if (strcmp(vars[i].name, name) == 0)
		{
			*val = vars[i].val;
			return 1;
		}
	}
	// we could not find this id
	return 0;
}


// get pointer to human readable name of a variable
// returns: pointer to string or NULL if a variable with this id was not found
const char* cfgGetName(int id)
{
	// check all ids
	for (int i=0; i<n_vars; i++)
	{
		if (vars[i].id == id)
			return vars[i].name;
	}
	// we could not find this id
	return NULL;
}


// Used to get a list of all variable names
// i: index counter, 0 returns the first name, 1 the second and so on. This is not the id value assigned to
//    the config variables.
// returns: pointer to string or NULL if no more names exist
const char* cfgGetNameList(int i)
{
	// return next name if it is valid
	if (i<n_vars)
		return vars[i].name;
	else
		return NULL;	// no more variables
}


// get variable struct with given id
// id: id of the variable to be looked up
// v: pointer to struct where information will be copied
// returns: 1 on success, 0 if id is not found or v is NULL
int cfgGetStructId(int id, cfgVar_t* v)
{
	if (v == NULL)
		return 0;

	// check all ids
	for (int i=0; i<n_vars; i++)
	{
		if (vars[i].id == id)
		{
			memcpy(v, &(vars[i]), sizeof(cfgVar_t));
			return 1;
		}
	}
	// we could not find this id
	return 0;
}

// get variable struct with given name
// id: id of the variable to be looked up
// v: pointer to struct where information will be copied
// returns: 1 on success, 0 if id is not found or v is NULL
int cfgGetStructName(const char* n, cfgVar_t* v)
{
	if (v == NULL)
		return 0;

	// check all ids
	for (int i=0; i<n_vars; i++)
	{
		if (strcmp(vars[i].name, n) == 0)
		{
			memcpy(v, &(vars[i]), sizeof(cfgVar_t));
			return 1;
		}
	}
	// we could not find this id
	return 0;
}

// set variable (given by id) to new value
// if the new value exceeds the limits (min/max) of the variable it is limited accordingly
// trigCb: trigger a callback if this is true
// returns 1 on success and 0 on error
int cfgSetId(int id, int32_t val, bool trigCb)
{
	// check all ids
	for (int i=0; i<n_vars; i++)
	{
		if (vars[i].id == id)
		{
			return cfgSetInd(i, val, trigCb);
		}
	}
	// we could not find this id
	return 0;
}


// set variable (given index into global cfg var array) to new value
// if the new value exceeds the limits (min/max) of the variable it is trimmed accordingly
// trigCb: trigger a callback if this is true
// returns 1 on success and 0 on error
int cfgSetInd(int i, int32_t val, bool trigCb)
{
    if (i < 0)
        return 0;
	if (i > n_vars)
        return 0;

    // limit new value
    if (val > vars[i].max)
        val = vars[i].max;
    if (val < vars[i].min)
        val = vars[i].min;
    // set new value
    vars[i].val = val;
    // execute the callback if requested and available
    if (trigCb && (vars[i].wr_cb!=NULL))
        vars[i].wr_cb(&(vars[i]), false, vars[i].wr_cb_data);
    return 1;
}


int cfgSetCallback(int id, cfgCallback_t cb, bool read, void* data)
{
    // check all ids
	for (int i=0; i<n_vars; i++)
	{
		if (vars[i].id == id)
		{
		    if (read)
            {
                vars[i].rd_cb = cb;
                vars[i].rd_cb_data = data;
            }
            else
            {
                vars[i].wr_cb = cb;
                vars[i].wr_cb_data = data;
            }
			return 1;
		}
	}
	// we could not find this id
	return 0;
}

// end of file config.c

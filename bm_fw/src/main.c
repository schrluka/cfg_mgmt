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
*   main.c
*
*   Top Level File
*
******************************************************************************************************************************/

// std headers
#include <stdint.h>
#include <stddef.h>
// xilinx headers
#include <xuartps.h>
#include <xil_printf.h>
#include <xscugic.h>
#include <xil_exception.h>

#include "remoteproc.h"
#include "config.h"
#include "config_vars.h"
#include "uart.h"



/******************************************************************************************************************************
*   G L O B A L S
*/

// pointer to GPIO module with LEDS. CAVE: can't read current value, no HW support for this, thanks xilinx!
volatile uint32_t* const pLed = (void*)(0x41200000);
// pointer to GPIO module for switch signals (simple test)
volatile uint32_t* const p_sw = (void*)(0x41200008);

// Instance of the interrupt controller driver's data structure, this contains all information required by the driver
XScuGic IntcInst;

// rpmsg channel for STDIO
struct rpmsg_channel* rpmsg_stdio = NULL;




/******************************************************************************************************************************
*   P R O T O T Y P E S
*/

void irq_init();

void var_cb (struct cfg_var* var, bool isread, void* data);



/******************************************************************************************************************************
*   I M P L E M E N T A T I O N
*/

int main(void)
{
    int i=0;
    int busy;

    (*pLed) = 1;    // led uses neg logic

    if (UartInit() != XST_SUCCESS)
    {
        while(1);
    }

    xil_printf("CFG_MGMT - Example Firmware\n");

    irq_init();

    remoteproc_init();
    //xil_printf("remoteproc_init done\n");

    // create a channel for printf message, we don't receive from the kernel
    //rpmsg_stdio = rpmsg_create_ch ("stdio", 0);

    cfgInit();

    xil_printf("registering wr callback: %d\n", cfgSetCallback(CFG_VAR_2, &var_cb, false, NULL));
    xil_printf("registering rd callback: %d\n", cfgSetCallback(CFG_VAR_1, &var_cb, true, NULL));

    xil_printf("n_vars: %d, name 1: %s\n", n_vars, vars[1].name);

    xil_printf("init done\n");



    while(1)
    {
        busy = 0;
        // periodically call the rpmsg workhorse
        busy |= rpmsg_poll();

        /*i++;
        if (i == 1500)
        {
            i = 0;
            // show the variable values
            int32_t v;
            cfgGetValId(CFG_VAR_1, &v);
            (*pLed) = !v;
            xil_printf("var1: %d  ", v);
            cfgGetValId(CFG_VAR_2, &v);
            xil_printf("var2: %d\n", v);
        } */

        // update switch signals
        int32_t v;
        cfgGetValId(CFG_SW_TEST, &v);
        v ^= 0x1ff0;
        *p_sw = v;

        // go to sleep to save energy
        if (!busy)
        {
            __asm__ __volatile__ ("wfe" ::: "memory");
        }
    }
}


// configure the interrupt controller
void irq_init()
{
    XScuGic_Config *IntcConfig;

	// Initialize the interrupt controller driver
	IntcConfig = XScuGic_LookupConfig(XPAR_SCUGIC_0_DEVICE_ID);
	if (NULL == IntcConfig)
	{
		xil_printf("XScuGic_LookupConfig failed\n");
		return;
	}

    xil_printf("GIC base address: 0x%08x\n", IntcConfig->CpuBaseAddress);

    int Status = XScuGic_CfgInitialize(&IntcInst, IntcConfig, IntcConfig->CpuBaseAddress);
	if (Status != XST_SUCCESS)
	{
        xil_printf("XScuGic_CfgInitialize failed\n");
	}

	// Initialize the exception table (in BSP)
    Xil_ExceptionInit();

	// Register the interrupt controller's handler in the exception table, instance pointer is passed as parameter
    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT, (Xil_ExceptionHandler)XScuGic_InterruptHandler, &IntcInst);
}


// callback function for variable read/write
void var_cb (struct cfg_var* var, bool isread, void* data)
{
    if (isread)
        xil_printf("read ");
    else
        xil_printf("write ");
    xil_printf("callback of %s, val=%d\n", var->name, var->val);
}


// xilinx lib specific function which receives all stdout data
void outbyte (char ch)
{
    static char buf[DATA_LEN_MAX];   // has to be shorter than max message size
    static int ind=0;

    if (ch == '\n')
        outbyte('\r');
    while (XUartPs_Send(&UartPs, (void*)&ch, 1) != 1)
        __asm("nop");

    // if we have a stdio channel via rpmsg, collect data in lines and send it once buffer
    // is full or when a \n comes
    if (rpmsg_stdio != NULL)
    {
        buf[ind++] = ch;

        if ((ch == '\n') || (ind == DATA_LEN_MAX-1))
        {
            // append \0 string termination
            buf[ind++] = '\0';
            rpmsg_send(rpmsg_stdio, buf, ind);
            ind = 0;
        }

    }
}

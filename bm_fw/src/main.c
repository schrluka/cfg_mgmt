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
#include <stdio.h>
#include <stddef.h>
#include <unistd.h>
// xilinx headers
#include <xuartps.h>
#include <xscugic.h>
#include <xil_exception.h>
#include <xil_mmu.h>
#include <xil_cache.h>
#include <xscutimer.h>

#include "remoteproc.h"
#include "config.h"
#include "config_vars.h"
#include "uart.h"



/******************************************************************************************************************************
*   G L O B A L S
*/

// pointer to GPIO module with LEDS. CAVE: can't read current value, no HW support for this, thanks xilinx!
volatile uint32_t* const pLed = (void*)(0x41210000);

// Instance of the interrupt controller driver's data structure, this contains all information required by the driver
XScuGic IntcInst;

// SCU private timer is used to periodically execute the scara controller
XScuTimer timerInst;

// rpmsg channel for STDIO
struct rpmsg_channel* rpmsg_stdio = NULL;

static int stdio_init = 0;

volatile uint32_t sys_tick = 0;



/******************************************************************************************************************************
*   P R O T O T Y P E S
*/

void mmu_init();

void irq_init();

void sys_timer_init();

void sys_timer_ISR(void* data);

void var_cb (struct cfg_var* var, bool isread, void* data);

void stdio_msg_handler(struct rpmsg_channel* ch, uint8_t* data, uint32_t len);

size_t _write(int fd, const void* data, size_t len);



/******************************************************************************************************************************
*   I M P L E M E N T A T I O N
*/

int main(void)
{
    int i=0;
    int led = 0;
    int busy;

    (*pLed) = 1;

    mmu_init();

    // wait several milliseconds to give the kernel time to boot, otherwise the kernel oopses
    for (i=0; i<2500; i++)
        usleep(1000);

    (*pLed) = 2;

    puts("CFG_MGMT - Example Firmware");

    irq_init();

    //sys_timer_init();

    remoteproc_init();
    puts("remoteproc_init done");

    //FILE* fp = fdopen(3, "w");
    //fprintf(fp, "trace file print\n");
    //fflush(fp);

    (*pLed) = 3;

    // create a channel for stdio messages
    puts("creating stdio channel");
    stdio_init = 0;
    rpmsg_stdio = rpmsg_create_ch ("bm_stdio", stdio_msg_handler);

    *pLed = 4;

    // wait until the kernel has told us his rpmsg address (it sends a dummy message)
    while (!stdio_init)
    {
        if (!rpmsg_poll())
            __asm__ __volatile__ ("wfe" ::: "memory");
    }

    (*pLed) = led = 5;

    /*fflush(stdout);
    while(1)
        __asm("nop");*/

    cfgInit();

    printf("registering wr callback: %d\n", cfgSetCallback(CFG_VAR_2, &var_cb, false, NULL));
    printf("registering rd callback: %d\n", cfgSetCallback(CFG_VAR_1, &var_cb, true, NULL));

    puts("init done");

    while(1)
    {
        busy = 0;
        // periodically call the rpmsg workhorse
        busy |= rpmsg_poll();

        //if (i < sys_tick)
        //{
          //  i = sys_tick + 2000;

        usleep(1000);

        i++;
        if (i > 1000)
        {
            i = 0;
            // show the variable values
            int32_t v;
            cfgGetValId(CFG_VAR_1, &v);
            printf("var1: %d  ", (int)v);
            cfgGetValId(CFG_VAR_2, &v);
            printf("var2: %d\n", (int)v);
            //(*pLed) = led++;
        }

        // go to sleep to save energy
        if (!busy)
        {
        //    __asm__ __volatile__ ("wfe" ::: "memory");
        }
    }
}

void mmu_init()
{
    // get start and end values from linker script
    uint32_t addr = ELF_START;
    uint32_t end = addr + ELF_LEN;

    fprintf(stderr, "%s: start x%08x end x%08x\n", __func__, (unsigned int)addr, (unsigned int)end);

    // mark our memory space as shareable, use write back caches (TEX,C,B) and allow full access
    for (;addr <= end; addr+=0x100000)
    {
        Xil_SetTlbAttributes(addr, 0x15de6);  // S=b1 TEX=b101 AP=b011, Domain=b1111, C=b0, B=b1
        //Xil_SetTlbAttributes(addr, 0x04de2);
    }

    // disable caches on LED
    Xil_SetTlbAttributes((unsigned int) pLed, 0x04de2);  // S=b0 TEX=b100 AP=b011, Domain=b1111, C=b0, B=b0

    //Xil_EnableMMU();
}


// configure the interrupt controller
void irq_init()
{
    XScuGic_Config *IntcConfig;

	// Initialize the interrupt controller driver
	IntcConfig = XScuGic_LookupConfig(XPAR_SCUGIC_0_DEVICE_ID);
	if (NULL == IntcConfig)
	{
		puts("XScuGic_LookupConfig failed");
		return;
	}

    printf("GIC base address: 0x%08x\n", (unsigned int)IntcConfig->CpuBaseAddress);

    int Status = XScuGic_CfgInitialize(&IntcInst, IntcConfig, IntcConfig->CpuBaseAddress);
	if (Status != XST_SUCCESS)
	{
        puts("XScuGic_CfgInitialize failed");
	}

	// Initialize the exception table (in BSP)
    Xil_ExceptionInit();

	// Register the interrupt controller's handler in the exception table, instance pointer is passed as parameter
    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT, (Xil_ExceptionHandler)XScuGic_InterruptHandler, &IntcInst);

    // enabled interrupts
    Xil_ExceptionEnable();
}

void sys_timer_init()
{
     // register ISR
    XScuGic_Connect(&IntcInst, XPAR_SCUTIMER_INTR, &sys_timer_ISR, NULL);
    // priority: inc by 8 (0 is highest), high level sensitive
    XScuGic_SetPriorityTriggerType(&IntcInst, XPAR_SCUTIMER_INTR, 8, 3);

    // enable SCU timer interrupt
    XScuGic_Enable(&IntcInst, XPAR_SCUTIMER_INTR);

    // enable timer for periodic execution of scara controller
    // lookup config
    XScuTimer_Config* timerCfg = XScuTimer_LookupConfig(XPAR_SCUTIMER_DEVICE_ID);
    // init timer config
    XScuTimer_CfgInitialize(&timerInst, timerCfg, XPS_SCU_PERIPH_BASE);
    // configure interrupt frq, timer runs at HALF cpu clock rate (see TRM)
    uint32_t tmr_cmp = (XPAR_PS7_CORTEXA9_1_CPU_CLK_FREQ_HZ / 2 / 1000) - 1;
    tmr_cmp = 1000000;
    XScuTimer_LoadTimer(&timerInst, tmr_cmp);
    XScuTimer_EnableAutoReload(&timerInst);
    XScuTimer_SetPrescaler(&timerInst, 0);
    XScuTimer_EnableInterrupt(&timerInst);

    // start timer
    XScuTimer_Start(&timerInst);
}

void sys_timer_ISR(void* data)
{
    static uint32_t led = 0;
    sys_tick++;
    if ((sys_tick%1000) == 0)
        *pLed = led++;
}

// callback function for variable read/write
void var_cb (struct cfg_var* var, bool isread, void* data)
{
    if (isread)
        printf("read ");
    else
    {
        if (var->id == CFG_VAR_1)
            (*pLed) = var->val;
        printf("write ");
    }
    printf("callback of %s, val=%d\n", var->name, (int)var->val);
}


// xilinx lib specific function which receives all stdout data
void outbyte (char ch)
{
    static char buf[DATA_LEN_MAX];
    static int ind=0;

    buf[ind++] = ch;

    // to minimize the number of rpmsg we have to send:
    // collect data in a buffer until it is full or we have \n
    if ((ch == '\n') || (ind == DATA_LEN_MAX-2))
    {
        _write(STDERR_FILENO, (void*)buf, ind);
        ind = 0;
    }
}

/*
int _open(const char *name, int flags, ...)
{
    printf("_open called for '%s' with flags x%x\n", name, flags);
    return -1;
}*/

size_t _write(int fd, const void* data, size_t len)
{
    static char* trace_buf = NULL;
    static int trace_ind = 0;
    static int trace_size = 0;
    size_t done=0;

    if (!trace_buf)
    {
        // load trace buffer settings from remoteproc
        struct fw_rsc_trace rsc_trace;
        rpmsg_get_trace_buf_settings (&rsc_trace);
        trace_buf = (void*)(rsc_trace.da);
        trace_size = rsc_trace.len;
        trace_ind = 0;
    }

    switch (fd)
    {
    case STDOUT_FILENO:
        // if we have a stdio channel via rpmsg, transmit the data (in chunks as big as possible)
        if ((rpmsg_stdio != NULL) && stdio_init)
        {
            /*memcpy(trace_buf+trace_ind, "STDOUT: ", 8);
            trace_ind += 8;*/
            done = 0;
            while (len > done)
            {
                size_t n = len - done;
                n = ((DATA_LEN_MAX-1) < n) ? DATA_LEN_MAX-1 : n;
                rpmsg_send(rpmsg_stdio, (void*)( ((uint8_t*)data) + done), n);
                done += n;
            }
        }
    case STDERR_FILENO:
    case 3:
        // send data on FD3 to the trace buffer only (eg. for debugging rpmsg communication
        // which must not trigger a new rpmsg transmission)
        if (trace_buf)
        {
            // limit length to trace buffer size
            if ((trace_size-trace_ind) > len)
            {
                memcpy(trace_buf+trace_ind, data, len);
                trace_ind += len;
            }
            done = len;
            //Xil_DCacheFlush();  // make new data visible to other core
            Xil_DCacheFlushRange((unsigned int)trace_buf, trace_size);
        }
        break;
    default:
        printf("%s: unknown fd: %d\n", __func__, fd);
    }
    return done;
}


// receives stdio coming from the kernel
void stdio_msg_handler(struct rpmsg_channel* ch, uint8_t* data, uint32_t len)
{
    if (!stdio_init)
    {
        // this is the first message from the kernel, which is used to tell the rpmsg logic
        // the kernel side address. we can igonre it as there is no meaningful data in it
        stdio_init = 1;
        return;
    }
    data[len] = '\0';
    fprintf(stderr, "stdio input: %s", data);
}

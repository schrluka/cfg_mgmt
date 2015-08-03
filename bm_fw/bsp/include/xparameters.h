#include "xparameters_ps.h"

// USER CONFIGURATION (usually done by Xilinx SDK libgen)

// use UART1 as stdout
#define STDOUT_BASEADDRESS      0xE0001000

// CPU runs with 666MHz core clock
#define XPAR_CPU_CORTEXA9_0_CPU_CLK_FREQ_HZ     666000000UL

// we compile to the code for CPU 1 (cpu 0 is running linux)
#define XPAR_CPU_ID	1

/******************************************************************/

/* Definitions for driver SCUGIC */
#define XPAR_XSCUGIC_NUM_INSTANCES 1

/* Definitions for peripheral PS7_SCUGIC_0 */
#define XPAR_PS7_SCUGIC_0_DEVICE_ID 0
#define XPAR_PS7_SCUGIC_0_BASEADDR 0xF8F00100
#define XPAR_PS7_SCUGIC_0_HIGHADDR 0xF8F001FF
#define XPAR_PS7_SCUGIC_0_DIST_BASEADDR 0xF8F01000

/* Canonical definitions for peripheral PS7_SCUGIC_0 */
#define XPAR_SCUGIC_0_DEVICE_ID XPAR_PS7_SCUGIC_0_DEVICE_ID
#define XPAR_SCUGIC_0_CPU_BASEADDR 0xF8F00100
#define XPAR_SCUGIC_0_CPU_HIGHADDR 0xF8F001FF
#define XPAR_SCUGIC_0_DIST_BASEADDR 0xF8F01000


// UART


/* Definitions for driver UARTPS */
#define XPAR_XUARTPS_NUM_INSTANCES 1

/* Definitions for peripheral PS7_UART_1, pointing to UART0 */
#define XPAR_PS7_UART_1_DEVICE_ID           1
#define XPAR_PS7_UART_1_BASEADDR            XPS_UART1_BASEADDR
#define XPAR_PS7_UART_1_HIGHADDR            0xE0001FFF
#define XPAR_PS7_UART_1_UART_CLK_FREQ_HZ    50000000
#define XPAR_PS7_UART_1_HAS_MODEM           0

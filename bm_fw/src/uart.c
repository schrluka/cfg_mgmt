#include "xil_exception.h"
#include "xscugic.h"
#include "xuartps.h"
#include "xparameters.h"
//#include "psInterrupt.h"
#include "xuartps_hw.h"

/* Private variables ---------------------------------------------------------*/

#define TXBUFFERSIZE    64

#define INTC_DEVICE_ID  XPAR_SCUGIC_0_DEVICE_ID


uint8_t RxBuffer[TXBUFFERSIZE];

uint32_t NbrOfDataToRead = 0; //MESSAGE_FROM_AMFT_SIZE;
//__IO uint8_t TxCounter = 0;
//__IO uint16_t RxCounter = 0;

uint32_t RxCounter = 0;
uint32_t RxIndex = 0;
extern uint8_t TxBuffer[TXBUFFERSIZE];
//extern xSemaphoreHandle amftDataReceivedSemaphore;

XUartPs UartPs	;		/* Instance of the UART Device */

static XUartPs_Config Config;


int SetupInterruptSystem(XScuGic *IntcInstancePtr,
				XUartPs *UartInstancePtr,
				u16 UartIntrId);

// register addresses for uart configuration and signal routing
#define SLCR_BASE       0xF8000000

//#define MIO_PIN_10      0x728
//#define MIO_PIN_11      0x72c
//#define MIO_MST_TRI0    0x80c

#define UART_CLK_CTRL   0x154
#define APER_CLK_CTRL   0x12c
#define UART_RST_CTRL   0x218

#define UART1_CPU1X_RST         1
#define CLKACT1                 1
#define UART1_CPU_1XCLKACT      21





/**************************************************************************/
/**
*
* This function is the handler which performs processing to handle data events
* from the device.  It is called from an interrupt context. so the amount of
* processing should be minimal.
*
* This handler provides an example of how to handle data for the device and
* is application specific.
*
* @param	CallBackRef contains a callback reference from the driver,
*		in this case it is the instance pointer for the XUartPs driver.
* @param	Event contains the specific kind of event that has occurred.
* @param	EventData contains the number of bytes sent or received
*
* @return	None.
*
* @note		None.
*
***************************************************************************/
void Handler(void *CallBackRef, u32 Event,unsigned int EventData)
{

	if (Event == XUARTPS_EVENT_RECV_DATA)
    {
		 XUartPs_Recv(&UartPs, RxBuffer,EventData);
    }
}


int UartInit()
{
    //char msg[] = "Hello World\n";

    // reset uart
    uint32_t reg = Xil_In32(SLCR_BASE+UART_RST_CTRL);
    reg |= (1<<UART1_CPU1X_RST);
    Xil_Out32(SLCR_BASE+UART_RST_CTRL, reg);

    // enable uart amba clock
    reg = Xil_In32(SLCR_BASE+APER_CLK_CTRL);
    reg |= (1<<UART1_CPU_1XCLKACT);
    Xil_Out32(SLCR_BASE+APER_CLK_CTRL, reg);

    // enable uart clock
    reg = Xil_In32(SLCR_BASE+UART_CLK_CTRL);
    reg |= (1<<CLKACT1);
    Xil_Out32(SLCR_BASE+UART_CLK_CTRL, reg);

    // release uart from reset
    reg = Xil_In32(SLCR_BASE+UART_RST_CTRL);
    reg &= ~(1<<UART1_CPU1X_RST);
    Xil_Out32(SLCR_BASE+UART_RST_CTRL, reg);

    // configuer IO pins MIO10 and 11 for uart IO
    /* NO config for EMIO required?
    reg = (1<<12);  // pullup
    reg |= (3<<9);  // LVCMOS 3.3
    reg |= (7<<5);  // uart 0
    reg |= (1<<0);  // tri enable
    Xil_Out32(SLCR_BASE+MIO_PIN_10,reg);
    reg &= ~(1<<0);  // tri enable
    Xil_Out32(SLCR_BASE+MIO_PIN_11,reg); */


    UartPs.InputClockHz = XPAR_PS7_UART_1_UART_CLK_FREQ_HZ;

    Config.DeviceId = XPAR_PS7_UART_1_DEVICE_ID;
    Config.BaseAddress = XPAR_PS7_UART_1_BASEADDR;
    Config.InputClockHz = XPAR_PS7_UART_1_UART_CLK_FREQ_HZ;
    Config.ModemPinsConnected = 0;

    int ret = XUartPs_CfgInitialize(&UartPs, &Config, Config.BaseAddress);

    if (ret != XST_SUCCESS)
        return ret;

    XUartPs_SetBaudRate(&UartPs, 115200);

    XUartPs_SetOperMode(&UartPs, XUARTPS_OPER_MODE_NORMAL);

    XUartPs_EnableUart(&UartPs);

    if (UartPs.IsReady != XIL_COMPONENT_IS_READY)
        return 1337;

    //XUartPs_Send(&UartPs, (void*)msg, sizeof(msg));

    return XST_SUCCESS;
 }


/**
*
* This function does a minimal test on the UartPS device and driver as a
* design example. The purpose of this function is to illustrate
* how to use the XUartPs driver.
*
* This function sends data and expects to receive the same data through the
* device using the local loopback mode.
*
* This function uses interrupt mode of the device.
*
* @param	IntcInstPtr is a pointer to the instance of the Scu Gic driver.
* @param	UartInstPtr is a pointer to the instance of the UART driver
*		which is going to be connected to the interrupt controller.
* @param	DeviceId is the device Id of the UART device and is typically
*		XPAR_<CANPS_instance>_DEVICE_ID value from xparameters.h.
* @param	UartIntrId is the interrupt Id and is typically
*		XPAR_<UARTPS_instance>_INTR value from xparameters.h.
*
* @return	XST_SUCCESS if successful, otherwise XST_FAILURE.
*
* @note
*
* This function contains an infinite loop such that if interrupts are not
* working it may never return.
*
**************************************************************************/

int UartPsIntrInit(XScuGic *IntcInstPtr, XUartPs *UartInstPtr,
			u16 DeviceId, u16 UartIntrId)
{
	int Status;
	XUartPs_Config *Config;
	u32 IntrMask;


	   Config = XUartPs_LookupConfig(DeviceId);
		if (NULL == Config) {
		//#ifdef print_status
		//	printf("UART Lookup Failed \n\r");
		//#endif
			return XST_FAILURE;
		}

		Status = XUartPs_CfgInitialize(UartInstPtr, Config, Config->BaseAddress);
		if (Status != XST_SUCCESS) {
		//#ifdef print_status
		//	printf("UART Configuration Failed \n\r");
		//#endif
			return XST_FAILURE;
		}
	/*
		 * Connect the UART to the interrupt subsystem such that interrupts
		 * can occur. This function is application specific.
		 */
		Status = SetupInterruptSystem(IntcInstPtr, UartInstPtr, UartIntrId);
		if (Status != XST_SUCCESS) {
		//#ifdef print_status
		//	printf("Setup Interrupt System failed \n\r");
		//#endif

			return XST_FAILURE;
		}


	/*
	 * Initialize the UART driver so that it's ready to use
	 * Look up the configuration in the config table, then initialize it.
	 */


	/*
	 * Check hardware build
	 */
	//Status = XUartPs_SelfTest(UartInstPtr);
	//if (Status != XST_SUCCESS) {
	//	xil_printf("SelfTest Failed \n\r");
	//	return XST_FAILURE;
	//}



	/*
	 * Setup the handlers for the UART that will be called from the
	 * interrupt context when data has been sent and received, specify
	 * a pointer to the UART driver instance as the callback reference
	 * so the handlers are able to access the instance data
	 */
	XUartPs_SetHandler(UartInstPtr, (XUartPs_Handler) Handler, (void *)UartInstPtr);

	/*
	 * Enable the interrupt of the UART so interrupts will occur, setup
	 * a local loopback so data that is sent will be received.
	 */
	IntrMask =  XUARTPS_IXR_RXFULL | XUARTPS_IXR_RXOVR;
	XUartPs_SetInterruptMask(UartInstPtr, IntrMask);

	XUartPs_SetOperMode(UartInstPtr, XUARTPS_OPER_MODE_NORMAL);

	/*
	 * Set the receiver timeout. If it is not set, and the last few bytes
	 * of data do not trigger the over-water or full interrupt, the bytes
	 * will not be received. By default it is disabled.
	 *
	 * The setting of 2 will timeout after 2 x 4 = 8 character times.
	 * Increase the time out value if baud rate is high, decrease it if
	 * baud rate is low.
	 */
	  XUartPs_SetRecvTimeout(UartInstPtr, 2);

    //#ifdef print_status
	//	printf("Exit from UART Interrupt Setup \n\r");
	//#endif



	return XST_SUCCESS;
}

/*****************************************************************************/
/**
*
* This function sets up the interrupt system so interrupts can occur for the
* Uart. This function is application-specific. The user should modify this
* function to fit the application.
*
* @param	IntcInstancePtr is a pointer to the instance of the INTC.
* @param	UartInstancePtr contains a pointer to the instance of the UART
*		driver which is going to be connected to the interrupt
*		controller.
* @param	UartIntrId is the interrupt Id and is typically
*		XPAR_<UARTPS_instance>_INTR value from xparameters.h.
*
* @return	XST_SUCCESS if successful, otherwise XST_FAILURE.
*
* @note		None.
*
****************************************************************************/
int SetupInterruptSystem(XScuGic *IntcInstancePtr,
				XUartPs *UartInstancePtr,
				u16 UartIntrId)
{
	int Status;

	XScuGic_Config *IntcConfig; /* Config for interrupt controller */

	Xil_ExceptionInit();
	/*
	 * Initialize the interrupt controller driver
	 */
	IntcConfig = XScuGic_LookupConfig(INTC_DEVICE_ID);
	if (NULL == IntcConfig) {
	//#ifdef print_status
	//	printf("Interrupt Controller Lookup Failed \n\r");
	//#endif
		return XST_FAILURE;
	}

	Status = XScuGic_CfgInitialize(IntcInstancePtr, IntcConfig,
					IntcConfig->CpuBaseAddress);
	if (Status != XST_SUCCESS) {
	//#ifdef print_status
	//	printf("Interrupt Controller Configuartion Failed\n\r");
	//#endif
		return XST_FAILURE;
	}



	/*
	 * Connect the interrupt controller interrupt handler to the
	 * hardware interrupt handling logic in the processor.
	 */
	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,
				(Xil_ExceptionHandler) XScuGic_InterruptHandler,
				(void *)IntcInstancePtr);


	/*
	 * Connect a device driver handler that will be called when an
	 * interrupt for the device occurs, the device driver handler
	 * performs the specific interrupt processing for the device
	 */
	Status = XScuGic_Connect(IntcInstancePtr, UartIntrId,
				  (Xil_ExceptionHandler) XUartPs_InterruptHandler,
				  (void *)UartInstancePtr );    //

	if (Status != XST_SUCCESS) {
	//#ifdef print_status
		//printf("Interrupt Controller Connect Failed\n\r");
	//#endif
		return XST_FAILURE;
	}
	//XScuGic_SetPriorityTriggerType(IntcInstancePtr, UartIntrId,
	//					 0x04, SFI);
	/*
	 * Enable the interrupt for the device
	 */
	XScuGic_Enable(IntcInstancePtr, UartIntrId);
	XScuGic_SetPriorityTriggerType(IntcInstancePtr,UartIntrId ,0xa0, 3);

	/*
	 * Enable interrupts
	 */
	 Xil_ExceptionEnable();


	return XST_SUCCESS;
}


/******************* (C) COPYRIGHT 2013 Muhammad Fayyaz *****END OF FILE****/

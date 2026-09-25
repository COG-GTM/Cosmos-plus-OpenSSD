/* Implementations for the Xilinx BSP stand-ins (HOST_TEST build only). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "xil_printf.h"
#include "xtime_l.h"
#include "xscugic.h"

static char inbyteValue;
static int verboseState = -1;

static int Verbose(void)
{
	if (verboseState < 0)
		verboseState = getenv("FTL_TEST_VERBOSE") != NULL;
	return verboseState;
}

void xil_printf(const char *format, ...)
{
	va_list args;
	if (!Verbose())
		return;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
}

void print(const char *str)
{
	if (Verbose())
		fputs(str, stdout);
}

void outbyte(char c)
{
	if (Verbose())
		putchar(c);
}

char inbyte(void)
{
	return inbyteValue;
}

void XilStubSetInbyte(char c)
{
	inbyteValue = c;
}

void XTime_SetTime(XTime Xtime_Global)
{
	(void)Xtime_Global;
}

void XTime_GetTime(XTime *Xtime_Global)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	*Xtime_Global = (XTime)ts.tv_sec * COUNTS_PER_SECOND + (XTime)ts.tv_nsec * COUNTS_PER_SECOND / 1000000000u;
}

static XScuGic_Config gicConfig = {0, XPAR_SCUGIC_0_CPU_BASEADDR, XPAR_SCUGIC_0_DIST_BASEADDR, {{0, 0}}};

XScuGic_Config *XScuGic_LookupConfig(u16 DeviceId)
{
	(void)DeviceId;
	return &gicConfig;
}

s32 XScuGic_CfgInitialize(XScuGic *InstancePtr, XScuGic_Config *ConfigPtr, u32 EffectiveAddr)
{
	(void)EffectiveAddr;
	InstancePtr->Config = ConfigPtr;
	InstancePtr->IsReady = XIL_COMPONENT_IS_READY;
	InstancePtr->UnhandledInterrupts = 0;
	return XST_SUCCESS;
}

s32 XScuGic_Connect(XScuGic *InstancePtr, u32 Int_Id, Xil_InterruptHandler Handler, void *CallBackRef)
{
	if (Int_Id >= XSCUGIC_MAX_NUM_INTR_INPUTS)
		return XST_FAILURE;
	InstancePtr->Config->HandlerTable[Int_Id].Handler = Handler;
	InstancePtr->Config->HandlerTable[Int_Id].CallBackRef = CallBackRef;
	return XST_SUCCESS;
}

void XScuGic_Disconnect(XScuGic *InstancePtr, u32 Int_Id)
{
	if (Int_Id < XSCUGIC_MAX_NUM_INTR_INPUTS)
	{
		InstancePtr->Config->HandlerTable[Int_Id].Handler = 0;
		InstancePtr->Config->HandlerTable[Int_Id].CallBackRef = 0;
	}
}

void XScuGic_Enable(XScuGic *InstancePtr, u32 Int_Id) { (void)InstancePtr; (void)Int_Id; }
void XScuGic_Disable(XScuGic *InstancePtr, u32 Int_Id) { (void)InstancePtr; (void)Int_Id; }
void XScuGic_InterruptHandler(XScuGic *InstancePtr) { (void)InstancePtr; }

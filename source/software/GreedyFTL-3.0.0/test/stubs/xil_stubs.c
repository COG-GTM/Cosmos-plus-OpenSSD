/*
 * Host implementations of the Xilinx BSP entry points used by the firmware.
 *
 * xil_printf output is suppressed unless GREEDYFTL_TEST_VERBOSE is set in the
 * environment, because the FTL init path prints thousands of lines. inbyte() returns a
 * value tests can program (the firmware reads it once at boot to decide whether to
 * rebuild the bad block table).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "xil_printf.h"
#include "xtime_l.h"
#include "xscugic.h"
#include "xil_stubs.h"

static int verboseInitialised;
static int verbose;
static char nextInbyte;

static int isVerbose(void)
{
	if (!verboseInitialised)
	{
		verbose = getenv("GREEDYFTL_TEST_VERBOSE") != NULL;
		verboseInitialised = 1;
	}
	return verbose;
}

void xil_printf(const char *format, ...)
{
	va_list args;

	if (!isVerbose())
		return;

	va_start(args, format);
	vprintf(format, args);
	va_end(args);
}

void print(const char *str)
{
	if (isVerbose())
		fputs(str, stdout);
}

void outbyte(char c)
{
	if (isVerbose())
		putchar(c);
}

char inbyte(void)
{
	char c = nextInbyte;
	nextInbyte = 0;
	return c;
}

void xil_stub_set_next_inbyte(char c)
{
	nextInbyte = c;
}

void XTime_GetTime(XTime *Xtime_Global)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	*Xtime_Global = (XTime)ts.tv_sec * 1000000000ull + (XTime)ts.tv_nsec;
}

void XTime_SetTime(XTime Xtime_Global)
{
	(void)Xtime_Global;
}

static XScuGic_Config gicConfig;

XScuGic_Config *XScuGic_LookupConfig(u16 DeviceId)
{
	gicConfig.DeviceId = DeviceId;
	return &gicConfig;
}

s32 XScuGic_CfgInitialize(XScuGic *InstancePtr, XScuGic_Config *ConfigPtr, u32 EffectiveAddr)
{
	InstancePtr->Config = ConfigPtr;
	InstancePtr->Config->CpuBaseAddress = EffectiveAddr;
	InstancePtr->IsReady = 1;
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
		InstancePtr->Config->HandlerTable[Int_Id].Handler = NULL;
		InstancePtr->Config->HandlerTable[Int_Id].CallBackRef = NULL;
	}
}

void XScuGic_Enable(XScuGic *InstancePtr, u32 Int_Id) { (void)InstancePtr; (void)Int_Id; }
void XScuGic_Disable(XScuGic *InstancePtr, u32 Int_Id) { (void)InstancePtr; (void)Int_Id; }
void XScuGic_SetPriorityTriggerType(XScuGic *InstancePtr, u32 Int_Id, u8 Priority, u8 Trigger)
{
	(void)InstancePtr; (void)Int_Id; (void)Priority; (void)Trigger;
}
void XScuGic_InterruptHandler(XScuGic *InstancePtr) { (void)InstancePtr; }

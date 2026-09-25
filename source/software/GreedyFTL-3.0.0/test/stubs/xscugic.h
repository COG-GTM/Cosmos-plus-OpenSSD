/* Host stand-in for the Xilinx BSP xscugic.h (HOST_TEST build only).
 * Interrupt controller calls are no-ops that report success. */
#ifndef XSCUGIC_H
#define XSCUGIC_H

#include "xil_types.h"
#include "xscugic_hw.h"

#ifndef XST_SUCCESS
#define XST_SUCCESS 0L
#define XST_FAILURE 1L
#endif

typedef void (*Xil_InterruptHandler)(void *CallBackRef);

typedef struct
{
	Xil_InterruptHandler Handler;
	void *CallBackRef;
} XScuGic_VectorTableEntry;

typedef struct
{
	u16 DeviceId;
	u32 CpuBaseAddress;
	u32 DistBaseAddress;
	XScuGic_VectorTableEntry HandlerTable[XSCUGIC_MAX_NUM_INTR_INPUTS];
} XScuGic_Config;

typedef struct
{
	XScuGic_Config *Config;
	u32 IsReady;
	u32 UnhandledInterrupts;
} XScuGic;

XScuGic_Config *XScuGic_LookupConfig(u16 DeviceId);
s32 XScuGic_CfgInitialize(XScuGic *InstancePtr, XScuGic_Config *ConfigPtr, u32 EffectiveAddr);
s32 XScuGic_Connect(XScuGic *InstancePtr, u32 Int_Id, Xil_InterruptHandler Handler, void *CallBackRef);
void XScuGic_Disconnect(XScuGic *InstancePtr, u32 Int_Id);
void XScuGic_Enable(XScuGic *InstancePtr, u32 Int_Id);
void XScuGic_Disable(XScuGic *InstancePtr, u32 Int_Id);
void XScuGic_InterruptHandler(XScuGic *InstancePtr);

#endif

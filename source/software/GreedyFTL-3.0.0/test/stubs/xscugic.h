/* Host-native stub of the Xilinx standalone BSP xscugic.h (no-ops). */
#ifndef XSCUGIC_H
#define XSCUGIC_H

#include "xil_types.h"
#include "xscugic_hw.h"

typedef void (*Xil_InterruptHandler)(void *data);

typedef struct {
	u16 DeviceId;
	u32 CpuBaseAddress;
	u32 DistBaseAddress;
} XScuGic_Config;

typedef struct {
	XScuGic_Config Config;
	u32 IsReady;
} XScuGic;

static inline XScuGic_Config *XScuGic_LookupConfig(u16 deviceId)
{
	static XScuGic_Config config;
	config.DeviceId = deviceId;
	return &config;
}

static inline s32 XScuGic_CfgInitialize(XScuGic *inst, XScuGic_Config *config, u32 effectiveAddr)
{
	(void)effectiveAddr;
	inst->Config = *config;
	inst->IsReady = XIL_COMPONENT_IS_READY;
	return XST_SUCCESS;
}

static inline s32 XScuGic_Connect(XScuGic *inst, u32 intId, Xil_InterruptHandler handler, void *ref)
{
	(void)inst;
	(void)intId;
	(void)handler;
	(void)ref;
	return XST_SUCCESS;
}

static inline void XScuGic_Enable(XScuGic *inst, u32 intId)
{
	(void)inst;
	(void)intId;
}

static inline void XScuGic_InterruptHandler(void *inst) { (void)inst; }

#endif /* XSCUGIC_H */

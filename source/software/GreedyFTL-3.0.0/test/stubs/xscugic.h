#ifndef XSCUGIC_H
#define XSCUGIC_H
/* Host stub: generic interrupt controller with no-op API. */
#include "xil_types.h"
#include "xil_exception.h"
typedef struct { u16 DeviceId; u32 CpuBaseAddress; u32 DistBaseAddress; } XScuGic_Config;
typedef struct { XScuGic_Config *Config; u32 IsReady; } XScuGic;
static XScuGic_Config XScuGic_ConfigTable_Host = {0, 0xF8F00100, 0xF8F01000};
static inline XScuGic_Config *XScuGic_LookupConfig(u16 id) { (void)id; return &XScuGic_ConfigTable_Host; }
static inline s32 XScuGic_CfgInitialize(XScuGic *g, XScuGic_Config *c, u32 a) { g->Config = c; g->IsReady = 1; (void)a; return XST_SUCCESS; }
static inline s32 XScuGic_Connect(XScuGic *g, u32 id, Xil_ExceptionHandler h, void *d) { (void)g; (void)id; (void)h; (void)d; return XST_SUCCESS; }
static inline void XScuGic_Enable(XScuGic *g, u32 id) { (void)g; (void)id; }
static inline void XScuGic_Disable(XScuGic *g, u32 id) { (void)g; (void)id; }
static inline void XScuGic_InterruptHandler(XScuGic *g) { (void)g; }
#endif

#ifndef XSCUGIC_H
#define XSCUGIC_H
#include "xil_types.h"
#include "xil_exception.h"
typedef struct { u32 DeviceId; u32 CpuBaseAddress; u32 DistBaseAddress; } XScuGic_Config;
typedef struct { XScuGic_Config *Config; u32 IsReady; } XScuGic;
static inline XScuGic_Config *XScuGic_LookupConfig(u16 id) { static XScuGic_Config c; (void)id; return &c; }
static inline s32 XScuGic_CfgInitialize(XScuGic *g, XScuGic_Config *c, u32 base) { (void)g; (void)c; (void)base; return 0; }
static inline s32 XScuGic_Connect(XScuGic *g, u32 id, Xil_ExceptionHandler h, void *d) { (void)g; (void)id; (void)h; (void)d; return 0; }
static inline void XScuGic_Enable(XScuGic *g, u32 id) { (void)g; (void)id; }
static inline void XScuGic_InterruptHandler(XScuGic *g) { (void)g; }
#endif

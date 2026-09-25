/* Host-native stub of the Xilinx standalone BSP xil_types.h. */
#ifndef XIL_TYPES_H
#define XIL_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef char char8;
typedef uintptr_t UINTPTR;
typedef intptr_t INTPTR;

#ifndef TRUE
#define TRUE 1U
#endif
#ifndef FALSE
#define FALSE 0U
#endif

#define XIL_COMPONENT_IS_READY 0x11111111U
#define XST_SUCCESS 0L
#define XST_FAILURE 1L

#endif /* XIL_TYPES_H */

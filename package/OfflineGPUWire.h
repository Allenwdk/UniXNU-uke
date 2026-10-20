// SPDX-License-Identifier: MIT
// Private offline QEMU instrument protocol, not a Metal or hardware ABI.
#ifndef SHENG_OFFLINE_GPU_WIRE_H
#define SHENG_OFFLINE_GPU_WIRE_H
#include <stdint.h>
// All fields are little endian. Both supported participants are ARM64 LE.
enum { GPU_WIRE_MAGIC=0x5550474e, GPU_WIRE_VERSION=1,
       GPU_WIRE_CLEAR=1, GPU_WIRE_ORIGINAL_DRAW=2, GPU_WIRE_RESPONSE=0x8000,
       GPU_WIRE_R32_FLOAT=55, GPU_WIRE_WIDTH=64, GPU_WIRE_HEIGHT=64,
       GPU_WIRE_PIXEL_BYTES=64*64*4 };
typedef struct { uint32_t magic;uint16_t version,operation;uint32_t bytes,request; } GPUWireHeader;
typedef struct { uint32_t width,height,format,status;uint64_t start_ns,end_ns; } GPUWireResult;
#if defined(__cplusplus)
static_assert(sizeof(GPUWireHeader)==16 && sizeof(GPUWireResult)==32,"Offline wire layout");
#else
_Static_assert(sizeof(GPUWireHeader)==16 && sizeof(GPUWireResult)==32,"Offline wire layout");
#endif
#endif

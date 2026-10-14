#ifndef KUBRIDGE_MODERN_PATCH_H
#define KUBRIDGE_MODERN_PATCH_H

#include <stddef.h>
#include <stdint.h>
#include <psp2/types.h>

#define KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT           3
#define KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT               4
#define KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION    1

typedef struct KuKernelExceptionContext {
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12;
    uint32_t sp, lr, pc, SPSR;
    uint32_t exceptionType;
    uint32_t FSR;
    uint32_t FAR;
} KuKernelExceptionContext;

typedef void (*KuKernelExceptionHandler)(KuKernelExceptionContext *ctx);

struct KuKernelExceptionHandlerOpt {
    uint32_t size;
};

typedef struct SceKernelAddrPair {
    uint32_t addr;
    uint32_t size;
} SceKernelAddrPair;

int kuKernelRegisterExceptionHandler(uint32_t type, KuKernelExceptionHandler handler, KuKernelExceptionHandler *old_handler, struct KuKernelExceptionHandlerOpt *opt);
int kuKernelReleaseExceptionHandler(uint32_t type);
int kuKernelCpuUnrestrictedMemcpy(void *dst, const void *src, size_t size);
int kuKernelFlushCaches(const void *addr, size_t size);

#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RW 0x0c20d060

SceUID sceKernelAllocMemBlock(const char *name, uint32_t type, int size, void *opt);
int sceKernelGetMemBlockBase(SceUID uid, void **base);
int sceKernelFreeMemBlock(SceUID uid);

#endif

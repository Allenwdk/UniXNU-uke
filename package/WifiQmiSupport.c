/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Private allocation/log hooks for the original Linux QMI codec. Its buffers
 * are ordinary kernel memory, never mapped device firmware memory. */
#include <IOKit/IOLib.h>
#include <sys/systm.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

union Allocation { size_t bytes; uint64_t alignment[2]; };
void *uke_wlfw_alloc(size_t bytes, int zero)
{
    if (bytes > SIZE_MAX - sizeof(union Allocation)) return NULL;
    union Allocation *allocation = IOMallocData(sizeof *allocation + bytes);
    if (!allocation) return NULL;
    allocation->bytes = bytes;
    void *payload = allocation + 1;
    if (zero) bzero(payload, bytes);
    return payload;
}
void uke_wlfw_free(void *payload)
{
    if (!payload) return;
    union Allocation *allocation = (union Allocation *)payload - 1;
    IOFreeData(allocation, sizeof *allocation + allocation->bytes);
}
void uke_wlfw_log(int level, const char *format, ...)
{
    (void)level;
    char line[256];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    printf("UKE WIFI QMI: %s", line);
}

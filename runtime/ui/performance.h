#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *cpu_name, *gpu_name;
    unsigned vram_mib, vram_budget_mib;
    int cpu_usage, gpu_usage;  // -1 while sampling or when a counter is unavailable
} VulkanOverlayStats;

VulkanOverlayStats vk_overlay_stats(void);
void vk_overlay_memory(uint64_t used, uint64_t budget);

#ifdef __cplusplus
}
#endif

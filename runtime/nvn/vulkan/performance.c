#include <stdatomic.h>
#include "../../ui/performance.h"
#include "vk.h"

static _Atomic unsigned vram_mib, vram_budget_mib;

void vk_overlay_memory(uint64_t used, uint64_t budget) {
    atomic_store(&vram_mib, (unsigned)(used >> 20));
    atomic_store(&vram_budget_mib, (unsigned)(budget >> 20));
}

#ifdef _WIN32
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static char cpu_name[128];
static LUID adapter;
static bool adapter_valid, started;
static _Atomic int cpu_usage = -1, gpu_usage = -1;
static _Atomic uint64_t last_visible;

static uint64_t file_time(FILETIME t) {
    return (uint64_t)t.dwHighDateTime << 32 | t.dwLowDateTime;
}

static int gpu_load(PDH_HCOUNTER counter) {
    DWORD bytes = 0, count = 0;
    if (!counter || PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, NULL) != PDH_MORE_DATA)
        return -1;
    PDH_FMT_COUNTERVALUE_ITEM_W *items = malloc(bytes);
    if (!items) return -1;
    double engines[8][64] = {{0}};
    int result = -1;
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, items) == ERROR_SUCCESS) {
        for (DWORD i = 0; i < count; i++) {
            unsigned long high, low;
            unsigned node, engine;
            if (swscanf(items[i].szName, L"pid_%*u_luid_0x%lx_0x%lx_phys_%u_eng_%u", &high, &low, &node, &engine) != 4 ||
                high != (unsigned long)adapter.HighPart || low != adapter.LowPart || node >= 8 || engine >= 64)
                continue;
            PDH_FMT_COUNTERVALUE value = items[i].FmtValue;
            if (value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA) continue;
            double sum = (engines[node][engine] += value.doubleValue);
            if (sum > result) result = sum >= 100 ? 100 : (int)(sum + 0.5);
        }
    }
    free(items);
    return result;
}

static int sample_hardware(void *unused) {
    (void)unused;
    PDH_HQUERY query = NULL;
    PDH_HCOUNTER counter = NULL;
    if (adapter_valid && PdhOpenQueryW(NULL, 0, &query) == ERROR_SUCCESS)
        PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter);
    uint64_t old_idle = 0, old_total = 0;
    for (;;) {
        if (SDL_GetTicks() - atomic_load(&last_visible) < 3000) {
            FILETIME idle, kernel, user;
            if (GetSystemTimes(&idle, &kernel, &user)) {
                uint64_t idle_ns = file_time(idle), total = file_time(kernel) + file_time(user);
                if (old_total && total > old_total) {
                    uint64_t elapsed = total - old_total, idle_delta = idle_ns - old_idle;
                    atomic_store(&cpu_usage, idle_delta >= elapsed ? 0 : (int)(100 * (elapsed - idle_delta) / elapsed));
                }
                old_idle = idle_ns;
                old_total = total;
            }
            if (counter && PdhCollectQueryData(query) == ERROR_SUCCESS) atomic_store(&gpu_usage, gpu_load(counter));
        } else {
            old_total = 0;
        }
        SDL_Delay(1000);
    }
}

static void start_hardware_sampler(void) {
    DWORD size = sizeof cpu_name;
    RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
                 RRF_RT_REG_SZ | RRF_ZEROONFAILURE, NULL, cpu_name, &size);
    VkPhysicalDeviceIDProperties id = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &props);
    adapter_valid = id.deviceLUIDValid;
    if (adapter_valid) memcpy(&adapter, id.deviceLUID, sizeof adapter);
    SDL_Thread *thread = SDL_CreateThread(sample_hardware, "Hardware overlay", NULL);
    if (thread) SDL_DetachThread(thread);
}
#endif

VulkanOverlayStats vk_overlay_stats(void) {
    VulkanOverlayStats s = {
        .gpu_name = g_vk.props.deviceName,
        .vram_mib = atomic_load(&vram_mib), .vram_budget_mib = atomic_load(&vram_budget_mib),
        .cpu_usage = -1, .gpu_usage = -1,
    };
#ifdef _WIN32
    atomic_store(&last_visible, SDL_GetTicks());
    if (!started) { started = true; start_hardware_sampler(); }
    s.cpu_name = cpu_name;
    s.cpu_usage = atomic_load(&cpu_usage);
    s.gpu_usage = atomic_load(&gpu_usage);
#endif
    return s;
}

#pragma once
// ── Lightweight per-core CPU & internal heap utilization telemetry ────────────
// Samples FreeRTOS IDLE task runtime counters per core every 2 seconds in O(1)
// time (no uxTaskGetSystemState heap allocation, no scheduler suspension).
// Requires CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y and U64 esp_timer clock.

#include <Arduino.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdint.h>

struct SysResourceSnapshot {
    bool     cpuValid   = false;
    float    cpu0Pct    = 0.0f;
    float    cpu1Pct    = 0.0f;
    float    cpuAvgPct  = 0.0f;
    uint32_t heapFree   = 0;
    uint32_t heapMin    = 0;
    uint32_t heapTotal  = 0;
    float    memUsedPct = 0.0f;
    float    memPeakPct = 0.0f;
};

static inline float sysStatsComputeUsagePct(uint64_t deltaIdleUs,
                                            uint64_t deltaTotalUs,
                                            bool stuckInIdle) {
    if (deltaTotalUs == 0) return 0.0f;
    if (deltaIdleUs == 0 && stuckInIdle) return 0.0f;
    if (deltaIdleUs >= deltaTotalUs) return 0.0f;
    const uint64_t busyUs = deltaTotalUs - deltaIdleUs;
    const uint64_t tenths = (busyUs * 1000ULL + deltaTotalUs / 2ULL) / deltaTotalUs;
    return static_cast<float>(tenths > 1000ULL ? 1000ULL : tenths) * 0.1f;
}

static inline float sysStatsComputeMemPct(uint32_t freeBytes, uint32_t totalBytes) {
    if (totalBytes == 0) return 0.0f;
    const uint32_t usedBytes = (freeBytes >= totalBytes) ? 0u : (totalBytes - freeBytes);
    const uint64_t tenths =
        (static_cast<uint64_t>(usedBytes) * 1000ULL + static_cast<uint64_t>(totalBytes) / 2ULL) /
        static_cast<uint64_t>(totalBytes);
    return static_cast<float>(tenths > 1000ULL ? 1000ULL : tenths) * 0.1f;
}

namespace sys_stats_detail {

static portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;
static bool     gCpuValid     = false;
static float    gCpu0Pct      = 0.0f;
static float    gCpu1Pct      = 0.0f;
static bool     gInitialized  = false;
static uint64_t gPrevTimeUs   = 0;
static uint64_t gPrevIdle0Us  = 0;
static uint64_t gPrevIdle1Us  = 0;

static constexpr uint64_t kSampleWindowUs = 2000000ULL;  // 2 s sliding window

}  // namespace sys_stats_detail

static inline void serviceSysStats() {
#if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    const uint64_t nowUs = static_cast<uint64_t>(esp_timer_get_time());
    if (!sys_stats_detail::gInitialized) {
        sys_stats_detail::gPrevTimeUs = nowUs;
        sys_stats_detail::gPrevIdle0Us = static_cast<uint64_t>(ulTaskGetIdleRunTimeCounterForCore(0));
#if portNUM_PROCESSORS > 1
        sys_stats_detail::gPrevIdle1Us = static_cast<uint64_t>(ulTaskGetIdleRunTimeCounterForCore(1));
#endif
        sys_stats_detail::gInitialized = true;
        return;
    }

    if (nowUs <= sys_stats_detail::gPrevTimeUs) return;
    const uint64_t deltaTotalUs = nowUs - sys_stats_detail::gPrevTimeUs;
    if (deltaTotalUs < sys_stats_detail::kSampleWindowUs) return;

    const uint64_t curIdle0 = static_cast<uint64_t>(ulTaskGetIdleRunTimeCounterForCore(0));
    const uint64_t dIdle0 = (curIdle0 >= sys_stats_detail::gPrevIdle0Us)
        ? (curIdle0 - sys_stats_detail::gPrevIdle0Us) : 0ULL;
    const bool idle0Current =
        (xTaskGetCurrentTaskHandleForCore(0) == xTaskGetIdleTaskHandleForCore(0));
    const float pct0 = sysStatsComputeUsagePct(dIdle0, deltaTotalUs, idle0Current);

#if portNUM_PROCESSORS > 1
    const uint64_t curIdle1 = static_cast<uint64_t>(ulTaskGetIdleRunTimeCounterForCore(1));
    const uint64_t dIdle1 = (curIdle1 >= sys_stats_detail::gPrevIdle1Us)
        ? (curIdle1 - sys_stats_detail::gPrevIdle1Us) : 0ULL;
    const bool idle1Current =
        (xTaskGetCurrentTaskHandleForCore(1) == xTaskGetIdleTaskHandleForCore(1));
    const float pct1 = sysStatsComputeUsagePct(dIdle1, deltaTotalUs, idle1Current);
    sys_stats_detail::gPrevIdle1Us = curIdle1;
#else
    const float pct1 = 0.0f;
#endif

    sys_stats_detail::gPrevTimeUs = nowUs;
    sys_stats_detail::gPrevIdle0Us = curIdle0;

    portENTER_CRITICAL(&sys_stats_detail::gMux);
    sys_stats_detail::gCpu0Pct  = pct0;
    sys_stats_detail::gCpu1Pct  = pct1;
    sys_stats_detail::gCpuValid = true;
    portEXIT_CRITICAL(&sys_stats_detail::gMux);
#endif
}

static inline SysResourceSnapshot getSysResourceSnapshot() {
    SysResourceSnapshot snap;
    portENTER_CRITICAL(&sys_stats_detail::gMux);
    snap.cpuValid = sys_stats_detail::gCpuValid;
    snap.cpu0Pct  = sys_stats_detail::gCpu0Pct;
    snap.cpu1Pct  = sys_stats_detail::gCpu1Pct;
    portEXIT_CRITICAL(&sys_stats_detail::gMux);
#if portNUM_PROCESSORS > 1
    snap.cpuAvgPct = (snap.cpu0Pct + snap.cpu1Pct) * 0.5f;
#else
    snap.cpuAvgPct = snap.cpu0Pct;
#endif
    snap.heapFree   = ESP.getFreeHeap();
    snap.heapMin    = ESP.getMinFreeHeap();
    snap.heapTotal  = ESP.getHeapSize();
    snap.memUsedPct = sysStatsComputeMemPct(snap.heapFree, snap.heapTotal);
    snap.memPeakPct = sysStatsComputeMemPct(snap.heapMin, snap.heapTotal);
    return snap;
}

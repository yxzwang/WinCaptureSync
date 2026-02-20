#include "time/time_utils.h"

#include <Windows.h>

namespace wcs::time {

namespace {

constexpr int64_t kWinToUnixEpoch100ns = 116444736000000000LL;
constexpr int64_t kNsPer100ns = 100LL;
constexpr int64_t kNsPerSecond = 1000000000LL;

int64_t FileTimeToUnixEpochNs(const FILETIME& ft) {
    ULARGE_INTEGER uli{};
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    const int64_t win_100ns = static_cast<int64_t>(uli.QuadPart);
    return (win_100ns - kWinToUnixEpoch100ns) * kNsPer100ns;
}

}  // namespace

int64_t QpcClock::Frequency() {
    LARGE_INTEGER freq{};
    if (!QueryPerformanceFrequency(&freq)) {
        return 0;
    }
    return freq.QuadPart;
}

int64_t QpcClock::NowTicks() {
    LARGE_INTEGER ticks{};
    if (!QueryPerformanceCounter(&ticks)) {
        return 0;
    }
    return ticks.QuadPart;
}

int64_t QpcClock::NowUtcEpochNs() {
    FILETIME ft{};
    GetSystemTimePreciseAsFileTime(&ft);
    return FileTimeToUnixEpochNs(ft);
}

UtcAnchor QpcClock::SampleUtcAnchor() {
    const int64_t freq = Frequency();
    LARGE_INTEGER before{};
    LARGE_INTEGER after{};
    QueryPerformanceCounter(&before);
    FILETIME ft{};
    GetSystemTimePreciseAsFileTime(&ft);
    QueryPerformanceCounter(&after);

    UtcAnchor anchor{};
    anchor.qpc_freq = freq;
    anchor.qpc_ticks = (before.QuadPart + after.QuadPart) / 2;
    anchor.utc_epoch_ns = FileTimeToUnixEpochNs(ft);
    return anchor;
}

int64_t QpcClock::QpcToUtcEpochNs(const int64_t qpc_ticks, const UtcAnchor& anchor) {
    if (anchor.qpc_freq <= 0) {
        return 0;
    }
    const int64_t delta = qpc_ticks - anchor.qpc_ticks;
    const long double ns =
        static_cast<long double>(anchor.utc_epoch_ns) +
        (static_cast<long double>(delta) * static_cast<long double>(kNsPerSecond) /
         static_cast<long double>(anchor.qpc_freq));
    return static_cast<int64_t>(ns);
}

double QpcClock::QpcDeltaToSeconds(const int64_t delta_ticks, const int64_t qpc_freq) {
    if (qpc_freq <= 0) {
        return 0.0;
    }
    return static_cast<double>(delta_ticks) / static_cast<double>(qpc_freq);
}

}  // namespace wcs::time

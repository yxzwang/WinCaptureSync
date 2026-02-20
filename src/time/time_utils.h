#pragma once

#include <cstdint>

namespace wcs::time {

struct UtcAnchor {
    int64_t qpc_ticks = 0;
    int64_t utc_epoch_ns = 0;
    int64_t qpc_freq = 0;
};

class QpcClock {
public:
    static int64_t Frequency();
    static int64_t NowTicks();
    static int64_t NowUtcEpochNs();
    static UtcAnchor SampleUtcAnchor();
    static int64_t QpcToUtcEpochNs(int64_t qpc_ticks, const UtcAnchor& anchor);
    static double QpcDeltaToSeconds(int64_t delta_ticks, int64_t qpc_freq);
};

}  // namespace wcs::time

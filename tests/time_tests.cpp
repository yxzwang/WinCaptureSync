#include <iostream>

#include "time/time_utils.h"

int main() {
    using wcs::time::QpcClock;
    const int64_t freq = QpcClock::Frequency();
    if (freq <= 0) {
        std::cerr << "QPC frequency invalid: " << freq << std::endl;
        return 1;
    }

    const auto anchor = QpcClock::SampleUtcAnchor();
    if (anchor.qpc_freq <= 0 || anchor.qpc_ticks <= 0 || anchor.utc_epoch_ns <= 0) {
        std::cerr << "utc anchor invalid" << std::endl;
        return 1;
    }

    const int64_t now_qpc = QpcClock::NowTicks();
    const int64_t now_utc_ns = QpcClock::QpcToUtcEpochNs(now_qpc, anchor);
    if (now_utc_ns <= 0) {
        std::cerr << "qpc->utc convert failed" << std::endl;
        return 1;
    }

    const int64_t later_qpc = now_qpc + freq / 2;
    const int64_t later_utc_ns = QpcClock::QpcToUtcEpochNs(later_qpc, anchor);
    if (later_utc_ns <= now_utc_ns) {
        std::cerr << "qpc->utc monotonic check failed" << std::endl;
        return 1;
    }

    std::cout << "time_tests passed" << std::endl;
    return 0;
}

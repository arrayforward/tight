#include "test_framework.hpp"

#include "tight/bandwidth.hpp"

#include <chrono>

using namespace tight;
using namespace std::chrono_literals;

TEST_CASE(bandwidth_initial_value) {
    BandwidthEstimator est(100 * 1024 * 1024);
    CHECK_EQ(est.bytes_per_second(), 100ULL * 1024 * 1024);
    CHECK_EQ(est.btl_bw_bps(), 100ULL * 1024 * 1024);
    CHECK_EQ(est.rtt().count(), 0);
}

TEST_CASE(bandwidth_zero_initial_gets_floor) {
    BandwidthEstimator est(0);
    CHECK(est.bytes_per_second() >= 1024);
}

TEST_CASE(bandwidth_seed_raises_but_never_lowers) {
    BandwidthEstimator est(10 * 1024 * 1024);
    est.seed_bandwidth(40 * 1024 * 1024);
    CHECK_EQ(est.btl_bw_bps(), 40ULL * 1024 * 1024);
    est.seed_bandwidth(1 * 1024 * 1024);
    CHECK_EQ(est.btl_bw_bps(), 40ULL * 1024 * 1024);
    est.seed_bandwidth(0);
    CHECK_EQ(est.btl_bw_bps(), 40ULL * 1024 * 1024);
}

TEST_CASE(bandwidth_ack_sample_raises_only) {
    BandwidthEstimator est(10 * 1024 * 1024);
    // 1 MB acked in 100ms -> 10 MB/s sample, equal to current btl
    est.on_ack(1024 * 1024, 100ms);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024);
    // 10 MB acked in 100ms -> 100 MB/s sample, raises
    est.on_ack(10 * 1024 * 1024, 100ms);
    CHECK_EQ(est.btl_bw_bps(), 100ULL * 1024 * 1024);
    // small sample must never lower the estimate
    est.on_ack(1024, 10s);
    CHECK_EQ(est.btl_bw_bps(), 100ULL * 1024 * 1024);
}

TEST_CASE(bandwidth_pure_rtt_sample_tracks_rtprop) {
    BandwidthEstimator est(10 * 1024 * 1024);
    est.on_ack(0, 50ms);  // 50000us: 首样本
    est.on_ack(0, 100ms); // (50000*7 + 100000)/8 = 56250
    est.on_ack(0, 80ms);  // (56250*7 + 80000)/8  = 59218
    CHECK_EQ(est.rtt().count(), 59218); // smoothed, 单位微秒
    CHECK_GT(est.btl_bw_bps(), 0ULL);
}

TEST_CASE(bandwidth_rtt_smoothing) {
    BandwidthEstimator est(1);
    for (int i = 0; i < 3; ++i) est.on_ack(0, 100ms);
    CHECK_EQ(est.rtt().count(), 100000); // 100ms = 100000us
    est.on_ack(0, 300ms);
    CHECK_EQ(est.rtt().count(), 125000); // (100000*7 + 300000)/8
}

TEST_CASE(bandwidth_zero_rtt_ignored) {
    BandwidthEstimator est(1024 * 1024);
    est.on_ack(1024, 0s);
    CHECK_EQ(est.rtt().count(), 0);
    CHECK_EQ(est.btl_bw_bps(), 1024ULL * 1024);
}

TEST_CASE(bandwidth_delivery_rate_drop_follows) {
    BandwidthEstimator est(100 * 1024 * 1024);
    est.on_delivery_rate(10 * 1024 * 1024, false, false);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024);
    CHECK(est.fec_probe()); // 无 L4S 跟跌后进入 FEC 2x 探测
    // probe 片增益 1.25
    CHECK_GE(est.bytes_per_second(), 10ULL * 1024 * 1024);
}

TEST_CASE(bandwidth_app_limited_never_collapses) {
    BandwidthEstimator est(100 * 1024 * 1024);
    // app_limited 时投递率低于阈值也不得跟跌
    est.on_delivery_rate(10 * 1024 * 1024, true, false);
    CHECK_EQ(est.btl_bw_bps(), 100ULL * 1024 * 1024);
    CHECK(est.app_limited_state());
    CHECK_EQ(est.bytes_per_second(), 100ULL * 1024 * 1024 * 5 / 4);
}

TEST_CASE(bandwidth_ce_reduces_and_doubles) {
    BandwidthEstimator est(100 * 1024 * 1024);
    est.on_delivery_rate(10 * 1024 * 1024, false, false);
    est.on_ce(0.5);
    // btl = last_delivery x (1 - 0.5*ce) = 10MB x 0.75
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 * 3 / 4);
    CHECK(!est.fec_probe()); // L4S 激活后停用 FEC 探测
    est.on_ce(0.0);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 * 3 / 4 * 2);
    // 恢复后仍受 2x 最近投递率封顶
    est.on_ce(0.0);
    CHECK_LE(est.btl_bw_bps(), 10ULL * 1024 * 1024 * 2);
}

TEST_CASE(bandwidth_ce_clamped_ratio) {
    BandwidthEstimator est(100 * 1024 * 1024);
    est.on_delivery_rate(10 * 1024 * 1024, false, false);
    est.on_ce(2.0); // clamped to 1.0
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 / 2);
}

TEST_CASE(bandwidth_late_ratio_clamped) {
    BandwidthEstimator est(1024 * 1024);
    est.on_late_ratio(5.0);  // clamped to 1.0
    est.on_late_ratio(-3.0); // clamped to 0.0
    CHECK_GT(est.bytes_per_second(), 0ULL);
}

TEST_CASE(bandwidth_delivery_rate_zero_is_safe) {
    BandwidthEstimator est(100 * 1024 * 1024);
    est.on_delivery_rate(0, false, false);
    CHECK_GT(est.btl_bw_bps(), 0ULL);
}

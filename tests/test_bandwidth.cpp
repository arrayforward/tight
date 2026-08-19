#include "test_framework.hpp"

#include "tight/bandwidth.hpp"

#include <chrono>

using namespace tight;
using namespace std::chrono_literals;

// on_report 参数：p50_ms, late_ratio, loss_ratio, ce_ratio, rtt_us, pacer_limited
namespace {
void rep(BandwidthEstimator& e, std::uint32_t p50, double late, double loss = 0.0,
         double ce = 0.0, bool pacer = false) {
    e.on_report(p50, late, loss, ce, 10000, pacer);
}
} // namespace

TEST_CASE(bandwidth_initial_value) {
    BandwidthEstimator est(1250000); // 默认种子 10Mbps
    CHECK_EQ(est.bytes_per_second(), 1250000ULL);
    CHECK_EQ(est.btl_bw_bps(), 1250000ULL);
    CHECK_EQ(est.rtt().count(), 0);
}

TEST_CASE(bandwidth_zero_initial_gets_floor) {
    BandwidthEstimator est(0);
    CHECK(est.bytes_per_second() >= 1024);
}

TEST_CASE(bandwidth_min_btl_floor) {
    // 100kbps 下限（kMinBtlBps = 12500 B/s）
    BandwidthEstimator est(1);
    CHECK_GE(est.btl_bw_bps(), 12500ULL);
}

TEST_CASE(bandwidth_congestion_halves_btl) {
    BandwidthEstimator est(10 * 1024 * 1024);
    // 报告 1：P50 建立 RTprop，无拥塞 → 恢复台阶 1（btl×1.5 被种子封顶）
    rep(est, 20, 0.0);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024);
    CHECK_EQ(est.fec_probe_extra(), 2U);
    CHECK(!est.congested());
    // 报告 2：P50 升至 100ms，排队延迟 EWMA = 0.3×80 = 24ms > 20ms → 拥塞
    rep(est, 100, 0.0);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 / 2);
    CHECK_EQ(est.fec_probe_extra(), 0U); // 拥塞清除 FEC 探测
    CHECK(est.congested());
    CHECK(est.delay_congested());
}

TEST_CASE(bandwidth_late_ratio_triggers_congestion) {
    BandwidthEstimator est(10 * 1024 * 1024);
    // 迟到率 3% > 2% 拥塞阈值：首个报告即判定拥塞（btl 减半）
    rep(est, 10, 0.03);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 / 2);
    CHECK_EQ(est.fec_probe_extra(), 0U);
    // 迟到率 0.4% < 0.5% 且无排队延迟：恢复台阶（btl ×1.5，种子封顶 10MB）
    rep(est, 10, 0.004);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 / 2 * 3 / 2);
    CHECK_EQ(est.fec_probe_extra(), 2U);
    // 中间区（0.5%~2% 且延迟 <10ms）：保持不动（迟滞防摆动）
    rep(est, 10, 0.006);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 / 2 * 3 / 2);
    CHECK_EQ(est.fec_probe_extra(), 2U);
}

TEST_CASE(bandwidth_ce_ratio_triggers_congestion) {
    BandwidthEstimator est(10 * 1024 * 1024);
    // CE 占比 2% > 1% 阈值：直接判定拥塞（L4S 信号）
    rep(est, 10, 0.0, 0.0, 0.02);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 / 2);
    CHECK(est.congested());
}

TEST_CASE(bandwidth_pacer_limited_uses_loss_not_late) {
    BandwidthEstimator est(10 * 1024 * 1024);
    // 令牌受限（pacer=true）时：迟到率是本地排队伪信号，不用它判拥塞；
    // 丢包率 0.1% 低 → 不拥塞（走恢复台阶）
    rep(est, 10, 0.9, 0.001, 0.0, true);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024); // 未被 late 拖崩
    // 丢包率 5% > 2% → 真实链路拥塞，但令牌受限温和降（×0.75）
    rep(est, 10, 0.0, 0.05, 0.0, true);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 * 3 / 4);
}

TEST_CASE(bandwidth_recovery_two_step_ramp) {
    BandwidthEstimator est(10 * 1024 * 1024);
    // 先拥塞降到底：btl = 5MB
    rep(est, 10, 0.5);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 / 2);
    // 台阶 1：btl ×1.5 = 7.5MB + FEC 探测冗余
    rep(est, 10, 0.0);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 / 2 * 3 / 2);
    CHECK_EQ(est.fec_probe_extra(), 2U);
    // 台阶 2：无拥塞 → btl ×1.5 = 11.25MB（种子 10MB 封顶），移除 FEC 探测
    rep(est, 10, 0.0);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024);
    CHECK_EQ(est.fec_probe_extra(), 0U);
    // 台阶走完回到起点（连续爬升），种子上限封顶不变
    rep(est, 10, 0.0);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024);
}

TEST_CASE(bandwidth_recovery_capped_by_seed) {
    BandwidthEstimator est(10 * 1024 * 1024);
    // 拥塞减半后恢复提升，最多回到种子 10MB
    rep(est, 10, 0.5);
    CHECK_EQ(est.btl_bw_bps(), 5ULL * 1024 * 1024);
    rep(est, 10, 0.0);
    CHECK_EQ(est.btl_bw_bps(), 5ULL * 1024 * 1024 * 3 / 2);  // 7.5MB
    rep(est, 10, 0.0);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024);          // 封顶种子
    rep(est, 10, 0.0);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024);          // 不再提升
    CHECK_EQ(est.fec_probe_extra(), 0U);
}

TEST_CASE(bandwidth_pacer_limited_vetoes_congestion) {
    BandwidthEstimator est(10 * 1024 * 1024);
    rep(est, 10, 0.0); // 恢复台阶 1
    // 本地限速中（pacer_limited）：P50 高是本地制造，不判拥塞（防崩底死锁）
    rep(est, 100, 0.0, 0.0, 0.0, true);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024); // 未被减半
    CHECK_EQ(est.fec_probe_extra(), 0U);             // 走台阶 2：FEC 探测移除
    CHECK(!est.congested());
}

TEST_CASE(bandwidth_btl_never_drops_below_min) {
    BandwidthEstimator est(1024 * 1024);
    for (int i = 0; i < 20; ++i) {
        rep(est, 10, 0.5);
    }
    CHECK_EQ(est.btl_bw_bps(), 12500ULL); // 100kbps 下限
}

TEST_CASE(bandwidth_ack_only_tracks_rtt) {
    BandwidthEstimator est(10 * 1024 * 1024);
    // 投递率样本不再用于估计：大样本不得提升 btl
    est.on_ack(100 * 1024 * 1024, 10ms);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024);
    CHECK_EQ(est.rtt().count(), 10000);
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

TEST_CASE(bandwidth_bps_matches_btl_no_gain) {
    BandwidthEstimator est(1024 * 1024);
    rep(est, 10, 0.02); // 拥塞减半
    CHECK_EQ(est.bytes_per_second(), est.btl_bw_bps());
}

TEST_CASE(bandwidth_app_limited_stays_false) {
    BandwidthEstimator est(1024 * 1024);
    rep(est, 10, 0.0);
    CHECK(!est.app_limited_state()); // AIMD 不依赖投递率，恒不更新
}

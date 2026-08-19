#include "test_framework.hpp"

#include "tight/bandwidth.hpp"

#include <chrono>

using namespace tight;
using namespace std::chrono_literals;

// on_report 参数：p50_ms, late_ratio, loss_ratio, ce_ratio, rtt_us,
//                 pacer_limited, sustained_overload
namespace {
void rep(BandwidthEstimator& e, std::uint32_t p50, double late, double loss = 0.0,
         double ce = 0.0, bool pacer = false, bool sustained = true) {
    e.on_report(p50, late, loss, ce, 10000, pacer, sustained);
}
} // namespace

TEST_CASE(bandwidth_initial_value) {
    BandwidthEstimator est(3750000); // 默认种子 30Mbps
    CHECK_EQ(est.bytes_per_second(), 3750000ULL);
    CHECK_EQ(est.btl_bw_bps(), 3750000ULL);
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

TEST_CASE(bandwidth_congestion_delay_only_light_tier) {
    BandwidthEstimator est(10 * 1024 * 1024);
    // 报告 1：P50 建立 RTprop，无拥塞 → 恢复台阶 1（btl×1.5 被种子封顶）
    rep(est, 20, 0.0);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024);
    CHECK_EQ(est.fec_probe_extra(), 2U);
    CHECK(!est.congested());
    // 报告 2：排队延迟 EWMA = 0.3×80 = 24ms > 20ms → 拥塞；
    // 持续超发 + 无 late/CE（strength=0）→ 轻档兜底 ×0.65
    rep(est, 100, 0.0, 0.0, 0.0, false, true);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 * 65 / 100);
    CHECK_EQ(est.fec_probe_extra(), 0U);
    CHECK(est.congested());
    CHECK(est.delay_congested());
    // 轻档（×0.65）不触发排空窗口
    CHECK_EQ(est.last_congest_at().time_since_epoch().count(), 0);
}

TEST_CASE(bandwidth_congestion_quantized_tiers) {
    BandwidthEstimator est(10 * 1024 * 1024);
    // strength ≥ 50% → ×0.20
    rep(est, 10, 0.6, 0.0, 0.0, false, true);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 * 20 / 100);
    // 20%~50% → ×0.30
    rep(est, 10, 0.3);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 * 20 / 100 * 30 / 100);
    // 5%~20% → ×0.45
    rep(est, 10, 0.08);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 * 20 / 100 * 30 / 100 * 45 / 100);
    // 1%~5% → ×0.65（>2% 拥塞阈值才触发）
    rep(est, 10, 0.03);
    CHECK_EQ(est.btl_bw_bps(),
             10ULL * 1024 * 1024 * 20 / 100 * 30 / 100 * 45 / 100 * 65 / 100);
}

TEST_CASE(bandwidth_sustained_overload_gate) {
    BandwidthEstimator est(10 * 1024 * 1024);
    // 无持续超发（关键帧突刺）：CE/late 是瞬时信号，不降速
    rep(est, 10, 0.5, 0.0, 0.0, false, false);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024);
    CHECK(est.congested()); // 信号仍在，但 btl 保持
    // 持续超发 → 量化降速
    rep(est, 10, 0.5, 0.0, 0.0, false, true);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 * 20 / 100);
}

TEST_CASE(bandwidth_ce_ratio_triggers_congestion) {
    BandwidthEstimator est(10 * 1024 * 1024);
    // CE 占比 2%（strength=0.02）→ 轻档 ×0.65（L4S 直接信号）
    rep(est, 10, 0.0, 0.0, 0.02);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 * 65 / 100);
    CHECK(est.congested());
}

TEST_CASE(bandwidth_pacer_limited_uses_loss_not_late) {
    BandwidthEstimator est(10 * 1024 * 1024);
    // 令牌受限（pacer=true）时：迟到率是本地排队伪信号，不用它判拥塞；
    // 丢包率 0.1% 低 → 不拥塞（走恢复台阶）
    rep(est, 10, 0.9, 0.001, 0.0, true, true);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024); // 未被 late 拖崩
    // 丢包率 5%（strength=0.05）→ 真实链路拥塞 → 中档 ×0.45
    rep(est, 10, 0.0, 0.05, 0.0, true, true);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 * 45 / 100);
    // 中档（≤×0.45）触发排空窗口时刻记录
    CHECK_GT(est.last_congest_at().time_since_epoch().count(), 0);
}

TEST_CASE(bandwidth_recovery_two_step_ramp) {
    BandwidthEstimator est(10 * 1024 * 1024);
    // 先剧烈拥塞降到底：strength=0.5 → ×0.20 → 2MB
    rep(est, 10, 0.5);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 * 20 / 100);
    // 台阶 1：btl ×1.5 = 3MB + FEC 探测冗余
    rep(est, 10, 0.0);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 * 20 / 100 * 3 / 2);
    CHECK_EQ(est.fec_probe_extra(), 2U);
    // 台阶 2：无拥塞 → ×1.5 = 4.5MB，移除 FEC 探测
    rep(est, 10, 0.0);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 * 20 / 100 * 3 / 2 * 3 / 2);
    CHECK_EQ(est.fec_probe_extra(), 0U);
    // 台阶走完回到起点（连续爬升）
    rep(est, 10, 0.0);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024 * 20 / 100 * 3 / 2 * 3 / 2);
}

TEST_CASE(bandwidth_recovery_capped_by_seed) {
    BandwidthEstimator est(10 * 1024 * 1024);
    // 拥塞减到 2MB 后恢复提升，最多回到种子 10MB
    rep(est, 10, 0.5);
    CHECK_EQ(est.btl_bw_bps(), 2ULL * 1024 * 1024);
    rep(est, 10, 0.0);
    CHECK_EQ(est.btl_bw_bps(), 3ULL * 1024 * 1024);
    rep(est, 10, 0.0);
    CHECK_EQ(est.btl_bw_bps(), 4ULL * 1024 * 1024 * 9 / 8); // 4.5MB
    for (int i = 0; i < 10; ++i) rep(est, 10, 0.0);
    CHECK_EQ(est.btl_bw_bps(), 10ULL * 1024 * 1024);        // 封顶种子
    CHECK_EQ(est.fec_probe_extra(), 0U);
}

TEST_CASE(bandwidth_pacer_limited_vetoes_congestion) {
    BandwidthEstimator est(10 * 1024 * 1024);
    rep(est, 10, 0.0); // 恢复台阶 1
    // 本地限速中（pacer_limited）：P50 高是本地制造，不判拥塞（防崩底死锁）
    rep(est, 100, 0.0, 0.0, 0.0, true, true);
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
    rep(est, 10, 0.02); // 拥塞减 35%
    CHECK_EQ(est.bytes_per_second(), est.btl_bw_bps());
}

TEST_CASE(bandwidth_app_limited_stays_false) {
    BandwidthEstimator est(1024 * 1024);
    rep(est, 10, 0.0);
    CHECK(!est.app_limited_state()); // AIMD 不依赖投递率，恒不更新
}

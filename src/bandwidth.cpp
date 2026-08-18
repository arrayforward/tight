#include "tight/bandwidth.hpp"

#include "tight/types.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>

namespace tight {

BandwidthEstimator::BandwidthEstimator(std::uint64_t initial_bytes_per_second)
    : m_btl_bw(std::max<std::uint64_t>(
          initial_bytes_per_second == 0 ? 1 : initial_bytes_per_second, kMinBtlBps)),
      m_btl_seed(m_btl_bw) {
}

void BandwidthEstimator::on_ack(std::size_t bytes, std::chrono::microseconds rtt) {
    (void)bytes;   // 投递率样本不再用于估计（AIMD 只依赖报告三信号）
    std::lock_guard<std::mutex> lock(m_mu);
    auto rtt_count = rtt.count();
    if (rtt_count <= 0) return;
    if (m_rt_prop.count() == 0 || rtt < m_rt_prop) m_rt_prop = rtt;
    if (m_rtt.count() == 0) {
        m_rtt = rtt;
    } else {
        m_rtt = std::chrono::microseconds((m_rtt.count() * 7 + rtt_count) / 8);
    }
}

void BandwidthEstimator::on_report(std::uint32_t p50_ms, double late_ratio,
                                   double ce_ratio, std::uint32_t rtt_us,
                                   bool pacer_limited) {
    (void)ce_ratio;   // CE 已并入接收端迟到统计
    (void)rtt_us;
    std::lock_guard<std::mutex> lock(m_mu);

    // RTprop：单程延迟最小值（对报告 P50 做 min filter）
    std::uint64_t p50_us = static_cast<std::uint64_t>(p50_ms) * 1000;
    if (m_rt_prop_us == 0 || p50_us < m_rt_prop_us) m_rt_prop_us = p50_us;

    // 排队延迟 = P50 - RTprop（EWMA 平滑趋势）
    double q_ms = (p50_us > m_rt_prop_us)
                      ? static_cast<double>(p50_us - m_rt_prop_us) / 1000.0
                      : 0.0;
    m_delay_ewma = 0.7 * m_delay_ewma + 0.3 * q_ms;

    // 拥塞判定（迟滞）：delay-based（排队延迟超阈值）或 late-based（迟到
    // 率超阈值，含丢包/CE）。恢复判定更严：delay < 10ms 且 late < 0.5% 才
    // 提升。中间区（10~20ms 或 0.5%~2%）保持不动——消除"刚过阈值就反向"
    // 的摆动（弱网段实测 btl 在 0.2-0.8M 振荡的根源）。
    // 本地令牌桶限速中（pacer_limited）不判拥塞：p50 高是本地限速制造
    // 的排队（btl 低估 → 令牌 < 供给），走恢复台阶提升 btl 解除限速——
    // 否则"崩底 → 令牌不足 → 自造积压 → 误判拥塞 → 更崩"死锁。
    bool congested = !pacer_limited &&
                     ((m_delay_ewma > static_cast<double>(kDelayThresholdMs)) ||
                      (late_ratio > kLateThreshold));
    bool recovered = !pacer_limited &&
                     (m_delay_ewma < static_cast<double>(kRecoverDelayMs)) &&
                     (late_ratio < kRecoverLateThreshold);

    if (congested) {
        // 每报告（333ms）乘性下降 50%，无冷却——以 report 为准，链路超发
        // 立即响应；下限 100kbps 防打穿
        m_btl_bw = std::max(static_cast<std::uint64_t>(
                                static_cast<double>(m_btl_bw) * kCongestFactor),
                            kMinBtlBps);
        m_recover_step = 0;
        m_fec_probe_extra = 0;
    } else if (pacer_limited || recovered) {
        // 两步台阶法提升（防止提升负载带来卡顿），台阶间隔 = 一个报告周期：
        //   台阶 1（本报告）：btl ×1.5，压上 FEC 校验片负载感知链路——
        //            FEC 可丢失、不伤业务（对端缺校验片不影响数据片组装）；
        //            探测持续 1 个报告周期（333ms > 100ms 即可测量链路余量）
        //   台阶 2（下一报告，无拥塞才走到）：确认链路有余量 → btl ×1.5，
        //            移除 FEC 探测，业务流量自然替换 FEC 流量（video_capacity
        //            用实际冗余率折算，探测片移除后冗余率回落 → 编码码率上升）
        // 提升上限 = 初始种子（btl 不超过配置种子，防种子被台阶推高振荡）
        if (m_recover_step == 0) {
            m_btl_bw = static_cast<std::uint64_t>(
                std::min(static_cast<double>(m_btl_bw) * kRecoverFactor,
                         static_cast<double>(m_btl_seed)));
            if (m_btl_bw < kMinBtlBps) m_btl_bw = kMinBtlBps;
            m_fec_probe_extra = kProbeExtraParity;   // FEC 先行探测
            m_recover_step = 1;
        } else if (m_recover_step == 1) {
            // 上一报告 FEC 探测无拥塞 → 业务替换 FEC（移除探测冗余）
            m_btl_bw = static_cast<std::uint64_t>(
                std::min(static_cast<double>(m_btl_bw) * kRecoverFactor,
                         static_cast<double>(m_btl_seed)));
            if (m_btl_bw < kMinBtlBps) m_btl_bw = kMinBtlBps;
            m_fec_probe_extra = 0;
            m_recover_step = 2;
        } else {
            // 一轮两步提升完成：回到 step0 开始下一轮（连续提升，直到
            // 种子上限或拥塞信号）。此前 step=2 保持导致 btl 卡在中途
            // （实测 703K 后不再爬升，永远到不了真实带宽）。
            m_recover_step = 0;
        }
    }
    {
        static std::atomic<std::uint64_t> dbg_ts{0};
        auto dbg_now = std::chrono::steady_clock::now().time_since_epoch().count();
        if (dbg_now - dbg_ts.load() > 200000000LL) {
            dbg_ts.store(dbg_now);
            std::printf("DBG aimd [%llu] p50=%ums q=%.1fms late=%.2f pacer=%d cong=%d step=%d probe=%u btl=%llu\n",
                        (unsigned long long)unix_millis(), p50_ms, m_delay_ewma, late_ratio,
                        (int)pacer_limited, (int)congested, m_recover_step,
                        (unsigned)m_fec_probe_extra, (unsigned long long)m_btl_bw);
            fflush(stdout);
        }
    }
}

std::uint32_t BandwidthEstimator::fec_probe_extra() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_fec_probe_extra;
}

std::uint64_t BandwidthEstimator::bytes_per_second() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return std::max<std::uint64_t>(m_floor, m_btl_bw);
}

std::chrono::microseconds BandwidthEstimator::rtt() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_rtt;
}

bool BandwidthEstimator::app_limited_state() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_app_limited;
}

std::uint64_t BandwidthEstimator::btl_bw_bps() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_btl_bw;
}

}  // namespace tight

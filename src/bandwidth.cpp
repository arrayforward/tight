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
                                   double loss_ratio, double ce_ratio,
                                   std::uint32_t rtt_us, bool pacer_limited,
                                   bool sustained_overload) {
    (void)rtt_us;
    std::lock_guard<std::mutex> lock(m_mu);
    // 报告恢复到达：清除停滞标志（允许下一段停滞再次降速）
    m_report_stall = false;

    // RTprop：单程延迟最小值（对报告 P50 做 min filter）
    std::uint64_t p50_us = static_cast<std::uint64_t>(p50_ms) * 1000;
    if (m_rt_prop_us == 0 || p50_us < m_rt_prop_us) m_rt_prop_us = p50_us;

    // 排队延迟 = P50 - RTprop（EWMA 平滑趋势）
    double q_ms = (p50_us > m_rt_prop_us)
                      ? static_cast<double>(p50_us - m_rt_prop_us) / 1000.0
                      : 0.0;
    m_delay_ewma = 0.7 * m_delay_ewma + 0.3 * q_ms;

    // 拥塞判定（迟滞）：delay-based（排队延迟超阈值）、late-based（迟到
    // 率超阈值，含丢包）或 ECN（CE 标记占比超阈值）。恢复判定更严：
    // delay < 10ms 且 late < 0.5% 才提升。中间区保持——消除摆动。
    // 本地令牌桶限速（pacer_limited = 发送 < 供给）时，迟到率主体是"本地
    // 排队"伪信号：应用码率下限（QSV 1.5M）> 令牌 → outbound 积压 →
    // 接收端 p50 超线 → late 高——这不是链路拥塞，降速无益（发送已被
    // 令牌限制）且制造更多排队（L4S 实测：令牌卡死 → late 100% → 连降
    // 崩底 12.5K 死锁）。令牌卡时只用真实链路信号判定：丢包率（网络真
    // 丢）与 CE（proxy 链路积压）。非令牌受限时迟到率才是链路排队的
    // 真实反映，完整采用。
    bool congested = (m_delay_ewma > static_cast<double>(kDelayThresholdMs) && !pacer_limited) ||
                     (pacer_limited ? (loss_ratio > kLateThreshold)
                                    : (late_ratio > kLateThreshold)) ||
                     (ce_ratio > kCeThreshold);
    // 恢复判定同样豁免令牌卡死下的 late（本地排队不阻塞链路，btl 应继续
    // 提升解禁）：令牌卡死时丢包/CE 低即可恢复
    bool recovered = (m_delay_ewma < static_cast<double>(kRecoverDelayMs)) &&
                     (pacer_limited ? (loss_ratio < kRecoverLateThreshold &&
                                       ce_ratio < kCeThreshold)
                                    : (late_ratio < kRecoverLateThreshold)) &&
                     !pacer_limited;
    m_congested = congested;

    if (congested) {
        // 突刺门控（sustained_overload）：无持续超发（报告期平均发送 ≤
        // 接收速率）时，CE/late 是关键帧突刺的瞬时信号（I 帧 40-60KB 在
        // 低带宽链路瞬时排队数十 ms——帧自身传输，平均流量远低于链路）。
        // 此时不降速：btl 保持，突刺排空后恢复台阶自然回升——避免关键
        // 帧排队把 btl 打崩（30M→0.23M 崩底 → 贷款循环）。持续超发才
        // 按强度量化降速（见下）。
        if (sustained_overload) {
            // 降速量化：按信号强度（迟到/CE 报文占比）分级降幅，一次报告
            // 收敛到位。strength = max(late, ce)；pacer_limited（令牌卡死 =
            // 本地排队伪信号，late 不可信）时用真实链路信号 max(loss, ce)
            // 走同一阶梯（CE 高 = 真实超发，照降）。相比固定 ×0.5 每报告
            // 连降：超发积压排空期 CE 持续 → 连降 7 轮把 btl 崩到远低于
            // 真实链路（30M→0.23M）→ 视频下限都"超发"→ 贷款循环；量化
            // 大降一次到位 → btl 收敛在真实链路附近 → CE 即停 → 不崩底
            // 不循环。下限 100k 防打穿。
            double strength = pacer_limited ? std::max(loss_ratio, ce_ratio)
                                            : std::max(late_ratio, ce_ratio);
            double factor;
            if (strength >= kCongestTier4Threshold) {
                factor = kCongestTier4Factor;
            } else if (strength >= kCongestTier3Threshold) {
                factor = kCongestTier3Factor;
            } else if (strength >= kCongestTier2Threshold) {
                factor = kCongestTier2Factor;
            } else if (strength >= kCeThreshold) {
                factor = kCongestTier1Factor;
            } else {
                // late/ce 都低于阈值但 delay 信号触发的拥塞（或信号统计窗口
                // 边界）：轻档兜底
                factor = kCongestTier1Factor;
            }
            m_btl_bw = std::max(static_cast<std::uint64_t>(
                                    static_cast<double>(m_btl_bw) * factor),
                                kMinBtlBps);
            // 剧烈档（×0.45 及以下，strength≥5%）记录降速时刻：transport
            // 以此为排空窗口起点（窗口内视频码率输出排空值，3s 排完积压）。
            // 轻档（×0.65）不触发（轻度拥塞不折腾编码器）。
            if (factor <= kCongestTier2Factor) {
                m_last_congest_at = std::chrono::steady_clock::now();
            }
            m_recover_step = 0;
            m_fec_probe_extra = 0;
        }
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
            // L4S 活跃（CE 标记存在）时无需 FEC 探测（CE 即链路反馈——
            // 探测冗余会使线上超发 → 更多 CE → 连降，L4S 实测自伤）；
            // 无 CE 环境用 FEC 探测感知链路余量
            m_fec_probe_extra = (ce_ratio < kCeThreshold) ? kProbeExtraParity : 0;
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

bool BandwidthEstimator::congested() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_congested;
}

std::chrono::steady_clock::time_point BandwidthEstimator::last_congest_at() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_last_congest_at;
}

void BandwidthEstimator::on_report_timeout() {
    std::lock_guard<std::mutex> lock(m_mu);
    // 每段停滞只降一次（m_report_stall 防重复叠加——停滞期间无反馈，
    // 反复减半是盲猜且下坠过冲，恢复 ×1.5/报告爬回很慢）。
    if (m_report_stall) return;
    m_report_stall = true;
    // 报告持续收不到 = 链路严重卡顿/断流：×0.5 单次降（下限防打穿），
    // 重置恢复台阶与 FEC 探测；报告恢复后恢复台阶（×1.5/报告）自然回升。
    m_btl_bw = std::max(static_cast<std::uint64_t>(
                            static_cast<double>(m_btl_bw) * kCongestFactor),
                        kMinBtlBps);
    m_recover_step = 0;
    m_fec_probe_extra = 0;
}

bool BandwidthEstimator::delay_congested() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_delay_ewma > static_cast<double>(kDelayThresholdMs);
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

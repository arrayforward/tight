#include "tight/bandwidth.hpp"

#include <algorithm>
#include <cstdio>

namespace tight {

BandwidthEstimator::BandwidthEstimator(std::uint64_t initial_bytes_per_second)
    : m_btl_bw(initial_bytes_per_second == 0 ? 1 : initial_bytes_per_second),
      m_floor(1024) {
}

void BandwidthEstimator::on_ack(std::size_t bytes, std::chrono::microseconds rtt) {
    std::lock_guard<std::mutex> lock(m_mu);
    auto rtt_count = rtt.count();
    if (rtt_count <= 0) return;

    // RTprop (min filter) + smoothed RTT; keep the previous smoothed value
    // for trend detection.
    if (m_rt_prop.count() == 0 || rtt < m_rt_prop) m_rt_prop = rtt;
    if (m_rtt.count() == 0) {
        m_rtt = rtt;
    } else {
        m_prev_rtt = m_rtt;
        m_have_prev_rtt = true;
        m_rtt = std::chrono::microseconds((m_rtt.count() * 7 + rtt_count) / 8);
    }

    recompute_gain();

    if (bytes == 0) return;  // pure RTT sample (heartbeat/report transit)

    // Delivery-rate sample (bytes/s) from a direct ACK. 只升不降：
    // ACK 只出现在握手期，其 RTT 可能被拥塞队列放大数十秒，样本会低
    // 到几百 B/s——直接写窗口会毒化 BtlBw（实测 btl 被压到 0.2KB/s，
    // pace 饿死、链路坍缩）。正规的投递率样本来自 Report 的 recv_rate
    // （带 pacer/位置防护与丢包校正），这里只允许 ACK 样本提升估计。
    std::uint64_t sample = (static_cast<std::uint64_t>(bytes) * 1000000ULL)
                         / static_cast<std::uint64_t>(rtt_count);
    if (sample > m_btl_bw) m_btl_bw = sample;
}

void BandwidthEstimator::on_late_ratio(double late_ratio) {
    std::lock_guard<std::mutex> lock(m_mu);
    if (late_ratio < 0.0) late_ratio = 0.0;
    if (late_ratio > 1.0) late_ratio = 1.0;
    m_late_rising = m_have_late && late_ratio > m_last_late_ratio + 0.001;
    m_last_late_ratio = late_ratio;
    m_have_late = true;
    recompute_gain();  // late ratio is the secondary gain signal
}

void BandwidthEstimator::on_delivery_rate(std::uint64_t bytes_per_second, bool app_limited,
                                          bool pacer_limited) {
    std::lock_guard<std::mutex> lock(m_mu);
    m_app_limited = app_limited;
    m_probe_pacer_limited = m_probe_pacer_limited || pacer_limited;
    m_last_delivery = bytes_per_second;
    std::uint32_t pos = cycle_pos();
    std::printf("DBG deliv: R=%llu appLim=%d pacer=%d pos=%u btl=%llu\n",
                (unsigned long long)bytes_per_second, (int)app_limited, (int)pacer_limited,
                pos, (unsigned long long)m_btl_bw);
    fflush(stdout);
    if (bytes_per_second == 0) {
        recompute_gain();
        return;
    }
    // 立即跟跌（无 L4S 时的兜底下降 + L4S 超发后的回落）：投递率跌破
    // 阈值（探测片 0.75×btl，排空片 0.5×btl）即收敛到实际投递率，清窗
    // 防旧高样本顶回。R==0 已单独处理（应用暂停不跟跌）。
    // 应用受限（出站无积压，投递率=应用速率而非链路容量）时绝不跟跌：
    // 否则 btl 被压到应用速率后码率继续下降，形成螺旋坍缩。
    bool probe = pos < kProbePeriods;
    std::uint64_t low_thresh = probe ? m_btl_bw * 3 / 4 : m_btl_bw / 2;
    if (bytes_per_second < low_thresh && !app_limited) {
        m_btl_bw = bytes_per_second;
        m_window.fill(0);
        m_window_pos = 0;
        m_window_count = 1;
        m_window[0] = bytes_per_second;
        // 无 L4S 时跟跌后启动 FEC 2× 探测（限时），重新发现链路余量。
        if (!m_l4s_seen) {
            m_fec_probe = true;
            m_fec_probe_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        }
        recompute_gain();
        return;
    }
    if (!probe) {
        recompute_gain();
        return;
    }
    m_window[m_window_pos] = bytes_per_second;
    m_window_pos = (m_window_pos + 1) % m_window.size();
    if (m_window_count < m_window.size()) ++m_window_count;
    std::uint64_t best = 0;
    for (std::size_t i = 0; i < m_window_count; ++i) best = std::max(best, m_window[i]);
    // 仅无 L4S 时由投递率窗口驱动爬升（L4S 激活后爬升交给 on_ce 的 ×2，
    // 避免两处重复爬升造成振荡）。
    if (!m_l4s_seen && best > m_btl_bw) {
        // 爬升限速 ≤1.25×/样本（×2 曾导致排空恢复后 btl 快速顶回旧高值，
        // 段切换（10Mbps→2Mbps）后 2-3 个报告内超发 → 反复排空循环实测；
        // 1.25× 保守爬升给链路变化留观察窗口）
        std::uint64_t cap = m_btl_bw + m_btl_bw / 4;
        m_btl_bw = best < cap ? best : cap;
    }
    recompute_gain();
}

// L4S/ECN 反馈：ce_ratio 为接收端在报告周期内测得的 CE 标记占比。
//   ce_ratio > 0 → 比例下降（Scalable：DCTCP 式 ×(1 - 0.5·ce_ratio)），
//   让发送端提前降速、队列维持极小。
//   ce_ratio == 0 → ×2 比例提升（恢复后快速上升，远大于 1.25×）。
void BandwidthEstimator::on_ce(double ce_ratio) {
    std::lock_guard<std::mutex> lock(m_mu);
    m_ce_ratio = ce_ratio;
    std::printf("DBG ce: ratio=%.3f btl=%llu seen=%d R=%llu\n", ce_ratio,
                (unsigned long long)m_btl_bw, (int)m_l4s_seen,
                (unsigned long long)m_last_delivery);
    fflush(stdout);
    if (ce_ratio > 0.0) {
        m_l4s_seen = true;
        m_fec_probe = false;  // 有 L4S，停用无 L4S 的 FEC 探测
        if (ce_ratio > 1.0) ce_ratio = 1.0;
        // 降速相对实测投递率 R 收敛：btl = R × (1 - 0.5·ce)。R 是链路真实
        // 瓶颈，有界、不随队列未排空叠加崩溃（对比相对 btl 的乘法）。
        std::uint64_t r = m_last_delivery > 0 ? m_last_delivery : m_btl_bw;
        std::uint64_t reduced = static_cast<std::uint64_t>(
            static_cast<double>(r) * (1.0 - 0.5 * ce_ratio));
        if (reduced < m_floor) reduced = m_floor;
        m_btl_bw = reduced;
        m_window.fill(0);
        m_window_pos = 0;
        m_window_count = 1;
        m_window[0] = m_btl_bw;
    } else if (m_l4s_seen) {
        // L4S 已激活且本间隔无 CE：×2 比例提升（恢复后快速上升），
        // 上界为 2×最近投递率，防远高于实测值。
        std::uint64_t doubled = m_btl_bw * 2;
        if (doubled == 0) doubled = m_floor;
        if (m_last_delivery > 0 && doubled > m_last_delivery * 2) {
            doubled = m_last_delivery * 2;
        }
        m_btl_bw = doubled;
    }
    // 从未见过 CE：无 L4S，爬升由 FEC 探测（Part 3）驱动，此处不动作。
    recompute_gain();
}

// 推进时间片循环，返回当前周期位置；探测片起点（pos==0）清空
// m_probe_pacer_limited（只统计本探测片的限速事件）。
std::uint32_t BandwidthEstimator::cycle_pos() {
    auto now = std::chrono::steady_clock::now();
    if (m_cycle_start.time_since_epoch().count() == 0) {
        m_cycle_start = now;
        m_cycle_count = 0;
    }
    while (now - m_cycle_start >= kCycleMs) {
        m_cycle_start += kCycleMs;
        ++m_cycle_count;
    }
    std::uint32_t pos = m_cycle_count % (kProbePeriods + kDrainPeriods);
    if (pos == 0) m_probe_pacer_limited = false;
    return pos;
}

// 增益只由时间片循环决定（kProbePeriods 个 1.25 探测 + kDrainPeriods 个
// 0.75 排空，1:1 使链路受限时的队列有界）。RTT 不参与增益：纯 RTT 触发
// 排空在"应用速率恒大于链路速率"时会永久停在 0.75（队列排不空 → RTT
// 不降 → 估计值坍缩，弱网实测复现）。应用受限（出站无积压）时恒为
// 1.25——应用速率即上限，排空只会人为制造延迟。排空片仅在探测片确实
// 建过队列（令牌桶卡住过）时生效，否则跳过——btl_bw ≈ 应用速率时
// 探测片 pace=demand、无积压，排空只会自造永不排空的积压。
void BandwidthEstimator::recompute_gain() {
    std::uint32_t pos = cycle_pos();
    if (m_app_limited) {
        m_gain = kGainUp;
        return;
    }
    if (pos >= kProbePeriods) {
        m_gain = m_probe_pacer_limited ? kGainDown : kGainUp;
    } else {
        m_gain = kGainUp;
    }
}

void BandwidthEstimator::seed_bandwidth(std::uint64_t bytes_per_second) {
    if (bytes_per_second == 0) return;
    std::lock_guard<std::mutex> lock(m_mu);
    // 只允许测速播种提升 BtlBw，且绝不动 m_floor：
    //  - 测得值低于当前假设时两种可能：
    //    (a) 测量被拥塞队列展宽污染（探测列车排在被洪泛填满的队列后，
    //        跨度被拉长，测得值可低到几百 B/s）——播种会把 pace 永久
    //        锁死在地板值，管线塞满后投递率样本全被拒，链路坍缩到
    //        0.7% 送达（实测复现）；
    //    (b) 链路确实更慢——无需播种，洪泛期（尚未限速、样本被采纳）
    //        投递率样本自然收敛到真实值。
    //  - m_floor 一旦被播种抬高，带宽下降时 max(floor, btl×gain) 会让
    //    pace 永久钉在旧值（实测 step-down 后 btl 已跟进 50KB/s，
    //    pace 仍为 583.9KB/s，应用继续洪泛）。
    if (bytes_per_second > m_btl_bw) {
        m_btl_bw = bytes_per_second;
    }
}

std::uint64_t BandwidthEstimator::bytes_per_second() const {
    std::lock_guard<std::mutex> lock(m_mu);
    double paced = static_cast<double>(m_btl_bw) * m_gain;
    return std::max<std::uint64_t>(m_floor, static_cast<std::uint64_t>(paced));
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

bool BandwidthEstimator::fec_probe() const {
    std::lock_guard<std::mutex> lock(m_mu);
    if (!m_fec_probe || m_l4s_seen) return false;
    return std::chrono::steady_clock::now() < m_fec_probe_deadline;
}

}

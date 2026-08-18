#pragma once

// Public BBR-style bandwidth/RTT estimator. The implementation lives in
// tight/bandwidth.cpp.
//
// Model (simplified BBR):
//  - BtlBw: windowed max of delivery-rate samples (bytes acked / ack RTT).
//  - RTprop: minimum RTT ever observed.
//  - Pacing gain from two signals:
//      PRIMARY   RTT trend: smoothed RTT well above RTprop (queue building)
//                drains (0.75); RTT hugging RTprop and not rising probes
//                (1.25); otherwise cruises (1.0).
//      SECONDARY late-packet ratio reported by the peer: it can only make
//                the estimate more conservative (veto probe / force drain),
//                never more aggressive.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace tight {

class BandwidthEstimator {
public:
    explicit BandwidthEstimator(std::uint64_t initial_bytes_per_second);

    // Feeds an ACK sample: bytes acknowledged within rtt. bytes == 0 means a
    // pure RTT sample (e.g. derived from heartbeat/report one-way transit).
    void on_ack(std::size_t bytes, std::chrono::microseconds rtt);

    // Feeds the peer-reported late-packet ratio [0,1] (secondary gain signal).
    void on_late_ratio(double late_ratio);

    // Feeds an interval-based delivery-rate sample (bytes acknowledged per
    // report interval) plus the sender's limiting state:
    //  - app_limited: outbound queue empty -> the app rate is the ceiling,
    //    gain stays at probe (no drain needed).
    //  - pacer_limited: the token bucket actually throttled with backlog ->
    //    the delivery sample merely mirrors the pacer, it carries NO link
    //    information and must be rejected (under loss a drain-phase sample
    //    reads ~0.6*BtlBw and would ratchet the estimate down to collapse).
    // Report 是数据面的主要确认通道：无此样本时，一旦测速列车被拥塞
    // 队列展宽导致播种值偏低，BtlBw 将永久停在低值（ACK 只在握手期
    // 出现，on_ack 拿不到投递样本）。
    void on_delivery_rate(std::uint64_t bytes_per_second, bool app_limited,
                          bool pacer_limited);

    // L4S/ECN 反馈（接收端测量的 CE 标记占比）：>0 比例下降，==0 ×2 提升。
    void on_ce(double ce_ratio);

    // Adopts an externally measured bandwidth (the peer's speed-test train
    // measurement) as the new BtlBw baseline and floor.
    void seed_bandwidth(std::uint64_t bytes_per_second);

    std::uint64_t bytes_per_second() const;
    std::chrono::microseconds rtt() const;
    // 诊断：上次样本时的应用受限状态
    bool app_limited_state() const;
    // 诊断：原始 BtlBw 测量值（bytes/s，不含增益）
    std::uint64_t btl_bw_bps() const;
    // 无 L4S 的 FEC 2× 探测是否激活（发送侧据此追加冗余校验片）。
    bool fec_probe() const;

private:
    static constexpr std::size_t kWindowSize = 5;    // BtlBw max-filter window（短窗口加速带宽下降跟进）
    static constexpr double kGainUp = 1.25;          // probe
    static constexpr double kGainDown = 0.75;        // drain
    // 时间片增益循环：kProbePeriods 个探测周期（1.25）+ kDrainPeriods 个
    // 排空周期（0.75），1:1 使链路受限时队列有界（探测积攒的队列在
    // 排空片恰好清空，平均速率 = BtlBw）。
    // 关键约束：
    //  - 纯 RTT 触发排空（队列升高才排、降了才恢复）在"应用速率恒大于
    //    链路速率"时会永久停在 0.75（队列排不空 → RTT 不降 → 坍缩），
    //    因此增益只由时间片决定，RTT 不参与。
    //  - 应用受限（出站无积压）时恒为 1.25：应用速率即上限，排空只会
    //    人为制造延迟。
    static constexpr std::uint32_t kProbePeriods = 8;
    static constexpr std::uint32_t kDrainPeriods = 8;
    static constexpr std::chrono::milliseconds kCycleMs{1000};

    void recompute_gain();
    // 推进时间片循环并返回当前周期位置（探测片起点清空 m_probe_pacer_limited）
    std::uint32_t cycle_pos();

    mutable std::mutex m_mu;
    std::array<std::uint64_t, kWindowSize> m_window{};
    std::size_t m_window_pos{0};
    std::size_t m_window_count{0};
    std::uint64_t m_btl_bw;
    // 估计值下限（1KB/s 的防零保护）。注意：初始带宽假设只是 BtlBw 的
    // 种子，绝不能作为下限——实测慢链路（如 12.5KB/s）时 max(100MB,
    // 样本) 会永远返回 100MB，限速完全失效，队列被洪水填满。
    std::uint64_t m_floor;
    double m_gain{1.0};
    double m_last_late_ratio{0.0};
    bool m_have_late{false};
    bool m_late_rising{false};
    // 应用受限标志：出站队列无积压时发送速率由应用决定，增益恒为探测。
    bool m_app_limited{false};
    // 最近一次 CE 反馈占比（诊断用）。
    double m_ce_ratio{0.0};
    // 是否收到过 CE 标记（L4S 路径激活）：激活后爬升由 on_ce 的 ×2 驱动，
    // 投递率样本不再重复爬升；从未见过 CE 则走无 L4S 路径（FEC 探测）。
    bool m_l4s_seen{false};
    // 最近一次投递率样本（bytes/s）：CE 降速相对它收敛，避免相对 btl 的
    // 乘法随队列未排空而叠加到崩溃。
    std::uint64_t m_last_delivery{0};
    // 无 L4S 时的 FEC 2× 探测：跟跌后置位（附截止时间），让发送侧追加
    // 冗余校验片把线速顶到 ~2×btl，用投递率样本重新发现链路余量。
    bool m_fec_probe{false};
    std::chrono::steady_clock::time_point m_fec_probe_deadline{};
    // 探测片内令牌桶真正卡住过（有积压）：只有探测片建过队列，排空片
    // 才有意义。btl_bw ≈ 应用速率时探测片 pace=demand、无积压，若仍
    // 按时间片排空，会人为制造永不排空的积压（弱网实测 RTT 飙升至
    // 秒级、送达率跌到 ~75%）。
    bool m_probe_pacer_limited{false};
    std::chrono::microseconds m_rtt{0};
    std::chrono::microseconds m_rt_prop{0};
    std::chrono::microseconds m_prev_rtt{0};
    bool m_have_prev_rtt{false};
    // 时间片循环状态：周期起点与已过周期数。
    std::chrono::steady_clock::time_point m_cycle_start{};
    std::uint32_t m_cycle_count{0};
};

} // namespace tight

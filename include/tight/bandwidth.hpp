#pragma once

// 三信号 AIMD 拥塞估计器（GCC 风格，替代 BBR 投递率/时间片循环）。
//
// 信号（全部来自对端报告，每 report_interval 评估一次）：
//   delay-based ：排队延迟 = P50 - RTprop（发送端对报告 P50 做 min filter），
//                 EWMA 平滑趋势
//   late-based  ：迟到率 p = 单程延迟超迟到线（P50+late_buffer）或 CE 标记
//                 的报文占比（替代丢包率——实时流语义：延迟超线即"丢"）
//   ECN/L4S    ：CE 标记报文在接收端计入迟到统计，自动并入 late-based
//
// 调整规则：
//   拥塞（排队延迟 EWMA > 20ms 或 迟到率 > 1%）→ btl ×= 0.5（一次降 50%）
//   恢复（无拥塞）→ 两步台阶法：第一步 btl ×= 1.5，下一报告周期无拥塞再
//                 ×= 1.5（每步 +50%，两步共 +125%；台阶间隔一个报告周期
//                 观察，避免抖动误提）
//   btl 下限 100kbps（kMinBtlBps），防止长距离高 RTT 误判把 btl 打穿。

#include <chrono>
#include <cstdint>
#include <mutex>

namespace tight {

class BandwidthEstimator {
public:
    explicit BandwidthEstimator(std::uint64_t initial_bytes_per_second);

    // 每报告周期调用：三信号评估 + AIMD 调整。
    //  p50_ms    ：对端上报的单程延迟中位数
    //  late_ratio：迟到率（延迟超线 或 CE 标记 或 丢失 的报文占比，0~1——
    //              丢包 = 永远迟到，接收端统计时并入）
    //  ce_ratio  ：CE 标记占比（诊断，已并入 late_ratio 时不直接使用）
    //  rtt_us    ：对端 RTT（发送端平滑 RTT 由 on_ack 维护）
    //  pacer_limited：本地令牌桶限速中（发送被 btl 约束、本地排队）——此时
    //                p50 高是本地限速制造，非链路拥塞，不判拥塞（走恢复
    //                台阶），防止"btl 崩底 → 令牌<供给 → 自造积压 → 误判
    //                拥塞 → 更崩"的死锁
    void on_report(std::uint32_t p50_ms, double late_ratio, double ce_ratio,
                   std::uint32_t rtt_us, bool pacer_limited);

    // 当前 FEC 探测冗余片数（两步台阶提升的第一步使用）：恢复提升时先用
    // FEC 校验片压上负载感知链路（可丢失、不伤业务），第二步确认后移除。
    // fragmenter 据此刻意追加校验片（仅视频通道）。
    std::uint32_t fec_probe_extra() const;

    // RTT 样本（ACK/报告往返）：维护平滑 RTT（供 FEC 关闭、诊断等使用）。
    void on_ack(std::size_t bytes, std::chrono::microseconds rtt);

    std::uint64_t bytes_per_second() const;
    std::chrono::microseconds rtt() const;
    // 诊断：原始 BtlBw 测量值（bytes/s）
    std::uint64_t btl_bw_bps() const;
    // 诊断保留：应用受限状态（AIMD 不依赖投递率，恒不更新）
    bool app_limited_state() const;

private:
    static constexpr std::uint64_t kMinBtlBps = 12500;      // 100kbps 下限（btl 估计值）
    static constexpr double kCongestFactor = 0.5;           // 拥塞：一次降 50%
    static constexpr double kRecoverFactor = 1.5;           // 恢复台阶：+50%
    static constexpr std::uint32_t kDelayThresholdMs = 20;    // 拥塞：排队延迟阈值
    static constexpr double kLateThreshold = 0.02;            // 拥塞：迟到率阈值 2%
    static constexpr std::uint32_t kRecoverDelayMs = 10;      // 恢复：排队延迟须 <10ms
    static constexpr double kRecoverLateThreshold = 0.005;    // 恢复：迟到率须 <0.5%
    static constexpr std::uint32_t kProbeExtraParity = 2;     // 提升第一步的 FEC 探测冗余片数

    mutable std::mutex m_mu;
    std::uint64_t m_btl_bw;
    std::uint64_t m_btl_seed;           // 初始种子（提升上限：btl 不超过种子）
    std::uint64_t m_floor{1024};
    std::uint64_t m_rt_prop_us{0};      // min(P50)（单程 RTprop）
    double m_delay_ewma{0.0};           // 排队延迟 EWMA（ms）
    int m_recover_step{0};              // 提升台阶状态：0=无 1=FEC探测 2=业务替换
    std::uint32_t m_fec_probe_extra{0}; // 当前 FEC 探测冗余（台阶 1 时 = kProbeExtraParity）
    std::chrono::microseconds m_rtt{0};
    std::chrono::microseconds m_rt_prop{0};
    bool m_app_limited{false};
};

}  // namespace tight

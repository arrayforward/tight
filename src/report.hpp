#pragma once

// Internal report packet handling: builds periodic loss/late reports and
// processes incoming reports (ACK pruning + loss retransmission). Not part
// of the public API.
//
// Wire payload layout:
//   offset 0  (4) ack cursor (highest contiguous received sequence)
//   offset 4  (2) late-packet ratio * 10000 (slow-packet rate, NOT loss rate)
//   offset 6  (2) lost sequence count N
//   offset 8  (4) reserved (legacy heartbeat-tick echo, deprecated, always 0)
//   offset 12 (4N) lost sequence numbers
//   offset 12+4N (4) optional: receiver-measured inbound bandwidth (bytes/s)
//                    from the latest speed-test probe train

#include "tight/types.hpp"

#include <chrono>
#include <cstdint>
#include <functional>

namespace tight::tight_detail {

struct Peer;

// Report 处理结果：peer 测速带宽 + peer 本间隔实际接收速率 + 本间隔
// 数据序列丢包率（×10000，供发送端做损失校正，见 handle_report）。
struct ReportResult {
    std::uint64_t probe_bw{0};
    std::uint64_t recv_rate{0};
    std::uint16_t loss_ratio{0};
    // L4S/ECN CE 标记占比 ×10000（接收端测得的 CE 标记数 / 数据包数）。
    std::uint16_t ce_ratio{0};
};

class Report {
public:
    // Retransmits one still-pending packet.
    using ResendCallback = std::function<void(Peer* peer, const PacketHeader& header,
                                              const Bytes& payload)>;

    // Builds the report payload (ack cursor, late ratio, lost sequences,
    // probed bandwidth) and resets the peer's per-interval counters.
    // report_interval 用于推导 NACK 放弃时限（kMaxRetries + 2 个周期）。
    static Bytes build_payload(Peer& peer, std::chrono::milliseconds report_interval,
                               std::uint32_t late_buffer_ms);

    // Handles an incoming report payload: updates the peer's late ratio,
    // prunes acknowledged pendings, and retransmits lost ones via the
    // callback. Returns the peer-measured bandwidth (bytes/s) and the peer's
    // inbound delivery rate over the last interval (bytes/s).
    static ReportResult handle(Peer& peer, const Bytes& payload,
                               const ResendCallback& resend);

private:
    static constexpr std::uint32_t kMaxRetries = 10;
};

}

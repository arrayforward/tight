#include "fragmenter.hpp"

#include "peer.hpp"
#include "wire_format.hpp"

#include "tight/fec.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace tight::tight_detail {

std::uint16_t Fragmenter::compute_parity_count_for(double late_ratio, std::size_t data_count,
                                                   std::uint8_t stage) {
    if (stage == 0) return 0;             // 无长尾，零冗余
    if (stage == 1) return 1;             // 偶发长尾，单校验片
    // 档 2：熵公式 + 高 p 补偿。h(p) 在 p=0.5 峰值、p→1 时趋 0，若直接
    // 采纳，p 很高（链路劣化）时冗余反而暴跌；用 max(h×1.2, p) 保证高
    // p 区间冗余跟随 p 单调上升。冗余率上限 20%（防拥塞/长尾场景冗余
    // 过大加剧排队——L4S 弱网实测 fec=100% 使线上超发 → 恶性循环）；
    // 单分片消息至少 1 片保护（100% 冗余是必要最小保护）。
    if (late_ratio <= 0.0001 || late_ratio >= 0.9999) return 1;
    double h = -late_ratio * std::log2(late_ratio)
             - (1.0 - late_ratio) * std::log2(1.0 - late_ratio);
    double redundancy = std::max(h * kFecSafetyCoefficient, late_ratio);
    std::uint16_t p = static_cast<std::uint16_t>(
        std::ceil(static_cast<double>(data_count) * redundancy));
    if (p < 1) p = 1;
    // 冗余率上限 20%：parity ≤ ceil(data×0.2)，至少 1 片
    std::uint16_t cap = static_cast<std::uint16_t>(
        std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(static_cast<double>(data_count) * 0.20))));
    if (p > cap) p = cap;
    if (p > 100) p = 100;  // 安全阀门
    return p;
}

void Fragmenter::fragment_and_send(Peer& peer, Bytes payload, std::size_t mtu,
                                   const SendFragmentCallback& send_fragment,
                                   std::uint8_t channel,
                                   std::uint16_t channel_fec_extra,
                                   std::uint16_t probe_extra_parity) {
    std::size_t frag_payload = mtu > kHeaderSize ? mtu - kHeaderSize : 64;
    if (frag_payload <= 4) frag_payload = 64;
    std::uint32_t msg_id;
    double late_ratio;
    {
        std::lock_guard<std::mutex> lock(peer.m_mu);
        // message_id 使用独立计数器，不消耗数据序列号 m_sequence_out
        do {
            msg_id = static_cast<std::uint32_t>((peer.m_msg_id_out++) & 0x7FFFFFFFu);
        } while (msg_id == 0);
        // FEC redundancy tracks the peer-reported late-packet ratio (each XOR
        // parity fragment recovers exactly one late/lost fragment).
        late_ratio = peer.m_peer_late_ratio;
    }
    std::size_t total = payload.size();
    std::uint32_t total_be = to_be32(static_cast<std::uint32_t>(total & 0xFFFFFFFFULL));
    // 4 字节总长前缀 + 负载：唯一的整体缓冲（分片以区间视图引用，零拷贝）
    Bytes full(4 + payload.size());
    std::memcpy(full.data(), &total_be, 4);
    std::memcpy(full.data() + 4, payload.data(), payload.size());
    std::size_t real_total = full.size();
    std::size_t data_count = (real_total + frag_payload - 1) / frag_payload;
    if (data_count == 0) data_count = 1;
    std::size_t width = frag_payload;
    // 分段 FEC（迟滞状态机）：未收到对端 report 时起步 2 片（保护初始
    // 关键帧与时钟同步前的无统计窗口）。
    // RTT 长期 >200ms（m_fec_disable，长距离或重拥塞）时关闭全部 FEC：
    // 少量阻塞时冗余恢复有用；大量阻塞时冗余本身挤占带宽加剧拥塞，
    // 让出带宽给数据（起始保护/自适应/通道额外/探测冗余全部归零）。
    std::uint16_t parity_count;
    if (peer.m_fec_disable.load()) {
        parity_count = 0;
    } else if (!peer.m_have_late_report) {
        parity_count = 2;
    } else {
        std::uint8_t& stage = peer.m_fec_stage;
        if (stage == 0) {
            if (late_ratio > 0.003) stage = 1;          // ≥0.3%（333ms 窗口 1 样本）
        } else if (stage == 1) {
            if (late_ratio > 0.012) stage = 2;          // 进入熵公式（>1%×1.2 迟滞）
            else if (late_ratio <= 0.0) stage = 0;      // 无超线退回零冗余
        } else {
            if (late_ratio < 0.008) stage = 1;          // 退出熵公式（<1%×0.8 迟滞）
        }
        parity_count = compute_parity_count_for(late_ratio, data_count, stage);
    }
    {
        static std::atomic<std::uint64_t> dbg_frag_last{0};
        auto dbg_frag_now = std::chrono::steady_clock::now().time_since_epoch().count();
        if (dbg_frag_now - dbg_frag_last.load() > 50000000LL) {
            dbg_frag_last.store(dbg_frag_now);
            std::printf("DBG frag [%llu] ch=%u total=%zu data=%zu parity=%u stage=%u fecOff=%d\n",
                        (unsigned long long)tight::unix_millis(),
                        (unsigned)channel, total, data_count, (unsigned)parity_count,
                        (unsigned)peer.m_fec_stage, (int)peer.m_fec_disable.load());
            fflush(stdout);
        }
    }
    // 通道固定冗余 + 探测冗余：两者都叠加在自适应冗余之上，总校验片
    // 仍受 data_count 约束（最多补足到 data_count 片，超出按 data_count 封顶）。
    parity_count = static_cast<std::uint16_t>(
        parity_count + channel_fec_extra + probe_extra_parity);
    if (parity_count > data_count) parity_count = static_cast<std::uint16_t>(data_count);

    // RS 直接以 full 上的分片区间为输入（不足 width 的尾部按零处理，
    // 与补齐后编码结果一致）；区间/校验缓冲用 thread_local 复用，
    // 热点路径摊销零堆分配。
    thread_local std::vector<ReedSolomon::Span> spans;
    spans.clear();
    spans.reserve(data_count);
    for (std::size_t i = 0; i < data_count; ++i) {
        std::size_t off = i * frag_payload;
        std::size_t len = std::min(frag_payload, real_total - off);
        spans.push_back({full.data() + off, len});
    }
    thread_local std::vector<Bytes> parities;
    ReedSolomon::encode_into(spans, parity_count, width, parities);

    // 实际 FEC 冗余统计（滑动窗口 1s）：transport 读取计算冗余率
    // （video_capacity_bps 用）。encode 线程单线程累计，窗口过期清零重开。
    {
        auto now_ms = tight::unix_millis();
        std::uint64_t ts = peer.m_fec_stat_ts.load();
        if (ts == 0 || now_ms - ts > 1000) {
            peer.m_fec_stat_ts.store(now_ms);
            peer.m_fec_data_pkts.store(0);
            peer.m_fec_parity_pkts.store(0);
        }
        peer.m_fec_data_pkts.fetch_add(data_count);
        if (parity_count > 0) peer.m_fec_parity_pkts.fetch_add(parity_count);
    }

    std::uint16_t cnt = static_cast<std::uint16_t>(data_count + parity_count);
    std::uint16_t d_cnt = static_cast<std::uint16_t>(data_count);
    for (std::size_t i = 0; i < data_count; ++i) {
        send_fragment(&peer, msg_id, static_cast<std::uint16_t>(i), cnt, d_cnt,
                      static_cast<std::uint16_t>(spans[i].size),
                      spans[i].data, spans[i].size, width, true);
    }
    for (std::uint16_t p = 0; p < parity_count; ++p) {
        send_fragment(&peer, msg_id, static_cast<std::uint16_t>(data_count + p), cnt, d_cnt,
                      static_cast<std::uint16_t>(width), parities[p].data(), width, width, true);
    }
}

}

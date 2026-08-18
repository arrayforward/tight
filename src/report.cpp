#include "report.hpp"

#include "peer.hpp"
#include "wire_format.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace tight::tight_detail {

Bytes Report::build_payload(Peer& peer, std::chrono::milliseconds report_interval,
                            std::uint32_t late_buffer_ms) {
    auto now = std::chrono::steady_clock::now();
    std::vector<std::uint32_t> lost_seqs;
    std::uint16_t ratio_val;
    std::uint32_t ack_seq;
    std::uint64_t probe_bw;
    std::uint16_t loss_ratio_val{0};
    std::uint64_t p50_us = 0;  // 本间隔单程延迟中位数（μs，延迟信号上报）
    {
        // m_missing_seqs / m_transit_samples / m_late_samples /
        // m_next_expected_seq / m_probe_* are also mutated by
        // Reassembler::handle_data() and handle_probe() on the receiver
        // thread. Guard with peer.m_mu.
        std::lock_guard<std::mutex> seq_lock(peer.m_mu);
        std::uint32_t rtt_threshold = peer.m_sender_rtt_us > 0 ? peer.m_sender_rtt_us : 10000;
        std::uint32_t loss_threshold = rtt_threshold * 7 / 2;
        if (rtt_threshold < 10000) loss_threshold = 100000;
        const auto give_up_us = std::chrono::duration_cast<std::chrono::microseconds>(
            report_interval * (kMaxRetries + 2)).count();

        // 本间隔数据序列丢包率（×10000）：本间隔跳过（确认丢失）的序号
        // 数 / 游标推进数。发送端用它把投递率样本校正为
        // recv_rate/(1-loss)，避免把丢包误当成链路容量。
        std::uint32_t cursor_before = peer.m_seq_initialized ? peer.m_next_expected_seq : 0;
        std::uint32_t lost_this_interval = 0;

        // 跳过缺口的公共动作：推进游标并消化已收的连续序号
        auto skip_gap = [&peer](std::uint32_t g) {
            if (peer.m_seq_initialized && g == peer.m_next_expected_seq) {
                ++peer.m_next_expected_seq;
                while (peer.m_recv_seqs.count(peer.m_next_expected_seq)) {
                    peer.m_recv_seqs.erase(peer.m_next_expected_seq);
                    ++peer.m_next_expected_seq;
                }
            }
        };

        for (auto mit = peer.m_missing_seqs.begin(); mit != peer.m_missing_seqs.end();) {
            auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - mit->second).count();
            // per-channel 可靠：仅可靠通道（file/data ARQ）的缺口上报 NACK；
            // 不可靠通道（视频纯 FEC）或对端未通告重传时缺口立即跳过，
            // ack 游标照常前进。
            std::uint8_t gch = 0;
            auto cit = peer.m_missing_channel.find(mit->first);
            if (cit != peer.m_missing_channel.end()) gch = cit->second;
            bool reliable = (gch < 8) && peer.m_channel_reliable[gch] && peer.m_peer_retransmit;
            if (!reliable) {
                skip_gap(mit->first);
                if (cit != peer.m_missing_channel.end()) peer.m_missing_channel.erase(cit);
                mit = peer.m_missing_seqs.erase(mit);
                continue;
            }
            if (elapsed_us > give_up_us) {
                // 对端长期未重传（Report 全丢或对端已放弃）：停止上报。
                // 缺口已在越限时跳过，ack 游标不受影响。
                if (cit != peer.m_missing_channel.end()) peer.m_missing_channel.erase(cit);
                mit = peer.m_missing_seqs.erase(mit);
                continue;
            }
            if (elapsed_us > static_cast<long long>(loss_threshold)) {
                // 确认前每个周期重复上报（不擦除）；重传到达时由
                // Reassembler 从 m_missing_seqs 移除，自然停止。
                lost_seqs.push_back(mit->first);
                ++lost_this_interval;
                // 超过 3.5×RTT 即跳过缺口：ack 游标不停滞，发送端可正常
                // 修剪已确认 pending；迟到的重传仍会被正常投递。
                skip_gap(mit->first);
            }
            ++mit;
        }
        // 硬上限：防御极端丢包下 missing 表无限增长（约 3MB/peer/千条级）
        while (peer.m_missing_seqs.size() > 4096) {
            auto first = peer.m_missing_seqs.begin();
            peer.m_missing_channel.erase(first->first);
            peer.m_missing_seqs.erase(first);
        }
        static std::atomic<std::uint64_t> dbg_nack_last{0};
        auto dbg_nack_now = std::chrono::steady_clock::now().time_since_epoch().count();
        if (!lost_seqs.empty() && dbg_nack_now - dbg_nack_last.load() > 200000000LL) {
            dbg_nack_last.store(dbg_nack_now);
            std::printf("DBG nack: seqs=%zu ack=%u th=%u rtt=%u\n",
                        lost_seqs.size(), peer.m_next_expected_seq,
                        (unsigned)loss_threshold, (unsigned)peer.m_sender_rtt_us);
            fflush(stdout);
        }

        // Late-packet ratio over this report interval (slow-packet rate, NOT
        // loss rate). late_buffer_ms > 0 时由延迟直方图按动态迟到线
        // （P50 + late_buffer_ms）重算超线比例，与线定义一致；否则沿用
        // reassembler 按 late_multiplier×RTT 判定的计数。
        double late_ratio = 0.0;
        std::uint64_t line_us = 0, p999_us = 0;
        if (late_buffer_ms > 0 && peer.m_hist_samples > 0) {
            // P50（中位数，对长尾鲁棒）；bin 宽 8ms
            std::uint64_t target = peer.m_hist_samples / 2;
            std::uint64_t cum = 0;
            for (std::size_t b = 0; b < peer.m_latency_hist.size(); ++b) {
                cum += peer.m_latency_hist[b];
                if (cum > target) { p50_us = static_cast<std::uint64_t>(b) * 8000 + 4000; break; }
            }
            line_us = p50_us + static_cast<std::uint64_t>(late_buffer_ms) * 1000;
            // 超线比例 p：直方图中延迟 > line 的样本占比（线所在 bin 按比例折算）
            std::uint64_t over = 0;
            std::size_t line_bin = static_cast<std::size_t>(line_us / 8000);
            if (line_bin + 1 < peer.m_latency_hist.size()) {
                for (std::size_t b = line_bin + 1; b < peer.m_latency_hist.size(); ++b) {
                    over += peer.m_latency_hist[b];
                }
                std::uint64_t bin_lo = line_bin * 8000;
                std::uint64_t frac = 8000 - (line_us - bin_lo);
                over += peer.m_latency_hist[line_bin] * frac / 8000;
            }
            late_ratio = static_cast<double>(over) / static_cast<double>(peer.m_hist_samples);
            if (late_ratio > 1.0) late_ratio = 1.0;
            peer.m_late_line_us = line_us;
            // P99.9 尾部均值（最慢 0.1% 样本的加权平均，延迟曲线诊断）
            std::uint64_t tail_target = std::max<std::uint64_t>(1, peer.m_hist_samples / 1000);
            std::uint64_t tail_sum = 0, tail_cnt = 0;
            for (std::size_t b = peer.m_latency_hist.size(); b-- > 0;) {
                std::uint64_t take = peer.m_latency_hist[b];
                if (tail_cnt + take > tail_target) take = tail_target - tail_cnt;
                tail_sum += take * (static_cast<std::uint64_t>(b) * 8000 + 4000);
                tail_cnt += take;
                if (tail_cnt >= tail_target) break;
            }
            if (tail_cnt > 0) p999_us = tail_sum / tail_cnt;
        } else if (peer.m_transit_samples > 0) {
            late_ratio = static_cast<double>(peer.m_late_samples) /
                         static_cast<double>(peer.m_transit_samples);
        }
        ratio_val = static_cast<std::uint16_t>(late_ratio * 10000.0);
        ack_seq = peer.m_seq_initialized ? (peer.m_next_expected_seq > 0 ? peer.m_next_expected_seq - 1 : 0) : 0;
        // 丢包率 = 本间隔跳过数 / 游标推进数（跳过数最多等于推进数）
        std::uint32_t cursor_advance = ack_seq + 1 - cursor_before;
        if (cursor_advance > 0 && lost_this_interval > 0) {
            loss_ratio_val = static_cast<std::uint16_t>(
                std::min<std::uint32_t>(10000,
                    static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(lost_this_interval) * 10000ULL /
                        cursor_advance)));
        } else {
            loss_ratio_val = 0;
        }
        peer.m_transit_samples = 0;
        peer.m_late_samples = 0;
        // 延迟曲线直方图清零（333ms 窗口刷新；迟到线 m_late_line_us 保留）
        peer.m_latency_hist.fill(0);
        peer.m_hist_samples = 0;
        if (late_buffer_ms > 0 && (p50_us > 0 || line_us > 0)) {
            std::printf("DBG report: p50=%lluus line=%lluus p999=%lluus late=%.2f%% off=%lld rtt=%u\n",
                        (unsigned long long)p50_us, (unsigned long long)line_us,
                        (unsigned long long)p999_us, late_ratio * 100.0,
                        (long long)peer.m_clock_offset_us, (unsigned)peer.m_sender_rtt_us);
            fflush(stdout);
        }

        // Finalize any in-flight speed-test train and attach the measured
        // inbound bandwidth once so the sender can seed its estimator.
        finalize_probe_train(peer, now);
        probe_bw = peer.m_probe_bw_bps;
        peer.m_probe_bw_bps = 0;
    }

    std::uint16_t lost_count = static_cast<std::uint16_t>(lost_seqs.size());
    if (lost_count > 256) lost_count = 256;
    // 尾部可选字段：[probe_bw 4B][recv_rate 4B][loss_ratio 2B][ce_ratio 2B][p50_ms 2B]
    // （追加在丢失列表之后）。新读端按 size 判断字段是否存在；
    // 旧读端只取 probe_bw、忽略多余尾部，向后兼容。
    Bytes payload(16 + lost_count * 4 + 14);
    std::uint32_t ack_seq_be = to_be32(ack_seq);
    std::uint16_t ratio_be = to_be16(ratio_val);
    std::uint16_t lost_be = to_be16(lost_count);
    std::uint32_t reserved_be = 0;  // offset 8: legacy hb-tick echo, deprecated
    std::memcpy(payload.data(), &ack_seq_be, 4);
    std::memcpy(payload.data() + 4, &ratio_be, 2);
    std::memcpy(payload.data() + 6, &lost_be, 2);
    std::memcpy(payload.data() + 8, &reserved_be, 4);
    for (std::uint16_t i = 0; i < lost_count; ++i) {
        std::uint32_t seq_be = to_be32(lost_seqs[i]);
        std::memcpy(payload.data() + 12 + i * 4, &seq_be, 4);
    }
    std::uint32_t bw_be = to_be32(static_cast<std::uint32_t>(probe_bw & 0xFFFFFFFFULL));
    std::memcpy(payload.data() + 12 + lost_count * 4, &bw_be, 4);
    // 本间隔实际接收速率（bytes/s）：接收端直接测量，等于链路的真实
    // 瓶颈速率，不受 ACK 游标跳缺影响。间隔内几乎没有流量（如握手后
    // 第一个间隔只收到几十字节控制包）时上报 0，避免把发送端估计器
    // 压到无意义低值（实测 91B/间隔 曾把 pace 拖到 1KB/s 一秒）。
    {
        std::lock_guard<std::mutex> seq_lock(peer.m_mu);
        std::uint64_t interval_s = static_cast<std::uint64_t>(
            std::max<std::int64_t>(report_interval.count(), 1));
        std::uint64_t recv_rate = 0;
        if (peer.m_recv_bytes >= 4096) {
            recv_rate = peer.m_recv_bytes * 1000ULL / interval_s;
        }
        // L4S CE 占比：ce_marks / (ce_marks + data_pkts)，×10000 打包。
        std::uint64_t ce_marks = peer.m_ce_marks;
        std::uint64_t data_pkts = peer.m_data_pkts;
        std::uint64_t total = ce_marks + data_pkts;
        std::uint64_t ce_ratio_10000 = total > 0 ? (ce_marks * 10000ULL / total) : 0;
        if (ce_ratio_10000 > 10000) ce_ratio_10000 = 10000;
        std::printf("DBG report: recv_bytes=%llu rate=%llu ce=%llu/%llu\n",
                    (unsigned long long)peer.m_recv_bytes, (unsigned long long)recv_rate,
                    (unsigned long long)ce_marks, (unsigned long long)data_pkts);
        fflush(stdout);
        peer.m_recv_bytes = 0;
        peer.m_ce_marks = 0;
        peer.m_data_pkts = 0;
        std::uint32_t rate_be = to_be32(static_cast<std::uint32_t>(recv_rate & 0xFFFFFFFFULL));
        std::memcpy(payload.data() + 16 + lost_count * 4, &rate_be, 4);
        std::uint16_t ce_be = to_be16(static_cast<std::uint16_t>(ce_ratio_10000));
        std::memcpy(payload.data() + 22 + lost_count * 4, &ce_be, 2);
    }
    // 本间隔数据序列丢包率（×10000）：供发送端校正投递率样本
    std::uint16_t loss_be = to_be16(loss_ratio_val);
    std::memcpy(payload.data() + 20 + lost_count * 4, &loss_be, 2);
    // 本间隔单程延迟中位数 P50（ms）：发送端做延迟信号码率控制（拥塞
    // 直接证据，P50 高 → 强制降码率防积压爆炸）。
    std::uint16_t p50_be = to_be16(static_cast<std::uint16_t>(p50_us / 1000));
    std::memcpy(payload.data() + 24 + lost_count * 4, &p50_be, 2);
    return payload;
}

ReportResult Report::handle(Peer& peer, const Bytes& payload, const ResendCallback& resend) {
    if (payload.size() < 12) return {};
    std::uint32_t ack_seq_be = 0;
    std::uint16_t late_ratio_be = 0;
    std::uint16_t lost_count_be = 0;
    std::uint32_t reserved_be = 0;
    std::memcpy(&ack_seq_be, payload.data(), 4);
    std::memcpy(&late_ratio_be, payload.data() + 4, 2);
    std::memcpy(&lost_count_be, payload.data() + 6, 2);
    std::memcpy(&reserved_be, payload.data() + 8, 4);
    std::uint32_t ack_seq = to_be32(ack_seq_be);
    std::uint16_t late_ratio_raw = to_be16(late_ratio_be);
    std::uint16_t lost_count = to_be16(lost_count_be);
    (void)to_be32(reserved_be);  // offset 8: legacy hb-tick echo, ignored
    std::size_t expected = 12U + static_cast<std::size_t>(lost_count) * 4U;
    if (payload.size() < expected) return {};

    // Optional trailing fields: [probe_bw 4B][recv_rate 4B][loss_ratio 2B].
    // probe_bw is the inbound bandwidth measured by the peer from the
    // speed-test probe train; recv_rate is the peer's inbound delivery rate
    // over the last interval; loss_ratio (×10000) is the data-seq loss rate.
    std::uint64_t probe_bw = 0;
    std::uint64_t recv_rate = 0;
    std::uint16_t loss_ratio = 0;
    std::uint16_t ce_ratio = 0;
    std::uint16_t p50_ms = 0;
    if (payload.size() >= expected + 4) {
        std::uint32_t bw_be = 0;
        std::memcpy(&bw_be, payload.data() + expected, 4);
        probe_bw = to_be32(bw_be);
    }
    if (payload.size() >= expected + 8) {
        std::uint32_t rate_be = 0;
        std::memcpy(&rate_be, payload.data() + expected + 4, 4);
        recv_rate = to_be32(rate_be);
    }
    if (payload.size() >= expected + 10) {
        std::uint16_t loss_be = 0;
        std::memcpy(&loss_be, payload.data() + expected + 8, 2);
        loss_ratio = to_be16(loss_be);
    }
    if (payload.size() >= expected + 12) {
        std::uint16_t ce_be = 0;
        std::memcpy(&ce_be, payload.data() + expected + 10, 2);
        ce_ratio = to_be16(ce_be);
    }
    if (payload.size() >= expected + 14) {
        std::uint16_t p50_be = 0;
        std::memcpy(&p50_be, payload.data() + expected + 12, 2);
        p50_ms = to_be16(p50_be);
    }

    {
        std::lock_guard<std::mutex> lock(peer.m_mu);
        // Latest late-packet ratio reported by the peer; drives the FEC
        // redundancy rate and acts as the secondary bandwidth-gain signal.
        double p = static_cast<double>(late_ratio_raw) / 10000.0;
        if (p < 0.0) p = 0.0;
        if (p > 1.0) p = 1.0;
        peer.m_peer_late_ratio = p;
        peer.m_have_late_report = true;
        // 对端上报的 P50 单程延迟（ms）：发送端延迟信号码率控制。
        peer.m_peer_p50_ms = p50_ms;
    }

    std::set<std::uint32_t> lost_seqs;
    for (std::uint16_t i = 0; i < lost_count; ++i) {
        std::uint32_t lost_be = 0;
        std::memcpy(&lost_be, payload.data() + 12 + i * 4, 4);
        lost_seqs.insert(to_be32(lost_be));
    }

    // Snapshot pending under lock, then do the work without holding the lock.
    std::map<std::uint32_t, PendingSend> snapshot;
    std::size_t no_pending = 0;
    {
        std::lock_guard<std::mutex> lock(peer.m_mu);
        for (auto& kv : peer.m_pending) {
            if (kv.first <= ack_seq && !lost_seqs.count(kv.first)) {
                continue;
            }
            if (lost_seqs.count(kv.first) && kv.second.m_retries < kMaxRetries) {
                snapshot[kv.first] = kv.second;
            }
        }
        for (auto& ls : lost_seqs) {
            if (!peer.m_pending.count(ls)) ++no_pending;
        }
        for (auto it = peer.m_pending.begin(); it != peer.m_pending.end();) {
            // 重传次数耗尽的 pending 一并修剪：接收端放弃后 ack 游标
            // 会跳过该缺口，此处防止 m_pending 无限滞留。
            if ((it->first <= ack_seq && !lost_seqs.count(it->first)) ||
                it->second.m_retries >= kMaxRetries) {
                it = peer.m_pending.erase(it);
            } else {
                ++it;
            }
        }
    }

    auto now = std::chrono::steady_clock::now();
    static std::atomic<std::uint64_t> dbg_resend_last{0};
    auto dbg_resend_now = std::chrono::steady_clock::now().time_since_epoch().count();
    if (!lost_seqs.empty() && dbg_resend_now - dbg_resend_last.load() > 500000000LL) {
        dbg_resend_last.store(dbg_resend_now);
        std::printf("DBG resend: snap=%zu no_pending=%zu ack=%u pending_total=%zu\n",
                    snapshot.size(), no_pending, (unsigned)ack_seq, peer.m_pending.size());
        fflush(stdout);
    }
    for (auto& kv : snapshot) {
        resend(&peer, kv.second.m_header, kv.second.m_payload);
        std::lock_guard<std::mutex> lock(peer.m_mu);
        auto it = peer.m_pending.find(kv.first);
        if (it != peer.m_pending.end() && it->second.m_retries < kMaxRetries) {
            it->second.m_last_send = now;
            ++it->second.m_retries;
        }
    }
    return {probe_bw, recv_rate, loss_ratio, ce_ratio};
}

}

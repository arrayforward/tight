#pragma once

#include "tight/types.hpp"
#include "tight/packet_codec.hpp"
#include "tight/fec.hpp"
#include "tight/bandwidth.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tight {

class TightTransport {
public:
    using MessageCallback = std::function<void(const std::string& peer_id, Bytes payload)>;
    using PeerCallback    = std::function<void(const PeerEvent& event)>;
    using CommandCallback = std::function<void(const std::string& peer_id, Bytes payload)>;
    // 接收端消息重组失败回调：某条消息的分片经 FEC 仍无法恢复而丢失时
    // 调用（视频场景 = 一帧丢失，应用层可据此请求关键帧快速恢复画面）。
    // channel 为逻辑通道号（0=视频/数据，1=音频...），区分哪条流。
    // 回调在接收线程内同步执行，须快速返回（只做标记/发命令，勿阻塞）。
    using MessageLossCallback = std::function<void(const std::string& peer_id,
                                                   std::uint8_t channel)>;
    // 文件完整接收回调（file 通道）：重组完成后交付文件名与内容。
    using FileCallback = std::function<void(const std::string& peer_id,
                                            const std::string& name, Bytes data)>;
    // 可靠数据消息回调（data 通道）：消息级去重，每条只投递一次。
    using DataCallback = std::function<void(const std::string& peer_id, Bytes data)>;

    explicit TightTransport(TightConfig config);
    ~TightTransport();

    TightTransport(const TightTransport&) = delete;
    TightTransport& operator=(const TightTransport&) = delete;

    void set_message_callback(MessageCallback callback);
    void set_peer_callback(PeerCallback callback);
    void set_command_callback(CommandCallback callback);
    void set_message_loss_callback(MessageLossCallback callback);
    void set_file_callback(FileCallback callback);
    void set_data_callback(DataCallback callback);

    bool start();
    void stop();

    bool connect(const RemotePeer& remote);
    bool send(const std::string& peer_id, Bytes payload);
    // 发送到指定逻辑通道（通道号 0..7，写入数据报 flags，接收端可识别）。
    // 各通道可经 TightConfig::channel_fec_extra 独立设置额外 FEC 冗余，
    // 例如通道 1 作音频通道单独加强冗余。
    bool send_channel(const std::string& peer_id, Bytes payload, std::uint8_t channel);
    bool send_priority(const std::string& peer_id, Bytes payload, int priority);

    // 发送文件（file 通道 = 2，可靠 ARQ）：先发文件清单（文件名+分块
    // 信息），再逐块发送，块级 NACK 重传保证完整到达。接收端完整重组
    // 后经 set_file_callback 交付。name 长度 ≤65535，data 任意大小。
    // 返回 false = 队列背压或通道未可靠配置。
    bool send_file(const std::string& peer_id, const std::string& name, const Bytes& data);
    // 发送可靠数据消息（data 通道 = 3，ARQ 重传 + 消息级去重 only once）。
    // 接收端经 set_data_callback 交付。
    bool send_data(const std::string& peer_id, Bytes payload);

    // Sends a command packet (control / button-key information). Commands
    // fit in a single datagram, so no fragmentation is needed, and they jump
    // the queue ahead of any pending data. The peer receives them via its
    // CommandCallback in order: out-of-order packets are held for at most
    // 3 RTT before the gap is skipped (late arrivals are then dropped).
    // Returns false when the payload exceeds one datagram.
    bool send_command(const std::string& peer_id, Bytes payload);

    // 运行时动态切换精简模式（本端本地属性，不影响对端）：
    //   true  —— 单线程（receiver/encode/sender 职责并入 reactor 节拍），
    //            64KB 小栈、小缓冲小队列，空闲实例约 76KB
    //   false —— 4 线程（reactor/receiver/encode/sender 全分离）
    // 队列容量按构造时配置固定，切换只改变线程模型；start() 前后均可调用。
    void set_lite_mode(bool lite);
    bool lite_mode() const;

    std::vector<PeerEvent> peers() const;
    std::uint16_t local_port() const;

    // 诊断接口：当前带宽估计（bytes/s，BtlBw×gain 的限速值）与
    // 发送状态。供应用观测拥塞控制行为/测试验证，不影响协议。
    std::uint64_t estimated_bandwidth_bps() const;
    std::uint64_t btl_bw_bps() const;
    bool pacer_app_limited() const;
    bool pacer_limited() const;
    // 对端上报的单程延迟中位数 P50（ms，无该 peer 或未上报时返回 0）：
    // 应用可据此做延迟信号码率控制（P50 高 = 拥塞积压的直接证据）。
    std::uint32_t peer_p50_ms(const std::string& peer_id) const;

    // 出站积压数据报总数（send + encode + outbound 队列）：应用据此检测
    // 网络拥塞导致的发送队列阻塞（本地即时信号，先于对端报告）。
    std::size_t outbound_queue_size() const;
    // 清空出站积压（丢帧止损）：丢弃全部待发送数据报（仅数据面，握手/
    // 报告/命令保留）。视频场景：网络变差编码器降码率前队列已阻塞时调用，
    // 配合应用侧 force_keyframe 让链路快速恢复到最新画面。
    void clear_outbound();
    // file/data 通道待发送负载（字节）：应用据此做带宽预算
    // （file+data 有负载时 video 让出一半 btl）。
    std::uint64_t file_data_pending_bytes() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace tight

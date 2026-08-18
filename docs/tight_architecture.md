# tight — 架构总结

tight 的分层架构、线程模型、模块划分与核心数据流。实现主体在 `src/`（命名空间 `tight::tight_detail`），公共 API 在 `include/tight/`。

## 1. 分层架构

```mermaid
flowchart TB
    subgraph L0["应用层"]
        A1["video-sender / video-player"]
        A2["tight-test / test-file-data"]
        A3["宿主应用"]
    end
    subgraph L1["公共 API (include/tight)"]
        B1["tight.hpp — TightTransport 主接口<br/>set_message_callback / send / send_file / send_data /<br/>send_command / set_lite_mode / clear_outbound ..."]
        B2["types.hpp — TightConfig / PacketType / PacketHeader / PeerEvent"]
        B3["packet_codec.hpp — 线格式编解码 + CRC32"]
        B4["fec.hpp — ReedSolomon 擦除码"]
        B5["bandwidth.hpp — BandwidthEstimator (BBR)"]
    end
    subgraph L2["核心实现 (src, tight_detail)"]
        C1["transport.cpp — TightTransport::Impl<br/>线程模型/状态机/收发/握手/加密接线/通道"]
        C2["reassembler.cpp — 分片重组 + 缺口跟踪 + FEC 恢复"]
        C3["fragmenter.cpp — 分片 + 动态 FEC 冗余"]
        C4["report.cpp — ACK/NACK 报告构建与处理"]
        C5["command.cpp — 命令通道保序"]
        C6["bandwidth.cpp — BBR 带宽估计"]
        C7["fec.cpp / crypto.cpp / packet_codec.cpp / crc32.cpp"]
        C8["address.cpp / wsa.cpp / socket_platform.hpp"]
    end
    subgraph L3["基础设施"]
        D1["peer.hpp — 每对端状态 (PendingSend/IncomingMessage/FileRecv/时钟/FEC档位)"]
        D2["buffer_pool.hpp — 2048B 出站缓冲池 (thread_local 无锁)"]
        D3["blocking_queue.hpp — 有界阻塞队列"]
        D4["small_thread.hpp — 小栈线程"]
        D5["wire_format.hpp — 大端转换 / 通道编码常量"]
        D6["ecn_platform.hpp — L4S CE 标记常量"]
    end
    subgraph L4["系统层"]
        E1["Windows: ws2_32 + WSAStartup"]
        E2["Linux: POSIX sockets"]
    end

    L0 --> L1 --> L2 --> L3 --> L4
    C1 --> C2
    C1 --> C3
    C1 --> C4
    C1 --> C5
    C1 --> C6
```

## 2. 模块职责

| 模块 | 职责 |
| --- | --- |
| `transport.cpp` | 唯一入口 `TightTransport::Impl`：线程编排、Peer 状态机、握手/心跳/保活、加密接线、file/data 通道、诊断接口 |
| `reassembler.cpp` | 接收路径：sequence 缺口跟踪、分片收集、FEC 解码恢复、消息投递、丢帧通知（`on_message_loss`） |
| `fragmenter.cpp` | 发送路径：消息分片、RS 校验片生成、分段 FEC 状态机（stage 0/1/2） |
| `report.cpp` | 周期报告：ack 游标、迟到率、丢失序号、测速带宽、投递率、p50、CE 占比；处理端：重传 + ack 剪枝 |
| `command.cpp` | 命令通道：单报文、保序插队、乱序最多等 3×RTT 后跳号 |
| `bandwidth.cpp` | BBR 估计器：BtlBw max filter、RTprop、增益时间片、app/pacer limited、CE/FEC 探测 |
| `crypto.cpp` | X25519 / SHA-256 / HKDF / AES-256-GCM（纯 C++ 内置实现） |
| `fec.cpp` | Reed-Solomon GF(2⁸) Vandermonde 编码 / 高斯消元解码 |
| `packet_codec.cpp` | 48B 头编解码 + 流式 CRC 校验（零堆分配变体） |

## 3. 线程模型

```mermaid
flowchart TB
    subgraph Normal["普通模式（4 线程，服务器端）"]
        RT["reactor 线程<br/>握手重发/心跳/Online通告/报告/命令flush/掉线检查/收包调度"]
        RC["receiver 线程<br/>recvfrom → 解码 → handle_packet"]
        EN["encode 线程<br/>process_send_queue 取任务 → 分片+FEC → 加密 → 入出站队列"]
        SD["sender 线程<br/>令牌桶 pacing → sendto"]
    end
    subgraph Lite["lite 模式（1 线程，IoT 端侧）"]
        RR["reactor 线程<br/>合并 receiver/encode/sender 全部职责<br/>64KB 小栈"]
    end

    RT --> RC
    RT --> EN
    RT --> SD
    RT -. 运行时切换 set_lite_mode .-> RR

    m_send_queue["m_send_queue<br/>(priority map<int, deque>)"]
    m_encode_queue["m_encode_queue<br/>(有界阻塞队列)"]
    m_outbound_queue["m_outbound_queue<br/>(有界阻塞队列)"]
    RC --> RT
    EN --> m_outbound_queue
    SD --> m_outbound_queue
```

### 3.1 线程分工

| 线程 | 职责 |
| --- | --- |
| reactor | 周期节拍：握手重发退避（500ms 起步、封顶 5s）、心跳、Online 幂等重发、报告发送（333ms/1s）、命令 flush、dead_timeout 掉线检查、`handle_packet` 分发 |
| receiver | 独占 `recvfrom`，保证收包不被发送阻塞 |
| encode | 独占 CPU 密集的 FEC 编码 + 加密，reactor 保持空闲处理收包 |
| sender | 独占 `sendto`，令牌桶背压不阻塞 reactor/encode |

设计原则：**单生产者多消费者**的出站队列 —— reactor/encode 只入队，sender 只出队并 `sendto`，任何 socket 阻塞都不影响协议核心。

### 3.2 lite 模式

- reactor 合并全部职责，receiver/encode/sender 线程被 join 回收；
- 队列容量收紧：`encode≤64`、`outbound≤256`、`queue_limit≤128`、socket≤16KB；
- `flush_interval` 钳制 ≥10ms（省 CPU 唤醒），`drop_log` 强制关闭（静默丢弃）；
- `set_lite_mode()` 运行时切换：先让 reactor 接管再回收线程（反之先清空 `m_lite_pending`）。

## 4. 发送数据流

```mermaid
sequenceDiagram
    participant App as 应用
    participant Impl as TightTransport::Impl
    participant PSQ as process_send_queue (reactor)
    participant ENC as encode_loop
    participant FRAG as fragment_and_send
    participant PKT as send_data_packet
    participant OUT as outbound_queue
    participant SD as sender_loop (sendto)

    App->>Impl: send()/send_channel()/send_file()/send_data()
    Impl->>Impl: send_message() 按优先级入 m_send_queue<br/>(file/data 写入对应通道, 校验 channel_reliable)
    PSQ->>PSQ: 节拍内取消息 → 入 m_encode_queue
    ENC->>ENC: 取任务 → 查 peer → 检查 max_message_bytes
    ENC->>FRAG: fragment_and_send(peer, payload, channel)
    FRAG->>FRAG: 分片 + 计算 data_count +<br/>分段FEC状态机确定 parity_count<br/>+ channel_fec_extra + probe_extra
    FRAG->>PKT: 每个分片回调 send_data_packet
    PKT->>PKT: 分配 sequence/msg_id、AES-256-GCM 加密、<br/>keep_pending(可靠通道)、构建 48B 头
    PKT->>OUT: PooledBytes 入队
    SD->>SD: 令牌桶 → sendto(对端地址)
```

### 4.1 关键点

- **sequence 与 message_id 分离**：数据序列号 `m_sequence_out` 与消息组 id `m_msg_id_out` 独立计数器，避免对端缺口跟踪出现"幽灵序号"导致 ack 冻结；
- **可靠通道**（`channel_reliable`）：分片保留在 `m_pending` 供 NACK 重传，缺口由对端上报；
- **带宽预算**：视频发送端检测 `file_data_pending_bytes()>0` 时码率减半（file+data 各让 25%）；
- **止损**：`outbound_queue_size()>3000` 或 sendFail 1s≥5 → `clear_outbound()` + 应用侧 force_keyframe + `set_bitrate(500k)`（冷却 2s）。

## 5. 接收数据流

```mermaid
sequenceDiagram
    participant SD as sender_loop (sendto)
    participant RC as receiver_loop (recvfrom)
    participant HP as handle_packet
    participant HS as 握手/心跳/报告/命令处理
    participant RS as Reassembler::handle_data
    participant DV as deliver_message
    participant CB as 应用回调

    SD->>RC: UDP 数据报
    RC->>RC: recvfrom → PacketCodec::decode<br/>(流式 CRC, 零拷贝)
    RC->>HP: handle_packet(from, header, payload)
    HP->>HP: 按包类型分发
    alt Data / Parity
        HP->>RS: 序列号缺口跟踪 / 迟到统计 / 分片收集
        RS->>RS: try_assemble: 缺片则 FEC 解码<br/>RS 解码失败 → on_message_loss(peer, channel)
        RS->>DV: 组装完整 → deliver_message
        DV->>DV: tag 0x01/0x02/0x03 → file/data 内部处理<br/>否则走通用消息
        DV->>CB: 回调
    else Report
        HP->>HP: 更新迟到率/投递率/测速带宽/p50/CE<br/>剪枝 ack、重传 NACK 缺口
    else Command
        HP->>HP: 保序投递（3×RTT 内等缺口）
    else 握手/心跳/在线/bye
        HP->>HP: 状态机推进 + 时钟对表
    end
```

### 5.1 reassembler 内部

```mermaid
flowchart TD
    P["收到 Data/Parity 分片"] --> S{"已完成消息?<br/>m_completed 查重"}
    S -- "是" --> D["丢弃"]
    S -- "否" --> G{"seq 缺口跟踪"}
    G --> C["收集进 m_incoming[msg_id]<br/>(防 total_count 不符/idx 越界)"]
    C --> T{"try_assemble<br/>have == data_count?"}
    T -- "是" --> V["FEC 校验片修复缺失<br/>(RS decode 回填)"]
    V -- "恢复成功" --> F["重组 → deliver_message"]
    V -- "恢复失败" --> L["on_message_loss(peer, channel)<br/>丢帧止损/请求关键帧"]
    T -- "否" --> W["等待更多分片/校验片"]
```

## 6. Peer 状态机

```mermaid
stateDiagram-v2
    [*] --> Closed
    Closed --> Handshake: connect() / 收到握手请求
    Handshake --> Established: 收到 HandshakeAck / 回复握手
    Established --> Online: 收到 Online 通告
    Online --> Established: (Online 幂等重发直至确认)
    Handshake --> Closed: 握手超时 / bye
    Established --> Closed: dead_timeout / bye
    Online --> Closed: dead_timeout / bye
    Online --> Online: 每心跳周期重发 Online (幂等)
```

- **握手重发退避**：500ms 起步、封顶 5s，进入 Handshake 状态时重置；
- **Online 幂等重发**：Established/Online 状态下按心跳周期重发（避免单次 Online 丢包导致对端应用层状态永久停在 Established）；
- **时钟对表**：握手时估 offset = `(remote_tick - local_arrival) - rtt/2`，之后每次心跳再同步漂移。

## 7. 内存与缓冲

```mermaid
flowchart LR
    subgraph Buf["缓冲体系"]
        B1["buffer_pool (2048B thread_local 块池)<br/>出站数据报零堆分配复用"]
        B2["m_outbound_queue (PooledBytes)"]
        B3["m_pending (可靠通道重传缓冲)"]
        B4["m_incoming (接收重组, 按 msg_id)"]
        B5["m_files (file 重组上下文)"]
    end
    B1 --> B2
    B3 -. 仅可靠通道 .-> B2
    B4 -. 重组后释放 .-> B2
```

| 场景 | 内存档案 |
| --- | --- |
| 普通模式空闲 | ~460KB |
| lite 模式空闲 | ~76KB |
| lite 无重传在途 | 常数 ~24KB（与码率无关） |
| lite 有重传在途 | ∝ 码率 × 确认窗口，队列封顶最坏 ~5.4MB |

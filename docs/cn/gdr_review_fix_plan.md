# GDR review 修复方案

针对 6 条 review 意见的修复计划。实施顺序：6 → 4 → 5 → 1 → 2 → 3（先小后大，
每步都能单独编译跑测）。

---

## 1. meta 提前序列化，不要等到 `AppendAndDestroySelf`

**结论：可以完全提前到构造函数。**

`DeviceMessage` 的两个构造点（`baidu_rpc_protocol.cpp` 响应侧 ~735 行、请求侧
~1549 行）都被 `!device_attachment().empty()` 守着，所以 `device_size != 0` 恒成立，
framing 一定是 GDRB。而两种落地方式的 header + meta 字节**完全一致**：`body_size`
两边都只算 host 字节，`device_size` 固定在 16 字节头的第 12 字节。所以选哪条线落地
不影响 header 内容，序列化不必等到看见 socket。

改法：

- 成员 `RpcMeta _meta; IOBuf _body;` → `IOBuf _host;`（`device_size` 直接从
  `_device_data.size()` 读，不另存）。
- 构造函数里做 `SerializeRpcHeaderAndMeta(&_host, meta, body->size(),
  device_data->size())`，随后 `_host.append(body->movable())`。
- `AppendAndDestroySelf()` 只剩“选落地位置”这一件事：走第二 QP 就
  `AppendForSend()` + `out->append(_host.movable())`；回退 TCP 就先 D2H 拷到
  `device_buf`，成功后再 `out->append(_host.movable())` + `device_buf`。
  暂存 `device_buf` 的理由不变——拷贝会失败，失败时 host 半边不能已经进 `out`。
- `EstimatedByteSize()` 从估算变精确：`return _host.size();`。原来注释里那段
  “回退路径少报 device_size” 的说明保留（少报的仍然是 device 那部分）。

收益：protobuf 序列化从 socket 单写者的临界路径移到调用方线程；省掉一次 `RpcMeta`
拷贝；`GetProtobufByteSize(_meta)` 从算两次变一次。

---

## 2. 删掉 `DeviceStream` 抽象基类

`DeviceStream` 只有 `RdmaEndpoint` 一个实现，基类确实多余。但不能直接让核心代码去
引用 `rdma::RdmaEndpoint`：`baidu_rpc_protocol.cpp` 要调 `cutn()` / `AppendForSend()`，
需要完整类型，等于把 `brpc/rdma/rdma_endpoint.h` 和 `#if BRPC_WITH_RDMA` 塞进核心
协议文件。

改成**组合**而不是继承：

- `DeviceStream` 变成 `device_attachment.{h,cpp}` 里的具体类（无虚函数），持有收发
  两个缓冲：`_recv_stream`、`_cut_offset`、`_send_stream`、`_send_offset`、
  `_send_queued_bytes`、`_pending_bytes`、`_pending_msgs`。
  这些成员今天就都在 `RdmaEndpoint` 上，且没有一个碰 verbs——纯缓冲。
- `size()` / `cutn()` / `cut_offset()` / `AppendForSend()` / `SetPending()` 全部变
  普通成员函数，实现从 `rdma_endpoint.cpp` 搬到 `device_attachment.cpp`。
- `RdmaEndpoint` 持有一个 `DeviceStream _device_stream` 成员，不再继承它。做 RDMA 的
  部分（post WR、信用）留在 endpoint，读写缓冲走这个成员。
- `AppendForSend()` 里 `!_device_channel_on` 的检查去掉：`GetDeviceStream()` 已经用
  `has_device_channel()` 把门关住了，拿不到 stream 的调用方走不到这里。
- `Transport::GetDeviceStream()` 保持虚函数（`Transport` 本来就是多态的），
  `RdmaTransport` 返回 `&_rdma_ep->device_stream()`。

结果：核心文件依然零 RDMA 依赖、零条件编译，设备数据路径上少一层 vtable。

---

## 3. `RdmaEndpoint` host/device 去重

现状：17 对镜像成员（`_device_sq_current` / `_sq_current` …）+ 8 对镜像函数。

抽一个 `struct QpChannel`，装一条 QP 的全部流控状态：

```
sq_size, rq_size, local_window_capacity, remote_window_capacity,
sq_current, sq_unsignaled, sq_sent, rq_received, accumulated_ack,
sq_imm_window_size, remote_rq_window_size, sq_window_size, new_rq_wrs,
remote_recv_block_size, send_cq_events, recv_cq_events
```

两个实例 `_host` / `_device`。`_unsolicited`、`_unsolicited_bytes` 只有 host 用，留在
endpoint 上。

**合并的函数**（改成接收 `QpChannel&` + `ibv_qp*`）：

- `SendImm` / `SendDeviceImm` —— 只差 qp 和流控实例。
- `SendAck` / `SendDeviceAck` —— 同上。
- `BringUpQp` / `BringUpDeviceQp` —— RESET→INIT→RTR→RTS 的 attr 序列一模一样，只有
  qp、`dest_qp_num`、ECE 不同。
- `HandleCompletion` / `HandleDeviceCompletion` 的 send-completion 半边（信用返还、
  `sq_sent` 推进）抽成公共函数。

**保持独立**（语义真的不同，强行合并只会多出一堆分支）：

- `PostRecv` / `DoPostRecv` vs `PostDeviceRecv` / `DoPostDeviceRecv`：host 走 block
  pool 且有小包拷贝路径，device 走独立注册表且永远 zerocopy。
- `HandleCompletion` 的 recv 半边：IOBuf vs DeviceAttachment。
- `AllocateResources` vs `AllocateDeviceResources`：device 要禁 mlx5
  scatter-to-CQE、复用 host 的 comp_channel。只抽一个公共的建 CQ 小函数。

**代价（先说清楚）**：要改约 154 处 master 侧的成员名（`_sq_current` →
`_host.sq_current` 等），diff 偏大、跟上游容易冲突。但改动全在
`rdma_endpoint.{h,cpp}` 两个文件内，是机械替换。综合“减少冗余”的收益，决定做。

---

## 4. polling + GDR 必须启动期报错

现状：`rdma_endpoint.cpp:1485` 在 polling 模式下静默 `_want_device_channel = false`，
一路走到 `rdma_endpoint.cpp:684` 的需求检查，表现为每条连接
`SetFailed(EDEVICECHANNEL)`——不可解释的建连失败。

改法：在 `RdmaTransport::OptionsAvailableForRdma(ChannelOptions*)` 和
`OptionsAvailableOverRdma(ServerOptions*)` 里（两者都能读到 `socket_mode`，且都在
`GlobalRdmaInitializeOrDie()` 之前跑）加：

```
socket_mode == SOCKET_MODE_RDMA_AND_DEVICE && FLAGS_rdma_use_polling
    → LOG(ERROR) + return false
```

`ContextInitOrDie` 返回 -1，`Server::Start()` / `Channel::Init()` 直接失败。

同一类缺陷的第三例一起修：`!IsGdrAvailable()`（没编 CUDA、或没有可用设备）今天也是
静默降级成同样的 `EDEVICECHANNEL`。加同样的启动期拒绝。

`rdma_endpoint.cpp:1485` 那个 polling 分支随之退化成不可达，去掉条件、保留
`AllocateDeviceResources()` 失败即返回。

---

## 5. GDR + v2 握手启动期拒绝

`rdma_endpoint.cpp:616` 是 `_want_device_channel = (_handshake_version == 3) && ...`，
而 `rdma_handshake.cpp:41` 的 `FLAGS_rdma_client_handshake_version` 默认 2。握手版本
只有客户端挑，所以检查只加在 `OptionsAvailableForRdma`：

```
socket_mode == SOCKET_MODE_RDMA_AND_DEVICE && FLAGS_rdma_client_handshake_version < 3
    → LOG(ERROR) + return false
```

---

## 6. `IsWritable()` 的 bug

现状（`rdma_endpoint.cpp:927-947`）host 窗口检查在前，于是：

| host 窗口 | device 排队 | device 窗口 | 现在 | 应该 |
|---|---|---|---|---|
| 关 | 有 | 开 | false（挂起） | **true** ← bug |
| 关 | 有 | 关 | false | false |
| 关 | 无 | — | false | false |
| 开 | 有 | 关 | false | false |
| 开 | 其他 | — | true | true |

第一行是 bug：host 信用耗尽但 device 队列能排干，`CutFromIOBufList()` 会先
`FlushDeviceSendQueue()`，却在这里被挂起了。

改法：先看 `HasQueuedDeviceData()`，有排队就只由两个 device 窗口决定；没排队再看
host 窗口。

**不能**简单写成“任一 QP 可写就 true”：第 4 行「host 开 + device 有排队 + device 关」
必须继续返回 false，否则 KeepWrite 空转——走到 `IsWritable()` 且 host 窗口开着，说明
host 字节已经发完了，而 `Socket::IsWriteComplete()` 会因为 `HasPendingWrite()` 让
KeepWrite 一直循环。

不会空转的依据：`CutFromDeviceAttachment()` 只在两个 device 窗口都为 0 时返回
EAGAIN，否则必定 post 至少一个 WR，即“device 窗口开 + 队列非空 ⇒ 必有进展”。

---

## 单测（`test/brpc_rdma_unittest.cpp`）

- **1**：构造后 `EstimatedByteSize()` 精确等于 header + meta + body；第二 QP 与 TCP
  回退两种落地下 `out` 前 16 字节（GDRB header）逐字节相同。
- **4 / 5**：启动期拒绝——device mode + `-rdma_use_polling` 下 `Channel::Init()` /
  `Server::Start()` 返回非 0；device mode + `-rdma_client_handshake_version=2` 同理；
  device mode + `!IsGdrAvailable()` 同理。
- **6**：按上表四种组合直接构造流控状态验证。
- **2 / 3**：靠现有 e2e 回归覆盖（无硬件 159 + 硬件 153 + `--config=rdma` 153）。

`docs/cn/gdr_design.md` 里 polling / 握手版本 / `IsWritable` 三处描述随 4、5、6 同步
更新。

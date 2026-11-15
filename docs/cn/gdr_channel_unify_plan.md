# GDR review 第二轮:host/device 通道归一方案

针对 3 条 review 意见的修复计划。实施顺序:**2 → 1 → 3**(先无语义变化的,再语义变化的,
最后大面积机械合并;每步都能单独编译跑测)。改动全部落在
`src/brpc/rdma/rdma_endpoint.{h,cpp}`,外加 `rdma_handshake.cpp` 一处守卫、
`rdma_transport.cpp` 两处取值、`test/brpc_rdma_unittest.cpp`,以及 `docs/cn/gdr_design.md`。

---

## 1. `butil::optional<DeviceChannel> _device`:用「在不在」表示「开没开」

### 现状

`DeviceChannel` 上有三个 bool,含义交叠:

| 字段 | 含义 | 何时写 |
|---|---|---|
| `channel_mode` | `socket_mode == SOCKET_MODE_RDMA_AND_DEVICE`,配置 | 构造,`Reset()` 不清 |
| `wanted` | 本端愿不愿意开 | 握手开头写,失败路径上被清 4 次 |
| `established` | device QP 到 RTS 了 | `BringUpQp()` 末尾 |

而 `_device` 本身永远存在:一条普通 RDMA 连接也带着一整个 `DeviceChannel`(两个 vector、
一个 `DeviceStream`、16 个流控计数器),只是全空。判断「这条连接有没有第二通道」的说法有
四种(`wanted` / `established` / `resource != nullptr` / `remote.device.has_value()`),
四种在某些中间态并不等价 —— 这正是 `ApplyRemoteHello()`、`DoAllocateResources()`、
`BringUpQp()` 里那几处互相打补丁的清标志代码的来源。

### 改法

```cpp
// RdmaEndpoint
const bool _device_channel_mode;          // 唯一保留的配置位,来自 socket_mode
butil::optional<DeviceChannel> _device;   // 装着 == 这条连接有第二通道
```

- `channel_mode` 上移成端点的 `const bool _device_channel_mode`。它必须活过 `Reset()`
  (重连不能悄悄降级成 host-only),而 `_device` 每次握手都要重建,所以这一位放不进
  `DeviceChannel`。它也是 `_device` 之外**唯一**的 device 相关状态。
- `wanted` / `established` 删除,全部由 `_device.has_value()` 表达。
- `has_device_channel()` → `return _device.has_value();`
- 「失败即整条连接失败」:任何一步没成,`_device` 就不存在,跟 host 侧完全一样 ——
  host 分配失败 → 回退 TCP;device 分配失败 → 也回退 TCP(client 端再由需求检查变成
  `EDEVICECHANNEL`)。

### engage 点与各条失败路径的归宿

| 时机 | 动作 |
|---|---|
| client,握手开头(分配之前) | `_device_channel_mode` → `EnableDeviceChannel()`。client 的 hello 必须带 device qp_num,所以 QP 得先存在 |
| server,解析完 client hello | `remote.device.has_value() && _device_channel_mode` → `EnableDeviceChannel()`。对端没问就不 engage,不给不用第二通道的 peer 花一个 QP 和一整套注册接收块 |
| `ApplyRemoteHello()`,对端没给 device 字段 | `_device.reset()`(client 的投机 QP 在这里还回去;server 走不到这个分支) |
| `DoAllocateResources()`,device 资源分配失败 | 直接 `return -1` —— 跟 host 一样,整个 `AllocateResources()` 失败 |
| `BringUpQp()`,device QP 起不来 | 直接 `return -1` —— 同上 |
| RDMA 整体回退 TCP | 需求检查之前统一 `_device.reset()`:device 通道挂在 host QP 上,host 没了它也没了 |
| `Reset()` / 析构 | `_device.reset()` |

于是 client 末尾的需求检查退化成一行:

```cpp
if (ep->_device_channel_mode && !ep->_device) { /* SetFailed(EDEVICECHANNEL) */ }
```

`_device` engaged ⇒ `_device_channel_mode`(两个 engage 点都要求它),所以不存在
「不要第二通道却开出来了」的状态。

### 拆解搬进析构函数

`~DeviceChannel()` 接管今天 `DeallocateDeviceResources()` 的全部工作:ack 掉未 ack 的 CQ
事件 → `delete resource`(QP 先销毁,NIC 才不会再写已挂出去的块)→ 归还 `rbuf_data` 里剩
下的 device 块。于是 `_device.reset()` 就是完整拆解,`DeallocateDeviceResources()` 删除。

`DeallocateResources()` 开头仍然先 `_device.reset()`:device 的两个 CQ 挂在
`_host.resource->comp_channel` 上,必须先于 comp_channel 销毁。这条顺序依赖保留原注释。

### `Reset()` 塌缩

今天 `Reset()` 手抄 10 行清 device 字段,还要重新读一遍
`FLAGS_rdma_device_sq_size/rq_size`(因为 server 在分配之前就要用它们算窗口)。改完只剩
`_device.reset()`:下次 `EnableDeviceChannel()` 时 `DeviceChannel` 的构造函数重新读 flag。
顺带把 host 的深度读取也搬进 `HostChannel` 的构造函数 —— `_host` 是直接成员、只构造一次,
天然保持「host 深度构造时定死、重连不变」的现有语义,两者的差别从注释变成了结构。

### 接受的行为变化

1. **device 块 / device QP 的偶发分配失败,现在会带走整条连接**(server 回退 TCP,
   demanding 的 client 拿 `EDEVICECHANNEL`),不再静默降级成 host-only RDMA。这正是
   review 说的「跟 host 一样就好了」:host 侧的同类失败本来就是这个结果。少一种运行时
   状态组合,也就少一类「连上了但第二通道莫名其妙没开」的现场。
2. **端点内的 `IsGdrAvailable()` 与 `_handshake_version == 3` 检查删除。** 两者都已经由
   `RdmaTransport::ContextInitOrDie()` 在 `Channel::Init()` / `Server::Start()` 时拒绝
   (`docs/cn/gdr_review_fix_plan.md` 第 4、5 条),端点里再查一遍只是把启动期错误翻译成
   建连期错误。唯一还能漏进来的是「`Channel::Init()` 之后把
   `-rdma_client_handshake_version` 改回 2」,那时 v2 hello 带不了 device qp_num,连接以
   `EDEVICECHANNEL` 失败 —— 对运行时改配置来说,这是诚实的结果。
3. **`FillLocalRdmaHello()` 的守卫从 `_device.resource` 改成 `_device.has_value()`**,
   qp_num 取不到就填 0(与 host 侧 `if (BAIDU_LIKELY(ep->_host.resource))` 的写法一致)。
   新设计下 engaged 就意味着资源已经分配成功,唯一的例外是 UT(`g_skip_rdma_init` 跳过分
   配)。效果是 UT 里也能协商出 device 通道 —— 握手校验早就为 UT 放行了 `qp_num == 0` ——
   于是下面的无硬件握手用例才写得出来。现有用例不受影响:所有端到端 device 用例都被
   `--gdr_test_real_device` 挡着。

### 生命周期:不需要新的同步

`_device` 的存在性只被三类线程读:握手 bthread(唯一的写者)、writer、poller。后两者都在
`_rdma_state == RDMA_ON` 之后才可能碰到它(`GetDeviceStream()` / `CutFromIOBufList()` /
`IsWritable()` / `PollCq()` 都在 RDMA_ON 之后),而 `_device.reset()` 只发生在 RDMA_ON
发布之前(握手中)或连接已经静默之后(`Reset()` / 析构)。这跟今天
`DeallocateDeviceResources()` 无锁调用 `_device.stream.Reset()` 所依赖的是同一条所有权规
则,原注释照搬。`DebugInfo()`(`/connections`)是唯一可能在握手中途读到它的并发读者,今天
读 `_device.established` / `_device.resource` 也一样没有同步,竞争面不变。

一个小坑记一笔:`butil::optional` 的拷贝构造是 `= default`,底层 `ManualConstructor` 是裸
字节存储,所以 `optional<DeviceChannel>` 可以被浅拷贝,`DeviceChannel` 上的
`DISALLOW_COPY_AND_ASSIGN` 拦不住。挡它的是 `RdmaEndpoint` 自己的
`DISALLOW_COPY_AND_ASSIGN` 和 `_device` 的 private。

---

## 2. `name()` 做成虚函数,删掉 `is_device`

`QpChannel` 变成抽象基类:

```cpp
virtual ~QpChannel() {}
virtual const char* name() const = 0;   // HostChannel -> "host", DeviceChannel -> "device"
```

`const bool is_device` 和带参数的 `QpChannel(bool)` 构造函数一并删除。今天读 `is_device`
的地方只有两处:

- `TakeAcks()` 里「只有 host 方向会扣信用」→ 改成 `&qc == &_host`。就一处,不值得为它在
  基类挂一个身份位;真正想问的本来也是「你是不是我的 host 通道」。
- `PollTarget::is_device` → 换成 `QpChannel* channel`,见第 3 条(合并之后 `PollCq()` 连
  分流都不需要了)。

vtable 的开销:每个 CQE 多一次虚调用,跟它旁边的 `ibv_post_recv` 比可以忽略;`HostChannel`
上「不做虚函数」的那条注释随之删掉 —— 虚表既然来了,第 3 条就可以顺理成章地用虚函数表达
两条通道真正的差异。

---

## 3. host/device 函数合并

前提:**两种资源结构体统一**。`RdmaDeviceResource` 删除,device 也用 `RdmaResource`,只是
`comp_channel` / `polling_cq` / `next` 留空(两个析构函数今天的唯一差别就是 comp_channel,
而 device 的那个指针是 nullptr,`~RdmaResource()` 原样可用)。于是 `resource` 和
`qp()` 上移到 `QpChannel`,`host_qp()` / `device_qp()` 两个私有访问器删除,`SendImm()` /
`SendAck()` / `DoPostRecv()` 的 `ibv_qp*` 参数全部去掉(从 `qc.qp()` 取)。

| 今天 | 合并后 | 差异怎么表达 |
|---|---|---|
| `HandleCompletion` / `HandleDeviceCompletion` | `HandleCompletion(QpChannel&, ibv_wc&)` | 收:虚 `TakeRecvData(len)`;发:虚 `ReleaseSendBuf(i)` |
| `PostRecv(num, zerocopy)` / `PostDeviceRecv(num)` | `PostRecv(QpChannel&, num)` | 虚 `PrepareRecvSlot(i, &block, &size, &lkey)` / `ReleaseRecvSlot(i)` |
| `ReturnSendCredits<BufArray>()` 模板 | `ReturnSendCredits(QpChannel&, wnd)` | 同上的 `ReleaseSendBuf()`,模板消失 |
| `CutFromIOBufList` / `CutFromDeviceAttachment` 的发送尾巴 | `PostDataWr(QpChannel&, wr, len, must_solicit, force_signal, &rwnd, &swnd)` | 虚 `SolicitWr(len, imm, must_solicit)` |
| `BringUpQp` / `BringUpDeviceQp` | `BringUpOneQp(QpChannel&, remote, dest_qp_num, const ibv_ece* in, ibv_ece* out)` | ECE 参数;device 传 nullptr |
| `DoAllocateResources` / `AllocateDeviceResources` 的公共尾巴 | `ArmChannelCqs(QpChannel&, fatal)` + 虚 `AllocateBuffers()` | CQ 名字取 `qc.name()` |
| `GetAndAckEvents` / `ReqNotifyAllCqs` / `CollectPollTargets` 里的「host,有 device 再来一遍」 | `int CollectChannels(QpChannel**) const` 返回 1 或 2,三处都改成遍历 | — |
| `DebugInfo()` 的两段窗口输出 | `DumpChannel(os, connector, prefix, qc)`,前缀 `rdma_` / `gdr_` | 各自独有的字段留在调用点 |

几处要说明的:

**`HandleCompletion` 返回值统一成「进了 messenger read_buf 的字节数」**,device 的
`TakeRecvData()` 返回 0。于是 `PollCq()` 里 `is_device ? ... : ...` 的分流和
`nr > 0 && !is_device` 的记账全部消失,只剩
`targets[current].channel` 一个指针传进去。

**接收槽的空位约定统一。** device 侧的约定是「`rbuf_data[i] == nullptr` 表示块已经交出去
了,需要新的」;host 侧同样的事情今天靠 `HandleCompletion` 把 `zerocopy` 传给
`PostRecv()` 表达。改成 host 也在 zerocopy 切走之后把槽置空,`PostRecv()` 就只剩一个
「补空槽 → `DoPostRecv` → 环形推进」的公共循环。等价性:zerocopy 切走 ⇒ 需要新块;小包走
拷贝 ⇒ 老块原地复用 —— 跟今天逐字对应。唯一的细微变化是 `byte_len == 0`(纯 ACK WR)
今天会白白换一个新块,改完则原地复用,更省。

**唤醒策略统一。** host 的发送完成只在 `remote_rq_window_size >= capacity/8` 时唤醒
(避免 writer 频繁前后台切换),device 则无条件唤醒(挂在 device 积压上的 writer 没有别的
东西会叫醒它)。合并成一条:

```cpp
if (HasQueuedDeviceData() || qc.remote_rq_window_size >= qc.local_window_capacity / 8)
    _socket->WakeAsEpollOut();
```

有 device 积压就唤醒,覆盖了 device 那条的全部理由;没有积压时两条通道都走 host 的省 CPU
启发式。收到 imm 信用的那半边同理,并入 host 现有的 `sq_window_size > 0 && (...)` 条件。

**发送信号策略不合并成一条规则。** device 每个 WR 都 solicited、且最后一个 WR 强制
signaled;host 靠 `unsolicited` / `unsolicited_bytes` / `accumulated_ack` 三个计数做批量摊
销。给 host 也加上「最后一个 WR 强制 signaled」会让几乎每个 WR 都产生发送完成,CPU 开销正
是那套启发式要省的。所以 `force_signal` 是 `PostDataWr()` 的普通参数(host 传 false),
solicited 决策是虚函数(`HostChannel` 实现那套启发式并顺带更新自己的三个计数,
`DeviceChannel` 直接 `return true`)。

**保持独立的**:两个 `CutFrom*` 的取数据循环(`IOBuf::cut_into_sglist_and_iobuf()` vs
`DeviceAttachment` 的 segment 遍历,数据源真的不同)、`FlushDeviceSendQueue()`、
`IsWritable()`(本来就同时看两条)。

**字段搬家**:`accumulated_ack` 从 `QpChannel` 移进 `HostChannel` —— device 侧只在
`CutFromDeviceAttachment()` 里写 0、从不读(device 恒 solicited),那一行删掉。它和
`unsolicited` / `unsolicited_bytes` 是同一套启发式的三个计数,应该住在一起。

---

## 单测(`test/brpc_rdma_unittest.cpp`)

改:

- `GdrProtocolTest::MakeSocket()` 的 `_rdma_ep->_device.established = true` →
  `_rdma_ep->EnableDeviceChannel()`;`socket_mode` 从 `SOCKET_MODE_RDMA` 改成
  `SOCKET_MODE_RDMA_AND_DEVICE`,让这个测试用 socket 的配置位和通道状态自洽。
- `is_writable_follows_the_queue_that_still_has_work` 里 `set_window(_ep->_device, ...)`
  → `set_window(*_ep->_device, ...)`(4 处)。

加:

- **optional 生命周期**:`EnableDeviceChannel()` 之后 `has_device_channel()` 为真、
  `device_stream()` 非空;`Reset()` 之后两者都为假,且 `HasQueuedDeviceData()` 为假
  (这是 `RdmaTransport::HasPendingWrite()` 在连接死掉之后仍会走的那条路)。
- **合并后的 `HandleCompletion`**:直接造 `IBV_WC_SEND`(`wr_id = n`)分别喂给两条通道,
  断言只有对应通道的 `sq_sent` / `sq_window_size` 前进、另一条不动;`wr_id == 0` 断言
  `sq_imm_window_size += 1`。这是第 3 条唯一「两条路合成一条」且无硬件可测的地方。
- **无硬件握手用例**(`RdmaTest` 那套假 peer 的写法 + GDR fixture 的 flag 设置):
  1. device-mode client 的 v3 hello 带 `device` 子消息,`sq_size` / `rq_size` /
     `block_size` 合法;
  2. 假 server 回一个**不带** `device` 的合法 v3 hello → 连接以 `EDEVICECHANNEL` 失败。
     这条正好盯住第 1 条里「对端拒绝 → `_device.reset()` → 需求检查」这条链。
- 其余靠现有回归覆盖:无硬件 159 + `--config=rdma` 153 + 硬件端到端 GDR 用例。

验证命令(见 memory):

```
https_proxy="10.7.4.2:3128" bazel test --config=test --config=rdma //test:brpc_rdma_unittest
```

---

## `docs/cn/gdr_design.md` 同步更新

- **§9 开头那段**(918-938 行):`DeviceChannel` 字段清单去掉
  `channel_mode` / `wanted` / `established`,改说「`butil::optional<DeviceChannel>`
  装着即开启」;「用 `QpChannel::is_device` 表达比比较地址清楚」那句反过来写成
  `name()` 虚函数 + `&qc == &_host`;`ResetCounters()` 那段补上 host/device 深度语义的
  新表达(host 在 `HostChannel` 构造函数里定一次,device 每次 engage 时重读 flag)。
- **§6.3 / 601 行**:`_device.channel_mode` → `RdmaEndpoint::_device_channel_mode`。
- **§6.2**:「握手回来对端没给 → 建连失败」的规则不变,补一句失败面扩大了:device 资源分
  配失败现在也走同一条路,不再降级。
- **§9.3**:`ReturnSendCredits()` 仍在,但不再是模板;顺带提一句 `PostDataWr()`。

---

## 实施记录:与本方案的偏差

三步都已落地,`bazel test --config=test --config=rdma //test:brpc_rdma_unittest`
163 个用例全过(改动前 159)。与上面写的方案有几处不同,记在这里:

1. **`BringUpOneQp()` 的 ECE 出参是 `butil::optional<ibv_ece>*`**,不是方案里写的
   `ibv_ece*`。这样「查到了」就是这个 optional 变成 engaged,不必再带一个平行的 bool ——
   和第 1 条对 `_device` 做的是同一件事。
2. **`ReqNotifyCq()` 也跟着归一了**(方案的合并表里没有它)。它签名取
   `const QpChannel&` 并从 `solicited_only` 推出「send / recv」,而不是接一个现成的名字
   字符串:`ReqNotifyAllCqs()` 在每次 poller 唤醒时都会调用它,不该在那条路上构造
   `std::string`。
3. **`DumpChannel()` 只收了九个真正共用的字段**,`gdr_remote_recv_block_size` 留在调用点
   (它挪到了共用那一段之后)。于是 `/connections` 的 host 段一个字段都没少,device 段的
   字段顺序略有变化、内容不变。
4. **`CollectChannels()` 不是 `const`**,`CollectPollTargets()` 也跟着去掉了 `const`:
   它们交出去的是可写的通道指针。
5. **「失败面扩大」那句写进了 §6.3 的第 2 条而不是 §6.2**,因为「协商不到就建连失败」这条
   规则本身住在 §6.3;§6.2 只是引用它。
6. **`Reset()` 多清一个 `_host.accumulated_ack`** —— 它从 `QpChannel` 搬进 `HostChannel`
   之后,和 `unsolicited` / `unsolicited_bytes` 一样要在复用连接时归零。
7. **单测是四条而不是三组**:`device_channel_lives_and_dies_with_the_optional`、
   `send_completion_credits_only_the_channel_it_came_on`、
   `device_mode_client_hello_carries_the_device_channel`、
   `server_hello_without_a_device_field_fails_the_connection`(都在 `GdrProtocolTest` 下,
   因为那个 fixture 的 SetUp 已经把 GDR 池和 v3 握手准备好了)。后两条用
   `butil::tcp_listen()` + `accept()` 的假 peer,新增一个 `ReadV3Hello()` 辅助函数
   (`MakeV3Packet()` 的逆),并给测试文件补了 `rdma_device_sq_size` /
   `rdma_device_rq_size` 两个 `DECLARE_int32`。`gdr_design.md` §13.3 记了这四条。
8. **`rdma_transport.cpp` 最后一行没改**(开头说会改两处取值)。它只通过
   `has_device_channel()` / `device_stream()` / `HasQueuedDeviceData()` 三个访问器碰
   device 通道,这三个的名字和语义都没变,optional 的存在性被挡在了访问器后面。

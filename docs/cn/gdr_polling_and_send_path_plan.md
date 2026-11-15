# GDR review 第四轮:polling 模式支持第二通道、发送/完成路径归位

针对 7 条 review 意见的修复计划。实施顺序:**5 → 6 → 7 → 2 → 3 → 4 → 1**。
前三条是互不相干的机械搬迁,2/3/4 是同一片发送/完成路径上的三处,第 1 条最大(它反转了
`gdr_design.md` §10「明确不做 polling」这个结论),放最后,让它站在干净的基础上。

改动落在 `src/brpc/rdma/rdma_endpoint.{h,cpp}`、`src/brpc/rdma/rdma_handshake.{h,cpp}`、
`src/brpc/rdma_transport.cpp`、`test/brpc_rdma_unittest.cpp`,以及 `docs/cn/gdr_design.md`。

---

## 5. `PostRecv` 搬进 `QpChannel`

### 现状

```cpp
int RdmaEndpoint::PostRecv(QpChannel& qc, uint32_t num);
int RdmaEndpoint::DoPostRecv(QpChannel& qc, void* block, size_t block_size, uint32_t lkey);
```

两个函数体里一个 `_socket` / `_state` / `_device` 都没碰,用到的全部是 `qc` 自己的东西:
`qc.qp()`、`qc.PrepareRecvSlot()`、`qc.ReleaseRecvSlot()`、`qc.rq_received`、`qc.rq_size`、
`qc.name()`。它们挂在端点上纯粹是「上一轮把 host 一份 device 一份合成一份」时就近落的脚。

### 改法

```cpp
struct QpChannel {
    // 把 `num' 个接收块挂回 RQ,从 rq_received 那个槽开始。
    int PostRecv(uint32_t num);
private:
    int DoPostRecv(void* block, size_t block_size, uint32_t lkey);
};
```

两处调用点:`HandleCompletion()` 里 `PostRecv(qc, 1)` → `qc.PostRecv(1)`;
`BringUpOneQp()` 里 `PostRecv(qc, qc.rq_size)` → `qc.PostRecv(qc.rq_size)`。
端点上那两个声明删掉。

`DoPostRecv` 做成私有:除了 `PostRecv()` 没有别的调用者,而它只有在配一个 `PrepareRecvSlot()`
的时候才是安全的(失败要 `ReleaseRecvSlot()` 还原)。

### 行为变化

无。纯搬迁。

---

## 6. 预分配池改成 `std::vector`,不再 new/delete 池节点

### 现状

`PreparedResource` 带一个 `next`,池是一条手写的侵入式链表:

```cpp
static PreparedResource* g_rdma_resource_list = nullptr;
```

于是每条连接要 `new` 一个节点(`DeallocateResources()` 归还时)又 `delete` 一个节点
(`DoAllocateResources()` 取用时)—— 而节点里装的只是两个指针,`next` 更是纯粹为了这条
链表而存在的字段,不属于「一个 comp_channel 配一套 host QP/CQ」这件事本身。

### 改法

`PreparedResource` 变成一个**值类型**:去掉 `next`,加上移动语义(两个裸指针的所有权转移),
池变成

```cpp
static std::vector<PreparedResource>* g_rdma_resource_pool = nullptr;  // 和 mutex 一起 new
```

```cpp
struct PreparedResource {
    RdmaCompChannel* comp_channel{nullptr};   // polling 模式下为空
    RdmaResource* resource{nullptr};

    PreparedResource() = default;
    PreparedResource(PreparedResource&& o) noexcept;
    PreparedResource& operator=(PreparedResource&& o) noexcept;
    ~PreparedResource();                      // 先 resource 再 comp_channel,顺序不能反

    // 所有权已经移交给连接,别在析构里销毁。
    void release() { comp_channel = nullptr; resource = nullptr; }
    // 有没有装东西。comp_channel 在 polling 模式下本来就是空的,所以问 resource。
    bool valid() const { return resource != nullptr; }
};
```

`AllocatePreparedResource()` 改成返回值而不是堆指针:

```cpp
static PreparedResource AllocatePreparedResource(uint16_t sq_size, uint16_t rq_size);
// 失败返回一个 !valid() 的空壳,半成品由它自己的析构清掉
```

取用(`DoAllocateResources()`):

```cpp
PreparedResource prepared;
if (_host.sq_size <= FLAGS_rdma_prepared_qp_size &&
    _host.rq_size <= FLAGS_rdma_prepared_qp_size) {
    BAIDU_SCOPED_LOCK(*g_rdma_resource_mutex);
    if (!g_rdma_resource_pool->empty()) {
        prepared = std::move(g_rdma_resource_pool->back());
        g_rdma_resource_pool->pop_back();
    }
}
if (!prepared.valid()) {
    prepared = AllocatePreparedResource(_host.sq_size, _host.rq_size);
    if (!prepared.valid()) {
        return -1;
    }
}
_comp_channel = prepared.comp_channel;
_host.resource = prepared.resource;
prepared.release();
```

归还(`DeallocateResources()`):

```cpp
BAIDU_SCOPED_LOCK(*g_rdma_resource_mutex);
g_rdma_resource_pool->emplace_back(_comp_channel, _host.resource);
```

`GlobalInitialize()` 预热 `reserve(FLAGS_rdma_prepared_qp_cnt)` 之后 `emplace_back`,
`GlobalRelease()` 一句 `delete g_rdma_resource_pool`(vector 析构逐个销毁)。

**为什么是 `vector` 而不是 `deque`/`list`**:取和还都在尾部,`back()`/`pop_back()`/
`emplace_back()` 全是 O(1),而且后进先出正好复用最近用过的那一项(cache 友好,和原来的
头插链表行为一致)。`FLAGS_rdma_prepared_qp_cnt` 默认 1024,`reserve` 之后扩容也不会发生。

### 行为变化

- 池的容量不再无界增长:今天链表想挂多少挂多少,`FLAGS_rdma_prepared_qp_cnt` 只管预热的
  条数;改成 vector 之后**行为保持一致**(不设上限),只是内存从一个个节点变成一段连续数组。
  归还时的 `emplace_back` 在极端情况下会扩容一次(摊还 O(1)),仍然远小于 `ibv_create_qp`。
- 每条连接省掉一次 `new` + 一次 `delete`。

---

## 7. `ParsedHello` 的 host 参数抽成 struct

### 现状

```cpp
struct DeviceChannelParams {
    uint32_t qp_num;
    // The peer's DEVICE receive block size. Unrelated to ParsedHello's
    // block_size, which describes its host RQ.       ← 这句注释就是味道
    uint32_t block_size;
    uint16_t sq_size;
    uint16_t rq_size;
};

struct ParsedHello {
    uint32_t block_size;    // ← 和上面四个字段是同一组东西,只是属于 host
    uint16_t sq_size;
    uint16_t rq_size;
    uint16_t lid;           // ← 这两个才是真正连接级的
    ibv_gid  gid;
    uint32_t qp_num;
    butil::optional<ibv_ece> ece;
    butil::optional<DeviceChannelParams> device;
};
```

device 的四个参数已经是一个 struct 了,host 的同四个参数却平摊在外层,于是:
`remote.block_size` 和 `remote.device->block_size` 看起来像两个不相干的东西(所以才需要那句
「Unrelated to」注释),而 `ApplyRemoteHello()` 里两条通道的窗口计算是逐字重复的 6 行。

### 改法

两条通道广播的本来就是同一组东西,所以是**同一个** struct,不是两个长得一样的:

```cpp
// 一条通道(一个 QP)的参数,对端广播的那一份。两条通道广播的是同一组东西,所以共用
// 一个类型。lid / gid 不在这里 —— 两个 QP 跑在同一个端口上,那是连接的属性。
struct QpParams {
    uint32_t qp_num;
    uint32_t block_size;   // 对端在这条通道的接收块大小
    uint16_t sq_size;
    uint16_t rq_size;
};

struct ParsedHello {
    QpParams host;
    uint16_t lid;
    ibv_gid  gid;
    butil::optional<ibv_ece> ece;
    // nullopt = 对端带不了 device attachment,连接保持 host-only。v2 对端恒为 nullopt。
    butil::optional<QpParams> device;
};
```

`DeviceChannelParams` 这个名字随之消失(它现在不是「device 专属」的东西了)。

跟着改的取值点:`TranslateV2Hello()`、`v3_wire::TranslateHello()`(`out->host.xxx`)、
`BringUpQp()` 两处(`remote.qp_num` → `remote.host.qp_num`)。`ValidRdmaHello()` 里那句
「ParsedHello stores these as uint16_t」的注释改指 `QpParams`。

**顺带塌缩 `ApplyRemoteHello()`。** 这是这条改动真正的收益:两条通道的窗口计算本来逐字
重复,现在参数同型,可以只写一遍:

```cpp
static void ApplyChannelParams(QpChannel& qc, const QpParams& remote) {
    qc.remote_recv_block_size = remote.block_size;
    qc.local_window_capacity  = std::min(qc.sq_size, remote.rq_size) - RESERVED_WR_NUM;
    qc.remote_window_capacity = std::min(qc.rq_size, remote.sq_size) - RESERVED_WR_NUM;
    qc.sq_imm_window_size = RESERVED_WR_NUM;
    qc.remote_rq_window_size.store(qc.local_window_capacity, butil::memory_order_relaxed);
    qc.sq_window_size.store(qc.local_window_capacity, butil::memory_order_relaxed);
}

void RdmaEndpoint::ApplyRemoteHello(const ParsedHello& remote) {
    ApplyChannelParams(_host, remote.host);
    if (!_device.has_value()) {
        return;
    }
    if (!remote.device.has_value()) {
        ... // 那段「对端不要,还回去」的长注释原样留在这里
        _device.reset();
        return;
    }
    ApplyChannelParams(*_device, *remote.device);
}
```

### 行为变化

无。字段搬家 + 一处去重。

---

## 2. `CutFromIOBufList` 分流成两个函数

### 现状

`CutFromIOBufList()` 是 90 行的 host 发送主路径,开头插着 4 行 device:

```cpp
ssize_t RdmaEndpoint::CutFromIOBufList(butil::IOBuf** from, size_t ndata) {
    if (BAIDU_UNLIKELY(g_skip_rdma_init)) { ... }
    CHECK(from != nullptr);
    CHECK(ndata > 0);

    // 20 行注释解释为什么 device 积压要先走
    if (HasQueuedDeviceData() && FlushDeviceSendQueue() < 0) {
        return -1;
    }

    size_t total_len = 0;        // ← 以下全是 host
    ...
}
```

读起来像「host 发送函数顺便干了件 device 的事」,而它实际的结构是
「writer 这一趟要干两件事:先清 device 积压,再切 host 字节」。

### 改法

外层函数只剩分流,host 主体搬进自己的函数:

```cpp
// writer 这一趟的两件事。device 积压先走:它的字节是上一趟没发完的,而
// Socket::IsWriteComplete() 靠 HasPendingWrite() 把 KeepWrite 拽回这里,直到队列排空。
ssize_t RdmaEndpoint::CutFromIOBufList(butil::IOBuf** from, size_t ndata) {
    if (BAIDU_UNLIKELY(g_skip_rdma_init)) {
        errno = EAGAIN;
        return -1;
    }
    CHECK(from != nullptr);
    CHECK(ndata > 0);
    if (FlushDeviceSendQueue() < 0) {
        return -1;
    }
    return CutHostFromIOBufList(from, ndata);
}
```

`HasQueuedDeviceData() &&` 这个前置判断去掉:`FlushDeviceSendQueue()` 开头本来就有同一句
判断并 `return 0`(它是「没有第二通道 / 队列空」时的 no-op,那句注释也在那里),在调用点
再问一遍是重复。`CutHostFromIOBufList()` 就是今天从 `size_t total_len = 0;` 到末尾那一段,
一字不改,私有。

### 行为变化

无。`FlushDeviceSendQueue()` 的空队列快路径和原来的前置判断等价。

---

## 3. `HandleCompletion` 里的 `HasQueuedDeviceData()`:**不合理,要按通道区分**

### 现状

```cpp
case IBV_WC_SEND:
    ...
    if (HasQueuedDeviceData() ||
        qc.remote_rq_window_size.load(...) >= qc.local_window_capacity / 8) {
        _socket->WakeAsEpollOut();
    }

case IBV_WC_RECV:
    ...
    if (qc.sq_window_size.load(...) > 0 &&
        (HasQueuedDeviceData() || remote_rq_window_size >= wnd_thresh || acks >= wnd_thresh)) {
        _socket->WakeAsEpollOut();
    }
```

两处都不看 `qc` 是哪条通道。代码里的理由是「writer 可能正卡在 device 积压上,没有别的东西
会叫醒它」。**这个理由对 host 完成不成立**,推一遍就清楚:

- writer 会去 `WaitEpollOut()` 只可能是 `IsWritable()` 为假;
- `IsWritable()` 在有 device 积压时**只看 device 窗口**(`_device->IsWindowOpen()`);
- 所以「writer 挂着 + 有 device 积压」⟹ device 窗口是关的;
- device 窗口只有 device 通道的完成能打开(device 的 `IBV_WC_SEND` 还 sq 信用、device 的
  `IBV_WC_RECV` 带 imm 还对端 RQ 信用 —— 对端每次重挂接收块都会 `SendAck` 过来)。

于是 host 完成里那一句 `HasQueuedDeviceData()`,**唤醒的 writer 一定立刻发现
`IsWritable()` 还是假、转头再挂回去**。它不是保活,是一次纯浪费的唤醒 —— 而且是按 host
流量的频率浪费:host 是小包高频通道,device 积压一旦卡住,每个 host 发送完成都会踢一次
writer。「不要在 `IBV_WC_SEND` 之后马上唤醒 writer」这条规则本来就是为了避免这种抖动,
这一句正好在它自己要防的事情上开了个口子。

### 改法

问题出在「有没有积压」是个端点级的问题,而这里要问的是**这条通道**的。所以加一个虚函数,
让通道自己回答,基类仍然不带身份位:

```cpp
struct QpChannel {
    // 这条通道有没有「已经收下、但信用还不让发」的积压。host 通道没有这种东西 ——
    // 它的待发字节在 Socket 的写队列里,Socket 自己会叫醒 writer;只有 device 通道
    // 在传输层内部排队(DeviceStream),所以只有它需要靠完成事件把 writer 踢回来。
    virtual bool HasBacklog() const { return false; }
};

struct DeviceChannel : QpChannel {
    bool HasBacklog() const override { return stream.HasQueuedData(); }
};
```

两处 `HasQueuedDeviceData()` 改成 `qc.HasBacklog()`。device 通道上的语义和今天完全一样
(`HasQueuedDeviceData()` 在 `&qc == &*_device` 时就等于 `_device->stream.HasQueuedData()`),
host 通道上那次唤醒不再发生。

端点上的 `HasQueuedDeviceData()` 保留不动:`IsWritable()`、`RdmaTransport::HasPendingWrite()`、
`FlushDeviceSendQueue()` 问的确实是端点级的「这条连接还有没有 device 积压」。

### 行为变化

**device 积压卡住期间,host 发送/接收完成不再踢 writer。** 被去掉的唤醒都是空转
(唤醒后 `IsWritable()` 必为假),所以只少了 CPU 开销,没有少任何一次能推进的唤醒。

---

## 4. device 的 `SolicitWr`:**不该恒为 true,跟 host 用同一份实现**

### 现状

```cpp
// Every device WR is solicited. The bytes are large and rare compared to
// the host channel's, so there is nothing worth batching, and a peer
// sitting on a device payload has no other event to wake it.
bool SolicitWr(size_t, uint32_t, bool) override { return true; }
```

而调用点 `CutFromDeviceAttachment()` 里索性传了 `must_solicit = true`,并留了一句
「must_solicit is moot」。两条注释互相印证了一个事实:**device 这条通道上,
`SolicitWr()` 这个虚函数没有在表达任何差异**。

理由里「大而稀所以没什么可攒的」站不住:host 那三个闸门里有一个正是按**字节**开的,它本来
就是为「大」准备的。

```cpp
} else if (unsolicited_bytes > 1048576) {   // 已经攒了 1MB,这一个必须叫醒对端
```

注意这三个闸门都在累加**之前**判断,而且是 `>` 不是 `>=`。`-rdma_gdr_recv_block_size`
默认正好 1048576,所以一个满块 WR 过不了这道闸:第 1、2 个满块 WR 攒下,第 3 个才被强制
solicit。另一道闸 `unsolicited > local_window_capacity / 4`(device 窗口
`min(16,16) - 3 = 13`,即攒到第 4 个)兜在后面。**host 的逻辑原样用在 device 上,得到的正是
「攒够了再发」,而且上界很小 —— 最多 2MB 或 3 个 WR,先到先算。** 今天这份恒 true 则是
一条都不攒,把这个闸门整个让掉了;把块调小(比如 64KB 的小 attachment 场景)之后差距
更明显。

「对端没有别的事件可以叫醒它」这条也不成立:四个 CQ 共享一个 comp_channel,任何一个
solicited 事件都会让 `PollCq()` 把四个 CQ 一起排干(§4.3)—— 这正是共享 comp_channel 的
设计后果。而 host 逻辑的 `unsolicited > local_window_capacity / 4` 闸门本来就保证「最多攒
四分之一个窗口」,不可能攒到死锁。

### 改法

`SolicitWr()` 从虚函数降为 `QpChannel` 的普通成员,三个计数器
(`unsolicited` / `unsolicited_bytes` / `accumulated_ack`)从 `HostChannel` 搬进 `QpChannel`
(它们本来就只被 `SolicitWr()` 用,和 `local_window_capacity` / `remote_window_capacity`
同属流控那一层),`DeviceChannel` 那个 override 删掉。

调用点按 host 的规则算出真正的 `must_solicit`,不再恒传 true(和 `CutHostFromIOBufList()`
第 1294 行逐字同构,只是「最后一个」的判据从 `current + 1 >= ndata` 换成 `data->empty()`):

```cpp
// 最后一个 WR 必须 solicited,否则对端要等到下一次 flush 才被叫醒;窗口用尽的那一个也
// 必须,否则信用回不来。和 host 路径同一条规则。
const bool must_solicit = (remote_rq_window_size == 1 ||
                           sq_window_size == 1 || data->empty());
if (PostDataWr(*_device, &wr, this_len, must_solicit, data->empty(),
               &remote_rq_window_size, &sq_window_size) < 0) {
```

(`data->empty()` 在这里读的是 `cutn()` 之后的状态,所以它就是「这是本次 flush 的最后一个
WR」。)`force_signal` 仍然是 `data->empty()`,理由不变(device 的 WR 大而稀,最后一个
强制产生发送完成是划得来的),那段注释里「must_solicit is moot」一句删掉。

`QpChannel::Reset()` 接手这三个计数器的清零,`HostChannel::Reset()` 里对应三行删掉 ——
第三轮加的 `host_channel_resets_every_field_it_owns` 会盯住这次搬迁(字段挪到基类,
清零责任也得跟着挪,漏了就红)。

### 行为变化

- **单块 attachment 完全不变**(默认 1MB 块下最常见的一档):唯一那个 WR 就是最后一个,
  `must_solicit` 为真,和今天一样 solicited。
- **多块 attachment 开始攒**:中间的 WR 变成 unsolicited,攒过 1MB(默认块大小下是第 3 个
  WR)或攒过 3 个 WR 时强制 solicit 一次,**每次 flush 的最后一个 WR 一定 solicited**。
  所以「对端收到一个完整 attachment 却没被叫醒」不可能发生。
- 信用快用尽时(`remote_rq_window_size == 1` 或 `sq_window_size == 1`)`must_solicit` 直接
  为真,所以攒批不会把自己饿死。
- 少一个虚函数。`gdr_design.md` §9 里「真正的差异」仍然是八个(去掉 `SolicitWr`,
  加上第 3 条的 `HasBacklog`)。

---

## 1. polling 模式支持第二通道

### 现状

`gdr_design.md` §10 的结论是「明确不做」:

> polling 模式(`FLAGS_rdma_use_polling`)用一个不挂 comp_channel 的 `polling_cq`,与本设计
> 「四个 CQ 共享 comp_channel」的前提直接冲突。它需要独立设计,本期不做。

代码里有四道相应的闸:`DeviceChannelOptionsAvailable()` 启动时拒绝这个组合、
`DoAllocateResources()` 里一句 `DCHECK`、`AllocateDeviceResources()` 里
`CHECK(_comp_channel != nullptr)`、`PollCq()` 里 polling 分支硬编码
`ntargets = 1` 只poll `_host.resource->polling_cq`。

但「前提直接冲突」这个判断下得太重了。comp_channel 是**事件模式**下「一条连接一个事件源」
的实现;polling 模式根本不用事件源,它的等价物是 `polling_cq`,而 CQ 是**每条通道**一个的
东西(`RdmaResource::polling_cq`,第三轮已经把它放对了位置)。所以 polling 模式要支持第二
通道,不需要任何新设计,只需要让 poll 的目标从「一个」变成「每条通道一个」—— 也就是
`CollectPollTargets()` 已经在事件模式下做的那件事。

### 改法

**(a) 目标收集归一。** `CollectPollTargets()` 同时管两种模式:

```cpp
int RdmaEndpoint::CollectPollTargets(PollTarget* out) {
    QpChannel* channels[MAX_CHANNELS];
    const int nchannels = CollectChannels(channels);
    int n = 0;
    if (FLAGS_rdma_use_polling) {
        // 一条通道一个 CQ,两个方向都报到它上面。is_send 一律为 false:CQ 里有 recv
        // CQE,所以每一批都得跑解析循环。
        for (int i = 0; i < nchannels; ++i) {
            out[n].cq = channels[i]->resource->polling_cq;
            out[n].is_send = false;
            out[n].channel = channels[i];
            ++n;
        }
        return n;
    }
    ... // 事件模式:先 recv 后 send,不变
}
```

`PollCq()` 开头随之塌缩:

```cpp
if (!FLAGS_rdma_use_polling && ep->GetAndAckEvents(s) < 0) {
    return;
}
const int ntargets = ep->CollectPollTargets(targets);
```

**(b) polling 模式的空 CQ 早退要让位给轮转。** 今天是

```cpp
if (cnt == 0) {
    if (FLAGS_rdma_use_polling) {
        return;                  // ← 一个目标时对,两个目标时会漏掉第二个
    }
    if (current + 1 < ntargets) { ++current; continue; }
    ...
}
```

改成先轮转、轮完了再按模式分道:

```cpp
if (cnt == 0) {
    if (current + 1 < ntargets) {
        ++current;
        continue;
    }
    if (FLAGS_rdma_use_polling) {
        // 没有 comp_channel 要 re-arm,也没有 epoll 边沿要等:poller 线程马上就会
        // 再进来一次。
        return;
    }
    ... // 事件模式的 re-arm + 从头重轮,不变
}
```

**(c) device 资源分配去掉 polling 的假设。** `AllocateDeviceResources()` 里
`CHECK(_comp_channel != nullptr)` 删掉 —— polling 模式下它本来就是空,而
`AllocateQpCq(nullptr, ...)` 正是「建一个 polling CQ 收两个方向」那条路(第三轮拆分时就已经
写好了,这里只是让它被走到)。arm 那一句跟 host 侧对齐:

```cpp
if (!FLAGS_rdma_use_polling && ArmChannelCqs(*_device, false) < 0) {
    return -1;
}
```

`DoAllocateResources()` 里的 `DCHECK(!(_device.has_value() && FLAGS_rdma_use_polling))`
和它上面那段注释删掉。

**(d) 启动时不再拒绝。** `DeviceChannelOptionsAvailable()` 里 `FLAGS_rdma_use_polling`
那一支删掉(`-rdma_enable_gdr` 和 v3 握手两支保留)。

**(e) 顺带修一个 polling 模式下的销毁顺序问题。** `DeallocateResources()` 今天是:

```cpp
_device.reset();                       // ← 销毁 device 的 CQ
if (_host.resource == nullptr) { ... }
if (FLAGS_rdma_use_polling) {
    PollerRemoveCqSid();               // ← 才把这条连接从 poller 摘下来
}
```

今天无害(polling 模式下 `_device` 永远不会 engage),但第二通道一开,poller 线程可能正在
poll 这条连接、而 device 的 polling CQ 已经被销毁了。`PollerRemoveCqSid()` 提到函数最前面:

```cpp
void RdmaEndpoint::DeallocateResources() {
    if (FLAGS_rdma_use_polling) {
        // 必须在销毁任何 CQ 之前:polling 模式下 poller 线程是这些 CQ 唯一的读者,
        // 而它是顺着 _cq_sid 找过来的。
        PollerRemoveCqSid();
    }
    _device.reset();
    ...
```

`_cq_sid` 为 `INVALID_SOCKET_ID` 时 `PollerRemoveCqSid()` 自己会早退,所以提前调用对
「`_host.resource == nullptr` 就早退」那条路没有影响(那条路上 `StartCqEvents()` 还没跑过,
`_cq_sid` 必为无效)。

### 行为变化

- `SOCKET_MODE_RDMA_AND_DEVICE` + `-rdma_use_polling` 从「启动时报错退出」变成**可用**。
- 一条 GDR 连接在 polling 模式下有**两个 polling CQ**,poller 线程每轮多 poll 一个 CQ。
  这是 polling 模式的固有代价(它本来就是用 CPU 换延迟),而且只发生在真正开了第二通道的
  连接上。
- 事件模式的行为完全不变:`CollectPollTargets()` 的事件模式分支、re-arm 逻辑、
  `GetAndAckEvents()` 一行没动。
- `gdr_design.md` §10 从「明确不做 polling」整节重写,§10.1 的拒绝表少一行。

### 不做的事

**host 和 device 不共用一个 polling CQ。** 一个 CQ 服务多个 QP 是允许的,共用能把 poll 次数
减半、也更贴近「一条连接一个事件源」的说法。但那样一来 `PollCq()` 拿到一个 WC 之后必须靠
`wc.qp_num` 反查是哪条通道(现在是「哪个 CQ 出来的就是哪条通道」),等于在热路径上加一次
查表,而且 `RdmaResource` 就不再是「一条通道的 QP 和它的 CQ」了 —— 第三轮刚把这件事理清。
省一次 `ibv_poll_cq` 不值得换回那笔债。

---

## 单测(`test/brpc_rdma_unittest.cpp`)

改:

- `device_channel_mode_is_refused_in_polling_mode` → **反过来**:
  `device_channel_mode_is_allowed_in_polling_mode`,断言 `-rdma_use_polling` +
  `SOCKET_MODE_RDMA_AND_DEVICE` 的 `Channel::Init()` / `Server::Start()` 现在**成功**。
  这条正是第 1 条改动的落点,而且它盯住的是运维可见的那一面。
- `device_channel_mode_needs_the_v3_handshake` / `..._without_the_gdr_pool` 不动(两条拒绝
  仍然成立)。

加:

- **polling 模式的 poll 目标**(第 1 条,无硬件):`g_skip_rdma_init` 下给 `_host` 和
  `_device` 各塞一个手搭的 `RdmaResource`(只填 `polling_cq`,用两个不同的假地址),置上
  `-rdma_use_polling`,断言 `CollectPollTargets()` 返回 **2**、两项的 `cq` 分别是两条通道的
  `polling_cq`、`channel` 指向对应通道、`is_send` 都是 false。再把 `_device` reset 掉断言
  返回 1。这条抓的就是「polling 分支漏掉第二个 CQ」这个最可能写错的地方。
- **device 的 solicit 攒批**(第 4 条,无硬件):直接调
  `_device->SolicitWr(len, imm, false)`。第一次调用**必然**返回 false(三个闸门都在累加之前
  判断,所以初始状态下谁都过不去),并把 `unsolicited` / `unsolicited_bytes` 累上;用
  1MB 的 len 连调,第 3 次因为 `unsolicited_bytes > 1048576` 返回 true 并把三个计数器清零
  —— 这条断言就是上面那段「默认块大小下第 3 个 WR 才 solicit」的算术。再用小 len 连调,
  攒过 `local_window_capacity / 4` 之后由窗口闸门返回 true。`must_solicit = true` 时无条件
  返回 true 并清零。同一组断言对 `_host` 跑一遍(同一份实现,两条通道同样的答案)——
  这就是「不该是虚函数」的可执行证明。
- **积压唤醒只认自己的通道**(第 3 条,无硬件):`_device->stream` 里塞进待发字节,断言
  `_device->HasBacklog()` 为真而 `_host.HasBacklog()` 为假,并且端点级的
  `HasQueuedDeviceData()` 仍然为真(它问的是连接,不是通道)。
- **`QpChannel::PostRecv()` 的失败还原**(第 5 条,无硬件):`PrepareRecvSlot()` 返回 false
  时 `PostRecv()` 返回 -1 且 `errno == ENOMEM`、`rq_received` 不前进。搬家之后这条保证还在。
- **池的取还**(第 6 条,无硬件):往 `g_rdma_resource_pool` 里 `emplace_back` 一项手搭的
  `PreparedResource`,断言 `DoAllocateResources()` 取走它之后池空、`_comp_channel` /
  `_host.resource` 拿到的正是那两个指针,且池里那一项已经 `release()` 过(不会被 double
  free)。移动语义写错(忘了把源置空)就会在这条的析构里炸。
- **握手参数结构**(第 7 条,无硬件):v3 hello 解析出来的 `ParsedHello` 里
  `host.qp_num` / `host.block_size` / `host.sq_size` / `host.rq_size` 各就各位,
  `device` 那份不受影响;再断言 `ApplyRemoteHello()` 之后两条通道的窗口分别按自己那组参数
  算出来(共用 `ApplyChannelParams()` 之后,算错一条就会同时算错两条)。

真卡门控(`--gdr_test_real_device`)的 `four_cqs_report_to_one_comp_channel` 不动;polling +
device 的端到端只能在真卡上验,不新增门控用例(polling 模式本身现有单测就没有覆盖)。

验证命令(见 memory):

```
https_proxy="10.7.4.2:3128" bazel test --config=test --config=rdma \
    --cache_test_results=no //test:brpc_rdma_unittest
https_proxy="10.7.4.2:3128" bazel test --config=test --config=gdr \
    --cache_test_results=no //test:brpc_rdma_unittest
```

---

## `docs/cn/gdr_design.md` 同步更新

- **§10 整节重写**:从「明确不做:polling 模式」改成「polling 模式」,讲清 comp_channel 是
  事件模式下的事件源、polling 模式的等价物是每条通道一个 `polling_cq`,以及
  `CollectPollTargets()` 是两种模式唯一的分叉点;记下「不共用一个 polling CQ」及其理由。
- **§10.1 的拒绝表**:删掉 `-rdma_use_polling` 那一行,并说明它为什么不再是配错的组合。
- **§4.2 / §4.3**:`polling_cq` 的说明从「GDR 下不支持」改成「polling 模式下一条通道一个」;
  §4.3 末尾补一句 polling 模式走的是同一套 `PollTarget` 轮转。
- **§9**:「真正的差异」那份虚函数清单去掉 `SolicitWr`、加上 `HasBacklog`;补一句三个
  solicit 计数器搬进了 `QpChannel`(因为两条通道用同一份攒批逻辑)。
- **§9.2 / §9.3**:`PostRecv()` 的落点从 `RdmaEndpoint` 改成 `QpChannel`;写发路径那段补上
  `CutFromIOBufList()` → `FlushDeviceSendQueue()` + `CutHostFromIOBufList()` 的分流。
- **§5**:`ParsedHello` 的字段布局改成 `QpParams host` + `optional<QpParams> device`,
  并说明 lid / gid 为什么留在外层。
- **§13.3**:补这一轮新增的用例名,改掉那条反转了的配置拒绝用例。

---

## 实施记录与偏差

七条评审意见按 5 → 6 → 7 → 2 → 3 → 4 → 1 的顺序全部落地,`gdr_design.md` 已按上面那张清单
同步。生产代码没有偏离计划;单测有三处偏离,都是被测试环境的硬限制逼出来的,记在这里,
免得下一个人以为是漏写。

**1. polling 模式的接受断言查的是选项校验函数,不是 `Channel::Init()` / `Server::Start()`。**

计划里这条用例想走用户那条路。但 `Channel::Init()` / `Server::Start()` 会接着
`ContextInitOrDie()` → `InitPollingModeWithTag()` → `PollingModeInitialize()`,而那里要索引
`_poller_groups[tag]`;全局 RDMA 初始化跑完之后再把 `-rdma_use_polling` 打开的进程里那个
vector 是空的,于是越界。改成直接断言
`RdmaTransport::OptionsAvailableForRdma()` / `OptionsAvailableOverRdma()` —— 这一轮改的本来
就是 `DeviceChannelOptionsAvailable()` 一个函数,查它等于查这条评审意见;而且同一个用例
里仍然断言另外两条拒绝在 polling 模式下照样成立,「顺手把整个函数删空」跑不过去。

**2. 池子那条用例测 `PreparedResource` 的移动语义,不经过池。**

计划里写的是往 `g_rdma_resource_pool` 里 `emplace_back` 一项、再让 `DoAllocateResources()`
取走。两个拦路的东西:

- `g_rdma_resource_pool` 在 `rdma_endpoint.cpp` 里是 `static`(内部链接),测试 TU 根本
  看不见它 —— `-fno-access-control` 解决的是访问控制,不是链接性。
- 就算能塞,`DoAllocateResources()` 的 device 那半边会调 `CreateDeviceQp()` →
  `IsAttachmentMemoryDevice()`(默认为真)→ `InitDeviceQpCreationImpl()`,后者解引用
  `g_context->device->name`;`g_skip_rdma_init` 的进程里 `g_context` 是空指针。

于是 `prepared_resource_is_a_movable_value` 直接压住这次改动真正引入风险的地方 —— 值语义:
从 vector 里移动构造出来再 `pop_back()` 不能把两半销毁掉,`release()` 之后析构不销毁,移动
赋值必须销毁目标原先持有的东西。计划里那句「移动语义写错(忘了把源置空)就会在这条的析构
里炸」在这个形状下照样成立,而且断言得更直接(数 `IbvDestroyCompChannel` 的调用次数)。

顺带一条同源的限制:这一轮**没有**任何用例走得进 `DoAllocateResources()` 的 device 半边,
理由就是上面第二点。那条路仍然只有 `--gdr_test_real_device` 的真卡用例覆盖。

**3. §5 那条用例手搭 `ParsedHello`,不从 v3 hello 的线上字节解析出来。**

计划里是「解析出来的 `ParsedHello` 里 host 四个字段各就各位」+「`ApplyRemoteHello()` 之后
两条通道的窗口各按自己那组参数算」。后半截照做了(`ApplyRemoteHello()` 是真调的);前半截
改成直接构造 `ParsedHello`,因为这条用例要断言的是**两组参数不会串**,而串不串只取决于
`ApplyChannelParams()` 拿到哪个 `QpParams`,跟字节怎么来的无关;手搭还能给两条通道喂
明显不同的数字(host `{64, 48}` / device `{40, 24}`),解析路径拿不到这种对照。线上字节
那一侧本来就有 `device_mode_client_hello_carries_the_device_channel`(发)和
`server_hello_without_a_device_field_fails_the_connection`(收)两条用例在守。

**一个踩过的坑,值得记一句**:`polling_mode_walks_one_cq_per_channel` 里栈上的
`RdmaResource` 析构时会把假 CQ 句柄送进 `IbvDestroyCq()`,而 `g_skip_rdma_init` 的进程从没
dlopen 过 verbs 库,那是个空函数指针 —— 段错误,而且因为当时输出接了 `| tail`,看到的是
「静默停止 + EXIT=0」(tail 的退出码)。用例结束前必须手工把那些句柄置空;查这类崩溃时
别让管道吃掉退出码。

验证:

```
bazel test --config=test --config=rdma --cache_test_results=no //test:brpc_rdma_unittest
    → 173 tests PASSED
bazel test --config=test --config=gdr  --cache_test_results=no //test:brpc_rdma_unittest
    → 180 tests PASSED
```

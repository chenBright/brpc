# GDR review 第三轮:资源分层、构造期 engage、Reset 归位

针对 3 条 review 意见的修复计划。实施顺序:**2 → 3 → 1**(2 和 3 都是小范围、无行为
变化的归位,先做完让第 1 条的大改动站在干净的基础上)。改动落在
`src/brpc/rdma/rdma_endpoint.{h,cpp}`、`src/brpc/rdma_transport.cpp` 一处构造、
`test/brpc_rdma_unittest.cpp`,以及 `docs/cn/gdr_design.md`。

---

## 1. `AllocateQpCq`:comp_channel 是连接的,不是 QP 的

### 现状与问题

`gdr_design.md` 说一条 GDR 连接是「双 QP / 四 CQ / 共享 comp_channel」。代码里对应的
结构却是:

```cpp
struct RdmaResource {
    RdmaResource* next;      // 预分配池的链表指针
    ibv_qp* qp;              // 一条通道的 QP
    ibv_cq* polling_cq;      // 一条通道的 CQ(polling 模式)
    ibv_cq* send_cq;         // 一条通道的 CQ
    ibv_cq* recv_cq;         // 一条通道的 CQ
    ibv_comp_channel* comp_channel;   // ← 整条连接的
};
```

前五个字段是**每条通道一份**,最后一个是**每条连接一份**。`AllocateQpCq()` 把两件事
捏在一个函数里做,于是:

1. **device 侧没法复用它。** device 的两个 CQ 必须挂在 host 已经建好的 comp_channel
   上,而 `AllocateQpCq()` 只会自己新建一个。结果 `AllocateDeviceResources()` 把建 CQ
   建 QP 的 25 行原样抄了一遍 —— 正是上一轮 review「device 很多逻辑跟 host 差不多,尽量
   复用」还没清掉的最后一处。
2. **抄的那一份和原件不一致,而且原件是错的。** 抄过去的版本按 `_device->sq_size` /
   `rq_size` 建 CQ,是对的;`AllocateQpCq()` 却无论要多大的队列,CQ 一律建
   `FLAGS_rdma_prepared_qp_size`(128)条:

   ```cpp
   resource->send_cq = AllocateCq(FLAGS_rdma_prepared_qp_size, ...);
   resource->recv_cq = AllocateCq(FLAGS_rdma_prepared_qp_size, ...);
   resource->qp = AllocateQp(resource->send_cq, resource->recv_cq, sq_size, rq_size);
   ```

   `DoAllocateResources()` 只在 `sq_size <= 128 && rq_size <= 128` 时才走预分配池,**走到
   `AllocateQpCq()` 现场分配的那条路,恰恰就是队列比 128 大的那条**。于是
   `-rdma_sq_size=1024` 会得到一个 1024 深的 SQ 配 128 条的 send CQ:CQ 装不下队列能产生
   的完成事件,溢出后 CQ 进入 error 状态、QP 跟着废掉。polling 分支同理
   (`2 * FLAGS_rdma_prepared_qp_size`)。

   这条是 GDR 之前就有的上游 bug(`HEAD~1` 里一模一样),不是这次引入的;但它正是
   「comp_channel 和 QP/CQ 混在一个函数里」导致的:参数里明明有 `sq_size` / `rq_size`,
   建 CQ 时却没人想起来用。修掉它顺便让 host 和 device 的建法真正变成同一份。
3. `~RdmaResource()` 得靠一句注释说明「device 的 comp_channel 是空的,所以析构只销毁
   自己拥有的东西」;拆解顺序(device 的 CQ 必须先于 host 的 comp_channel 销毁)也只能
   靠 `DeallocateResources()` 开头的注释守住。类型本身说不出这件事。

### 改法:拆成三个各管一件事的类型

```cpp
// 一条连接的 completion channel。连接上所有 CQ 的事件都报到这里 —— 四个 CQ 一个 fd、
// 一次 epoll 注册,PollCq() 一次唤醒排干四个 CQ(gdr_design.md 第 10 节)。
// ibv_create_cq() 把 CQ 和 comp_channel 绑死,所以它必须先于任何 CQ 建出来、再传给
// AllocateQpCq(),而不是当作某个 QP 的副产品。polling 模式下为空。
struct RdmaCompChannel {
    ibv_comp_channel* channel{nullptr};
    static RdmaCompChannel* Create();   // 建 + close-on-exec + non-blocking
    ~RdmaCompChannel();
    int fd() const { return channel->fd; }
};

// 一条通道的 QP 和它上报的 CQ。一条 GDR 连接有两份(host 一份、device 一份),共用上面
// 那一个 RdmaCompChannel。它拥有的就是这里列出的这些,comp_channel 是连接的、不是它的。
struct RdmaResource {
    ibv_qp* qp{nullptr};
    ibv_cq* polling_cq{nullptr};        // polling 模式:一个 CQ 收两个方向
    ibv_cq* send_cq{nullptr};
    ibv_cq* recv_cq{nullptr};
    ~RdmaResource();
};

// 预分配池的一项:一个 comp_channel 加上在它上面建好的 host QP/CQ。两者只能成对预分配、
// 成对回收 —— CQ 建出来之后就改不了它属于哪个 comp_channel。
struct PreparedResource {
    PreparedResource* next{nullptr};
    RdmaCompChannel* comp_channel{nullptr};   // polling 模式下为空
    RdmaResource* resource{nullptr};
    ~PreparedResource();
};
```

`RdmaEndpoint` 多一个 `RdmaCompChannel* _comp_channel`(拥有),`RdmaResource` 去掉
`next` 和 `comp_channel` 两个本来就不属于它的字段。

`AllocateQpCq()` 改成「在给定的 comp_channel 上建一条通道的 QP 和 CQ」:

```cpp
// cc == nullptr 表示 polling 模式,改建一个 polling CQ 收两个方向。
// CQ 的深度就是队列的深度:比喂它的队列浅的 CQ 会溢出。
static RdmaResource* AllocateQpCq(RdmaCompChannel* cc, uint16_t sq_size,
                                  uint16_t rq_size, bool for_device,
                                  const char* name);
```

于是:

| 调用点 | 改成 |
|---|---|
| `GlobalInitialize()` 预热 | `AllocatePreparedResource(prepared_qp_size, prepared_qp_size)` |
| `DoAllocateResources()` 池里没货 | 同一个 `AllocatePreparedResource(_host.sq_size, _host.rq_size)` |
| `AllocateDeviceResources()` | `AllocateQpCq(_comp_channel, _device->sq_size, _device->rq_size, true, _device->name())`,那抄来的 25 行删掉 |

`AllocatePreparedResource(sq, rq)` 是「建 comp_channel(非 polling)+ 在它上面建
host QP/CQ」的唯一一份实现,预热和现场分配共用。`DoAllocateResources()` 拿到一项之后
把两个指针移进 `_comp_channel` / `_host.resource`,节点本身 `delete` 掉;
`DeallocateResources()` 回收时反过来 `new` 一个节点挂回池里。每条连接多一次几十字节的
`new`/`delete`,相对 `ibv_create_qp` 可以忽略。

### 顺带修掉的一个泄漏

`DeallocateResources()` 开头今天是:

```cpp
_device.reset();
if (_host.resource == nullptr) {
    return;
}
```

comp_channel 搬进 `_comp_channel` 之后,这个早退会漏掉一种情况:
`RdmaCompChannel::Create()` 成功、`AllocateQpCq()` 失败,此时 `_host.resource` 是空但
`_comp_channel` 不是。早退前补一句 `delete _comp_channel; _comp_channel = nullptr;`。
(今天这种情况下 comp_channel 挂在半成品 `RdmaResource` 上,由 `unique_ptr` 在
`AllocateQpCq()` 里销毁,所以今天不漏 —— 这是拆分新引入的、必须一起处理的边。)

### 跟着改的取值点

- `StartCqEvents()`:`options.fd = _comp_channel->fd()`
- `GetAndAckEvents()`:`IbvGetCqEvent(_comp_channel->channel, ...)`
- `DeallocateResources()` 销毁路径:先 `delete _host.resource`(销毁 CQ)再
  `delete _comp_channel`;`RemoveConsumer(fd)` 的顺序不变(销毁 comp_channel 会关掉 fd,
  必须先摘出 epoll)。
- `~DeviceChannel()` 里「`~RdmaResource()` 会发现 comp_channel 是空的」那句注释删掉 ——
  现在 `RdmaResource` 根本没有这个字段,类型自己就说清楚了。
- `AllocateDeviceResources()` 的 `CHECK(_host.resource->comp_channel != nullptr)` 改成
  `CHECK(_comp_channel != nullptr)`。

### 行为变化

**CQ 深度跟着队列走。** 队列 ≤ 128 时(默认配置、预分配池)完全不变;
`-rdma_sq_size` / `-rdma_rq_size` 大于 128 时,CQ 从 128 变成实际队列深度,即从「迟早
溢出」变成正确。polling CQ 从 `2 * 128` 变成 `sq_size + rq_size`。device 侧本来就是对的,
不变。

---

## 2. `_device_channel_mode` 删除:构造函数里就 engage

### 现状

`RdmaEndpoint` 带一个 `const bool _device_channel_mode`,由
`RdmaTransport::Init()` 从 `options.socket_mode` 算出来传进构造函数,读三处:

| 位置 | 用途 |
|---|---|
| client 握手开头(754) | 配置要第二通道 → `EnableDeviceChannel()` |
| client 需求检查(829) | 配置要、但最后没有 → `EDEVICECHANNEL` |
| server 解析完 client hello(972) | 对端问了、且本端配置愿意 → `EnableDeviceChannel()` |

而它是 `Socket::_socket_mode` 的一份拷贝:`socket_mode.h` 已经写明
`SOCKET_MODE_RDMA_AND_DEVICE` 就是「这条连接要第二通道」的全部含义。

### 改法

`Socket::Create()` 里 `_socket_mode = options.socket_mode;` 在
`_transport->Init(this, options)` **之前**(`socket.cpp:738` / `741`),所以端点在构造
函数里就能读到自己的 socket mode。于是:

- 构造函数退回单参数 `explicit RdmaEndpoint(Socket* s)`,`RdmaTransport::Init()` 少传一个
  参数;`const bool _device_channel_mode` 删除。
- 构造函数末尾 `ResetDeviceChannel()`:socket mode 是
  `SOCKET_MODE_RDMA_AND_DEVICE` 就 `_device.emplace()` 并挂上 pending hook。
  (`EnableDeviceChannel()` 改名成这个,因为它现在表达的是「回到配置该有的样子」,
  构造函数和 `Reset()` 各调一次。)
- **client 的 754 整段删掉** —— `_device` 一出生就在。那段注释(为什么是「提案」而不是
  能力探测)搬到构造函数。
- **server 的 972 整段删掉** —— `ApplyRemoteHello()` 里已经有
  `if (!remote.device.has_value()) { _device.reset(); return; }`,server 走的正是这一支。
  顺序也对:server 是 `ApplyRemoteHello()`(975)在 `AllocateResources()`(977)**之前**,
  所以没问的 client 一个 device QP 都不会让我们花。那段「同意是 opt-in、不能让 client
  替我们花注册内存」的注释搬进 `ApplyRemoteHello()` 的这一支,它现在同时解释 client 收回
  投机 QP 和 server 拒绝没问过的 peer 两件事。
- **需求检查(829)** 改读 `_socket->socket_mode() == SOCKET_MODE_RDMA_AND_DEVICE`。
  这是唯一还需要知道「配置要不要」的地方,而它本来就该问 socket mode —— 那段注释
  第一句说的就是「SOCKET_MODE_RDMA_AND_DEVICE 是硬要求,这里是唯一的执行点」。

于是 device 通道的状态彻底只剩 `_device` 一个:**在 = 有**,而「配不配」不再有副本,
每次问都问 socket。

### 行为变化

1. **`DeviceChannel` 的构造时机从握手开头提前到 socket 构造。** 它读
   `-rdma_device_sq_size` / `-rdma_device_rq_size` / `-rdma_gdr_recv_block_size` 三个 flag,
   所以运行期改这三个 flag,影响的从「下一次握手」变成「下一个 socket / 下一次
   `Reset()`」。对 gflags 这类启动期配置来说没有实际差别。
2. **server 端每条连接都会先造一个空的 `DeviceChannel`**,client 没问的话在
   `ApplyRemoteHello()` 里还回去。代价是几个空 vector 和一个 `DeviceStream` 的构造析构 ——
   注册内存是 `AllocateBuffers()` 才花的,而那要等 `_device` 活过
   `ApplyRemoteHello()`,所以「不让没问的 client 花我们的内存」这条性质不变。
3. `~RdmaEndpoint()` 走 `Reset()`,末尾会重新 emplace 一个马上就被成员析构掉的
   `DeviceChannel`。三次读 flag 的开销,留一句注释说明,不为它拆 `Reset()`。

---

## 3. `RdmaEndpoint::Reset()`:host 的字段归 `HostChannel` 自己清

### 现状

```cpp
void RdmaEndpoint::Reset() {
    DeallocateResources();
    _state.store(UNINIT, ...);
    _handshake_version = 0;
    _outgoing_ece.reset();
    _host.resource = nullptr;      // ← 以下 8 行都是 _host 的
    _host.ResetCounters();
    _cq_sid = INVALID_SOCKET_ID;
    _host.sbuf.clear();
    _host.rbuf.clear();
    _host.rbuf_data.clear();
    _host.input_processor.Reset();
    _host.unsolicited = 0;
    _host.unsolicited_bytes = 0;
    _host.accumulated_ack = 0;
}
```

端点在手抄 `HostChannel` 的字段清单,而这份清单端点看不见(`sbuf` / `unsolicited` /
`accumulated_ack` 都是 `HostChannel` 私有的东西)。加一个字段就要记得回来改这里,上一轮
`accumulated_ack` 从 `QpChannel` 搬进 `HostChannel` 时就差点漏掉。

### 改法

`ResetCounters()` 改名 `QpChannel::Reset()`,并把 `resource = nullptr` 一起收进去
(它也是 `QpChannel` 的字段);`HostChannel::Reset()` 覆盖它:

```cpp
// QpChannel:清掉基类这一层的全部字段。sq_size / rq_size 不动 —— 那是本端的配置,
// 每条通道定一次,重连不能换个深度上来。
void QpChannel::Reset() {
    resource = nullptr;
    local_window_capacity = 0;
    ...
    send_cq_events = 0;
    recv_cq_events = 0;
}

// HostChannel:自己那几个字段,加上基类的。
void HostChannel::Reset() {
    QpChannel::Reset();
    sbuf.clear();
    rbuf.clear();
    rbuf_data.clear();
    input_processor.Reset();
    unsolicited = 0;
    unsolicited_bytes = 0;
    accumulated_ack = 0;
}
```

端点塌缩成:

```cpp
void RdmaEndpoint::Reset() {
    DeallocateResources();          // 连 _device 一起带走
    _state.store(UNINIT, butil::memory_order_relaxed);
    _handshake_version = 0;
    _outgoing_ece.reset();
    _cq_sid = INVALID_SOCKET_ID;
    _host.Reset();
    ResetDeviceChannel();           // 回到构造完的样子(第 2 条)
}
```

`Reset()` 做成虚函数还是普通成员:做成**虚函数**。`DeviceChannel` 不需要它(它每次是整
个销毁重建的),但基类已经有 vtable(上一轮的 `name()`),而「清空自己这一层」本来就是
每个子类该能回答的问题;将来真要复用 device 通道也不必回头改签名。

`AllocateResources()` 失败路径上那三行 `_host.sbuf.clear()` / `rbuf.clear()` /
`rbuf_data.clear()` 保持原样:那里只是把半分配的缓冲扔掉,不该顺手清掉刚算好的窗口。

---

## 单测(`test/brpc_rdma_unittest.cpp`)

改:

- `GdrProtocolTest::MakeSocket()` 里的 `EnableDeviceChannel()` 调用删掉 —— socket mode 已
  经是 `SOCKET_MODE_RDMA_AND_DEVICE`,构造函数会做。
- `_ep->_device_channel_mode` 的断言(4627)改成断言 socket mode。

加:

- **构造期 engage**(第 2 条,无硬件):device-mode socket 刚建好、握手一步都没走时
  `has_device_channel()` 就为真;`SOCKET_MODE_RDMA` 的 socket 为假。再
  `Reset()` 一次,两者都回到各自构造完的样子 —— 这条盯住「`Reset()` 之后连接复用不会
  悄悄降级成 host-only」。
- **`HostChannel::Reset()` 的字段清单**(第 3 条,无硬件):把 `_host` 的每个字段都写成
  非零值(计数器、`sbuf`/`rbuf`/`rbuf_data`、`unsolicited`/`unsolicited_bytes`/
  `accumulated_ack`),`Reset()` 之后逐个断言归零,并断言 `sq_size` / `rq_size` **没有**被
  清。漏清一个新字段就会被这条抓住。
- **comp_channel 的生命周期**(第 1 条,无硬件):`IbvDestroyCompChannel` 是个全局函数
  指针,换成计数桩即可。给端点塞一个 `_comp_channel`、不给 `_host.resource`,
  `DeallocateResources()` 之后断言桩被调了一次且 `_comp_channel` 为空,再调一次断言不会
  重复销毁 —— 正是上面「拆分新引入的那条泄漏边」。
- **`RdmaCompChannel::Create()` 的 fd 属性**(第 1 条,无硬件):`IbvCreateCompChannel`
  换成返回一个 `fd` 指向真实 pipe 的假 channel,断言 `Create()` 之后该 fd 带
  `FD_CLOEXEC` 和 `O_NONBLOCK`。这两条今天埋在 `AllocateQpCq()` 里没人测。
- **四 CQ 一 channel 的形状**(第 1 条,`--gdr_test_real_device` 门控):一条真实 GDR 连接
  建立之后,断言两个 QP 互不相同、四个 CQ 互不相同且都非空、四个 `ibv_cq::channel` 全
  等于 `_comp_channel->channel`,并且每个 CQ 的 `cqe` 不小于喂它的队列深度。这条就是
  设计那句话本身,也正是能抓住 CQ 深度 bug 的断言。

验证命令(见 memory):

```
https_proxy="10.7.4.2:3128" bazel test --config=test --config=rdma //test:brpc_rdma_unittest
```

---

## `docs/cn/gdr_design.md` 同步更新

- **第 9 节开头**:`QpChannel::resource` 的说明从「QP 和它上报的 CQ,以及 host 独有的
  comp_channel」改成「QP 和它的两个 CQ」;补上 `RdmaEndpoint::_comp_channel` 是连接级的。
  `ResetCounters()` 改叫 `Reset()`、说明它是虚函数。
- **第 10 节**:「双 QP / 四 CQ / 共享 comp_channel」那段补上现在由**类型**表达 ——
  `RdmaCompChannel` 一条连接一个,`RdmaResource` 一条通道一个,两者的数量关系就是这句
  设计;顺带记 CQ 深度跟队列走。
- **第 6.3 节**:`RdmaEndpoint::_device_channel_mode` 改成
  `Socket::socket_mode() == SOCKET_MODE_RDMA_AND_DEVICE`,并说明 engage 点从两处握手中途
  移到了构造函数,server 端由 `ApplyRemoteHello()` 收回。
- **第 13.3 节**:补这一轮新增的用例名。

---

## 实施记录与偏差

三条都已落地,实施顺序按计划 **2 → 3 → 1**。验证:

```
https_proxy="10.7.4.2:3128" bazel test --config=test --config=rdma //test:brpc_rdma_unittest
  → 167 tests, all passed
https_proxy="10.7.4.2:3128" bazel test --config=test --config=gdr //test:brpc_rdma_unittest
  → 174 tests, all passed(门控用例在这个构型下才编译)
```

与计划不同的地方:

1. **多了一个 `RdmaEndpoint::device_channel_required()`**(计划里第 2 条只说需求检查改读
   `_socket->socket_mode() == SOCKET_MODE_RDMA_AND_DEVICE`)。因为这个条件有两个读者 ——
   `ResetDeviceChannel()` 和需求检查 —— 而「它在两端含义不同」那段解释需要一个落脚点。
   写成访问器之后,头文件里那段注释只需要写一遍,单测也能直接断言配置而不是断言实现。

2. **`AllocateCq()` 多了一个 name 参数**,从 `(size, channel, name)` 变成
   `(size, channel, chan_name, direction)`,日志从 `Fail to create send CQ` 变成
   `Fail to create host send CQ`。原因:CQ 现在有四个,只说方向分不出是哪条通道的 ——
   而这正是这一轮想让人能一眼看出的东西。

3. **`DeallocateResources()` 里 `delete _host.resource` 移到了 comp_channel 销毁之前。**
   计划只说「先销毁 CQ 再销毁 comp_channel」;实现上顺手把整个 `RdmaResource` 的销毁
   提到前面,这样「CQ 已经没了」是由类型保证的,而不是由三行手写的置空保证。净顺序和
   原来一致。

4. **销毁失败的日志从 `DeallocateResources()` 搬进了 `~RdmaCompChannel()`。** 计划没提。
   既然销毁只发生在析构里,日志跟着走,`DeallocateResources()` 那一段就只剩
   「先摘 epoll 再 delete」这一件事。

5. **`device_channel_lives_and_dies_with_the_optional` 必须改写**,计划里只列了新增用例
   没提这条。`Reset()` 之后 `_device` 现在会**重新 emplace**,所以原来的
   `ASSERT_FALSE(has_device_channel())` 不再成立。改成:`Reset()` 之后断言通道回来了且
   队列空了,再显式 `_device.reset()`(握手没谈成时的样子)去断言「真的没有」那半边的
   行为 —— `HasQueuedDeviceData()`、`HasPendingWrite()`、`Socket::device_stream()`。
   两个性质都还在测,只是分成了「重连」和「对端拒绝」两种情形。

6. **单测用例名**:计划里的「构造期 engage」落成
   `device_channel_is_built_by_the_constructor`;「`HostChannel::Reset()` 的字段清单」落成
   `host_channel_resets_every_field_it_owns`;「comp_channel 的生命周期」落成
   `deallocate_releases_a_comp_channel_without_a_qp`;「fd 属性」落成
   `comp_channel_fd_is_cloexec_and_nonblocking`;「四 CQ 一 channel 的形状」落成
   `four_cqs_report_to_one_comp_channel`(`#if BRPC_WITH_GDR` +
   `--rdma_test_enable --gdr_test_real_device` 门控)。测试文件多了一个
   `#include <fcntl.h>`(读 `FD_CLOEXEC` / `O_NONBLOCK`)和两个
   comp channel 函数指针的 `extern` 声明。

### `docs/cn/gdr_design.md` 同步更新已完成

第一轮丢失的 114 行由使用者恢复之后,上面「同步更新」那一节已落地。两处与计划不同:

- **节号是 §4.2 而不是 §10。** 计划里写的「第 9 节 / 第 10 节」是凭记忆写的:恢复后的文档里
  「双 QP / 四 CQ / 共享 comp_channel」是 **§4**(§4.2 专讲为什么共享 comp_channel),§9 是
  流控与背压,§10 是「明确不做:polling 模式」。所以类型分层那段改在 §4.2(连带把那里
  过时的 `struct RdmaResource` 草案换成落地后的三个类型、记上 CQ 深度跟队列走),§9 改的是
  `QpChannel` / `_comp_channel` 的字段归属和 `Reset()` 那几句。
- **§10 没动。** 它说的是「polling 模式与本设计冲突,本期不做」,对当前代码仍然成立;而
  第四轮 review 第 1 条正是要让 polling 支持第二通道,那一节会被整节重写,现在改是白改。


# GPU Direct RDMA (GDR) 设计

本文描述 bRPC 中 GPU Direct RDMA 的设计。它是对 [PR #3144](https://github.com/apache/brpc/pull/3144)
的一次重新设计:目标相同(让 GPU 显存里的数据不经过 host 内存直接走 RDMA 收发),
但数据通路的组织方式完全不同。末尾的「与 PR #3144 的差异」一节解释为什么。

---

## 1. 背景与目标

RDMA 网卡可以直接 DMA 到 GPU 显存(nv_peer_mem / dmabuf)。对于 AI 训练、推理、参数服务器
这类场景,张量本来就在显存里,如果 RPC 框架强制它先 D2H 拷到 host 再发送,就白白付出了
一次 PCIe 往返和一次 host 内存带宽。

**目标**:让用户能把一块显存作为 RPC 的附件收发,全程不经过 host 内存。

**非目标**(本设计明确不做):
- 不支持在 `IOBuf` 里混放 host 与 device 内存。
- 不支持 protobuf 消息体本身位于显存(只有附件可以)。
- 不做跨 GPU / 多卡拓扑感知的路由选择。

(polling 模式一度也在这张表上,现已支持,见 §10。)

---

## 2. 核心约束:IOBuf 不能承载显存

这是整个设计的出发点,也是与 PR #3144 分歧的根源。

`butil::IOBuf` 是 bRPC 里所有 host 数据的通用容器,框架的**大量代码会直接解引用它内部的指针**。
一旦某个 IOBuf 里混入了显存地址,这些代码就会段错误。而且这不是「用户小心一点就能避免」的
风险,是框架自己必然触发的:

- **服务端协议自动探测**。首个报文到达时 `preferred_index == -1`,`InputMessenger::CutInputMessage`
  会把 `_read_buf` 依次喂给**每一个**注册的协议(`src/brpc/input_messenger.cpp:83`)。
  `PROTOCOL_RDMA_HANDSHAKE`(索引 1)排在 `PROTOCOL_BAIDU_STD`(索引 2)前面。
  期间 `nshead_protocol.cpp:157` 的 `source->copy_to(header_buf, sizeof(header_buf))`、
  `streaming_rpc_protocol.cpp:65` 的同样调用、`http_rpc_protocol.cpp:1215` 的
  `ParseFromIOBuf(*source)` 都会对显存地址做 host memcpy。

- **客户端协议回退**。baidu_std 返回 `TRY_OTHERS` 时会自动回退到 `PROTOCOL_STREAMING_RPC`
  (`input_messenger.cpp:130`),同样是 host memcpy。

所以设计的第一条铁律:

> **显存永远不进入 `IOBuf`。它走一条独立的数据通路,有自己的容器类型 `DeviceAttachment`。**

---

## 3. 总体架构

```
                    ┌─────────────────────────────────────────┐
   应用层            │ Controller                              │
                    │   request_attachment()        -> IOBuf  │
                    │   request_device_attachment() -> Device │
                    └─────────────────────────────────────────┘
                                     │
                    ┌────────────────┴────────────────┐
   协议层            │ baidu_std                       │
   (policy/)        │  PRPC 帧头(12B,不变)           │
                    │  GDRB 帧头(16B,带 device 长度) │  ← 新增
                    │  parse: pre-flight + pending list│
                    └────────────────┬────────────────┘
                                     │
                    ┌────────────────┴────────────────┐
   传输层            │ Socket                          │
                    │   _read_buf (host 字节流)        │
                    │   device_stream() (device 字节流)│  ← 新增
                    └────────────────┬────────────────┘
                                     │
                    ┌────────────────┴────────────────┐
   RDMA             │ RdmaEndpoint                    │
                    │   host   QP + send_cq + recv_cq │
                    │   device QP + send_cq + recv_cq │  ← 新增
                    │   共享一个 comp_channel          │
                    └─────────────────────────────────┘
```

两条通路在传输层是**两条完全独立、各自保序的字节流**。传输层不认识报文边界,不做任何
host/device 的关联,也不做任何同步操作。把两条流对齐成一个完整报文,是协议层(baidu_std)
的职责。

---

## 4. 资源模型:双 QP / 四 CQ / 共享 comp_channel

### 4.1 为什么是两个 QP

一个 QP 也能跑,但会带来三个问题:

1. **流控被迫共享**。一次几百 MB 的显存传输会吃光整条连接的信用,让同连接上的小 RPC 全部饿死。
2. **块大小被迫统一**。host 侧的接收块是 8KB(`block_pool.cpp:56` 的默认 `BLOCK_DEFAULT`),
   显存侧合适的粒度是 MB 级。共用一个 QP 就得取一个折中值,两边都难受。
3. **接收缓冲无法分型**。RQ 上挂的 WR 必须预先指定落地地址,一个 QP 的 RQ 没法既挂 host 块
   又挂 device 块还能预测下一个报文落在哪。

所以 host 和 device 各自一个 QP,各自一套 `_sq_window_size` / `_remote_rq_window_size` /
`_new_rq_wrs` 流控状态,复用现有机制。

### 4.2 为什么共享 comp_channel

(本节讲的是**事件模式**。polling 模式不用 comp_channel,一条通道一个 `polling_cq`,见 §10。)

`Socket` 的输入事件 fd 就是 comp_channel 的 fd(`rdma_endpoint.cpp:1766`:
`options.fd = _comp_channel->fd();`)。一个 Socket 只有一个输入 fd,所以两个 QP 的
CQ 必须挂在同一个 comp_channel 上,否则 device 侧的完成事件没有地方唤醒。

好消息是现有代码已经是「多个 CQ 挂一个 comp_channel」的形态了 —— `AllocateQpCq`
本来就把 `send_cq` 和 `recv_cq` 挂在同一个 comp_channel 上。扩到 4 个 CQ 是量的变化,
不是质的变化。

这句设计现在**由类型表达**,不靠注释维持:comp_channel 是**连接**的,QP 和它的 CQ 是
**通道**的,两者的数量关系(1 : 2)就是「双 QP / 四 CQ / 共享 comp_channel」。

```cpp
// 一条连接一个。连接上所有 CQ 的事件都报到这里 —— 四个 CQ 一个 fd、一次 epoll 注册,
// PollCq() 一次唤醒排干四个 CQ。ibv_create_cq() 把 CQ 和 comp_channel 绑死,所以它必须
// 先于任何 CQ 建出来、再传给 AllocateQpCq(),而不是当作某个 QP 的副产品。
// polling 模式下整条连接为 null:没有人等 comp_channel 的事件(§10)。
struct RdmaCompChannel {                    // rdma_endpoint.h:95
    ibv_comp_channel* channel;
    static RdmaCompChannel* Create();       // 建 + close-on-exec + non-blocking
    int fd() const { return channel->fd; }
};

// 一条通道一个:QP 和它上报的 CQ。host 一份、device 一份,共用上面那一个
// RdmaCompChannel —— 它是连接的,不在这里。
struct RdmaResource {                       // rdma_endpoint.h:123
    ibv_qp* qp;
    ibv_cq* polling_cq;                     // polling 模式:一条通道一个,收发都报到它
    ibv_cq* send_cq;                        // 事件模式
    ibv_cq* recv_cq;
};

// 预分配池的一项:一个 comp_channel 加上在它上面建好的 host QP/CQ。两者只能成对预分配、
// 成对回收 —— CQ 建出来之后就改不了它属于哪个 comp_channel。
// 是个可移动的值,所以池子就是个 std::vector,取一项的代价是一次 move 而不是 new/delete。
struct PreparedResource {                   // rdma_endpoint.h:137
    RdmaCompChannel* comp_channel;          // polling 模式下同样为 null
    RdmaResource* resource;
    PreparedResource(PreparedResource&&) noexcept;
    PreparedResource& operator=(PreparedResource&&) noexcept;
    ~PreparedResource();                    // 没被 release() 掉的就在这里销毁
    bool valid() const { return resource != nullptr; }
    void release();                         // 交给连接之后,置空而不销毁
};
```

于是建一条通道只有一份实现:`AllocateQpCq(cc, sq_size, rq_size, for_device, name)`
(`rdma_endpoint.cpp:1606`)—— 「在给定的 comp_channel 上建一条通道的 QP 和 CQ」。host 走
`AllocatePreparedResource()`(`:1641`,顺带把 comp_channel 建出来),device 走
`AllocateQpCq(_comp_channel, ...)` 复用 host 已经建好的那一个。`AllocateDeviceResources()`
里原先抄了一遍的 25 行建 CQ / 建 QP 代码因此删掉了。

**CQ 的深度跟着喂它的队列走**,不是一个常数:比队列浅的 CQ 会溢出,溢出后 CQ 进入 error
状态、QP 跟着废掉。(这里原先有个 GDR 之前就存在的上游 bug:`AllocateQpCq()` 无论要多大的
队列,CQ 一律建 `FLAGS_rdma_prepared_qp_size`(128)条,而现场分配这条路恰恰只在队列
**大于** 128 时才走到,于是 `-rdma_sq_size=1024` 会得到一个 1024 深的 SQ 配 128 条的
send CQ。拆分之后 `sq_size` / `rq_size` 就在手边,这个 bug 一起修掉了。)

### 4.3 PollCq 的改造 —— 最容易写错的地方

`RdmaEndpoint::PollCq` 原先用一个 `bool send` 在两个 CQ 之间轮转。扩到四个 CQ 时有三处必须
一起改,漏一处就是间歇性挂死:

**(a) 轮转变量从 bool 变成一张表。**

原先「`send` 取反」这种隐式编码扩不到四态,而且真正要表达的东西有三项(轮哪个 CQ、它是不是
send 方向、它属于哪条通道),不止一项:

```cpp
// rdma_endpoint.h
struct PollTarget { ibv_cq* cq; bool is_send; QpChannel* channel; };
static const int MAX_POLL_TARGETS = 2 * MAX_CHANNELS;

// PollCq() 开头一次算好,之后只按下标走
PollTarget targets[MAX_POLL_TARGETS];
const int ntargets = ep->CollectPollTargets(targets);
```

`CollectPollTargets()` 是**两种 CQ 模式唯一的分叉点**(§10):事件模式下一条通道两个,recv 排
在 send 前面(先收数据再收完成,让同一次唤醒里到达的两半能一次解析完,不必挂 pending list);
polling 模式下一条通道一个 `polling_cq`,收发都在里面。下游那个 `while (true)` 轮转循环两种
模式共用,一行不分叉。

**(b) `if (send) { continue; }` 只能对 send 方向生效。**

原代码:
```cpp
// Send CQE has no messages to process.
if (send) { continue; }
```
device 的 recv CQ 如果被归进这个分支,device 数据到达时就不会调用 `ProcessNewMessage`,
挂在 pending list 上的报文永远等不到重新解析 —— 死锁。现在这个判断直接读
`targets[current].is_send`,是哪条通道的哪个方向由那张表说了算;polling 模式下它恒为 false,
因为那一个 CQ 里就是有 recv CQE。

同理,`HandleCompletion()` 要知道这个 CQE 是哪条通道的,也是从表里拿:
`ep->HandleCompletion(*targets[current].channel, wc[i])`。不靠 `wc.qp_num` 反查。

**(c) re-arm 之后必须把全部 CQ 从头重新 poll。**

现有代码里有一段很长的注释解释这个:RDMA 的完成通知是 one-shot 的,`ibv_req_notify_cq`
之后新到的 CQE 才会触发事件。落在「poll 已经返回 0」与「notify 生效」之间那个窗口里的 CQE,
既不会被那次 poll 看到,也不会触发后续事件,连接就卡死到下一个 CQE 碰巧到来为止。
现有代码为此在 re-arm 后显式把 `cq` 切回 `recv_cq` 重新轮一圈。多个 CQ 时同理:

```cpp
if (!notified) {
    if (0 != ep->ReqNotifyAllCqs(true)) { return; }
    notified = true;
    current = 0;            // 从头重新轮一圈,不能只轮最后一个
    continue;
}
```

polling 模式下这整段都不跑 —— 轮空一圈就直接返回,poller 线程马上会再进来一次,没有边沿
可漏,也没有东西要 arm。

`GetAndAckEvents()` 里「这个事件是哪个 CQ 的」的分派、以及 `send_cq_events` /
`recv_cq_events` 两个计数器,都跟着扩成每条通道一份 —— 它们现在是 `QpChannel` 的成员,
循环 `CollectChannels()` 的结果即可,不必按通道写两遍。arm 也一样:`ArmChannelCqs(qc)` 管
一条通道的两个 CQ,`ReqNotifyAllCqs()` 把它对每条通道各调一次。

### 4.4 重新触发解析是免费的

一个关键问题:device 字节到了、但 host 的 `_read_buf` 一个字节都没增长,谁来重新驱动解析?

答案是现有代码已经处理好了。RDMA 路径**不走** `InputMessenger` 的通用 `DoRead` 循环,
`PollCq` 是直接调用的(`rdma_endpoint.cpp:1596`):

```cpp
messenger->ProcessNewMessage(s.get(), bytes, false /* read_eof */, ...)
```

`bytes` 只是统计量,`read_eof` 硬编码 false。对比 TCP 路径(`input_messenger.cpp:345`)
`nr == 0` 会被当成对端关闭 —— RDMA 这边没有这个陷阱。所以一批只含 device CQE 的完成事件
照样会跑 `ProcessNewMessage → while(1) CutInputMessage`,pending list 自然被重新驱动。

代价是 `bytes` 里会混入 device 字节,进入 `AddInputBytes` 统计。这是可接受的(它们确实是
收到的字节),但要在实现里明确注释。

### 4.5 显存池与 GPU 的选择

第二通道的内存池(`rdma/device_memory.cpp`)是 4KB~1GB 的按 size class 分档的块池,块一旦
`ibv_reg_mr` 注册过就只回收进 free list、不反注册 —— 注册一次是毫秒级,放在连接热路径上
不可接受。

```
--rdma_attachment_memory      (默认 device;host 时下面几个只有 block 尺寸还起作用)
--rdma_gdr_device_id          (默认 -1)
--rdma_gdr_recv_block_size    (默认 1MB,握手时广播给对端)
--rdma_gdr_max_device_bytes   (默认 4GB,按 region 计,含尚未切出去的部分)
--rdma_gdr_region_size_mb     (默认 256)
--rdma_gdr_max_regions        (默认 32)
--rdma_gdr_tls_cache_num      (默认 128,每线程每档)
--rdma_gdr_tls_cache_bytes    (默认 8MB,每线程合计)
```

池子是显存还是 host 内存由 `-rdma_attachment_memory` 决定(§5.3),而这一节其余的一切 ——
size class、region、TLS 缓存、`GetDeviceLKey()` 的无锁扫描 —— 都是围绕「一块注册过的内存」
写的,不关心是哪个 allocator 产的,`ibv_reg_mr()` 也不关心。整个 `host` 模式就是
`RawDeviceAlloc/Free` 里的一个 `posix_memalign`/`free` 分支。

一个由此而来的、写错过一次的细节:region 表里**混着两种东西** —— 池自己的 region,和用户
交给 `RegisterDeviceMemory()` 的 buffer(`size_class == -1`)。所以「`GetDeviceLKey()` 命中」
只说明**注册过**,不说明是显存。判定内存类型要看命中的是哪一种:池块是
`IsAttachmentMemoryDevice()`,用户 region 按定义就是显存。接收路径正是拿着落点池块去问这个
问题的,把命中直接当成显存的话,`host` 模式下本进程收到的每一个 attachment 都会被标成不可
memcpy,而那些字节其实一直是可读的。

机制本身有三条硬约束,理由是同一个:**锁里干的活必须是 O(1) 且 region 数必须有界**。

- **slab 化**。一次 `Alloc` + 一次 `ibv_reg_mr` 买下一整片 region,再从里面切定长
  block,`region_size = clamp(block_size * 64, block_size, --rdma_gdr_region_size_mb)`。
  空闲链表存**区间**(start, len)而不是单个 block,所以开一个 region 是常数个节点。
  ≥ region 上限的 size class 退化成「一块一 region」,不比按块注册更差。
- **region 表是定长只追加数组**(`Region _regions[64]` + 高水位 `_region_num`),于是
  `GetDeviceLKey()` 是一次无锁线性扫。这正是「注册一次 KV cache、之后每次发切片」那种最热
  的形态所需要的。槽位**不复用**,`DeregisterDeviceMemory()` 只打墓碑,读侧因此不需要任何
  同步就不会看到「新 start 配旧 size」的撕裂值。数组满了回落到 `std::map` + 独立 mutex,
  只是慢一点,不会有悬崖。
- **per-size-class 的 `__thread` 缓存**,`butil::thread_atexit()` 归还,稳态 alloc/free
  一把锁都不碰。generation 计数让 `Destroy()` 之后的 TLS 缓存整体失效,单测反复
  release/initialize 才不会拿到指向已 `cudaFree` 内存的指针。

一个语义变化:显存**按 region 粒度持有**,一个 region 里只要还有一块在用,整片就不还给
CUDA(block_pool 也是这个语义,它根本不还)。所以 `--rdma_gdr_max_device_bytes` 的含义是
「已保留字节」而不是「已分配字节」—— 更严格,不会超发。`GetDeviceMemoryStat()` 同时暴露
`reserved` / `in_use` / `regions` 三个值,`regions` 应当随 block 数量增长而保持平坦。
(这些 flag 在 `Init()` 时被快照进 `MemoryPoolOptions`,不是每次扩容现读 —— 运行中改
`--rdma_gdr_max_device_bytes` 不会生效,要重建池子。)

#### MemoryPool:一套机制,可换 backend

上面这些机制**不是为 GDR 新写的**,`block_pool.cpp` 里早就有一份 —— 它在 host 内存上给
IOBuf 供块。两份实现的差别只有四个调用:怎么拿到裸内存、怎么还、怎么注册、怎么反注册。
所以把这四个调用抽成 `MemoryBackend`,其余全部(region 表、size class、无锁
`FindRegion`、TLS 缓存、配额)收进 `rdma/memory_pool.{h,cpp}` 的 `MemoryPool`,
**两个池子都跑在它上面**:

```
class MemoryBackend {           // 只有四个方法 + 一个 name()
    virtual void* Alloc(size_t size) = 0;
    virtual void Free(void* ptr) = 0;
    virtual uint32_t Register(void* ptr, size_t size) = 0;
    virtual void Deregister(void* ptr) = 0;
};

block_pool.cpp    HostBlockBackend       posix_memalign / free / RegisterCallback
device_memory.cpp HostAttachmentBackend  posix_memalign / free / ibv_reg_mr
                  CudaAttachmentBackend  cudaMalloc / cudaFree(ScopedCudaDevice 内)/ ibv_reg_mr
```

两个池的其余差异全部变成 `MemoryPoolOptions` 里的数据:`size_class_shifts`(host 池是
`{13,16,21}`,即老的 block type 0/1/2 —— **下标就是 type**,所以 `GetBlockType()` 语义原样
保留;GDR 池是 12..30 共 19 档)、region 几何(host 池按
`--rdma_memory_pool_increase_size_mb` 定长,GDR 池按 64 块/region 再被
`--rdma_gdr_region_size_mb` 截断)、`max_bytes`、TLS 缓存额度、以及
`user_specified_memory`。

两个公开头文件 `block_pool.h` / `device_memory.h` 一个字没动,所有调用方(`rdma_helper.cpp`、
`rdma_endpoint.cpp`、builtin service)也就都不用动。两点值得写下来:

- **`--rdma_memory_pool_buckets` 变成了被忽略的废弃 flag**。老的 host 池把每个 size class
  的空闲链表切成 N 个桶来降低锁竞争;这件事现在由每线程缓存做,而且做得更彻底 —— 稳态下
  一把锁都不碰,不像分桶只是把竞争摊薄成 1/N。flag 保留只是为了不让既有启动脚本报错。
- **host 池的每线程缓存额度是 `--rdma_memory_pool_tls_cache_num * 8KB`**。老实现只缓存
  8KB 这一档,所以一个线程最多占住这么多;按字节而不是按块数封顶,既保住了原来的每线程
  内存足迹,又让 64KB 档也能进缓存(IOBuf 确实会用到),而 2MB 档进不去 —— 频繁申请
  2MB 块的场景本来也不是缓存要解决的。

一个实现上的约束:`__thread` 不能是类成员,而按 backend 做间接调用去取缓存又得不偿失,
所以缓存是一个 `__thread TlsCache tls_caches[MAX_POOLS]`,每个 `MemoryPool` 构造时从全局
原子里取一个**永不归还**的槽位。也就是说 `MemoryPool` 被设计成进程生命期的单例,靠
`Init()`/`Destroy()` 原地重建,而不是随手 new 出来的对象 —— 两个池都是函数内静态单例,
且**故意泄漏**:线程通过 `thread_atexit()` 注册的回收回调活得比任何静态析构顺序都长。

`--rdma_gdr_device_id` 不是可有可无的:`cudaMalloc` 分配在**调用线程当前的 device** 上,
而池里的块是哪个 bthread 先用完就由哪个 bthread 补,进程里只要有别的线程调过
`cudaSetDevice`,接收块就会散落在不同 GPU 上、却全部注册到同一张网卡的 PD 上。所以
`RawDeviceAlloc/Free` 外面套了一层 `ScopedCudaDevice`,进入时切到指定卡、退出时切回调用者
原本的卡(不干扰用户自己的 device 状态)。默认 -1 表示沿用进程默认(通常是 0)。

多卡机器上这个值应当选**与 `--rdma_device` 挂在同一个 PCIe switch 下的 GPU**,否则每次
传输都要过 host bridge,GDR 省下的那次拷贝就白省了。启动时会校验 ordinal 越界并提前
`cudaSetDevice` 建好 primary context —— 那一步要 100ms 左右,放在启动日志里比放在第一个
请求里好读得多。

### 4.6 初始化时机与 scatter-to-CQE

`GlobalGdrInitialize()` 由 `GlobalRdmaInitializeOrDie()` 调用,排在最前面。它只需要 CUDA、
不需要 `g_pd`(块是首次分配时才注册的),所以提前调用是免费的;放最前面只是为了让
`--rdma_gdr_device_id` 写错时那条错误不被后面一大堆 RDMA 启动日志淹掉。

**`-rdma_attachment_memory=device` 且拿不到 GPU 时进程直接退出**,不降级。这一条曾经是反
的:早先的注释写着「best-effort,只打 WARNING 继续跑,否则 `-rdma_enable_gdr` 在混部集群
里不敢常开」。§5.3 把「通道在不在」和「块是什么做的」拆成两个 flag 之后,那个理由就没有了
—— 没 GPU 的机器配 `-rdma_attachment_memory=host`,`host` 模式根本不碰 CUDA,永远起得来。
明确要显存而这台机器给不了,是配置错误,应当在启动时炸掉,而不是表现成运行期每个 RPC 都悄悄
退化成 §7.3 的 TCP 回退 —— 那时已经没有任何错误码会提醒你配置没生效。

**scatter-to-CQE 必须在 device QP 上关掉。** mlx5 会把足够小的入站 payload 直接放进 CQE,
然后由**用户态驱动**把它 memcpy 到接收 WQE 的 scatter list 里 —— 一次普通的 host memcpy,
目标地址是 WQE 里填的那个地址。device QP 上那个地址是显存,于是 `ibv_poll_cq()` 在 libmlx5
里段错误。命中条件不只是"小的 DeviceAttachment":大的也会,因为尾部 WR 是小的 ——
1MB+7 的 tensor 打到 1MB 的接收块上会拆成 `1MB + 7`,崩的是那 7 字节的后半段。

开关做在**单 QP 粒度**上:device QP 走 `mlx5dv_create_qp()` 并带上
`MLX5DV_QP_CREATE_DISABLE_SCATTER_TO_CQE`,host QP 仍然走 `ibv_create_qp()`,继续享受小消息
省一次 DMA 的收益。落在 `rdma_helper.cpp` 的 `CreateDeviceQp()` 里,`rdma_endpoint.cpp` 侧
只是 `AllocateQp(..., for_device=true)` 多一个参数。

mlx5dv 不是硬依赖:头文件用 `__has_include(<infiniband/mlx5dv.h>)` 探测,符号用 `dlopen`
+ `dlsym` 取(`libmlx5.so.1` 本身就是 ibverbs 的 provider,进程里早就映射了),所以三套构建
系统一行都不用改,非 GDR 进程也不会去 dlopen 它。`mlx5dv_create_qp()` 失败时**不回退到**
`ibv_create_qp()` —— 那会造出一个"能建成、但第一个小包就崩"的 QP;返回 nullptr 让
`DoAllocateResources()` 退化成纯 host 连接,这条路径本来就有。

只有一种情况回到进程级的 `MLX5_SCATTER_TO_CQE=0`:设备名是 `mlx5*` 但 mlx5dv 取不到
(编译机没有 mlx5dv.h,或 libmlx5 太老没有这两个符号)。这时代价是 host QP 也一起丢掉
scatter-to-CQE,但比崩溃便宜。用 `setenv(..., 1)` 覆盖而不是"没设才设":环境里显式写了
`MLX5_SCATTER_TO_CQE=1` 的话,不覆盖就是一个 crash。非 mlx5 的 provider 什么都不做。

这个判定是 `pthread_once` 的,有两个入口:GDR 已就绪时在初始化里、`RdmaEndpoint::
GlobalInitialize()` 预建 QP 之前提前跑一次(进程还是单线程,是 `setenv` 唯一安全的时机);
以及 `CreateDeviceQp()` 里兜底一次,给那些初始化之后才打开 GDR 的进程(单测就是这样)。
兜底路径也是对的:libmlx5 是在 `create_qp` 里读那个环境变量、而不是在 context 初始化时读。

两个入口的门都是 `IsAttachmentMemoryDevice()` 而不是 `IsGdrAvailable()`,而且**必须两个都
带**:这个坑是显存独有的,落在 host 内存的第二通道有一模一样的 QP、没有那个 hazard。只在
初始化那处加门是不够的 —— 兜底入口正是「初始化之后才开 GDR」走的路,漏掉它的话,host 模式
下轻则多一条误导性的日志、重则在服务已经跑起来、早就不是单线程的时候执行那个进程级
`setenv`,而那恰恰是提前入口存在的全部理由。

### 4.7 构建开关

`BRPC_WITH_GDR` 蕴含 `BRPC_WITH_RDMA`,三套构建系统都是这样:

```bash
bazel build --config=gdr //...                       # 隐含 --config=rdma
cmake .. -DWITH_RDMA=ON -DWITH_GDR=ON                # 只给 WITH_GDR 会报错退出
./config_brpc.sh --with-rdma --with-gdr
```

bazel 侧不能把 `-I/usr/local/cuda/include` 塞进 `copts` —— 绝对路径会被拒绝
(*references a path outside of the execution root*),`--copt` 也一样。所以走
`rules_cuda` 的 `local_toolchain` 拿到 `@local_cuda//:cuda_runtime`(头文件 + libcudart),
只在 `brpc_with_gdr` 这个 `select()` 分支里依赖。module extension 是惰性的,没开 gdr 的机器
不会去探测 CUDA。

---

## 5. 握手协商

device 通道必须**显式协商**:一端请求、另一端启用,才建立。任何一端不满足,连接退化成纯
host RDMA(不是 fallback TCP)。

协商的是**通道**,不是内存。两端有没有 GPU、块是显存还是 host 内存,都不参与协商,也不上
线 —— 见 §5.3。

### 5.1 只在 v3 握手上支持

v2 握手是 36 字节定长二进制,没有扩展位。v3 握手是 protobuf(`rdma_handshake.proto`),
本来就有 `optional RdmaEce ece = 7` 这个可选扩展的先例。照抄:

```protobuf
message RdmaHello {
    required uint32 block_size = 1;
    // ... 略 ...
    optional RdmaEce ece = 7;

    // GDR device channel. 字段缺席 = 本端不请求 device 通道。
    // 双方都 present 才建立 device QP。
    optional RdmaDeviceChannel device = 8;
}

message RdmaDeviceChannel {
    required uint32 qp_num     = 1;
    required uint32 block_size = 2;  // 本端 device 接收块大小
    required uint32 sq_size    = 3;
    required uint32 rq_size    = 4;
}
```

v2 对端 / 老版本 v3 对端不认识字段 8,协商结果自然是「无 device 通道」,不需要额外的版本判断。

客户端要用 GDR 时必须走 v3(`FLAGS_rdma_client_handshake_version=3`)。若用户同时开了
GDR 和 v2,启动时明确报错,不静默降级 —— 落点见 §10.1。

解析之后的 `ParsedHello`(`rdma_handshake.h`)按**同一套参数出现两次**来组织,而不是把
device 的四个字段平铺在 host 的四个旁边:

```cpp
// 一条通道要从对端知道的全部信息
struct QpParams {
    uint32_t qp_num;
    uint32_t block_size;   // 对端在这条通道上的接收块大小,即我方一个 WR 的上限
    uint16_t sq_size;
    uint16_t rq_size;
};

struct ParsedHello {
    QpParams host;                        // 必有
    uint16_t lid;                         // 连接级:两条通道共用一个端口
    ibv_gid  gid;
    butil::optional<ibv_ece> ece;         // v3 且对端广播了才有
    butil::optional<QpParams> device;     // nullopt = 对端不要第二通道
};
```

这样 `ApplyChannelParams(QpChannel&, const QpParams&)` 才写得出来 —— 「按对端这一组参数把
这条通道的收发窗口算出来」,host 和 device 各调一次,窗口大小算错一条通道的事情就没有地方
发生。平铺的话每个取值点都要自己记得配对哪个前缀,`min(_host.sq_size, remote.device_rq_size)`
这种错写出来编译器不会说话。

**lid / gid 留在外层**,不进 `QpParams`:它们是**端口**的属性,两条通道的 QP 建在同一张卡的
同一个端口上,不可能各有一份。ECE 同理是连接级的协商结果。放进 `QpParams` 就等于声称
device 通道可以走另一个端口,那是这个设计从未打算支持的东西。

### 5.2 block_size 必须各自广播

PR #3144 的一个具体 bug 是:它引入了 `g_gdr_recv_block_size`(默认 512KB),却**从没改过
`rdma_handshake.cpp`** —— 那里三处(:160/:248/:293)广播的都是 `g_rdma_recv_block_size`。
于是发送侧按 host 块大小(默认 8128B)切分 WR(`rdma_endpoint.cpp:753`),接收侧却挂着
512KB 的显存块。结果是 **64 倍的显存放大**;而如果用户把 `--rdma_recv_block_type=huge`
(2MB)调大到超过 512KB,又会直接触发 sge 长度错误。

本设计里 `RdmaDeviceChannel.block_size` 是独立字段,与 host 的 `block_size` 分开广播、
分开生效,不共用任何全局变量。

### 5.3 内存类型不上握手

场景:**发送端在 host 内存、接收端要落显存,或者反过来。** 四种组合都要通,都要零拷贝。

能通是因为这两件事在 RDMA 硬件层面本来就是分开的,只是早期实现把它们捆死了:

| | 由什么决定 | 谁说了算 |
|---|---|---|
| **源内存类型** | 发送侧 sglist 里 lkey 指向哪 | 每次 append,发送端 |
| **落点内存类型** | 接收侧 RQ 里 post 了什么块 | 进程配置,接收端 |

NIC 不关心 lkey 指向 HBM 还是 DRAM,`ibv_reg_mr` 也不校验指针类型。所以「host 源 → 对端
显存落点」在硬件上完全可行,而且**比先 H2D 再发少过一次 PCIe**(直接 DRAM→NIC,而不是
DRAM→GPU→NIC)。反方向同理。

**落点不协商。** 考虑过在 `RdmaDeviceChannel` 里加 `memory_kind` 字段让两端互相知会,否决:
发送端拿到这个字段之后唯一能做的是「我不喜欢对端的落点所以拒发」,那是应用策略,传输层不该
替它决定;而接收端的落点是它自己配的,不可能对自己配出来的结果感到意外。所以
`RdmaDeviceChannel` **一个字段都没加**。发送端真正需要知道的只有 `block_size`(一个 WR 不能
超过对端块大小),它和内存类型无关,本来就在。

于是通道语义从「显存通道」退化成「一条带独立 credit 的带外大块流」,对应两个 flag:

| flag | 作用域 | 含义 |
|---|---|---|
| `-rdma_enable_gdr` | 进程 | 本进程支不支持第二通道 |
| `-rdma_attachment_memory` | 进程 | 本端的块是 `host` 还是 `device` 做的 |

**最容易实现错的一条**:`-rdma_attachment_memory` 管的是「本端**分配**什么、**接收**进什么」,
即 `append_new()` 和 RQ 里 post 的块;**不管用户交进来什么**。`append_user_data()` 两种指针
都收、不看这个 flag —— 「host 源 → 对端显存落点」那条零拷贝路径全靠这一点。

拆开之后,原先长得一样的几处 `IsGdrAvailable()` 含义各不相同,这是重构里最容易漏的地方:

| 位置 | 改成 | 为什么 |
|---|---|---|
| 客户端提议(`rdma_endpoint.cpp`) | `ChannelOptions::require_device_channel` | 提不提议是**每个 Channel 的配置**,不是能力探测 |
| 服务端接受(`rdma_endpoint.cpp`) | `remote.device.has_value() && FLAGS_rdma_enable_gdr` | 服务端被动接受,只看总开关 |
| 拒绝非 SINGLE(`channel.cpp`) | `require_device_channel` | 根因是 pending list 与 `SocketPool` 的 CHECK 冲突(§8.4),是**第二通道**的性质,`host` 落点一样中招 |
| scatter-to-CQE(`rdma_helper.cpp`,两处) | `IsAttachmentMemoryDevice()` | 打爆显存 QP 是显存独有的坑(§4.6) |

`IsGdrAvailable()` 的语义就此收窄成「第二通道的池起来了,不管它是什么做的」,真正想问
「是不是 GPU 内存」的只剩 scatter-to-CQE 那一处。

---

## 6. DeviceAttachment 与 Controller 接口

### 6.1 DeviceAttachment

不是 IOBuf,也不试图长得像 IOBuf —— 它故意做得很朴素,让人一眼看出不能当 host 内存用。

```cpp
namespace brpc {

class DeviceAttachment {
public:
    struct Segment {
        void*    ptr;      // 不是 is_host 就不许在 host 上解引用
        size_t   length;
        uint32_t lkey;
        bool     is_host;  // host 内存,memcpy 安全
    };

    DeviceAttachment();
    ~DeviceAttachment();
    DeviceAttachment(DeviceAttachment&&) noexcept;
    DeviceAttachment& operator=(DeviceAttachment&&) noexcept;

    // 从池里分配一块并追加。零拷贝路径。返回可写指针,失败 nullptr。
    void* append_new(size_t size);

    // 追加一块用户自己的内存,不拷贝。host / device 都收,是哪种在这里判定
    // 并记在 segment 上。没注册过的话内部注册、最后一个引用消失时注销。
    int append_user_data(void* ptr, size_t size, void (*deleter)(void*));

    // 调用方自己注册过、两个答案都已经知道。这里不注册也不注销。
    int append_user_data_with_lkey(void* ptr, size_t size, uint32_t lkey,
                                   bool is_host, void (*deleter)(void*));

    size_t size() const;
    size_t segment_count() const;
    const Segment& segment(size_t i) const;

    // 是不是每一段都是 host 内存,即整块能不能直接 memcpy 出来。
    bool is_host_readable() const;

    size_t cutn(DeviceAttachment* to, size_t n);
    void append_ref(const DeviceAttachment& other);
    void clear();
    void swap(DeviceAttachment& other);

private:
    // Block 带引用计数,cutn() 切在块中间也不拷贝。
    std::vector<Ref> _segments;
    DISALLOW_COPY_AND_ASSIGN(DeviceAttachment);
};

}  // namespace brpc
```

刻意的取舍:
- **禁止拷贝**,只允许移动。显存拷贝很贵,不能让它悄悄发生。
- **没有 `data()` 返回单一指针**。接收侧的数据天然可能横跨多个接收块,强迫调用方面对分段。
- **没有任何 host 侧的读写接口**,连 `is_host` 的段也没有。想读内容必须自己 `cudaMemcpy`。

`is_host` 挂在 **segment 上而不是整个 attachment 上**:发送侧一个 attachment 可以混装 host
段和 device 段(§5.3),接收侧不会 —— 接收侧的块全来自同一个池。`is_host_readable()` 是全段
的与运算,混装的答 false,想细看就逐段问。

这一位是**本地写入的、不过线**。发送端写的是「我交出去的这段是什么」,接收端写的是「我这个
池是什么做的」,两者互不相干,也没有任何字段把它带给对端。`Controller` 因此不需要新接口 ——
attachment 自己带着,再包一层既冗余、又说不清混装的情况。

### 6.2 Controller

```cpp
class Controller {
public:
    DeviceAttachment& request_device_attachment();
    DeviceAttachment& response_device_attachment();
    bool has_device_channel() const;
    // ...
};
```

语义与现有的 `request_attachment()` / `response_attachment()` 完全对称:客户端填
request 侧、读 response 侧,服务端反之。`Controller::Reset` 里清空。

若连接没有协商出 device 通道而用户设置了 device attachment,RPC **不失败**,而是走 §7.3
的 TCP 回退:发端 D2H 成 IOBuf、跟在 body 后面发走,收端 H2D 还原成 `DeviceAttachment`。
这一条曾经是反的 —— 早先直接 `EDEVICECHANNEL` 失败,理由是「静默地把零拷贝变成拷贝,比
报错更糟」。改掉是因为那个理由只覆盖了一半:报错确实让人看得见,但它同时让同一份业务代码
在没有 RoCE 的开发机、在灰度中尚未升级的实例上根本跑不起来,而这两种情况远比"没注意到
多了一次 D2H"常见。退化是可观测的(`has_device_channel()`),强制不退化也是可配的
(§6.3 的 `SOCKET_MODE_RDMA_AND_DEVICE`,协商不到第二通道的连接压根建不起来),所以保留
一个不可绕过的硬失败没有价值。

`has_device_channel()` 只在服务端、以及客户端 RPC **结束之后**有意义。原因是 brpc 在连接
建立之前就打包请求:一条新 channel 上的第一个 RPC 走到 `PackRpcRequest` 时握手根本还没跑,
问「有没有 device 通道」为时过早。传输层因此给的是三态
(`DeviceChannelState`: `OFF` / `ON` / `UNDECIDED`)。这个三态曾经用来在 `PackRpcRequest`
里快速失败 —— 只在明确 `OFF` 时失败、`UNDECIDED` 放行;第一版漏了后半句,写成
「`!has_device_channel()` 就失败」,结果每条连接的第一个带显存的 RPC 都必然报
`EDEVICECHANNEL`,而重试一次又会成功,是端到端测试发现的。有了回退之后这段逻辑整个不存在
了:`OFF` 是合法状态,`UNDECIDED` 也是,两者在 `AppendAndDestroySelf()` 里归一成同一个
问题「此刻这个 socket 有没有 device 流」—— 那时握手已经结束,答案是确定的。三态本身留着,
因为 `has_device_channel()` 仍要回答它。

### 6.3 SOCKET_MODE_RDMA_AND_DEVICE

「要不要第二通道」是每条连接的配置,不是进程能力探测,也不是每次 RPC 的选项。它表达成
一个新的 `SocketMode`:

```
SOCKET_MODE_TCP = 0, SOCKET_MODE_RDMA = 1, SOCKET_MODE_UBRING = 2,
SOCKET_MODE_RDMA_AND_DEVICE = 3
```

只有一端能发起协商,所以这个 mode 在两端读法不同:

- **Channel 侧是「要求」**,干三件事,全部在连接尺度:
  1. 建连时在 hello 里带上 `device` 字段。留在 `SOCKET_MODE_RDMA` 的 Channel(控制面 RPC
     之类)不用为对端 `rq_size × block_size` 的注册接收块买单。
  2. 握手回来对端没给 → **建连失败**,而不是「continue with a host-only connection」。
     失败面不止「对端没给」这一种:device 块、device QP 的分配失败,或 device QP 起不来,
     现在都走同一条路(server 整条连接回退 TCP,demanding 的 client 拿
     `EDEVICECHANNEL`),不再静默降级成 host-only —— 跟 host 侧同类失败的结果一样,也就
     少了一种「连上了但第二通道莫名其妙没开」的运行态(见 §9)。
  3. 进 `ChannelSignature`,即连接隔离。
- **Server 侧是「允许同意」**。客户端要,就给;客户端不要,就按 host-only 伺候 —— 服务端
  没有「要求」这一说,response 的 device attachment 是 `done` 跑完之后才打包的,那时拦已经
  晚了,而 service 跑之前 `has_device_channel()` 一直是准的,应用自己判断即可。但**同意是
  opt-in 的**:同意一次要花掉 `rq_size × --rdma_gdr_recv_block_size` 的注册内存,不该由客户
  端一句话就替服务端花掉。留在 `SOCKET_MODE_RDMA` 的 server 会拒绝。

**第 3 条必须先于第 2 条落地。** 第二通道强制 `CONNECTION_TYPE_SINGLE`,而 SINGLE 的 Socket
是走 `SocketMap` 跨 Channel 共享的,不隔离的后果不对称:普通 RDMA Channel 先连上,要求第二
通道的 Channel 复用这条没协商过的 Socket,**device attachment 全部失败,而且取决于哪个
Channel 先建连**;反过来只是白付一份接收块,浪费但不错。同时,「建连失败」这件事之前被否决
过,理由正是它会连带杀掉同一条连接上的 host 流量 —— 隔离做完,这个反对意见就消失了。反序
落地会在灰度中途炸掉无关流量。

**为什么是 mode 而不是 `ChannelOptions` 上的一个 bool**(最初就是个 bool)。隔离这件事要求
它进 `SocketMap` 的 key,而 `socket_mode` 本来就该在 key 里 —— 一条 TCP 连接和一条 RDMA 连接
当然不能互相复用。做成 mode 之后,`ComputeChannelSignature()` 只需要认识 `socket_mode`
一个字段,`SocketOptions` 也少一个平行的 bool 要跟着传。

落点是 `ComputeChannelSignature()`,**两处都要改**:开头那个枚举了所有参与签名 option 的
提前返回快路径,以及后面的 hash body。快路径原本不看 `socket_mode`,于是「只改了
socket_mode、其余全默认」这个最常见的情况照样返回零签名 —— 那不只是漏了新 mode,**RDMA 和
TCP 的 Channel 本来就会因此共用一条连接**,顺手一并修掉。命名服务那条路免费,signature 已经
塞进 `ns_opt`。

代码里几乎所有判断问的都是「这条连接说不说 RDMA」而不是「哪一种 RDMA」,所以
`socket_mode.h` 里给了个 `IsRdmaSocketMode()`,免得到处写 `== SOCKET_MODE_RDMA` 漏掉新
mode(`transport_factory.cpp`、`input_messenger.cpp`、`rdma_handshake_server.cpp`、
`rdma_transport.cpp` 都是这一类)。真正区分两种 mode 的只有
`RdmaEndpoint::device_channel_required()`,它**不存副本**,每次去问
`_socket->socket_mode() == SOCKET_MODE_RDMA_AND_DEVICE` —— `socket_mode.h` 已经写明这个
mode 就是「这条连接要第二通道」的全部含义,再拷一份到端点上只会多一个能和它不一致的东西。

端点在构造函数里就读得到它:`Socket::Create()` 里 `_socket_mode = options.socket_mode;`
在 `_transport->Init(this, options)` **之前**(`socket.cpp:738` / `741`)。所以
`_device` 的 engage 点是构造函数(`ResetDeviceChannel()`),`Reset()` 末尾再调一次放回
原样,握手中途不需要任何「打开第二通道」的代码。两个读者:`ResetDeviceChannel()`,以及
握手末尾那道需求检查 —— 配置要第二通道、最后却没协商出来,连接判 `EDEVICECHANNEL`。
两端含义不同:client 是硬要求,server 是「愿意」,所以 server 端那道检查只在对端问过时才
成立(见 §9)。

**为什么不做成 per-RPC 拦截。** 一度打算加在 `IssueRPC` 里(socket 选定之后、`_pack_request`
之前),否决,两个理由。(a) 那里已经有了:`_pack_request` 走到 baidu_std 就在做这个检查,
失败后 `HandleSendFailed()` 会把 peer 加进 `_accessed` 当排除集传给 `SelectServer`,「灰度
中途漂到开了 GDR 的实例上」这个性质今天就有。(b) 它会重踩 §6.2 那个已修的坑:per-RPC 检查
只能问 `UNDECIDED` 放行的三态,而新连接的第一个 RPC 恰恰恒为 `UNDECIDED` —— 唯一没被
保护的,正是最该保护的那个。放在握手完成点则没有这个洞,连接根本到不了 ESTABLISHED。

---

## 7. 协议层:baidu_std 扩展

### 7.1 帧格式

device 长度**不进 meta,进定长帧头**。baidu_std 原有的 12 字节帧头是
`[magic "PRPC"][body_size:u32][meta_size:u32]`;带 device 数据的报文换一个 magic,
帧头变成 16 字节:

```
PRPC:  | 'P''R''P''C' | body_size:u32 | meta_size:u32 |                    12 B
GDRB:  | 'G''D''R''B' | body_size:u32 | meta_size:u32 | device_size:u32 |  16 B
```

`body_size` / `meta_size` 的含义与 PRPC 完全一致,只统计 host 字节;`device_size`
是本报文在 device 流上占的字节数,**不计入 body_size**(它根本不走这条 socket)。

为什么不放 meta:

* **parse 阶段不能反序列化 meta。** parse 要决定的是"这个报文能不能现在就交出去",
  这需要 device 长度;而 meta 的反序列化本来是 process 阶段的事。把长度放进 meta,
  parse 就得多解一次 protobuf —— 而 parse 跑在 socket 的输入串行段里,这一份开销
  是**按连接**而不是按报文摊的:同一条连接上的小报文,哪怕自己不带 device 数据,
  也要为别人多付一次 `ParsePbFromIOBuf`。
* **定长偏移读不出歧义。** `device_size` 在固定字节位置上,一次算术读取,收发双方
  对它的理解不可能不一致 —— 这一点后面会用来砍掉一条本来要加的错位断言。
* **不带 device 数据的报文一个字节都不多付。** magic 仍是 `PRPC`,帧头仍是 12 字节。
  即使连接协商了 device 通道也一样,判据是"这条报文有没有 device 附件",不是
  "这条连接有没有 device 通道"。

magic 选 `GDRB` 而不复用 `PRPC` + 标志位,是因为 brpc 的协议自动探测按 magic 的前
4 字节分流,现有 magic(`BDMS` `HULU` `POST` `PRPC` `RDM3` `RDMA` `SOFA` `STRM`)
没有冲突,新增一个不影响任何其他协议的探测路径。

两条推论,parse 侧据此校验:

* 发送侧只在 `!device_attachment().empty()` 时才发 GDRB,所以
  **收到 `device_size == 0` 的 GDRB 是协议错误**,直接 `PARSE_ERROR_ABSOLUTELY_WRONG`。
* GDRB 只说"这个报文有 device 半边,有多长",**不说这半边走哪条线**。走哪条线由收
  报文的一端看自己的 `sock->device_stream()` 决定:非空就从 device 流上切,空就继续
  从本条 TCP 流上读 —— 后者就是 §7.3 的回退。这两个答案在一条连接的两端必然一致
  (device 通道是握手期协商的连接属性),所以不需要任何额外的协商位来区分两种摆放。

注意 `body_size` 在两种摆放下含义都不变:只统计 host 半边。回退时 device 字节跟在
body 之后、**不计入 body_size**,parse 侧据 `device_size` 另算。这样"只关心 host 半边"
的代码(长度校验、meta 切分)在两条路径上是同一份。

### 7.2 传输层接口 DeviceStream

协议层不应该 `#include` RdmaEndpoint —— 按项目规范,协议相关代码进 `policy/`,
通用改动不藏在协议文件里。所以在 Socket 上开一个与传输实现无关的接口:

```cpp
// brpc/device_attachment.h
namespace brpc {

// 一条连接的 device 通道,作为一个缓冲区。
class DeviceStream {
public:
    // ---- 接收侧,给协议层 ----
    size_t size() const;                        // 流头部还没被切走的字节数
    size_t cutn(DeviceAttachment* to, size_t n);// 切走 n 字节;短返回 = 还没到齐
    uint64_t cut_offset() const;                // 迄今切走的总字节数,见下

    // ---- 发送侧,给协议层 ----
    void AppendForSend(DeviceAttachment&& data);

    // ---- 背压,给协议层 ----
    void SetPending(size_t pending_msgs, size_t pending_bytes);

    // ---- 给持有它的传输层 ----
    DeviceAttachment* recv_stream();            // 到达的 device block append 到这
    DeviceAttachment* send_stream();            // 传输层从这里 post
    bool HasQueuedData() const;
    void DiscardQueuedData();
    void Reset();
    // ...
};

}  // namespace brpc
```

`cut_offset()` 不用来校验任何东西(长度在帧头的固定偏移上,收发双方不会理解不一致),
但它是唯一能让 `cutn()` 的跨 block 记账错误显形的数,所以留给单测和 `DebugInfo()`。

**它是具体类,不是基类。** 早先的版本把它写成纯虚基类、由 `RdmaEndpoint` 继承,那张
vtable 买到的只有一件事:让 `brpc/rdma/*.h` 和 `#if BRPC_WITH_RDMA` 不出现在核心协议
代码里。而组合同样买得到 —— 上面的状态没有一个字段碰 verbs,于是 endpoint **持有**一个
`DeviceStream` 成员,RDMA 那部分(post / 信用 / CQ)围着它做。实现只有一个,继承点只有
一个,基类就是纯粹的开销。`SetPending()` 之后要通知传输层,用的也是一个函数指针
(`set_pending_hook()`)而不是虚函数,理由相同。

`Socket::device_stream()` 对非 GDR 连接返回 `nullptr`,对 GDR 连接返回
`RdmaEndpoint` 里那个成员的地址。

### 7.3 回退:没有 device 通道时,device 半边走 TCP

`DeviceMessage`(原 `BaiduDeviceMessage`)是 baidu_std 里唯一同时产出两种摆放的地方。
它的 `AppendAndDestroySelf()` 是整条发送路径上**第一个、也是唯一一个**能看到 socket
的点(§8.1:device 半边必须从 socket 的单写者里入队,否则两条流的配对顺序就乱了),
所以"走 device 通道还是走 TCP"这个分岔只能开在这里。

但**只有摆放需要这个分岔,序列化不需要**:GDRB 头说的是 device 半边有多长,不是它从哪
条线下去;`body_size` 在两种摆放下都只数 host 字节,`device_size` 都在 16 字节头的第 12
字节上。也就是说**两种摆放下的 header + meta 字节完全相同**,与 socket 无关。所以头和
meta 在 `PackRpcRequest()` / 服务端的 `SendRpcResponse()` 里就序列化好,和非 GDR 路径
同一个时刻、同一段代码;`AppendAndDestroySelf()` 只做摆放:

```cpp
// 构造时(在 PackRpcRequest / SendRpcResponse 里,写路径之外)就已完成:
//   SerializeRpcHeaderAndMeta(&_host, meta, body->size(), _device_data.size());
//   _host.append(body->movable());
// 这时 EstimatedByteSize() 返回的 _host.size() 已经是准确值了。

// AppendAndDestroySelf():写路径上只剩摆放
DeviceStream* ds = sock->device_stream();
if (ds != nullptr) {                       // 有第二通道
    ds->AppendForSend(std::move(_device_data));
    out->append(_host.movable());
} else {                                   // 回退:整包都走这条 TCP
    butil::IOBuf device_buf;
    _device_data.copy_to(&device_buf);     // 设备内存在这里做 D2H
    out->append(_host.movable());
    out->append(device_buf.movable());
}
```

回退分支先把 D2H 拷进一个独立的 `device_buf` 再往 `out` 里放,是因为这个拷贝会失败,而
失败时 host 半边**不能已经在 `out` 里** —— Socket 没有把写队列里的字节收回来的办法。
`sock == nullptr`(消息在上线之前就被丢弃)也要正常走完析构:`~DeviceAttachment` 释放
显存,这正是让它走 `SocketMessage` 的原因。

省下来的不只是几微秒:`AppendAndDestroySelf()` 跑在 `Socket::KeepWrite` 的单写者上,
那是全连接串行的一段,把 protobuf 序列化留在这里等于让所有并发请求排队等它。

收端对称:parse 看 `sock->device_stream()`,非空走 §8 的 pending list,空就把 body
之后的 `device_size` 字节 H2D 拷进 `msg->device_payload`。

#### 为什么要有回退

不回退的版本里,`request_device_attachment()` 是一个"只有对端也开了 GDR 才能用"的
接口:同一份业务代码在没有 RoCE 网卡的测试环境、在灰度中还没升级的实例上、在被
`ChannelOptions` 配成普通 TCP 的旁路链路上,全都直接 `EDEVICECHANNEL` 失败。这不是
"性能退化",是"跑不起来",代价远大于一次 D2H。

回退**不是性能路径**,它比 GDR 路径多两次拷贝(发端 D2H、收端 H2D)还多一份 host
内存驻留。想知道自己有没有落在回退上,用 `Controller::has_device_channel()`;想强制
不回退,把 channel 配成 `SOCKET_MODE_RDMA_AND_DEVICE` —— 那条连接协商不到第二通道就
根本建不起来(§6.3),于是 `device_stream()` 要么非空、要么没有连接可发。

#### DeviceAttachment ↔ IOBuf

回退需要两个方向的转换,都加在 `DeviceAttachment` 上:

```cpp
// 发端:把每个 segment 拷到 *out 尾部。is_host 的 segment 直接 memcpy,
// 其余走 D2H。
int copy_to(butil::IOBuf* out) const;

// 收端:从 *from 头部搬走 n 字节,落进新分配的 attachment 内存。
int append_from_iobuf(butil::IOBuf* from, size_t n);
```

`copy_to()` 往 `IOBufAsZeroCopyOutputStream` 拿到的块里直接 D2H,不经中转缓冲,所以
设备段也只有一次拷贝。

`append_from_iobuf()` 按 `-rdma_gdr_recv_block_size` 切块分配,让**回退收到的
attachment 和 GDR 收到的形状一致** —— 同样是一串等长块,业务侧遍历 segment 的代码
不需要分两种情况写。分配失败时整批丢弃(先建在局部 attachment 上,成功才 move 进
去),不会留下半截。

#### 没有内存池的进程也要能用

回退的真正受益者是**根本没开 GDR 的进程**:它没有 `rdma::` 的显存池,`IsGdrAvailable()`
是 false。于是两处原来的硬门被拆掉:

* `append_new()`:池不可用时退化成 4KB 对齐的普通 host `posix_memalign`,`lkey = 0`、
  `is_host = true`,块上挂一个 `free()` deleter 所以永远不会被还回池里。收端能不能
  把报文交上去,不再取决于本进程有没有显存池。
* `append_user_data()`:去掉 `IsGdrAvailable()` 的 ENODEV 门。注册照常尝试
  (有池/有 PD 就注册),注册不了时**只在本进程确实没有第二通道时**才接受 `lkey = 0`
  的未注册段;池是好的却注册失败(比如 region 槽用尽)仍然失败 —— 那种段可能被发到
  device QP 上,而 `lkey = 0` 的 sge 是发不出去的。

由此得到一条不变式,值得写下来:**`lkey == 0` 的 segment 只可能诞生在没有第二通道的
进程里,所以它永远不可能走到 device QP 上**。这就是不需要在 `AppendForSend()` 前再补
一次注册的原因。

顺带一个坑:判断"用户给的指针是不是显存"要问 CUDA,而原来的 `IsDevicePointer()` 在
池没起来时直接返回 false(理由是那时显存也注册不了,问了也白问)。回退路径把这个理由
作废了 —— 现在会对着这个指针 `memcpy`,把显存当 host 内存读会直接段错误。所以补一次
懒探测:池没起来时,第一次走到这里才 `cudaGetDeviceCount()` 一次并缓存结果。

#### 还是会失败的情况

回退之后 `EDEVICECHANNEL` 只剩一种触发条件:**响应带 device attachment 又同时开了
stream**(`cntl->has_remote_stream()`)。因为 `SendStreamData()` 会重新切帧,而两种
摆放的长度记账都建立在"一个报文一个帧"上。这和有没有第二通道无关,所以不能回退,只能
报错。

相应地,`PackRpcRequest()` 里那个 `DEVICE_CHANNEL_OFF` 就直接失败的检查删掉了:
`DEVICE_CHANNEL_OFF` 现在是一个完全合法的状态,意思是"走回退"。`DEVICE_CHANNEL_UNDECIDED`
本来就是放行的,两者归一,那段三态逻辑连同它的注释一起消失。

还有一处量上的差别:回退时 device 字节是 host 字节,**要受 `-max_body_size` 管**。parse
侧因此在回退路径上用 `body_size + device_size` 去比这个上限,否则对端声明一个 4GB 的
`device_size` 就能让收端无上限地攒 read buffer。GDR 路径不需要这条,那边的量由 device
QP 的流控管着(§9)。

反过来,`DeviceMessage::EstimatedByteSize()` 仍然只算 host 半边,回退时因此**少算了
`device_size`**。这是有意的:这个估算在拿到 socket 之前就要给出,而它只影响写队列的
拥塞计数和 span 里报的报文大小;让主路径(GDR)把一个 1GB 的 tensor 算进 host 写队列,
比让回退路径少算一点危害大得多。

---

## 8. 解析:pre-flight + pending list

这是本设计最需要说清楚的部分。

### 8.1 两条流的对齐凭什么成立

host QP 和 device QP 之间**没有任何 ordering 保证**(不同 QP,RC 只保证 QP 内部保序)。
但对齐关系依然是确定的,理由是:

- 发送侧按报文顺序往 device 流写入 → device QP 内 RC 保序 → device 流的分段顺序 = 发送顺序。
- 发送侧按报文顺序往 host 流写入 → host QP 内 RC 保序 → host 报文顺序 = 发送顺序。
- 因此:**「host 报文序中第 k 个带 device 附件的报文」↔「device 流中第 k 段」**。

这个对应关系与两条通道的相对时序无关。**唯一的要求是:切 device 流的顺序,必须等于
host 报文的到达顺序。**

### 8.2 pending list:解析顺序与切分顺序解耦

朴素做法是严格队头阻塞:device 字节不够就一直等,后面的报文全堵着。正确,但一次大的显存
传输会把同连接上所有小 RPC 的延迟拖垮。

更好的做法是把**解析顺序**和**切分顺序**解耦:

```
CutInputMessage 每次被调用:

  ① 先尝试排空 pending list(FIFO):
       while (!pending.empty()):
           head = pending.front()
           if device_stream->size() < head.device_size: break   // 严格 FIFO,不跳过
           device_stream->cutn(&head.msg->device_payload, head.device_size)
           pending.pop_front();  派发 head.msg

  ② 再从 _read_buf 解析新报文:
       读 magic;PRPC -> header_size=12, device_size=0
                GDRB -> header_size=16, device_size 从 header+12 读出
       pre-flight: _read_buf 里 host 部分是否完整?不完整 -> NOT_ENOUGH_DATA(不消费)
       消费 host 部分,构造 msg
       if device_size == 0:
           立即派发                        // 不消费 device 字节,插队完全安全
       else if pending.empty() && device_stream->size() >= device_size:
           立即切、立即派发                 // 快路径
       else:
           挂到 pending list 尾部
```

关键点:**不带 device 附件的报文消费 0 个 device 字节,它插队对 device 流的偏移没有任何
扰动**,所以放它先走是安全的。而带 device 附件的报文全部经过同一条 FIFO,切分顺序被 list
的顺序锁死。

这样一次大传输只会阻塞后续同样需要 device 数据的报文,不影响普通小 RPC。

### 8.3 parse 必须可重入

device 字节不够时,已经从 `_read_buf` 消费掉的 host 字节吐不回来。所以 parse 必须
**先 pre-flight 判满,再消费**,沿用 baidu_std 现有的写法:

```cpp
// 现有:host 部分是否完整(header_size 由 magic 决定,12 或 16)
if (source->length() < header_size + body_size) {
    return MakeParseError(PARSE_ERROR_NOT_ENOUGH_DATA);
}
```

注意判据里**不包含** device 字节 —— host 部分完整就可以消费并构造消息,device 不够只是
挂到 pending list,不是回退。这与「严格 HOL」的区别正在于此。

### 8.4 pending list 挂在哪、要不要加锁

**不需要加锁。** 单个 Socket 的输入处理天然串行:`socket.cpp:2253` 是
`_nevent.fetch_add(1, ...) == 0` 才启动处理,其他事件只是累加计数让当前处理者再轮一圈;
`PollCq` 用的也是同一个 `MoreReadEvents(&progress)`(`rdma_endpoint.cpp:1548`)。
只要 pending list 只在 `ProcessNewMessage` / parse 路径里被访问,就是单线程访问。

**挂在 `Socket::parsing_context()`**,清理免费:`socket.cpp:1025` 在 socket reset 时
`reset_parsing_context(nullptr)` 会 `Destroy()` 掉它。连接失败时 pending 住的显存和挂起的
Controller 在那里统一释放。

**一个约束**:`socket.cpp:2814` 和 `:2835` 有两处
`CHECK(parsing_context() == nullptr) << "... the protocol implementation is buggy"`,
连接归还 SocketPool 时会触发。**因此 GDR 连接限定 `CONNECTION_TYPE_SINGLE`**,
在 Channel 初始化时校验,不满足直接报错。

### 8.5 乱序完成不是新语义

bRPC 本来就不保证一条连接上的报文按序处理:服务端每个请求派发到独立 bthread,客户端靠
`correlation_id` 匹配响应。pending list 让带 device 附件的报文晚于后到的小请求完成,
落在既有语义内,不引入新的约束。

---

## 9. 流控与背压

两条通道各自一套信用,复用现有的
`sq_window_size` / `remote_rq_window_size` / `new_rq_wrs` / `SendAck` 机制。

「各自一套」不是把每个字段抄一遍:一条通道的全部状态收在一个结构体里
(`rdma_endpoint.h`)。共用的流控计数器**和 QP 资源**在基类 `struct QpChannel`
(`resource` / `qp()` —— 每条通道一个 `RdmaResource`,即它自己的 QP 和 CQ,见 §4.2;
外加 `unsolicited` / `unsolicited_bytes` / `accumulated_ack` 三个摊销 solicited 通知的
计数器,见 §9.3);两条通道各自私有的字段在
`struct HostChannel : QpChannel`(`input_processor` / `sbuf` / `rbuf` / `rbuf_data`)和
`struct DeviceChannel : QpChannel`(`stream` / `sbuf` / `rbuf_data` / `rbuf_lkey` /
`recv_block_size` / `pending_over_watermark`)里。`RdmaEndpoint` 持有 `_host` 和
`butil::optional<DeviceChannel> _device`(装着 == 这条连接有第二通道,见下),自己只留
`_socket` / `_state` / `_handshake_version` / `_outgoing_ece` / `_comp_channel` /
`_cq_sid` / `_read_butex` 这些**整条连接**一份的字段 —— comp_channel 在这里而不在
`QpChannel` 里,正是因为它是连接的、不是通道的。

于是 `SendImm()` / `SendAck()` / `TakeAcks()` / `PostDataWr()` / `HandleCompletion()` /
`ReturnSendCredits()` / `BringUpOneQp()` / `ModifyQpToInit()` / `ModifyQpToRts()` /
`ArmChannelCqs()` / `DumpChannel()` 这些原本 host 一份、device 一份的函数都只剩一份,
第一个参数是要操作的那条通道;`GetAndAckEvents()` / `ReqNotifyAllCqs()` /
`CollectPollTargets()` 里「host 一遍,有 device 再来一遍」也归一成遍历
`CollectChannels()` 返回的 1 或 2 条通道。

`PostRecv()` / `DoPostRecv()` / `SolicitWr()` 更进一步,连参数都不要了 —— 它们只碰一条通道
自己的字段(`rq_received` / `rq_size` / 三个 solicited 计数器)和自己的虚函数
(`PrepareRecvSlot()` / `ReleaseRecvSlot()`),没有一处需要端点,所以它们就是 `QpChannel`
的成员函数。「补几个接收块」这件事本来就是通道的行为,不是端点对通道做的事。

两条通道**真正**的差异用虚函数表达,一共八个:`name()`(日志名)、`Reset()`、
`HasBacklog()`、`AllocateBuffers()`、`TakeRecvData()`、`ReleaseSendBuf()`、
`PrepareRecvSlot()`、`ReleaseRecvSlot()`。基类不带身份位:运行时需要区分是哪条通道的
只剩一处 ——「ack 路径可不可以扣住信用」(只有 host 方向会,见 §9.2),而它真正要问的本来
就是「你是不是我的 host 通道」,`TakeAcks()` 里比较 `&qc == &_host` 即可。

`sq_size` / `rq_size` 是本端配置,构造时定一次,`Reset()` 复用连接时只清运行态、
不动这两个,否则重连会以另一个深度起来。两条通道的「构造时」不是同一个时刻,这个差别现在
由结构表达而不是靠注释:`_host` 是直接成员、一个端点只构造一次,所以 host 深度在
`HostChannel()` 里读一次 flag 就定死;`_device` 每次协商出第二通道时重建,所以
`DeviceChannel()` 每次重读 `--rdma_device_sq_size` / `--rdma_device_rq_size` /
`--rdma_gdr_recv_block_size`。

`Reset()` 是虚函数,每条通道清自己那一层:`QpChannel::Reset()` 清基类的
`resource`、全部流控计数器和三个 solicited 计数器,`HostChannel::Reset()` 先调基类再清
自己的 `sbuf` / `rbuf` / `rbuf_data` / `input_processor`。端点因此不再手抄一份
`HostChannel` 的字段清单
—— 那份清单端点本来就看不见,加一个字段就得记得回来改,`RdmaEndpoint::Reset()` 塌缩成
「拆资源 + 清自己那几个连接级字段 + `_host.Reset()` + `ResetDeviceChannel()`」六行。

**第二通道的存在性就是 `_device` 这个 optional 的存在性**(`docs/cn/gdr_channel_unify_plan.md`
§1)。没有 `wanted` / `established` 之类的标志位可以跟它不一致,也没有「半开」的中间态:
`_device` 在**构造函数**里就按 socket mode engage(`ResetDeviceChannel()`),不再由握手
中途的两处代码各自打开;此后任何一步失败都 `_device.reset()` 并连带整条连接失败 ——
跟 host 侧同类失败的结果一样。server 端不给不用第二通道的 peer 花注册内存这条性质由顺序
保证:`ApplyRemoteHello()` 在 `AllocateResources()` **之前**跑,对端 hello 里没有 `device`
子消息就当场 `_device.reset()`,而真正花内存的 `AllocateBuffers()` 在那之后。
`~DeviceChannel()` 就是完整拆解(ack 掉 CQ 事件 → 销毁 QP → 归还剩下的 device 块),所以
`_device.reset()` 是唯一需要写的拆解动作;`DeallocateResources()` 里它排在很前面,因为
device 的 CQ 在事件模式下挂在连接的 `_comp_channel` 上,销毁顺序不能反 —— 唯一排在它
前面的是 polling 模式下的 `PollerRemoveCqSid()`,poller 线程是这些 CQ 唯一的读者(§10)。
「这条连接配没配第二通道」不再有副本:`device_channel_required()` 每次去问
`Socket::socket_mode() == SOCKET_MODE_RDMA_AND_DEVICE`(见 §6.3),`Reset()` 末尾据此把
`_device` 放回构造完的样子,所以重连不会悄悄降级成 host-only。


### 9.1 device 方向的反压是自限的

device 字节先到、对应的 host 报文还没到 → 没有 pending 条目认领 → device 接收块回收不了
→ device 信用耗尽 → 发送侧在 device 通道上自然被限速。这个方向不需要额外机制,而且限得对:
发送侧本来就跑在前面了。

### 9.2 host 方向必须显式扣信用

反向不自限。`SendAck(_host, 1)` 在 `HandleCompletion()` 里紧跟 `qc.PostRecv()` 调用
—— **接收块一重新挂上就还信用,与上层有没有消费完全无关**。
所以 host 跑在前面时,pending list 会无界增长。

处理:协议层每次增删 pending 条目都调用 `DeviceStream::OnPendingChanged`,传输层在
超过水位时扣住 host 侧的 `SendAck`:

```
--rdma_gdr_pending_bytes_watermark  (默认 256MB)
--rdma_gdr_pending_msgs_watermark   (默认 1024)
```

这条耦合只在真实过载时触发(严格 HOL 方案下它会是常态触发),属于保险丝而不是主路径,
但**不能省**:省掉它,慢消费者就是一条通往 OOM 的确定路径。

### 9.3 发送侧无锁:writer 是唯一的 poster

host 的 SQ ring 是严格的 SPSC:writer(Socket 的唯一活跃 writer,
`CutHostFromIOBufList()`)
推进 `_host.sq_current`、填 `_host.sbuf[sq_current]`;poller(`HandleCompletion()` 的
`IBV_WC_SEND`)推进 `_host.sq_sent`、`_host.sbuf[sq_sent].clear()`,中间一个
`MemoryBarrier()`,再 `sq_window_size.fetch_add()`。窗口保证两侧永不追尾,所以两个下标
各自单写、无锁。

这段归还信用的逻辑两条通道一字不差,抽成了 `ReturnSendCredits(QpChannel&, wnd)` ——
不是模板,因为「一个发送槽是什么」是虚函数 `ReleaseSendBuf()` 的事(host 清一个 IOBuf,
device 放掉一个 `DeviceAttachment`)。发送侧的另一半同理:两个 `CutFrom*` 各自取数据
(`IOBuf::cut_into_sglist_and_iobuf()` vs 遍历 `DeviceAttachment` 的 segment,数据源真的
不同),取完之后「填 imm、决定 solicited / signaled、`ibv_post_send`、推进 `sq_current`、
扣两个窗口」这条尾巴是共用的 `PostDataWr()`。其中 solicited 的决策是 `QpChannel` 上的
`SolicitWr()`,**两条通道跑同一套摊销**:`unsolicited` / `unsolicited_bytes` /
`accumulated_ack` 三个计数器任一越过阈值就 solicit,否则累加。device 侧曾经恒 `true`
(理由是「字节大而稀疏,没什么可摊销的」),但那个理由只对单个大包成立 —— 连续若干个
attachment 的场景下它就是每个 WR 一个 solicited 通知,而 1MB 的字节阈值恰好就是为这种
流量设的。阈值都取自本通道自己的 `local_window_capacity` /
`remote_window_capacity`,device 的队列浅(默认 16),所以它自然摊得比 host 短,不需要
另一套参数。而「最后一个 WR 强制 signaled」是 `PostDataWr()` 的普通参数,只有 device
传 true —— 给 host 也加上会让几乎每个 WR 都产生发送完成,那正是上面那套启发式要省的 CPU。

**关键在于 poller 从不 post。** 它只归还信用然后 `WakeAsEpollOut()`;真正的
`ibv_post_send` 永远由 writer 做。积压不在 endpoint 里,而在 Socket 的 `WriteRequest`
链表里,`KeepWrite` bthread 挂在 `_epollout_butex` 上等着被叫醒。

device 通道照抄这个结构。它比 host 多一个缺口:device 积压放在
`_device->stream` 的发送半边里、不在 Socket 写队列里,host 字节少、排干得快,`KeepWrite`
很可能早就退出了,没人再来消费。补法是让 `IsWriteComplete()` 知道传输层还欠着数据:

1. `Transport` 加 `virtual bool HasPendingWrite() const { return false; }`,
   `RdmaTransport` 转发到 `RdmaEndpoint::HasQueuedDeviceData()`(一个 atomic 字节数)。
2. `Socket::IsWriteComplete()` 把它并进 `return_when_no_more` —— 只要 device 还有积压,
   `_write_head` 就留成非空,`KeepWrite` 不退出。
3. `CutFromIOBufList()` 分成一句调度 + 两条独立的路:先 `FlushDeviceSendQueue()` 排 device
   积压,再 `CutHostFromIOBufList()` 走 host 的 WR 切分。device 那条不是塞在 host 逻辑前面
   的一段前言,而是自己一个函数 —— 它自己判断有没有积压、有没有 device 通道,没有就立刻
   返回 0,host 那条完全不必知道它存在。`AppendForSend()` 只入队,对齐 host 侧
   `WriteRequest::Setup()` 只往 `req->data` 追加。
4. `HandleCompletion()` 不再 post,只 `fetch_add` 信用 + `WakeAsEpollOut()`。唤醒条件两条
   通道也归一成一条:`qc.HasBacklog() ||`「窗口开得够大」—— 有积压就唤醒(挂在积压上的
   writer 没有别的东西会叫醒它),没有积压时两条通道都走 host 原有的省 CPU 启发式。
   问的是**这个 CQE 所属通道**有没有积压,不是整条连接有没有:`HasBacklog()` 是虚函数,
   host 恒 false、device 转发到 `stream.HasQueuedData()`。挂在 device 积压上的 writer 等的
   是 device 窗口,只有 device 完成能开;拿 host 的完成去唤醒它,`IsWritable()` 照样是
   false,白醒一次。
5. 于是 `_device->stream` 的发送半边 / `_device->sbuf` / `_device->sq_current` /
   `_device->sq_unsignaled` 全部 writer-only,`_device->sq_sent` poller-only,
   `_device_send_mutex` 删除。

跑起来是:`IsWriteComplete()` 返回 false → `KeepWrite` 循环 → `DoWrite()` 拿着空 IOBuf 调
`CutFromIOBufList()` → 排 device 队列 → `nw == 0` → `WaitEpollOut()` 挂 butex →
poller 归还信用时 `WakeAsEpollOut()` → 醒来接着排。就是 host 大包发送的那个循环。
butex 带版本号,「先唤醒后挂起」和「先挂起后唤醒」都安全。

三个落地细节:

- **`IsWritable()` 看哪条队列,由「还欠着谁」决定。** 它是 `WaitEpollOut()` 决定挂不挂起
  的判据。走到这里的 writer 一定是刚刚 `CutFromIOBufList()` 一个字节也没挪动,而那个函数
  是**先排 device 队列、再动 host WR** 的,所以:

  ```
  有 device 积压 → 只看 device 两个窗口(host 窗口此时无关:host 字节已经走完了)
  无 device 积压 → 只看 host 两个窗口(原有行为)
  ```

  两个方向都不能写成「任一 QP 能写就返回 true」:
  - 只看 host 窗口的话,「host 窗口全开、device 窗口关死、队列还在」会让 `KeepWrite`
    空转烧一个核 —— `IsWriteComplete()` 在 `HasPendingWrite()` 为真时不让它退出。
  - 反过来「host 窗口关死、device 窗口开着、队列还在」必须返回 true,否则 writer 被挂起,
    而唯一剩下的活(一次 device flush)明明有信用可做。这正是「任一 QP 能写」这种写法
    要用来解释、却解释不了的那一格。
  - 不会朝另一个方向空转:`CutFromDeviceAttachment()` 只在 device 两个窗口都为 0 时返回
    `EAGAIN`,否则至少 post 一个 WR。device 窗口开 + 队列非空 ⇒ 一定有进展。
- **`ReleaseAllFailedWriteRequests()` 会死循环。** 它靠「清空 `req->data` ⇒ 一定完成」
  收敛,传输层再说一句「还没完」它就永远转不出来。给 `IsWriteComplete()` 加一个默认 true
  的 `check_transport` 参数,这唯一一处传 false,同时 `DiscardPendingWrite()` 把积压丢掉
  (那些字节再也发不出去,留着只是占住显存到 socket 回收)。
- **device 侧的拆解不再需要那把锁。** 它原先靠锁挡住「poller 正在
  `CutFromDeviceAttachment()` 时把 QP 销毁在它脚下」;poller 不 post 之后这个互斥需求消失。
  调用时机也确实和 host 侧同款:要么握手还没把通道拉起来(从没 post 过),要么来自
  `Reset()`,而 `Socket::WaitAndReset()` 等到 nref 收敛、`Socket::OnRecycle()` 引用归零
  之后才会到那里。这条所有权规则现在由 `~DeviceChannel()` 承担 —— 拆解就是
  `_device.reset()`,独立的 `DeallocateDeviceResources()` 已经不存在(见 §9)。

为什么值得改:`butil::Mutex` 是 pthread mutex 不是 bthread 锁,poller(`PollCq`)抢不到
时阻塞的是整个 worker pthread;而事件模式下 host 和 device 的 CQ 共享一个 comp_channel,
一把 device 发送锁能把 host 的完成事件一起卡住(polling 模式下两条通道各有一个 CQ,但
poller 线程是同一个,一样卡)。host 路径刻意避开的就是这个。

---

## 10. polling 模式

polling 模式(`FLAGS_rdma_use_polling`)曾经是本设计明确不做的一项:它的 CQ 不挂
comp_channel,与「四个 CQ 共享一个 comp_channel」的前提直接冲突,所以 device 通道 +
polling 在启动时就被拒绝。

这个冲突其实只是把 comp_channel 当成了「一条连接的标识」。它不是——它是**事件模式下的
事件源**:一个 fd、一次 epoll 注册,唤醒一次就把这条连接所有 CQ 都轮一遍(§4.2)。
polling 模式没有事件源可言,poller 线程直接轮 CQ,所以它的等价物是**一条通道一个
`polling_cq`**(收发两个方向都报到这一个 CQ 上)。一条 GDR 连接在 polling 模式下因此有
**两个 polling CQ**,poller 每轮多轮一个,这是 polling 模式固有的代价(它本来就是拿 CPU
换延迟),而且只发生在真开了第二通道的连接上。

两种模式唯一的分叉点是 `CollectPollTargets()`:

```cpp
if (FLAGS_rdma_use_polling) {
    // 一条通道一个 CQ,收发都在里面,所以 is_send 恒为 false
    for (int i = 0; i < nchannels; ++i) {
        out[n].cq = channels[i]->resource->polling_cq;
        out[n].is_send = false;
        out[n].channel = channels[i];
        ++n;
    }
    return n;
}
// 事件模式:一条通道两个 CQ,recv 先于 send
```

下游全部不用改:`PollCq()` 拿到的仍然是一串 `PollTarget`,仍然按同一套轮转规则走,只是
事件模式下开头要 `GetAndAckEvents()`、轮空一圈之后要 re-arm 并从头重轮,polling 模式下
这两件事都没有(poller 线程马上会再进来一次,没有边沿可漏)。资源分配侧也只是把
`comp_channel == nullptr` 这条已有的分支走到:`AllocateQpCq(nullptr, ...)` 建一个
polling CQ,host / device 一视同仁;arm 那一步两条通道都按 `!FLAGS_rdma_use_polling` 跳过。

销毁顺序上多一条约束:polling 模式下 poller 线程是这些 CQ **唯一**的读者,而它是顺着
`_cq_sid` 找过来的,所以 `DeallocateResources()` 第一件事就是 `PollerRemoveCqSid()`,
在任何 CQ(含 `_device.reset()` 销毁的那两个)之前。

**不共用一个 polling CQ。** 一个 CQ 服务多个 QP 是允许的,两条通道共用能把轮的次数减半。
但那样 `PollCq()` 拿到一个 WC 之后必须靠 `wc.qp_num` 反查是哪条通道(现在是「哪个 CQ 出来
的就是哪条通道」),等于在热路径上加一次查表,而且 `RdmaResource` 就不再是「一条通道的 QP
和它的 CQ」这个干净的单位了。省一次 `ibv_poll_cq` 不值得。

### 10.1 配错的组合一律在启动时拒绝

这些配置每一个原先都是静默降级:endpoint 在握手中放弃 device 通道 → 握手末尾的
`require_device_channel` 检查把连接判成 `EDEVICECHANNEL` → 运维看到的只是「连接建不起来」,
且没有任何线索指向真正的原因。配置错误应该在启动日志里出现一次,而不是变成一串无法解释的
建连失败。

落点是 `RdmaTransport::ContextInitOrDie()`(`Server::Start()` / `Channel::Init()` 都会走到,
且**没有 once guard**,每次 Init 都重新检查)里的 `DeviceChannelOptionsAvailable()`,在
`GlobalRdmaInitializeOrDie()` **之前**返回 -1:

| 配置 | 为什么不可能成立 |
|---|---|
| `SOCKET_MODE_RDMA_AND_DEVICE` + `-rdma_enable_gdr=false` | 第二通道没有内存池可用 |
| `SOCKET_MODE_RDMA_AND_DEVICE` + `-rdma_client_handshake_version<3`(仅客户端) | device `qp_num` 在 v2 hello 里没有位置可放,见 §5.1 |

这张表曾经还有一行 `-rdma_use_polling`,理由是「device CQ 要挂在 host 连接的 comp_channel
上」。按上面 §10 的说法,polling 模式下两条通道都没有 comp_channel,各自有自己的
`polling_cq`,没有东西要共享,也就没有东西要拒绝。

第一条是本进程的属性,server / client 两个方向共用;握手版本只有客户端选得了,所以只在
客户端方向查(server 用什么版本由对端决定)。

这两条查的都是**配置**。初始化跑完之后再补一道查**结果**的:`socket_mode` 要 device 通道
但 `rdma::IsGdrAvailable()` 为假(典型是没编 `BRPC_WITH_GDR` 的构建,根本没有池可给),
同样在启动时返回 -1。`GlobalRdmaInitializeOrDie()` 对大部分 GDR 启动失败是直接退进程的,
但不是全部,所以这道也留着。

---

## 11. 分期

| 期 | 内容 | 可独立验证 |
|---|---|---|
| ① | 构建开关(`BRPC_WITH_GDR`,cmake/make/bazel)、CUDA 探测、device block pool、`DeviceAttachment` | 单测:分配/注册/释放/移动语义 |
| ② | 握手协商 `RdmaDeviceChannel`;双 QP / 四 CQ 分配与 `PollCq` 改造;device 侧流控**含 §9.2 的水位扣信用** | 单测:v3 握手字节布局、双方协商矩阵、v2 对端降级 |
| ③ | `DeviceStream` 接口 + `Socket::device_stream()`;`Controller` 的 device attachment | 单测:非 GDR 连接返回 nullptr、Reset 清空 |
| ④ | baidu_std meta 扩展、发送侧拆分、接收侧 pre-flight + pending list + 错位断言 | 单测:pending list 的 FIFO 与插队行为 |
| ⑤ | 端到端 RPC、示例程序、文档 | 需要真实 GPU + RoCE 卡 |
| ⑥ | 内存类型与通道解耦(§5.3):`-rdma_attachment_memory`、`IsGdrAvailable()` 分家、`require_device_channel` | 单测 + 四种组合的 E2E,后者需要真卡 |

**水位扣信用放在第 ② 期而不是最后**:它不是优化,是这个设计的必需组件。缺了它,
第 ④ 期一上真实负载就是 OOM,而且会被误判成 pending list 的实现 bug。

第 ⑥ 期内部也有顺序:`ChannelSignature` 隔离必须早于「握手不满足就建连失败」,理由见 §6.3。

---

## 12. 与 PR #3144 的差异

| | PR #3144 | 本设计 |
|---|---|---|
| 显存容器 | 复用 `IOBuf`,给 Block 加 `IOBUF_BLOCK_FLAGS_GPU_MEMORY` 标志 | 独立的 `DeviceAttachment`,显存永不进 IOBuf |
| 数据通道 | 单 QP,host/device 数据混在同一个 `_read_buf` | 双 QP,两条独立字节流 |
| 协议探测 | 显存 IOBuf 会被喂给所有协议的 host memcpy 解析器 → 必然段错误 | `_read_buf` 里只有 host 数据,协议探测不受影响 |
| 小包 | 强制 `zerocopy = true`,绕过 `rdma_zerocopy_min_size`,每个小包一次阻塞式 `cudaStreamSynchronize` D2H(在 worker pthread 上,阻塞该 worker 上所有 bthread)+ 一个 512KB 显存块 | 小包完全不碰 device 通道,零额外开销 |
| 块大小协商 | `g_gdr_recv_block_size` 从未写进握手,发送侧按 host 块大小切分 → 64 倍显存放大;`--rdma_recv_block_type=huge` 时 sge 长度错误 | `RdmaDeviceChannel.block_size` 独立广播 |
| 流控 | 与 host 共享 | 各自独立 + 水位耦合 |
| 同步 | 传输层做 D2H 同步 | 传输层不做任何同步 |
| 附件语义错误 | `FillResBufGpu` 把原来的 `break` 改成从 helper `return`,`attachment_size > res_size` 时控制流继续走到 `DeserializeRpcMessage(*res_buf_ptr, ...)`,而 `res_buf_ptr` 仍指向未切分的 device payload → 段错误 + 错误码被覆盖 | 不涉及 |

另外几处 PR #3144 的实现细节问题,本设计不会复现:
- `get_first_data_ptr()` 不检查 `IOBUF_BLOCK_FLAGS_USER_DATA`(对比 `get_first_data_meta()` 是检查的),
  且返回 `r.block->data` **漏了 `r.offset`** —— 对任何 pop 过的 IOBuf 都是错的。

---

## 13. 测试

单测统一写在 `test/brpc_rdma_unittest.cpp`。三种跑法:

```bash
# 1. 无 GPU、无 RoCE 卡的 CI(GDR 代码按 host 内存桩跑)
bazel test --config=test --config=rdma //test:brpc_rdma_unittest

# 2. 编译进真 CUDA,仍用桩跑(验证 --config=gdr 这条编译路径)
bazel test --config=test --config=gdr  //test:brpc_rdma_unittest

# 3. 真 GPU + 真卡
bazel build --config=test --config=gdr //test:brpc_rdma_unittest
./bazel-bin/test/brpc_rdma_unittest --rdma_test_enable --gdr_test_real_device
```

不依赖硬件的部分(握手字节布局、协商矩阵、`DeviceAttachment` 语义、pending list 的
FIFO/插队行为)必须在无 GPU、无 RoCE 卡的 CI 上跑通 —— 沿用现有 `g_skip_rdma_init` 的思路,
device 侧提供 `g_skip_device_alloc_for_test`:`RawDeviceAlloc` 改用 `posix_memalign`、
`RawDeviceRegister` 返回一个假 lkey。这不是「跳过测试」,池的指针簿记、
`DeviceAttachment` 的引用计数、pending list 的切分顺序在桩下跑的是同一份代码,
而被测代码本来就不允许解引用这些指针。

需要真实硬件的部分由 `--gdr_test_real_device` 控制(必须与 `--rdma_test_enable` 同时给,
否则 `g_pd` 为空、`ibv_reg_mr` 直接段错误 —— 已在 `EnableGdrForTest()` 里断言)。这部分
包括:块确实落在 `--rdma_gdr_device_id` 指定的 GPU 上(用 `cudaPointerGetAttributes` 校验,
且刻意选非 0 号卡,否则测了等于没测)、ordinal 越界被拒、以及唯一一个把显存真正放上线的
端到端用例。

端到端用例是**多轮**的,一条长连接跑 10 轮 × 16 个并发 RPC。轮次不是凑数:单轮总是从一个
全新的、块对齐的流开始,而那恰好是切分记账 bug 唯一不可见的状态。多轮之后才会出现接收块
被回收重投、device 信用回流、pending list 排空到底(host 信用不再被扣住)、以及
`Controller::Reset()` 是否真的丢掉了两个 device attachment —— 10 轮加起来搬运的字节数远超
16MB 的接收窗口。每轮换一个尺寸:

```
64KB, 8, 4KB, 1MB+7, 40, 700KB-13, 1536KB, 128KB, 1MB, 33KB+1
```

对着 1MB 的接收块选的:块内、正好一块、刚过一块(报文横跨两块),以及若干让下一条报文从块
内不同偏移开始的。其中 `8` 和 `40` 不是凑数的小尺寸,它们打的是 §4.6 那条 scatter-to-CQE 路径
(整条报文被内联);`1MB+7` 打的是同一条路径的另一半 —— 大报文的 7 字节尾部 WR。
每 4 个 RPC 里有 1 个不带显存,于是 PRPC 与 GDRB 帧在同一条连接上交错,不带显存的那条也就
有机会去插一个正在等别人 tensor 的 pending list 的队。填充用的是 `(k + tag) % 251` 这种
位置相关的模式而不是常量:常量填充会盖掉「整块错位」这一种切分错误,而 251 是质数,
任何小于 251 个块的位移都会表现成 memcmp 失败。服务端用 `append_ref` 原地回弹,客户端
D2H 拷回来逐段比对。

用例结尾还断言 `getenv("MLX5_SCATTER_TO_CQE") == nullptr`。这两件事合起来才有意义:8/40 字节
那两轮过了,说明 scatter-to-CQE 在 device QP 上确实关掉了;而环境变量没被设,说明关它走的是
§4.6 的单 QP 路径、host QP 没受牵连。少了任何一条,另一条都证明不了什么。

第二个端到端用例专打 §9.3 的信用耗尽路径:一条 64MB 的 device 附件对着 16 深的 device SQ
和对端 16 块的 device RQ,一次发不完,**只能**走「post 到信用耗尽 → 挂 butex → poller
唤醒 → 续发」,每个方向至少三轮;host 附件一个字节都不带,好让 host 流早早排干。
它是 §9.3 唯一的回归护栏 —— 两个方向都会露馅:`HasPendingWrite()` 没接上就是
`KeepWrite` 提前退出、积压永远发不出去(表现为 RPC 超时);`IsWritable()` 没看 device 窗口
就是空转烧核。后半段再并发发 4 条 12MB 的,在任何一条排干之前就把它们全部入队,证明积压
跨 `AppendForSend()` 调用仍然是 FIFO,而不只是跨 WR 是 FIFO。

`--rdma_gdr_*` 显存池那边有两个不依赖硬件的用例:512 个块必须正好落在 8 个 region 里
(region 数不再随 block 数线性增长),以及 4 个线程混着 alloc/free/`GetDeviceLKey` churn,
把所有指针过一遍 `std::set` 断言没有一块被发出去两次、lkey 都对、region 数有界,最后从
「与分配它的线程不同」的线程上全部释放。

### 13.2 内存类型解耦(§5.3)怎么测

`-rdma_attachment_memory` 是进程级的、且必须在池起来之前定下,所以它自己一个 fixture,
在 `GlobalGdrInitialize()` 之前改 flag、`TearDown` 里 release 并还原。四种组合的 E2E 因此
要跑**两遍**这个二进制:

```bash
BIN=./bazel-bin/test/brpc_rdma_unittest
FILTER=--gtest_filter=GdrRpcTest.source_and_landing_memory_kinds_are_independent
$BIN $FILTER --rdma_test_enable --gdr_test_real_device                            # host→device, device→device
$BIN $FILTER --rdma_test_enable --gdr_test_real_device --rdma_attachment_memory=host  # host→host, device→host
```

源内存类型是 per-append 的,一轮里两种都发;落点是进程配的,用例**断言自己正在跑哪一半**
(`ASSERT_EQ(!expect_host_landing, IsAttachmentMemoryDevice())`),否则一个悄悄落错内存的
run 会两遍都绿、等于只测了同一条路径。这个断言不是多余的谨慎:`host` 那一半最初就是**空跑
的**,因为 `--rdma_attachment_memory=host` 压根没传进去(见 13.4),修好之后第一次真跑就
挂了 —— 挂在 §4.5 那个 region 表混装、把落点池块当成显存的 bug 上。

不依赖硬件的部分覆盖三件事:`host` 模式下 `append_new()` 的块可读且自称可读(直接 memset
它,这是唯一能抓住池还在 `cudaMalloc` 的断言);`GetDeviceLKey()` 分得清池块和用户 region;
以及同一个池块,经 `append_user_data()` 进来的和经 `append_new()` 进来的,`is_host` 必须
一致 —— 最后这条就是上面那个 bug 的回归护栏。需要真卡的只剩一条:`device` 模式遇上拿不到
的 GPU 必须 `ENODEV`,而**同一台机器上** `host` 模式照样起得来(用越界的
`--rdma_gdr_device_id` 造这个场景,`CUDA_VISIBLE_DEVICES` 到这时候已经晚了)。这条不对称
正是「没 GPU 的节点也能跟有 GPU 的节点通」的全部依据。

### 13.3 不需要卡的那些用例

以下几组既不需要 RDMA 卡也不需要 GPU:

- **配置拒绝(§10.1)** —— `device_channel_mode_is_allowed_in_polling_mode` /
  `..._without_the_gdr_pool` / `device_channel_mode_needs_the_v3_handshake`。
  两条拒绝都在 `GlobalRdmaInitializeOrDie()` 之前返回 -1,所以用例没有副作用,也不依赖彼此
  的顺序(`ContextInitOrDie()` 没有 once guard,每次 `Channel::Init()` / `Server::Start()`
  都重新查)。正例一并断言:v2 + `SOCKET_MODE_RDMA` 必须照样成功,否则这两条检查就不是
  「拒绝配错的组合」而是「拒绝 v2」了。
  第一条用例原先叫 `..._is_refused_in_polling_mode`,断言的是拒绝;polling 模式支持之后它
  **反过来**断言接受(§10),而且仍旧断言另外两条拒绝在 polling 模式下照样生效 —— 否则
  「删掉一条检查」很容易顺手把整个函数删空。它查的是
  `RdmaTransport::OptionsAvailableForRdma()` / `OptionsAvailableOverRdma()`,不是
  `Channel::Init()` / `Server::Start()`:后者会接着去起这个 tag 的 poller bthread,而全局
  初始化时 `-rdma_use_polling` 是关的进程里根本没有 poller group 给它们跑(见
  `gdr_polling_and_send_path_plan.md` 的实施记录)。

- **帧不变性(§7.3)** —— `frame_does_not_depend_on_where_the_device_half_goes`
  把同一个请求打包两次(一次给有 device 通道的 socket,一次给普通 socket),断言两次的
  `EstimatedByteSize()` 相等、且「第一次的全部字节 + device 字节 == 第二次的全部字节」。
  这是把序列化移出写路径的**全部依据**:只要这条成立,序列化就与 socket 无关。
  `estimated_byte_size_counts_the_bytes_that_get_written` 补上第二通道那一侧的精确性。

- **`IsWritable()` 真值表(§9.3)** —— `is_writable_follows_the_queue_that_still_has_work`
  直接摆弄 `_host` / `_device` 的窗口,五格全查。最关键的两格是
  「有积压 + host 关 + device 开 ⇒ true」(修掉的那个 bug)和
  「有积压 + host 开 + device 关 ⇒ false」(说明它为什么不是「任一 QP 能写」)。

- **通道归一(§9)** —— 六条,都不需要卡:
  `device_channel_lives_and_dies_with_the_optional` 断言「有没有第二通道」只由 `_device`
  这个 optional 回答。`Reset()` 之后 `_device` 会按 socket mode **重新** engage(所以这条
  同时盯住「重连不会悄悄降级成 host-only」),要断言「真的没有」那半边的行为就显式
  `_device.reset()`(握手没谈成时的样子),再查 `HasQueuedDeviceData()` /
  `HasPendingWrite()` / `Socket::device_stream()` 都还答得出来 —— 这是连接已经死掉、
  `HasPendingWrite()` 仍会走的那条路。
  `device_channel_is_built_by_the_constructor` 是同一件事的另一头:device-mode socket 刚
  建好、握手一步都没走时 `has_device_channel()` 就为真,`SOCKET_MODE_RDMA` 的为假,各自
  `Reset()` 之后都回到自己构造完的样子。
  `send_completion_credits_only_the_channel_it_came_on` 直接造 `IBV_WC_SEND` 分别喂给两条
  通道,断言只有对应通道的 `sq_sent` / `sq_window_size` / 发送槽前进 —— 这是两条 poller
  路径合成一条之后唯一无硬件可测、且反了就会把 NIC 还在读的发送槽放掉的地方。
  `host_channel_resets_every_field_it_owns` 把 `_host` 的每个字段都写成非零值,`Reset()`
  之后逐个断言归零,并断言 `sq_size` / `rq_size` **没有**被清 —— 漏清一个新字段就会被它
  抓住,这正是端点不再手抄字段清单的护栏。
  最后两条盯住协商本身的线上字节:
  `device_mode_client_hello_carries_the_device_channel`(device-mode client 的 v3 hello 必须
  带完整的 `device` 子消息,`block_size` 取的是 device 自己那个 flag,见 §5.2)和
  `server_hello_without_a_device_field_fails_the_connection`(假 peer 回一个**不带**
  `device` 的合法 v3 hello → `_device.reset()` → 需求检查 → RPC 拿 `EDEVICECHANNEL`)。
  后者是混布场景(GDR 先上客户端后上服务端)的回归护栏,此前只有真卡的
  `GdrRpcTest` 覆盖得到。

- **资源分层(§4.2)** —— 前两条不需要卡,靠 `IbvCreateCompChannel` /
  `IbvDestroyCompChannel` 这两个全局函数指针换成桩(`ibv_req_notify_cq` /
  `ibv_poll_cq` 是 `verbs.h` 里的 static inline,换不了,所以形状断言只能上真卡):
  `deallocate_releases_a_comp_channel_without_a_qp` 给端点塞一个 `_comp_channel`、不给
  `_host.resource`,断言 `DeallocateResources()` 销毁它一次且置空、再调一次不会重复销毁
  —— comp_channel 从 `RdmaResource` 里拆出来新引入的正是这条泄漏边(`Create()` 成功而
  `AllocateQpCq()` 失败)。
  `comp_channel_fd_is_cloexec_and_nonblocking` 让假 channel 的 `fd` 指向一个真实 pipe,
  断言 `RdmaCompChannel::Create()` 之后该 fd 带 `FD_CLOEXEC` 和 `O_NONBLOCK` —— 这两条
  原先埋在 `AllocateQpCq()` 里没人测。
  `four_cqs_report_to_one_comp_channel`(`--gdr_test_real_device` 门控)就是设计那句话
  本身:一条真实 GDR 连接建立之后,两个 QP 互不相同、四个 CQ 互不相同且都非空、
  `polling_cq` 两条通道都为空、四个 `ibv_cq::channel` 全等于 `_comp_channel->channel`,
  并且每个 CQ 的 `cqe` 不小于喂它的队列深度 —— 最后这一格就是上面那个 CQ 深度 bug 的
  断言。(它查的是事件模式的形状,所以 `polling_cq` 为空;polling 模式那一半由下面那条
  纯软件用例覆盖。)
  `polling_mode_walks_one_cq_per_channel` 把 `CollectPollTargets()` 的两种模式都摆开:
  polling 模式下两条通道给出两个**互不相同**的 `polling_cq`、`channel` 指针各自对得上、
  `is_send` 都是 false;`_device.reset()` 之后只剩一个;把 flag 关掉再查一次,断回
  「一条通道两个、recv 先于 send」的事件模式轮转。这是两种 CQ 模式唯一的分叉点,值得一条
  用例正面压住。用完必须把假 CQ 句柄手工置空再让栈上的 `RdmaResource` 析构 ——
  `~RdmaResource()` 会把它们送进 `IbvDestroyCq()`,而没加载过 verbs 库的进程里那是个空的
  函数指针。
  `prepared_resource_is_a_movable_value` 盯住 `PreparedResource` 改成 vector 里的值之后的
  移动语义:从 vector 里移动构造出来再 `pop_back()` 不能把两半销毁掉,`release()` 之后
  析构不销毁,而移动赋值必须销毁目标原先持有的东西(靠 `IbvDestroyCompChannel` 桩的调用
  次数 0 → 1 → 2 断言)。直接测这个 struct 而不是走池子,因为 `g_rdma_resource_pool` 是
  `static` 的,测试 TU 看不见。

- **通道行为归一(§9.3 / §9)** —— 四条纯软件用例,都是这一轮把「两条通道各写一份」收成
  一份之后新加的:
  `both_channels_batch_solicited_wrs_the_same_way` 对 `_host` 和 `*_device` 跑同一段断言:
  第一次调用一定不 solicit、1MB 的 WR 到第三个才因字节阈值 solicit、五个小 WR 之后因
  `unsolicited` 越过 `local_window_capacity / 4` 而 solicit、三个 `imm=3` 之后因
  `accumulated_ack` 越过 `remote_window_capacity / 4` 而 solicit、`must_solicit=true` 把三个
  计数器全部清零。device 恒 `true` 的老行为会让这条用例第一格就红。
  `backlog_is_asked_of_the_channel_not_the_connection` 给 device 队列塞上积压,断言
  `_host.HasBacklog()` 为假而 `_device->HasBacklog()` 为真 —— 原先这里问的是整条连接的
  `HasQueuedDeviceData()`,于是 host 的每个发送完成都会去唤醒一个等 device 窗口的 writer。
  `post_recv_leaves_the_slot_index_alone_when_it_fails` 用一个 `PrepareRecvSlot()` 永远失败
  的假通道,断言 `PostRecv(1)` 返回 -1/`ENOMEM`、`rq_received` **没有**前进、prepare 调了
  一次、release 零次,且 `PostRecv(0)` 直接成功返回。下标错动一格就是把 NIC 还持有的
  接收块覆盖掉。
  `each_channel_sizes_its_windows_from_its_own_qp_params` 手搭一个两组参数明显不同的
  `ParsedHello`,断言两条通道的 `local_window_capacity` / `remote_window_capacity` 各自只由
  **自己那一组** `sq_size` / `rq_size` 算出来(§5);再 `remote.device.reset()`,断言
  `has_device_channel()` 变假。`QpParams` 抽出来之后配错前缀不会有编译错误,这是唯一的护栏。

### 13.4 `--define absl=1` 会吃掉 gflags 参数

**这是 bRPC 仓库既有的坑,不是本设计引入的,影响每一个 bazel 构建的 brpc 单测。**

`.bazelrc` 里有一行 `build --define absl=1`(上游 ad200820),它让 googletest 带着
`GTEST_HAS_ABSL=1` 编译,于是 `testing::InitGoogleTest()` 不再用自己的参数解析、改用 Abseil
的。那个解析器会把 argv 重写成只剩位置参数,并**静默丢弃所有它不认识的 `--flag=value`**
—— 也就是所有 gflags 的 flag。而两段式的 `--flag value` 会作为两个位置参数活下来,所以症状
是选择性的、看着像是某个 flag 特有的问题:`--rdma_test_enable` 到得了,
`--rdma_attachment_memory=host` 到不了,读出来是默认值,两边都不打日志。

修法是在 `main()` 里**先 gflags 后 gtest**,且解析 argv 的一份拷贝:

```cpp
std::vector<char*> gflags_argv(argv, argv + argc);
gflags_argv.push_back(nullptr);
int gflags_argc = argc;
char** p = gflags_argv.data();
GFLAGS_NAMESPACE::AllowCommandLineReparsing();      // 别死在 --gtest_*
GFLAGS_NAMESPACE::ParseCommandLineFlags(&gflags_argc, &p, false);
testing::InitGoogleTest(&argc, argv);               // 拿到的仍是原始 argv
```

拷贝不是洁癖:gflags 即使被要求「什么都别删」,也仍然会把位置参数挪到末尾。

一处需要说明的取舍:`device_size` 进帧头之后,原先那条比对 `cut_offset()` 的错位断言
被删掉了(理由见 §7.1)。它保的东西里唯一没被帧头覆盖的,是 `cutn()` **跨 block 时自己
把字节数记错** —— 这类 bug 收发双方都不会报错,只会切出对不齐的显存。所以
`cut_offset()` 本身留着,并由三条用例盯着:`device_bytes_spanning_blocks_are_stitched_together`
(一条报文横跨两个 block、下一条从 block 中间开始)、pending list 排空后的偏移断言,
以及端到端的 memcmp。这是纯本地的记账,单测足以覆盖,不值得为它在每个报文上加 8 字节。

### 13.1 已知的既有 flake(不是本设计引入的)

曾经有一条握手竞争,使一条新建的 RDMA 连接上的第一个 RPC 在真实硬件上有 **~15% 的概率**
失败(报 `[E1014]Got EOF`,服务端日志里是 `Too many bytes in handshake ACK, drop
connection`):`AllocateResources()` 在握手第一阶段就创建了 comp_channel 的 Socket
(`_cq_sid`,回调 `PollCq`),而 `PollCq` 调用的 `ProcessNewMessage()` 作用在**数据
socket** 上,不受后者 `_nevent` 的串行化保护,于是 TCP 线程和 CQ 线程会同时进入
`ExecuteServerHandshake`、同时碰 `_read_buf` / `parsing_context`。

master 的 `2ffdeca8`(*Fix RDMA server core dump on concurrent access to
`Socket::_read_buf`*,#3505)已经修掉了它:引入 `InputMessengerProcessor`(fd 流与 QP 流
各自的缓冲),并把 `StartCqEvents()` 推迟到 `_state.store(ESTABLISHED, release)` 之后,
QP 流不再可能在 fd 流还在握手时开始解析。本设计 rebase 到该提交之后,端到端用例里原先那个
warm-up 重试循环(`GdrWarmUpChannel()`)已经删掉,`--gtest_filter='GdrRpcTest.*'` 在硬件上
连跑 15 次全绿,日志里再没有出现过上面那条 `Too many bytes in handshake ACK`。

硬件上还有三个用例是 master 上就挂的,跟本设计无关,跑硬件时要排掉:

```
--gtest_filter='-*alloc_resource_fail_fallback_tcp*:*send_rpcs_as_short_connection*:*send_rpcs_as_pooled_connection*'
```

`client/server_alloc_resource_fail_fallback_tcp` 的故障注入点 `g_fail_resource_alloc_for_test`
被包在 `if (g_skip_rdma_init)` 里,真硬件上根本不会触发;另外两个是
`RdmaConnect::Run()` 读到已回收的 `WriteRequest`(短连接/连接池会把 Socket 回收掉,
而回调还攥着旧指针)。

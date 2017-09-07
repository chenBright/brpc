[redis](http://redis.io/)是最近几年比较火的缓存服务，相比memcached在server端提供了更多的数据结构和操作方法，简化了用户的开发工作，在百度内有比较广泛的应用。为了使用户更快捷地访问redis并充分利用bthread的并发能力，baidu-rpc直接支持redis协议。示例程序：[example/redis_c++](http://icode.baidu.com/repo/baidu/opensource/baidu-rpc/files/master/tree/example/redis_c++/)

相比使用[hiredis](https://github.com/redis/hiredis)(官方client)的优势有：

- 线程安全。用户不需要为每个线程建立独立的client。
- 支持同步、异步、批量同步、批量异步等访问方式，能使用ParallelChannel等组合访问方式。
- 支持多种[连接方式](client.md#连接方式)。支持超时、backup request、取消、tracing、内置服务等一系列RPC基本福利。
- 一个进程和一个redis-server只有一个连接。多个线程同时访问一个redis-server时更高效（见[性能](#性能)）。无论reply的组成多复杂，内存都会连续成块地分配，并支持短串优化(SSO)。

像http一样，baidu-rpc保证在最差情况下解析redis reply的时间复杂度也是O(N)，N是reply的字节数，而不是O(N^2)。当reply是个较大的数组时，这是比较重要的。

r32037后加上[-redis_verbose](#查看发出的请求和收到的回复)后会在stderr上打印出所有的redis request和response供调试。

# 访问单台redis

创建一个访问redis的Channel：

```c++
#include <brpc/redis.h>
#include <brpc/channel.h>
  
brpc::ChannelOptions options;
options.protocol = brpc::PROTOCOL_REDIS;
brpc::Channel redis_channel;
if (redis_channel.Init("0.0.0.0:6379", &options) != 0) {  // 6379是redis-server的默认端口
   LOG(ERROR) << "Fail to init channel to redis-server";
   return -1;
}
...
```

执行SET后再INCR：

```c++
std::string my_key = "my_key_1";
int my_number = 1;
...
// 执行"SET <my_key> <my_number>"
brpc::RedisRequest set_request;
brpc::RedisResponse response;
brpc::Controller cntl;
set_request.AddCommand("SET %s %d", my_key.c_str(), my_number);
redis_channel.CallMethod(NULL, &cntl, &set_request, &response, NULL/*done*/);
if (cntl.Failed()) {
    LOG(ERROR) << "Fail to access redis-server";
    return -1;
}
if (response.reply(0).is_error()) {
    LOG(ERROR) << "Fail to set";
    return -1;
}
// 你可以通过response.reply(0).c_str()访问到值或多种方式打印出结果。
LOG(INFO) << response.reply(0).c_str()  // OK
          << response.reply(0)          // OK
          << response;                  // OK
...
 
// 执行"INCR <my_key>"
brpc::RedisRequest incr_request;
incr_request.AddCommand("INCR %s", my_key.c_str());
response.Clear();
cntl.Reset();
redis_channel.CallMethod(NULL, &cntl, &incr_request, &response, NULL/*done*/);
if (cntl.Failed()) {
    LOG(ERROR) << "Fail to access redis-server";
    return -1;
}
if (response.reply(0).is_error()) {
    LOG(ERROR) << "Fail to incr";
    return -1;
}
// 返回了incr后的值，你可以通过response.reply(0).integer()访问到值，或以多种方式打印结果。
LOG(INFO) << response.reply(0).integer()  // 2
          << response.reply(0)            // (integer) 2
          << response;                    // (integer) 2
```

批量执行incr或decr

```c++
brpc::RedisRequest request;
brpc::RedisResponse response;
brpc::Controller cntl;
request.AddCommand("INCR counter1");
request.AddCommand("DECR counter1");
request.AddCommand("INCRBY counter1 10");
request.AddCommand("DECRBY counter1 20");
redis_channel.CallMethod(NULL, &cntl, &get_request, &response, NULL/*done*/);
if (cntl.Failed()) {
    LOG(ERROR) << "Fail to access redis-server";
    return -1;
}
CHECK_EQ(4, response.reply_size());
for (int i = 0; i < 4; ++i) {
    CHECK(response.reply(i).is_integer());
    CHECK_EQ(brpc::REDIS_REPLY_INTEGER, response.reply(i).type());
}
CHECK_EQ(1, response.reply(0).integer());
CHECK_EQ(0, response.reply(1).integer());
CHECK_EQ(10, response.reply(2).integer());
CHECK_EQ(-10, response.reply(3).integer());
```

# RedisRequest

一个[RedisRequest](http://icode.baidu.com/repo/baidu/opensource/baidu-rpc/files/master/blob/src/brpc/redis.h)可包含多个Command，调用AddCommand*增加命令，成功返回true，失败返回false并会打印调用处的栈。

```c++
bool AddCommand(const char* fmt, ...);
bool AddCommandV(const char* fmt, va_list args);
bool AddCommandByComponents(const base::StringPiece* components, size_t n);
```

格式和hiredis基本兼容：即%b对应二进制数据（指针+length)，其他和printf的参数类似。对一些细节做了改进：当某个字段包含空格时，使用单引号或双引号包围起来会被视作一个字段。比如AddCommand("Set 'a key with space' 'a value with space as well'")中的key是a key with space，value是a value with space as well。在hiredis中必须写成redisvCommand(..., "SET %s %s", "a key with space", "a value with space as well");

AddCommandByComponents类似hiredis中的redisCommandArgv，用户通过数组指定命令中的每一个部分。这个方法不是最快捷的，但效率最高，且对AddCommand和AddCommandV可能发生的转义问题免疫，如果你在使用AddCommand和AddCommandV时出现了“引号不匹配”，“无效格式”等问题且无法定位，可以试下这个方法。

如果AddCommand*失败，后续的AddCommand*和CallMethod都会失败。一般来说不用判AddCommand*的结果，失败后自然会通过RPC失败体现出来。

command_size()可获得（成功）加入的命令个数。

调用Clear()后可重用RedisReques

# RedisResponse

[RedisResponse](http://icode.baidu.com/repo/baidu/opensource/baidu-rpc/files/master/blob/src/brpc/redis.h)可能包含一个或多个[RedisReply](http://icode.baidu.com/repo/baidu/opensource/baidu-rpc/files/master/blob/src/brpc/redis_reply.h)，reply_size()可获得reply的个数，reply(i)可获得第i个reply的引用（从0计数）。注意在hiredis中，如果请求包含了N个command，获取结果也要调用N次redisGetReply。但在baidu-rpc中这是不必要的，RedisResponse已经包含了N个reply，通过reply(i)获取就行了。只要RPC成功，response.reply_size()应与request.command_size()相等，除非redis-server有bug（redis-server工作的基本前提就是response和request按序一一对应）

每个reply可能是：

- REDIS_REPLY_NIL：redis中的NULL，代表值不存在。可通过is_nil()判定。
- REDIS_REPLY_STATUS：在redis文档中称为Simple String。一般是操作的返回值，比如SET返回的OK。可通过is_string()判定（status和string暂时不让用户区分），c_str()或data()获得值。
- REDIS_REPLY_STRING：在redis文档中称为Bulk String。大多数值都是这个类型，包括那些可以incr的。可通过is_string()判定，c_str()或data()获得值。
- REDIS_REPLY_ERROR：操作出错时的返回值，包含一段错误信息。可通过is_error()判定，error_message()获得错误信息。
- REDIS_REPLY_INTEGER：一个64位有符号数。可通过is_integer()判定，integer()获得值。
- REDIS_REPLY_ARRAY：另一些reply的数组。可通过is_array()判定，size()获得数组大小，[i]获得对应的子reply引用。

比如response包含三个reply，类型分别是integer，string和array (size=2)。那么可以分别这么获得值：response.reply(0).integer()，response.reply(1).c_str(), repsonse.reply(2)[0]和repsonse.reply(2)[1]。如果类型对不上，调用处的栈会被打印出来，并返回一个undefined的值。

response中的所有reply的ownership属于response。当response析构时，reply也析构了。相应地，RedisReply被禁止拷贝。

调用Clear()后RedisResponse可以重用。

# 访问redis集群

暂时请沿用常见的[twemproxy](https://github.com/twitter/twemproxy)方案，像访问单点一样访问proxy。如果你之前用hiredis访问BDRP（使用了twemproxy），那把client更换成baidu-rpc就行了。通过client（一致性哈希）直接访问redis集群虽然能降低延时，但同时也（可能）意味着无法直接利用BDRP的托管服务，这一块还不是很确定。

如果你自己维护了redis集群，和memcache类似，应该是可以用一致性哈希访问的。但每个RedisRequest应只包含一个command或确保所有的command始终落在同一台server。如果request包含了多个command，在当前实现下总会送向同一个server。比方说一个request中包含了多个Get，而对应的key分布在多个server上，那么结果就肯定不对了，这个情况下你必须把一个request分开为多个。

# 查看发出的请求和收到的回复

 打开[-redis_verbose](http://brpc.baidu.com:8765/flags/redis_verbose)即可在stderr看到所有的redis request和response，注意这应该只用于线下调试，而不是线上程序。

打开[-redis_verbose_crlf2space](http://brpc.baidu.com:8765/flags/redis_verbose_crlf2space)可让打印内容中的CRLF (\r\n)变为空格，方便阅读。

| Name                     | Value | Description                              | Defined At                         |
| ------------------------ | ----- | ---------------------------------------- | ---------------------------------- |
| redis_verbose            | false | [DEBUG] Print EVERY redis request/response to stderr | src/brpc/policy/redis_protocol.cpp |
| redis_verbose_crlf2space | false | [DEBUG] Show \r\n as a space             | src/brpc/redis.cpp                 |

# 性能

redis版本：2.6.14 (不是最新的3.0）

分别使用1，50，200个bthread同步压测同机redis-server，延时单位均为微秒。

```
$ ./client -use_bthread -thread_num 1
TRACE: 02-13 19:42:04:   * 0 client.cpp:180] Accessing redis server at qps=18668 latency=50
TRACE: 02-13 19:42:05:   * 0 client.cpp:180] Accessing redis server at qps=17043 latency=52
TRACE: 02-13 19:42:06:   * 0 client.cpp:180] Accessing redis server at qps=16520 latency=54

$ ./client -use_bthread -thread_num 50
TRACE: 02-13 19:42:54:   * 0 client.cpp:180] Accessing redis server at qps=301212 latency=164
TRACE: 02-13 19:42:55:   * 0 client.cpp:180] Accessing redis server at qps=301203 latency=164
TRACE: 02-13 19:42:56:   * 0 client.cpp:180] Accessing redis server at qps=302158 latency=164

$ ./client -use_bthread -thread_num 200
TRACE: 02-13 19:43:48:   * 0 client.cpp:180] Accessing redis server at qps=411669 latency=483
TRACE: 02-13 19:43:49:   * 0 client.cpp:180] Accessing redis server at qps=411679 latency=483
TRACE: 02-13 19:43:50:   * 0 client.cpp:180] Accessing redis server at qps=412583 latency=482
```

200个线程后qps基本到极限了。这里的极限qps比hiredis高很多，原因在于baidu-rpc默认以单链接访问redis-server，多个线程在写出时会[以wait-free的方式合并](io.md#发消息)，从而让redis-server就像被批量访问一样，每次都能从那个连接中读出一批请求，从而获得远高于非批量时的qps。下面通过连接池访问redis-server时qps的大幅回落是另外一个证明。

分别使用1，50，200个bthread一次发送10个同步压测同机redis-server，延时单位均为微秒。

```
$ ./client -use_bthread -thread_num 1 -batch 10  
TRACE: 02-13 19:46:45:   * 0 client.cpp:180] Accessing redis server at qps=15880 latency=59
TRACE: 02-13 19:46:46:   * 0 client.cpp:180] Accessing redis server at qps=16945 latency=57
TRACE: 02-13 19:46:47:   * 0 client.cpp:180] Accessing redis server at qps=16728 latency=57

$ ./client -use_bthread -thread_num 50 -batch 10
TRACE: 02-13 19:47:14:   * 0 client.cpp:180] Accessing redis server at qps=38082 latency=1307
TRACE: 02-13 19:47:15:   * 0 client.cpp:180] Accessing redis server at qps=38267 latency=1304
TRACE: 02-13 19:47:16:   * 0 client.cpp:180] Accessing redis server at qps=38070 latency=1305
 
  PID USER      PR  NI  VIRT  RES  SHR S %CPU %MEM    TIME+  COMMAND 
16878 gejun     20   0 48136 2436 1004 R 93.8  0.0  12:48.56 redis-server   // thread_num=50

$ ./client -use_bthread -thread_num 200 -batch 10
TRACE: 02-13 19:49:09:   * 0 client.cpp:180] Accessing redis server at qps=29053 latency=6875
TRACE: 02-13 19:49:10:   * 0 client.cpp:180] Accessing redis server at qps=29163 latency=6855
TRACE: 02-13 19:49:11:   * 0 client.cpp:180] Accessing redis server at qps=29271 latency=6838
 
  PID USER      PR  NI  VIRT  RES  SHR S %CPU %MEM    TIME+  COMMAND 
16878 gejun     20   0 48136 2508 1004 R 99.9  0.0  13:36.59 redis-server   // thread_num=200
```

注意redis-server实际处理的qps要乘10。乘10后也差不多在40万左右。另外在thread_num为50或200时，redis-server的CPU已打满。注意redis-server是[单线程reactor](threading_overview.md#单线程reactor)，一个核心打满就意味server到极限了。

使用50个bthread通过连接池方式同步压测同机redis-server。

```
$ ./client -use_bthread -connection_type pooled
TRACE: 02-13 18:07:40:   * 0 client.cpp:180] Accessing redis server at qps=75986 latency=654
TRACE: 02-13 18:07:41:   * 0 client.cpp:180] Accessing redis server at qps=75562 latency=655
TRACE: 02-13 18:07:42:   * 0 client.cpp:180] Accessing redis server at qps=75238 latency=657
 
  PID USER      PR  NI  VIRT  RES  SHR S %CPU %MEM    TIME+  COMMAND
16878 gejun     20   0 48136 2520 1004 R 99.9  0.0   9:52.33 redis-server
```

可以看到qps相比单链接时有大幅回落，同时redis-server的CPU打满了。原因在于redis-server每次只能从一个连接中读到一个请求，IO开销大幅增加。

# Command Line Interface

example/redis_c++/redis_cli是一个类似于官方CLI的命令行工具，以展示baidu-rpc对redis协议的处理能力。当使用baidu-rpc访问redis-server出现不符合预期的行为时，也可以使用这个CLI进行交互式的调试。

```
$ ./redis_cli 
     __          _     __
    / /_  ____ _(_)___/ /_  __      _________  _____
   / __ \/ __ `/ / __  / / / /_____/ ___/ __ \/ ___/
  / /_/ / /_/ / / /_/ / /_/ /_____/ /  / /_/ / /__  
 /_.___/\__,_/_/\__,_/\__,_/     /_/  / .___/\___/  
                                     /_/            
This command-line tool mimics the look-n-feel of official redis-cli, as a 
demostration of baidu-rpc's capability of talking to redis server. The 
output and behavior is not exactly same with the official one.
 
redis 127.0.0.1:6379> mset key1 foo key2 bar key3 17
OK
redis 127.0.0.1:6379> mget key1 key2 key3
["foo", "bar", "17"]
redis 127.0.0.1:6379> incrby key3 10
(integer) 27
redis 127.0.0.1:6379> client setname baidu-rpc-cli
OK
redis 127.0.0.1:6379> client getname
"baidu-rpc-cli"
```

和官方CLI类似，redis_cli <command>也可以直接运行命令，-server参数可以指定redis-server的地址。
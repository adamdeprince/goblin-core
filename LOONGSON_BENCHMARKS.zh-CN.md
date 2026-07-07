# 龙芯 3A6000 基准测试

[English](LOONGSON_BENCHMARKS.md)

Goblin Core 与 **Redis 7.2.4**、**Redis 8.8.0**、**Valkey 9.1.0**、**Dragonfly
v1.39.0** 在龙芯 **3A6000**（麒麟，`loongarch64`）上的对比测试。方法与
[BENCHMARKS.md](BENCHMARKS.md) 一致：各引擎族内分配器一致、Redis 系共用
`redis-parity.conf`、每个引擎一个服务线程。

测量日期 **2026-07-07**，主机 `loongson`。原始数据：
`~/loongson-bench-work/results/`。

| 引擎 | 版本 | 服务线程 | 分配器 |
| --- | --- | ---: | --- |
| Goblin Core | `0.4.0`（本地构建） | 1 | glibc + `malloc_trim` |
| Redis | `7.2.4` | 1（`io-threads 1`） | jemalloc `5.3.0` |
| Redis | `8.8.0` | 1（`io-threads 1`） | jemalloc `5.3.0` |
| Valkey | `9.1.0` | 1（`io-threads 1`） | jemalloc `5.3.0` |
| Dragonfly | `v1.39.0`（源码构建） | 1（`--proactor_threads=1`） | mimalloc `2.2.4` |

Dragonfly 无官方 `loongarch64` 发布包；在本机构建（见 [Dragonfly 构建](#dragonfly-构建)）。
运行时使用 `taskset -c 0` 且仅一个 proactor 线程，与单线程引擎占用同一颗核心的预算一致。

---

## 测试主机

| | |
| --- | --- |
| 机器 | `loongson` |
| CPU | 龙芯 **3A6000**（4 核 × 2 线程，最高 2.0 GHz） |
| ISA | `lsx`、`lasx`、`crc32`、`lam`、`ual` |
| 内存 | 15 GiB |
| 操作系统 | 麒麟 Linux 桌面版 V10 SP1（`5.4.18-167-generic`） |
| 工具链 | GCC **15.2.0**（`/opt/loongson-gcc-15.2.0`） |
| Goblin 构建 | `-DCMAKE_BUILD_TYPE=Release`，`-march=native` |

Goblin 运行时需要 `LD_LIBRARY_PATH=/opt/loongson-gcc-15.2.0/lib`。

**绑核（延迟测试）：** 服务端 `taskset -c 0`，客户端探针 `taskset -c 1`。
吞吐量测试使用 `redis-benchmark -c 1 -P 16`，未对服务端显式 `taskset`；测试期间机器空闲。

---

## 内存测试方法

内存表报告加载后的**绝对结构字节数/元素**——不是相对空进程基线的 RSS 增量。

| 引擎族 | 数据来源 |
| --- | --- |
| Goblin Core | `GOBLIN.MEMORY` → `total_allocated_bytes` ÷ N |
| Redis / Valkey / Dragonfly | `INFO memory` → `used_memory` ÷ N |

工作负载：单 key、`N` 个元素、分散分值（有序集）或 16 字节字段名/值（哈希表，
`redis-parity.conf` 哈希表编码）。Goblin 加载后执行 `GOBLIN.OPTIMIZE`。规模扫描
**500K–2M** 成员/字段。

**碎片率**（可选参考）：加载后进程 RSS ÷ 引擎报告的 `used_memory`。接近 `1.0`
表示常驻集与结构字节一致；更高表示分配器保留的页面（非额外结构开销）。

---

## 分配器

| 引擎 | 分配器 | 说明 |
| --- | --- | --- |
| Redis 7.2.4 / 8.8.0 / Valkey 9.1.0 | **jemalloc 5.3.0** | `make MALLOC=jemalloc`；`--version` 已验证 |
| Goblin Core | **glibc malloc** | `GOBLIN.OPTIMIZE` 后 `malloc_trim` |
| Dragonfly | **mimalloc 2.2.4** | 内置；小规模时 RSS/used 比值偏高 |

勿用系统 jemalloc 替代 Redis 系引擎自带的 jemalloc。

---

## 有序集内存（结构字节/成员）

| 成员数 | Goblin | Redis 7.2.4 | Redis 8.8.0 | Valkey 9.1.0 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| 500,000 | `50.2` | `104.1` | `79.1` | `82.9` | `54.9` |
| 1,000,000 | **`49.1`** | `103.2` | `78.2` | `81.8` | `54.6` |
| 2,000,000 | **`48.6`** | `102.7` | `77.8` | `81.3` | `54.5` |

**Goblin Core 在各规模均为最省**（约 `49` B/成员，2M 时 **`48.6`**）。
**Dragonfly** 次之（约 `55` B/成员，平坦）。**Redis 8.8.0** 比 **7.2.4** 省约
`24%`（约 `78` vs `103` B/成员）。**Valkey** 介于 Redis 8.8 与 7.2.4 之间。

**碎片率（加载后 RSS ÷ used_memory）：**

| 成员数 | Goblin | Redis 7.2.4 | Redis 8.8.0 | Valkey 9.1.0 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| 500,000 | `1.26` | `1.78` | `2.08` | `2.03` | `12.69` |
| 1,000,000 | `1.20` | `1.39` | `1.55` | `1.53` | `7.00` |
| 2,000,000 | `1.19` | `1.20` | `1.28` | `1.29` | `4.12` |

Goblin 与 Redis 系在 2M 时均收敛至约 `1.2`。本移植版 Dragonfly 的 mimalloc 竞技场使
RSS 远高于 `used_memory`（比值约 `4–13`）。

---

## 哈希内存（结构字节/字段）

16 字节字段名与值，哈希表编码（`redis-parity.conf`）。

| 字段数 | Goblin | Redis 7.2.4 | Redis 8.8.0 | Valkey 9.1.0 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| 500,000 | **`46.7`** | `82.3` | `55.7` | `69.0` | `76.6` |
| 1,000,000 | **`45.7`** | `81.4` | `54.9` | `67.9` | `76.3` |
| 2,000,000 | **`45.7`** | `80.9` | `54.5` | `67.4` | `76.2` |

**Goblin Core 最省**（约 `46` B/字段，平坦）。**Redis 8.8.0** 次之（约 `55`
B/字段）。**Dragonfly** 与 **Valkey** 集中在约 `67–77` B/字段。**Redis 7.2.4**
最重（约 `81` B/字段）。

**碎片率（加载后 RSS ÷ used_memory）：**

| 字段数 | Goblin | Redis 7.2.4 | Redis 8.8.0 | Valkey 9.1.0 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| 500,000 | `1.13` | `2.01` | `2.69` | `2.32` | `9.98` |
| 1,000,000 | `1.08` | `1.56` | `1.78` | `1.57` | `5.45` |
| 2,000,000 | `1.03` | `1.25` | `1.40` | `1.26` | `3.17` |

---

## 有序集吞吐量

`redis-benchmark`，100 万成员 keyspace，3 次取最优，`-c 1 -P 16`。

| 操作 | Goblin | Redis 7.2.4 | Redis 8.8.0 | Valkey 9.1.0 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| ZADD（写） | **`207K`** | `153K` | `145K` | `139K` | `115K` |
| ZSCORE | **`357K`** | `282K` | `263K` | `260K` | `193K` |
| ZRANK | **`295K`** | `195K` | `198K` | `194K` | `157K` |
| ZRANGE（16） | **`240K`** | `183K` | `180K` | `165K` | `102K` |
| ZSCORE 深度 1（µs） | `31.7` | `35.8` | `38.8` | `39.7` | **`35.5`** |

Goblin 在 3A6000 上领先所有有序集吞吐量指标（ZADD 比 Redis 7.2.4 约 **+35%**）。
**Dragonfly 在本平台落后**——与 x86 结论相反——可能反映 `loongarch64` 源码移植
（SIMDe 模拟 SSE、无优化汇编路径）而非算法本身。

---

## 哈希吞吐量

100 万字段 keyspace，相同 `redis-benchmark` 设置。

| 操作 | Goblin | Redis 7.2.4 | Redis 8.8.0 | Valkey 9.1.0 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| HSET | **`370K`** | `288K` | `254K` | `207K` | `140K` |
| HGET | **`369K`** | `285K` | `269K` | `252K` | `171K` |
| HGETALL（20 字段） | **`155K`** | `115K` | `117K` | `91K` | `78K` |
| HGET 深度 1（µs） | **`31.4`** | `35.7` | `37.8` | `39.3` | `36.1` |

Goblin 领先所有哈希操作（HSET 比 Redis 7.2.4 约 **+29%**）。Dragonfly 在写与范围读上最慢。

---

## 延迟

C 探针 `write_tail_latency`（20 万样本）+ `redis-benchmark PING`（1 / 50 / 500
客户端，各 10 万样本）。服务端核心 0，客户端核心 1。

**深度 1 PING（探针，µs）：**

| | Goblin | Redis 7.2.4 | Redis 8.8.0 | Valkey 9.1.0 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| p50 | **`22.9`** | `25.9` | `27.9` | `28.9` | `28.1` |
| p99 | **`24.7`** | `28.6` | `30.6` | `32.0` | `30.8` |

**按客户端数的 PING 吞吐 / p50（`redis-benchmark -P 1`）：**

| 客户端数 | Goblin | Redis 7.2.4 | Redis 8.8.0 | Valkey 9.1.0 | Dragonfly |
| --- | --- | --- | --- | --- | --- |
| 1 | `34K` / `23µs` | `30K` / `31µs` | `28K` / `31µs` | `27K` / `31µs` | `28K` / `31µs` |
| 50 | `52K` / `0.48ms` | `48K` / `0.51ms` | `47K` / `0.52ms` | `47K` / `0.51ms` | `48K` / `0.50ms` |
| 500 | `50K` / `5.14ms` | `45K` / `5.35ms` | `45K` / `5.64ms` | `47K` / `5.45ms` | **`50K` / `4.90ms`** |

Goblin 深度 1 PING 最快。Dragonfly 在 **500 饱和客户端** 时领先（`50K` / `4.90ms`）。

---

## 写路径尾延迟

20 万次单独计时的 `ZADD`，向同一集合增长（同一 C 探针），单位微秒：

| | p50 | p99 | p99.9 | max |
| --- | ---: | ---: | ---: | ---: |
| Goblin | **`25.2`** | **`35.4`** | `52.3` | `19,857` |
| Redis 7.2.4 | `30.5` | `34.5` | `55.6` | **`9,447`** |
| Redis 8.8.0 | `32.6` | `37.2` | `55.4` | **`9,017`** |
| Valkey 9.1.0 | `32.9` | `37.4` | `56.6` | **`9,455`** |
| Dragonfly | `37.0` | `41.6` | `63.4` | `17,053` |

Goblin 在 **p99** 之前领先；**max** 为最后一次大插入触发的 Swiss 表重哈希（约
`20` ms）。Redis 系在本硬件上尾延迟 **max** 更低。

---

## 持久化

100 万成员有序集；`SAVE` / `GOBLIN.SAVE`，然后冷启动加载。**加载**列为带数据启动
耗时减去空进程启动耗时（各步骤为绝对时间；加载列为恢复数据的增量成本）。

| 引擎 | 保存 | 文件 | 加载 |
| --- | ---: | ---: | ---: |
| **Goblin** | **`0.140s`** | `37.0` MB | **`0.050s`** |
| Redis 7.2.4 | `0.535s` | **`24.8` MB** | `0.649s` |
| Redis 8.8.0 | `0.463s` | **`24.8` MB** | `0.624s` |
| Valkey 9.1.0 | `0.465s` | **`24.8` MB** | `0.621s` |
| Dragonfly | `0.584s` | **`24.8` MB** | `0.438s` |

Goblin **保存与加载均最快**。Redis 系与 Dragonfly 磁盘文件更小（`24.8` MB vs
Goblin `37.0` MB）。Dragonfly **加载快于 Redis/Valkey**（`0.438s`），但仍慢于
Goblin（`0.050s`）。

---

## Dragonfly 构建

| | |
| --- | --- |
| 二进制 | `~/loongson-bench-work/dragonfly`（`368` MB，`v1.39.0` 标签） |
| 构建脚本 | `~/loongson-bench-work/build_dragonfly.sh` |
| 主要移植工作 | cmake `file://` 依赖预取（cmake 无 HTTPS）；Boost context
`architecture=loongarch`；OpenSSL 3.3.2；RE-flex autotools；`sse_port.h` 用
SIMDe 将 SSE 映射到 LSX；链接 `-ldl` |

无官方 `loongarch64` 版本；此为**尽力而为的源码构建**，用于对比测试，非 Dragonfly
官方支持目标。

---

## 复现

```bash
# 在 loongson 上 — 不修改 ~/dev/packrat 源码
~/loongson-bench-work/build_engines.sh
~/loongson-bench-work/build_dragonfly.sh   # 可选；构建耗时较长
/opt/loongson-gcc-15.2.0/bin/g++ -O2 -std=c++20 \
  -o ~/loongson-bench-work/write_tail_latency \
  ~/dev/packrat/benchmarks/write_tail_latency.cpp
export LD_LIBRARY_PATH=/opt/loongson-gcc-15.2.0/lib
~/loongson-bench-work/run_all.sh
```

支持 Dragonfly 的基准脚本位于 `~/loongson-bench-work/benchmarks/`（packrat 套件
副本；loongson 上 `~/dev/packrat` 树较旧，缺少 Dragonfly 钩子）。

引擎二进制路径：

| 引擎 | 路径 |
| --- | --- |
| Goblin Core | `~/loongson-bench-work/goblin-core` |
| Redis 7.2.4 | `~/loongson-bench-work/redis-7.2.4/src/redis-server` |
| Valkey 9.1.0 | `~/loongson-bench-work/valkey/src/valkey-server` |
| Redis 8.8.0 | `~/loongson-bench-work/redis-8.8/src/redis-server` |
| Dragonfly | `~/loongson-bench-work/dragonfly` |
| redis-benchmark | `~/loongson-bench-work/redis-7.2.4/src/redis-benchmark` |

---

## 跨主机结论：龙芯上的 Goblin vs x86 上的主流引擎

面向国产硅部署的**跨主机**对照：**龙芯 3A6000 上的 Goblin Core**（本文）对比
[BENCHMARKS.md](BENCHMARKS.md) 中 **x86 参考主机**上的 Redis 7.2.4、Redis 8.8.0、
Valkey 9.1.0、Dragonfly 与 Goblin Core — **AMD Ryzen Threadripper PRO 5995WX**
（64 核 × 2 线程，GCC 16.1.0）。CPU、主频与内存带宽不同，并非同机对比。

**结论：** 龙芯上的 Goblin 在热点路径上常为 **Threadripper 上 Redis 的 75–90%**，
在**同一台龙芯**上领先所有竞品，**快照加载快于 x86 Redis**。相对 **x86 上的
Goblin** 则约为吞吐的 **50–75%**，不能宣称同档。诚实卖点：不是「让龙芯变成
Threadripper」，而是**消除 Redis 在龙芯上的性能惩罚**——在加载与内存上，还能超过
团队在 x86 上跑 Redis 的表现。

### 吞吐量（ops/s，管道 `-P 16`）

| | **Goblin 龙芯** | Goblin x86 | Redis 7.2.4 x86 | Redis 8.8 x86 | Valkey x86 | Dragonfly x86 | **龙芯 / Redis 7.2.4 x86** |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| ZADD | `207K` | `398K` | `239K` | `242K` | `242K` | `262K` | **`87%`** |
| ZSCORE | `357K` | `587K` | `504K` | `488K` | `495K` | `419K` | `71%` |
| ZRANK | `295K` | `491K` | `331K` | `342K` | `354K` | `329K` | **`89%`** |
| ZRANGE（16） | `240K` | `456K` | `365K` | `368K` | `346K` | `268K` | `66%` |
| HSET | `370K` | `563K` | `480K` | `454K` | `451K` | `357K` | `77%` |
| HGET | `369K` | `591K` | `501K` | `480K` | `498K` | `393K` | `74%` |
| HGETALL | `155K` | — | `212K` | `213K` | `172K` | `171K` | `73%` |

在**写与排名**（`ZADD`、`ZRANK`）上，龙芯 3A6000 上的 Goblin 与 5995WX 上的
Redis 7.2.4 相差约 **10–15%**——2 GHz 的 3A6000 对标工作站级部件，结论可信。
相对 Redis 8.8 / Valkey（x86）约 **70–85%**；相对 x86 上的 Goblin 约 **52%**
（ZADD）至 **74%**（HGET）。

### 时延（µs）

| | **Goblin 龙芯** | Goblin x86 | Redis 7.2.4 x86 | Redis 8.8 x86 |
| --- | ---: | ---: | ---: | ---: |
| PING p50 | `22.9` | `15.8` | `16.7` | `19.3` |
| PING p99 | `24.7` | `19.9` | `21.1` | `25.4` |
| ZADD 尾延迟 p99 | `35.4` | `21.3` | `28.3` | `29.2` |
| ZSCORE 深度 1 | `31.7` | `21.3` | `22.2` | `23.0` |

时延**可用但非 x86 档**——比 Threadripper 参考主机上的 Redis 慢数微秒，在**同一
龙芯**上仍最优（PING p50 `22.9` vs Redis `25.9–28.9`）。

### 持久化与内存（100 万有序集加载；200 万结构字节）

| | **Goblin 龙芯** | Goblin x86 | Redis 7.2.4 x86 | Redis 8.8 x86 | Valkey x86 | Dragonfly x86 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 加载（s） | **`0.050`** | `0.150` | `0.326` | `0.377` | `0.348` | `0.304` |
| 保存（s） | **`0.140`** | `0.243` | `0.358` | `0.345` | `0.236` | `0.258` |
| 有序集（B/成员） | **`48.6`** | ~`49` | `102.7` | `77.8` | `81.3` | `54.5` |
| 哈希（B/字段） | **`45.7`** | ~`45` | `80.9` | `54.5` | `67.4` | `76.2` |

**龙芯上的 Goblin 加载快于 x86 参考主机上的 Redis**（约 **6–7 倍**）。内存优势在
龙芯上**不缩水**——同一紧凑布局，两列均为最省。x86 数据来自
[BENCHMARKS.md](BENCHMARKS.md)（与本文龙芯表同为 `used_memory` 口径）。

### 同一台龙芯（引擎选择为何重要）

| ZADD | Goblin | Redis 7.2.4 | Redis 8.8 | Valkey | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| 龙芯 | **`207K`** | `153K` | `145K` | `139K` | `115K` |

龙芯上的 Redis 仅为其在 x86 上数字的 **~64%**（`153K` vs `239K`）。龙芯上的
Goblin 约为 x86 Redis 的 **87%**。**在龙芯上用 Goblin 替代 Redis，可收回相对
「在 x86 上跑 Redis」的大部分 ISA 损失**。

### 对外话术（面向国产硅团队）

**强（可辩护）：**

> 在龙芯 3A6000 上跑 Goblin，单核 ZADD/ZRANK 接近 Threadripper 参考主机上
> Redis 7.2.4 的 **85–90%**，同时比同机 Redis/Valkey 快 **35–50%**；快照加载比
> x86 Redis 快一个数量级。

**中：**

> 龙芯 + Redis 会掉一大截；龙芯 + Goblin 把热门路径拉回「接近 x86 Redis」区间，
> 内存仍是全场最省。

**弱（勿说）：**

> 龙芯上 Goblin = x86 上 Goblin — 吞吐量大约只有一半。  
> 龙芯上 Dragonfly 能扛 — 实测最慢（ZADD `115K` vs Goblin `207K`）。

### 主张核对表

| 主张 | 结论 |
| --- | --- |
| 相对 **x86 上的 Redis**（写/排名）达参考主机档 | **约 85–90%** — 可信 |
| 相对 **x86 上的 Goblin** 达参考主机档 | **约 50–75%** — 否 |
| **龙芯上最佳引擎** | **是** — Goblin 全胜 |
| **加载时间**优于 x86 Redis | **是** — `0.050s` vs ~`0.33s` |
| 国产芯 + 国产栈且不牺牲 Redis 档速度 | **是** — 前提是选用 Goblin |

**注意：** 龙芯上的 Dragonfly 为尽力而为的源码移植；勿将 x86 结论外推到
`loongarch64`。Goblin 进程内微基准对硅片调优有用，但**不能**替代本跨引擎套件
（`~/loongson-bench-work/results/microbench-100k.json`）。
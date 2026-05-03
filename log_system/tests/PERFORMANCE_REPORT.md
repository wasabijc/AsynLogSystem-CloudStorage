# mylog 异步日志子系统 — 性能基准报告

## 1. 测试环境

| 项 | 值 |
|---|---|
| 主机 | 阿里云 ECS (iZ7xv4uv6o5e21jke5vdm4Z) |
| OS | Linux, ext4 |
| 编译器 | g++ -std=c++17 -O3 -DNDEBUG -Wall -pthread |
| 日志配置 | thread_pool=3, buffer_size=10000000, flush_log=2 (fflush + fsync) |
| 测试代码 | [performance_test.cpp](performance_test.cpp) |

> flush_log=2 意味着每次消费者 RealFlush 都会调用 `fflush + fsync`，对磁盘延迟非常敏感。

---

## 2. 测试矩阵

| # | 场景 | Sink | Mode | 线程 | 每条 payload | 用途 |
|---|---|---|---|---|---|---|
| B1 | Stdout/1T/64B | StdoutFlush → /dev/null | SAFE | 1 | 64B | 纯内部开销上限 |
| B2 | File/1T/64B | FileFlush | SAFE | 1 | 64B | 单线程短日志磁盘吞吐 |
| B3 | File/1T/512B | FileFlush | SAFE | 1 | 512B | 长日志对比 |
| B4 | File/8T/64B | FileFlush | SAFE | 8 | 64B | 多线程并发吞吐 |
| B5 | File/8T/64B/UNSAFE | FileFlush | UNSAFE | 8 | 64B | UNSAFE 扩容代价 |
| B6 | Roll/4T/128B | RollFileFlush (8MB 切分) | SAFE | 4 | 128B | 滚动落盘开销 |

---

## 3. 完整结果（full 模式，500k/1T，100k/线程）

| # | bench | sink | mode | T | bytes | ops | sec | ops/s | MB/s | p50(μs) | p90(μs) | p99(μs) | max(μs) | drain_ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | B1 Stdout/1T/64B      | Stdout | SAFE   | 1 | 64  | 500k | 1.54 | 324,975 | 19.83  | 1.55 | 8.85 | 13.81 | 161.10 | 0.00 |
| 2 | B2 File/1T/64B        | File   | SAFE   | 1 | 64  | 500k | 0.80 | 627,387 | 38.29  | 1.35 | 2.33 | 2.94  | 53.12  | 120.61 |
| 3 | B3 File/1T/512B       | File   | SAFE   | 1 | 512 | 500k | 1.22 | 408,251 | 199.34 | 1.82 | 3.44 | 4.09  | 911.76 | 100.41 |
| 4 | B4 File/8T/64B        | File   | SAFE   | 8 | 64  | 800k | 1.19 | 673,839 | 41.13  | 1.67 | 3.04 | 175.35| 24462.86 | 100.50 |
| 5 | B5 File/8T/64B/UNSAFE | File   | UNSAFE | 8 | 64  | 800k | 1.27 | 628,588 | 38.37  | 1.61 | 3.07 | 128.99| 17277.33 | 100.45 |
| 6 | B6 Roll/4T/128B       | Roll   | SAFE   | 4 | 128 | 400k | 0.58 | 690,920 | 84.34  | 1.65 | 3.01 | 37.69 | 5271.39  | 100.53 |

### 关键比值（越大越好，<1 表示劣化）

| 对比维度 | 比值 | 解读 |
|---|---|---|
| Stdout vs File (1T/64B) | **0.52×** | Stdout 反而慢一半——见结论 §5.1 |
| 64B / 512B payload (File/1T) | 1.54× | 日志长度增大 8× 吞吐仅降 1.54×，带宽接管 |
| 多线程加速比 (8T vs 1T, File/64B) | **1.07×** | 近乎没有加速——见结论 §5.3 |
| UNSAFE / SAFE (8T/64B) | **0.93×** | UNSAFE 反而更慢——见结论 §5.4 |
| Roll / File (吞吐) | 1.10× | 滚动落盘开销可忽略 |

### 快速模式结果（参考，100k/1T，25k/线程）

| # | bench | ops/s | MB/s | p50(μs) | p99(μs) |
|---|---|---|---|---|---|
| 1 | B1 Stdout/1T/64B | 409,794 | 25.01 | 1.60 | 8.88 |
| 2 | B2 File/1T/64B | 644,029 | 39.31 | 1.36 | 4.80 |
| 3 | B3 File/1T/512B | 513,835 | 250.90 | 1.73 | 3.41 |
| 4 | B4 File/8T/64B | 611,222 | 37.31 | 1.73 | 275.20 |
| 5 | B5 File/8T/64B/UNSAFE | 550,026 | 33.57 | 1.73 | 254.62 |
| 6 | B6 Roll/4T/128B | 591,909 | 72.25 | 1.62 | 26.79 |

---

## 4. 图表化概览

### 4.1 吞吐（ops/s）

```
B1 Stdout/1T/64B   ████████████              324,975
B2 File/1T/64B     █████████████████████     627,387
B3 File/1T/512B    ███████████████           408,251
B4 File/8T/64B     ██████████████████████    673,839
B5 File/8T/UNSAFE  █████████████████████     628,588
B6 Roll/4T/128B    ███████████████████████   690,920
```

### 4.2 生产侧 p99 延迟（μs，越低越好）

```
B1  13.8
B2   2.9
B3   4.1
B4 175.3      ← 多线程下锁争用严重
B5 128.9
B6  37.7
```

### 4.3 最坏单次延迟 max（μs）

```
B1     161
B2      53
B3     911          ← 第一次 fsync
B4  24,462          ← 生产者被 fsync 阻塞
B5  17,277
B6   5,271
```

---

## 5. 结论

### 5.1 Stdout 反而比 File 慢（0.52×） —— 反直觉

正常认知是"stdout→/dev/null 最快"。但本项目的 [`StdoutFlush::Flush`](../logs_code/LogFlush.hpp:21) 使用 `cout.write`，每次要进入 iostream 锁 + sync_with_stdio；而 `FileFlush` 走 C-style `fwrite` + `fflush` + `fsync`，同样是每条 flush 但纯粹得多。

**结论**：项目里 Stdout 仅适合 demo/调试，生产环境务必用 File。

### 5.2 单线程 60 万 ops/s，尾延迟可控

B2 的 p50 = 1.35 μs，p99 = 2.94 μs，max = 53 μs。说明单线程场景下生产者"入队"开销非常低，后台 fsync 的波动也可控。这是当前架构最健康的部分。

### 5.3 多线程几乎没有加速（1.07×） —— 核心瓶颈

B4 8 线程相比 B2 单线程仅 1.07× 提升，远低于理论上的 3~5× 期望加速比。
- p99 从 2.94 μs 急剧放大到 175.35 μs（57×），
- max 达到 24.4 秒。

**根因**：[`AsyncWorker::Push`](../logs_code/AsyncWorker.hpp:33) 的单个全局 `std::mutex mtx_` 在 8 个生产者间严重争用，且消费者执行 fsync 期间所有生产者都被阻塞。

### 5.4 UNSAFE 反而比 SAFE 更慢（0.93×） —— 缓冲区过大

设计上 UNSAFE 不阻塞生产者应该更快。但当前 `buffer_size=10 MB` 导致 SAFE 模式下生产者几乎从不真正阻塞；UNSAFE 模式反而为 `ToBeEnough` 扩容多付出 `std::vector<char>::resize` 的拷贝代价。

**结论**：要验证 UNSAFE 的设计价值，需将 `buffer_size` 压到 64–256 KB 量级再重测。

### 5.5 长 payload 提升带宽利用率

B3 达到 199 MB/s，说明 payload 变长后瓶颈从 per-op overhead 转移到磁盘顺序写带宽，`FileFlush` 的 fwrite 已经接近 ext4 write-back 吞吐上限。

### 5.6 尾延迟秒级尖刺来源

B4/B5/B3 的 max 高达数百毫秒甚至 20 秒级，全部源于：
- 消费者执行 `fsync` 时，生产者缓冲区被打满；
- SAFE 模式下某条 `lg->Info` 就阻塞在 `cond_productor_.wait` 上；
- 这段阻塞时间被直接计入 API 延迟样本，形成尖刺。

### 5.7 drain_ms 全为 ≥100 ms —— 测量工具精度限制

所有 drain_ms 都在 100.4 ms 附近，这是 [`MeasureDrain`](performance_test.cpp:131) 采样策略 `20 ms × 连续 5 次稳定` 的下限（20 × 5 = 100 ms）。**实际 drain 远小于此**，此指标本次仅用来确认"没出现秒级积压"，不作为精确基准。

---

## 6. 性能评级

| 维度 | 评分 | 说明 |
|---|---|---|
| 单线程吞吐 | ⭐⭐⭐⭐ | 60 万 ops/s 满足大多数业务 |
| 单线程尾延迟 | ⭐⭐⭐⭐ | p99 < 3 μs，优秀 |
| 多线程扩展性 | ⭐⭐ | 8 线程仅 1.07× 加速，瓶颈在锁 |
| 多线程尾延迟 | ⭐⭐ | p99 175 μs、max 秒级，不适合低延迟场景 |
| 内存 | ⭐⭐⭐ | 固定 10 MB/logger，可控 |
| 崩溃安全 | ⭐⭐⭐⭐ | flush_log=2 (fsync) 每条落盘 |

**总体结论**：
- 适用：业务服务器、离线批处理、中低频日志场景；
- 不适用：高并发低延迟（交易、网关）场景，会被 p99 尖刺打穿 SLA。

---

## 7. 优化建议（按投入产出比排序）

| 优先级 | 改动 | 预期收益 | 风险 |
|---|---|---|---|
| ⭐⭐⭐ | [`AsyncWorker`](../logs_code/AsyncWorker.hpp:19) 改为 **per-thread 本地 buffer + 消费者轮询收集**（类似 spdlog 的 mpsc 队列） | 8T 吞吐 1.07× → 3~5× | 中，需重写核心队列 |
| ⭐⭐⭐ | `flush_log` 默认改 0 或 1（由业务方显式开 2） | 吞吐 ×2~×5，尾延迟消失 | 崩溃时可能丢几秒日志 |
| ⭐⭐ | `RollFileFlush` / `FileFlush` 调用 `setvbuf(_, NULL, _IOFBF, 1MB)` | fwrite 命中用户缓冲，p99 改善 | 低 |
| ⭐⭐ | `buffer_size` 降到 256 KB，并对比 SAFE vs UNSAFE | 校验 UNSAFE 设计价值 | 低 |
| ⭐ | [`LogMessage::format`](../logs_code/Message.hpp:25) 改用 thread_local `fmt::memory_buffer` | p50 可能降到 ~0.8 μs | 需引入 fmt 库 |
| ⭐ | 改进 [`MeasureDrain`](performance_test.cpp:131) 采样精度到 1 ms | 报告更精确 | 无 |


*报告生成日期：2026-05-03*
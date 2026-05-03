/**
 * @file  performance_test.cpp
 * @brief 异步日志子系统 —— 性能基准测试
 *
 * 目标
 *   在固定机器上量化 mylog 在不同场景下的吞吐与延迟，得到一份
 *   可用于对比 / 回归 / 调优的基准报告。
 *
 * 测试矩阵（6 组 benchmark）
 *   B1  Stdout   / SAFE   / 1T / 64B    —— 纯内部开销上限
 *   B2  File     / SAFE   / 1T / 64B    —— 单线程短日志磁盘吞吐
 *   B3  File     / SAFE   / 1T / 512B   —— 单线程长日志对比
 *   B4  File     / SAFE   / 8T / 64B    —— 多线程并发吞吐
 *   B5  File     / UNSAFE / 8T / 64B    —— UNSAFE 模式对比
 *   B6  Roll     / SAFE   / 4T / 128B   —— 滚动日志开销
 *
 * 度量指标
 *   - total_ns   : 所有生产者调用 lg->Info 的总耗时（墙钟时间）
 *   - ops        : 实际写入日志条数
 *   - throughput : ops / total_seconds 条/秒
 *   - bandwidth  : (ops * payload_bytes) / total / 1024^2  MB/s
 *   - p50 / p90 / p99 / max : 单次 Info 调用延迟分位数（ns）
 *   - drain_ms   : 生产结束到异步消费者把生产者缓冲区耗尽的等待时间
 *
 * 实现要点
 *   - B1 将 stdout 重定向到 /dev/null，把终端输出耗时排除；
 *   - 每个 bench 使用独立 logger 名 + 独立日志文件，测试后删除；
 *   - warmup 1 万条，丢弃其延迟样本；
 *   - 为控制测试时间，多数 bench 单线程 50 万条，多线程每线程 10 万条；
 *   - 延迟样本以 vector<uint64_t> 存储，数量受控在百万级以内；
 *   - drain 通过轮询"再写一条 -> 等文件大小增加"近似测量；
 *     这在异步场景下只是近似，但足以反映队列压力。
 *
 * 编译运行
 *   cd log_system/tests
 *   make perf && ./performance_test
 */

#include "../logs_code/MyLog.hpp"
#include "../logs_code/ThreadPoll.hpp"
#include "../logs_code/Util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

// AsyncLogger 依赖的全局符号
ThreadPool *tp = nullptr;
mylog::Util::JsonData *g_conf_data = nullptr;

// =====================================================================
// 工具 ：时间 / 文件 / 重定向
// =====================================================================
using Clock = std::chrono::steady_clock;

static inline uint64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now().time_since_epoch())
        .count();
}

static int64_t FileSizeOrZero(const std::string &path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return 0;
    return st.st_size;
}

// 将 FILE* stdout 重定向到 /dev/null，返回原 fd (dup)，便于稍后还原
static int RedirectStdoutToNull() {
    ::fflush(stdout);
    int saved = ::dup(fileno(stdout));
    int devnull = ::open("/dev/null", O_WRONLY);
    ::dup2(devnull, fileno(stdout));
    ::close(devnull);
    return saved;
}

static void RestoreStdout(int saved_fd) {
    ::fflush(stdout);
    ::dup2(saved_fd, fileno(stdout));
    ::close(saved_fd);
}

// =====================================================================
// 基准结果
// =====================================================================
struct BenchResult {
    std::string name;
    int threads = 0;
    std::string mode;        // SAFE / UNSAFE / -
    std::string sink;        // Stdout / File / Roll
    size_t payload_bytes = 0;
    size_t ops = 0;
    double total_sec = 0;
    double throughput = 0;   // ops / sec
    double bandwidth_mb = 0; // MB/s (按每条 ~ payload_bytes 计算)
    uint64_t p50_ns = 0, p90_ns = 0, p99_ns = 0, max_ns = 0;
    double drain_ms = 0;     // 生产者结束后等待消费者 drain 的耗时
};

// 从样本计算分位数（就地排序）
static void CalcPercentile(std::vector<uint64_t> &samples, BenchResult &r) {
    if (samples.empty()) return;
    std::sort(samples.begin(), samples.end());
    auto P = [&](double p) -> uint64_t {
        size_t idx = (size_t)(p * (samples.size() - 1));
        return samples[idx];
    };
    r.p50_ns = P(0.50);
    r.p90_ns = P(0.90);
    r.p99_ns = P(0.99);
    r.max_ns = samples.back();
}

// drain 评估：写一条"哨兵日志"，反复等文件大小稳定 N 次视为已刷盘完成
static double MeasureDrain(mylog::AsyncLogger::ptr lg,
                           const std::string &path) {
    if (path.empty()) return 0;  // Stdout 场景不测 drain
    int64_t last = FileSizeOrZero(path);
    auto t0 = Clock::now();
    int stable = 0;
    while (stable < 5) {  // 连续 5 次采样大小不变即认为已 drain
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        int64_t cur = FileSizeOrZero(path);
        if (cur == last) {
            ++stable;
        } else {
            stable = 0;
            last = cur;
        }
        // 安全上限 3s
        if (std::chrono::duration<double>(Clock::now() - t0).count() > 3.0)
            break;
    }
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// 生成固定长度 payload（含一个 %d 占位，让每条日志不完全相同）
static std::string MakePayload(size_t total_bytes) {
    // 留 8 字节给 "%-8d "
    const char *prefix = "%-8d ";
    size_t plen = std::string(prefix).size();
    if (total_bytes <= plen + 1) return "%-8d";
    std::string pad(total_bytes - plen, 'A');
    return std::string(prefix) + pad;
}

// =====================================================================
// 通用 benchmark 执行器
// =====================================================================
struct BenchConfig {
    std::string name;
    std::string logger_name;
    std::string sink;          // "Stdout" / "File" / "Roll"
    std::string file_path;     // "" for stdout
    mylog::AsyncType async_type = mylog::AsyncType::ASYNC_SAFE;
    int threads = 1;
    size_t ops_per_thread = 500000;
    size_t payload_bytes = 64;
    bool sample_latency = true;
};

// 构建一个 logger（按 sink 选择对应 flush）
static mylog::AsyncLogger::ptr BuildLogger(const BenchConfig &cfg) {
    auto build = std::make_shared<mylog::LoggerBuilder>();
    build->BuildLoggerName(cfg.logger_name);
    build->BuildLopperType(cfg.async_type);

    if (cfg.sink == "Stdout") {
        build->BuildLoggerFlush<mylog::StdoutFlush>();
    } else if (cfg.sink == "File") {
        build->BuildLoggerFlush<mylog::FileFlush>(cfg.file_path);
    } else if (cfg.sink == "Roll") {
        // 滚动阈值 8MB，避免测试数据量让文件数过多
        build->BuildLoggerFlush<mylog::RollFileFlush>(cfg.file_path,
                                                      8 * 1024 * 1024);
    }
    mylog::LoggerManager::GetInstance().AddLogger(build->Build());
    return mylog::GetLogger(cfg.logger_name);
}

// 执行一次 benchmark
static BenchResult RunBench(const BenchConfig &cfg) {
    BenchResult r;
    r.name = cfg.name;
    r.threads = cfg.threads;
    r.mode = (cfg.async_type == mylog::AsyncType::ASYNC_SAFE) ? "SAFE" : "UNSAFE";
    r.sink = cfg.sink;
    r.payload_bytes = cfg.payload_bytes;

    // 文件 sink 先清理旧文件
    if (cfg.sink == "File" && !cfg.file_path.empty())
        ::remove(cfg.file_path.c_str());

    auto lg = BuildLogger(cfg);
    if (!lg) {
        std::cerr << "!!! BuildLogger failed for " << cfg.name << std::endl;
        return r;
    }

    // warmup（丢弃，不计时、不采样）
    const std::string fmt = MakePayload(cfg.payload_bytes);
    for (int i = 0; i < 10000; ++i) lg->Info(fmt.c_str(), i);

    // 多线程生产
    std::atomic<size_t> done{0};
    std::vector<std::vector<uint64_t>> per_thread_samples(cfg.threads);
    // 每线程预留样本容量，限制为 5 万，避免占用巨量内存
    const size_t kMaxSamplesPerThread = 50000;
    for (auto &v : per_thread_samples) v.reserve(kMaxSamplesPerThread);

    auto worker = [&](int tid) {
        auto &samples = per_thread_samples[tid];
        size_t sample_step =
            std::max<size_t>(1, cfg.ops_per_thread / kMaxSamplesPerThread);
        for (size_t i = 0; i < cfg.ops_per_thread; ++i) {
            if (cfg.sample_latency && (i % sample_step == 0)) {
                uint64_t t0 = NowNs();
                lg->Info(fmt.c_str(), (int)i);
                uint64_t t1 = NowNs();
                samples.push_back(t1 - t0);
            } else {
                lg->Info(fmt.c_str(), (int)i);
            }
        }
        done.fetch_add(cfg.ops_per_thread);
    };

    auto t_start = Clock::now();
    std::vector<std::thread> ts;
    ts.reserve(cfg.threads);
    for (int t = 0; t < cfg.threads; ++t) ts.emplace_back(worker, t);
    for (auto &th : ts) th.join();
    auto t_end = Clock::now();

    r.ops = done.load();
    r.total_sec =
        std::chrono::duration<double>(t_end - t_start).count();
    r.throughput = r.ops / r.total_sec;
    r.bandwidth_mb =
        (r.ops * cfg.payload_bytes) / r.total_sec / (1024.0 * 1024.0);

    // 汇总所有线程延迟样本
    if (cfg.sample_latency) {
        std::vector<uint64_t> all;
        size_t total = 0;
        for (auto &v : per_thread_samples) total += v.size();
        all.reserve(total);
        for (auto &v : per_thread_samples)
            all.insert(all.end(), v.begin(), v.end());
        CalcPercentile(all, r);
    }

    // 测 drain（仅文件场景有效）
    if (cfg.sink != "Stdout")
        r.drain_ms = MeasureDrain(lg, cfg.file_path);

    // 清理文件（Roll 场景会留下滚动文件，简单目录扫描删除）
    if (cfg.sink == "File" && !cfg.file_path.empty())
        ::remove(cfg.file_path.c_str());

    return r;
}

// =====================================================================
// 打印报告
// =====================================================================
static void PrintHeader() {
    std::cout << std::left
              << std::setw(4)  << "#"
              << std::setw(22) << "bench"
              << std::setw(7)  << "sink"
              << std::setw(7)  << "mode"
              << std::setw(4)  << "T"
              << std::setw(7)  << "bytes"
              << std::setw(12) << "ops"
              << std::setw(9)  << "sec"
              << std::setw(12) << "ops/s"
              << std::setw(11) << "MB/s"
              << std::setw(10) << "p50(us)"
              << std::setw(10) << "p90(us)"
              << std::setw(10) << "p99(us)"
              << std::setw(10) << "max(us)"
              << std::setw(10) << "drain_ms"
              << "\n";
    std::cout << std::string(141, '-') << "\n";
}

static void PrintRow(int idx, const BenchResult &r) {
    auto ns_to_us = [](uint64_t ns) { return ns / 1000.0; };
    std::cout << std::left
              << std::setw(4)  << idx
              << std::setw(22) << r.name
              << std::setw(7)  << r.sink
              << std::setw(7)  << r.mode
              << std::setw(4)  << r.threads
              << std::setw(7)  << r.payload_bytes
              << std::setw(12) << r.ops
              << std::fixed << std::setprecision(2)
              << std::setw(9)  << r.total_sec
              << std::setw(12) << (uint64_t)r.throughput
              << std::setw(11) << r.bandwidth_mb
              << std::setw(10) << ns_to_us(r.p50_ns)
              << std::setw(10) << ns_to_us(r.p90_ns)
              << std::setw(10) << ns_to_us(r.p99_ns)
              << std::setw(10) << ns_to_us(r.max_ns)
              << std::setw(10) << r.drain_ms
              << "\n";
}

// =====================================================================
// main
// =====================================================================
int main(int argc, char **argv) {
    bool quick = false;
    if (argc > 1 && std::string(argv[1]) == "--quick") quick = true;

    g_conf_data = mylog::Util::JsonData::GetJsonData();
    tp = new ThreadPool(g_conf_data->thread_count);

    const std::string kDir = "./perf_logfile/";
    mylog::Util::File::CreateDirectory(kDir);

    std::cout << "==========================================================\n";
    std::cout << "  mylog Performance Benchmark  (" << (quick ? "quick" : "full")
              << ")\n";
    std::cout << "  thread_pool=" << g_conf_data->thread_count
              << "  buffer_size=" << g_conf_data->buffer_size
              << "  flush_mode=" << g_conf_data->flush_log << "\n";
    std::cout << "==========================================================\n";

    // 根据模式调整 ops 规模
    size_t n1 = quick ? 100000 : 500000;   // 单线程
    size_t n2 = quick ? 25000  : 100000;   // 多线程每线程

    std::vector<BenchResult> results;

    // ---------- B1 Stdout / SAFE / 1T / 64B ----------
    {
        // 把 stdout 重定向到 /dev/null，防止洗屏影响耗时
        int saved = RedirectStdoutToNull();
        BenchConfig c;
        c.name = "B1 Stdout/1T/64B";
        c.logger_name = "perf_b1";
        c.sink = "Stdout";
        c.file_path = "";  // stdout 无文件
        c.async_type = mylog::AsyncType::ASYNC_SAFE;
        c.threads = 1;
        c.ops_per_thread = n1;
        c.payload_bytes = 64;
        auto r = RunBench(c);
        RestoreStdout(saved);
        results.push_back(r);
    }

    // ---------- B2 File / SAFE / 1T / 64B ----------
    {
        BenchConfig c;
        c.name = "B2 File/1T/64B";
        c.logger_name = "perf_b2";
        c.sink = "File";
        c.file_path = kDir + "b2.log";
        c.threads = 1;
        c.ops_per_thread = n1;
        c.payload_bytes = 64;
        results.push_back(RunBench(c));
    }

    // ---------- B3 File / SAFE / 1T / 512B ----------
    {
        BenchConfig c;
        c.name = "B3 File/1T/512B";
        c.logger_name = "perf_b3";
        c.sink = "File";
        c.file_path = kDir + "b3.log";
        c.threads = 1;
        c.ops_per_thread = n1;
        c.payload_bytes = 512;
        results.push_back(RunBench(c));
    }

    // ---------- B4 File / SAFE / 8T / 64B ----------
    {
        BenchConfig c;
        c.name = "B4 File/8T/64B";
        c.logger_name = "perf_b4";
        c.sink = "File";
        c.file_path = kDir + "b4.log";
        c.threads = 8;
        c.ops_per_thread = n2;
        c.payload_bytes = 64;
        results.push_back(RunBench(c));
    }

    // ---------- B5 File / UNSAFE / 8T / 64B ----------
    {
        BenchConfig c;
        c.name = "B5 File/8T/64B/UNSAFE";
        c.logger_name = "perf_b5";
        c.sink = "File";
        c.file_path = kDir + "b5.log";
        c.async_type = mylog::AsyncType::ASYNC_UNSAFE;
        c.threads = 8;
        c.ops_per_thread = n2;
        c.payload_bytes = 64;
        results.push_back(RunBench(c));
    }

    // ---------- B6 Roll / SAFE / 4T / 128B ----------
    {
        BenchConfig c;
        c.name = "B6 Roll/4T/128B";
        c.logger_name = "perf_b6";
        c.sink = "Roll";
        c.file_path = kDir + "b6_roll_";
        c.threads = 4;
        c.ops_per_thread = n2;
        c.payload_bytes = 128;
        results.push_back(RunBench(c));
    }

    // =============== 打印报告 ===============
    std::cout << "\n";
    PrintHeader();
    int idx = 1;
    for (auto &r : results) PrintRow(idx++, r);
    std::cout << "\n";

    // 关键对比摘要
    auto Find = [&](const std::string &name) -> BenchResult * {
        for (auto &r : results)
            if (r.name.find(name) != std::string::npos) return &r;
        return nullptr;
    };
    auto Ratio = [](double a, double b) {
        return b > 0 ? a / b : 0.0;
    };

    std::cout << "---- Summary ----\n";
    auto *b1 = Find("B1 Stdout"), *b2 = Find("B2 File"), *b3 = Find("B3 File"),
         *b4 = Find("B4 File"), *b5 = Find("B5"),        *b6 = Find("B6 Roll");

    if (b1 && b2)
        std::cout << "  Stdout vs File (1T/64B) throughput: "
                  << std::fixed << std::setprecision(2)
                  << Ratio(b1->throughput, b2->throughput) << "x\n";
    if (b2 && b3)
        std::cout << "  64B vs 512B payload (File/1T) throughput drop: "
                  << std::fixed << std::setprecision(2)
                  << Ratio(b2->throughput, b3->throughput) << "x\n";
    if (b2 && b4)
        std::cout << "  MultiThread speedup (8T vs 1T, File/64B): "
                  << std::fixed << std::setprecision(2)
                  << Ratio(b4->throughput, b2->throughput) << "x\n";
    if (b4 && b5)
        std::cout << "  UNSAFE vs SAFE (8T/64B): "
                  << std::fixed << std::setprecision(2)
                  << Ratio(b5->throughput, b4->throughput) << "x\n";
    if (b2 && b6)
        std::cout << "  Roll vs File extra cost (throughput ratio): "
                  << std::fixed << std::setprecision(2)
                  << Ratio(b6->throughput, b2->throughput) << "x\n";

    std::cout << std::endl;

    delete tp;
    tp = nullptr;
    return 0;
}
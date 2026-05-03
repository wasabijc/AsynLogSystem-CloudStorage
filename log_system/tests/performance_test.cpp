/**
 * @file  performance_test.cpp
 * @brief 异步日志子系统 —— 性能基准测试
 *
 * 目标
 *   在固定机器上量化 mylog 在 RollFileFlush + SAFE 模式下，
 *   不同线程数 × 不同 payload 大小的吞吐与延迟。
 *
 * 测试矩阵（8 组 benchmark，全部 RollFileFlush / SAFE）
 *   B1  1T  /  64B   —— 单线程小包基线
 *   B2  1T  / 256B   —— 单线程中包
 *   B3  1T  / 1024B  —— 单线程大包（看带宽上限）
 *   B4  4T  /  64B   —— 中并发小包
 *   B5  8T  /  64B   —— 高并发小包（锁争用压力）
 *   B6  16T /  64B   —— 超高并发小包
 *   B7  8T  / 256B   —— 高并发中包
 *   B8  8T  / 1024B  —— 高并发大包
 *
 * 度量指标
 *   - ops        : 总写入日志条数
 *   - ops/s      : 吞吐（条/秒）
 *   - MB/s       : 字节吞吐 = ops × payload / 总时间
 *   - p50/p90/p99/max : 单次 lg->Info 调用延迟分位数（μs）
 *   - drain_ms   : 生产结束后等消费者把缓冲区排空的近似耗时
 *
 * 实现要点
 *   - 每组 bench 独立 logger 名 + 独立滚动文件前缀，互不干扰；
 *   - warmup 1 万条，不计入统计；
 *   - 单线程 bench 跑 500k 条，多线程每线程跑 100k 条；
 *     --quick 模式分别缩减到 100k / 25k；
 *   - Roll 阈值统一设为 8MB，避免文件数过多；
 *   - Summary 横向对比线程维度加速比与 payload 维度带宽变化。
 *
 * 编译运行
 *   cd log_system/tests
 *   make perf && ./performance_test
 *   make perf-quick               # 快速模式
 */

#include "../logs_code/MyLog.hpp"
#include "../logs_code/ThreadPoll.hpp"
#include "../logs_code/Util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
              << ")  sink=Roll  mode=SAFE\n";
    std::cout << "  thread_pool=" << g_conf_data->thread_count
              << "  buffer_size=" << g_conf_data->buffer_size
              << "  flush_mode=" << g_conf_data->flush_log << "\n";
    std::cout << "==========================================================\n";

    // ops 规模
    size_t n1 = quick ? 100000 : 500000;   // 单线程
    size_t n2 = quick ? 25000  : 100000;   // 多线程每线程

    // Roll 阈值统一 8MB，避免文件数过多
    const size_t kRollSize = 8 * 1024 * 1024;

    std::vector<BenchResult> results;

    // 辅助 lambda：构造一组 Roll/SAFE 的 BenchConfig 并运行
    auto MakeRollConfig = [&](const std::string &bname,
                               const std::string &lname,
                               int threads,
                               size_t payload_bytes,
                               size_t ops_per_thread) -> BenchConfig {
        BenchConfig c;
        c.name           = bname;
        c.logger_name    = lname;
        c.sink           = "Roll";
        c.file_path      = kDir + lname + "_";
        c.async_type     = mylog::AsyncType::ASYNC_SAFE;
        c.threads        = threads;
        c.ops_per_thread = ops_per_thread;
        c.payload_bytes  = payload_bytes;
        return c;
    };

    // ---------- B1  1T / 64B  —— 单线程小包基线 ----------
    results.push_back(RunBench(MakeRollConfig("B1 Roll/1T/64B",   "pb1",  1,  64,   n1)));
    // ---------- B2  1T / 256B ----------
    results.push_back(RunBench(MakeRollConfig("B2 Roll/1T/256B",  "pb2",  1,  256,  n1)));
    // ---------- B3  1T / 1024B —— 单线程大包，看带宽上限 ----------
    results.push_back(RunBench(MakeRollConfig("B3 Roll/1T/1024B", "pb3",  1,  1024, n1)));
    // ---------- B4  4T / 64B ----------
    results.push_back(RunBench(MakeRollConfig("B4 Roll/4T/64B",   "pb4",  4,  64,   n2)));
    // ---------- B5  8T / 64B  —— 高并发小包，锁争用压力 ----------
    results.push_back(RunBench(MakeRollConfig("B5 Roll/8T/64B",   "pb5",  8,  64,   n2)));
    // ---------- B6  16T / 64B —— 超高并发小包 ----------
    results.push_back(RunBench(MakeRollConfig("B6 Roll/16T/64B",  "pb6",  16, 64,   n2)));
    // ---------- B7  8T / 256B ----------
    results.push_back(RunBench(MakeRollConfig("B7 Roll/8T/256B",  "pb7",  8,  256,  n2)));
    // ---------- B8  8T / 1024B —— 高并发大包 ----------
    results.push_back(RunBench(MakeRollConfig("B8 Roll/8T/1024B", "pb8",  8,  1024, n2)));

    // =============== 打印报告 ===============
    std::cout << "\n";
    PrintHeader();
    int idx = 1;
    for (auto &r : results) PrintRow(idx++, r);
    std::cout << "\n";

    // =============== Summary ===============
    auto Find = [&](const std::string &prefix) -> BenchResult * {
        for (auto &r : results)
            if (r.name.find(prefix) != std::string::npos) return &r;
        return nullptr;
    };
    auto Ratio = [](double a, double b) -> double {
        return b > 0 ? a / b : 0.0;
    };

    auto *b1 = Find("B1"), *b2 = Find("B2"), *b3 = Find("B3");
    auto *b4 = Find("B4"), *b5 = Find("B5"), *b6 = Find("B6");
    auto *b7 = Find("B7"), *b8 = Find("B8");

    std::cout << "---- Summary (Roll / SAFE) ----\n";

    // 线程维度（固定 64B）：1T → 4T → 8T → 16T
    if (b1 && b4)
        std::cout << "  Thread scale-up  1T->4T  (64B): "
                  << std::fixed << std::setprecision(2)
                  << Ratio(b4->throughput, b1->throughput) << "x  ops/s "
                  << (uint64_t)b4->throughput << "\n";
    if (b1 && b5)
        std::cout << "  Thread scale-up  1T->8T  (64B): "
                  << std::fixed << std::setprecision(2)
                  << Ratio(b5->throughput, b1->throughput) << "x  ops/s "
                  << (uint64_t)b5->throughput << "\n";
    if (b1 && b6)
        std::cout << "  Thread scale-up  1T->16T (64B): "
                  << std::fixed << std::setprecision(2)
                  << Ratio(b6->throughput, b1->throughput) << "x  ops/s "
                  << (uint64_t)b6->throughput << "\n";

    // payload 维度（固定 1T）：64B → 256B → 1024B（关注 MB/s）
    if (b1 && b2)
        std::cout << "  Payload 64B->256B  (1T) throughput ratio: "
                  << std::fixed << std::setprecision(2)
                  << Ratio(b2->throughput, b1->throughput) << "x"
                  << "  bandwidth " << b2->bandwidth_mb << " MB/s\n";
    if (b1 && b3)
        std::cout << "  Payload 64B->1024B (1T) throughput ratio: "
                  << std::fixed << std::setprecision(2)
                  << Ratio(b3->throughput, b1->throughput) << "x"
                  << "  bandwidth " << b3->bandwidth_mb << " MB/s\n";

    // payload 维度（固定 8T）：64B → 256B → 1024B
    if (b5 && b7)
        std::cout << "  Payload 64B->256B  (8T) throughput ratio: "
                  << std::fixed << std::setprecision(2)
                  << Ratio(b7->throughput, b5->throughput) << "x"
                  << "  bandwidth " << b7->bandwidth_mb << " MB/s\n";
    if (b5 && b8)
        std::cout << "  Payload 64B->1024B (8T) throughput ratio: "
                  << std::fixed << std::setprecision(2)
                  << Ratio(b8->throughput, b5->throughput) << "x"
                  << "  bandwidth " << b8->bandwidth_mb << " MB/s\n";

    // p99 尾延迟对比（锁争用关键指标）
    if (b1 && b5 && b6)
        std::cout << "  p99 latency:  1T=" << b1->p99_ns / 1000.0
                  << "us  8T=" << b5->p99_ns / 1000.0
                  << "us  16T=" << b6->p99_ns / 1000.0 << "us\n";

    std::cout << std::endl;

    delete tp;
    tp = nullptr;
    return 0;
}
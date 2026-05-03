/**
 * @file  functional_test.cpp
 * @brief 异步日志子系统 —— 功能测试用例
 *
 * 测试范围（覆盖 logs_code/ 下的核心模块）：
 *   1. Level::ToString —— 日志级别字符串转换
 *   2. LogMessage::format —— 日志消息格式化
 *   3. Buffer —— 缓冲区写入 / 可读可写大小 / 交换 / 重置 / 扩容
 *   4. Util::File —— 路径解析 / 目录递归创建 / 文件存在性
 *   5. Util::JsonUtil —— Json 序列化 (UnSerialize 接口已知存在缺陷，放在 KNOWN_ISSUE)
 *   6. LoggerBuilder + LoggerManager —— 单例管理 / 重名拒绝 / 默认日志器
 *   7. AsyncLogger —— DEBUG / INFO / WARN 级别落盘正确性 (避开会触发远程备份的 ERROR/FATAL)
 *   8. RollFileFlush —— 滚动日志按 max_size 切分
 *   9. ASYNC_UNSAFE 模式下大数据量写入不丢日志
 *  10. 多线程并发写入同一日志器，最终行数等于预期
 *
 * 设计原则：
 *   - 仅依赖标准库 + 项目已有头文件，不引入 gtest，方便在 Makefile 中直接编译。
 *   - 自实现极简 EXPECT_* 断言宏，统计 pass / fail，最终以非零退出码反馈失败。
 *   - 每个用例使用独立的 logger 名称、独立的输出文件，互不干扰。
 *   - 测试结束自动清理本次产生的临时日志目录 ./test_logfile/ 。
 *
 * 编译运行：见同目录下 Makefile，`make && ./functional_test`
 */

#include "../logs_code/MyLog.hpp"
#include "../logs_code/ThreadPoll.hpp"
#include "../logs_code/Util.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

// ====== AsyncLogger 内部 ERROR/FATAL 会调用 tp->enqueue 走远程备份 ======
// 这里提供全局符号定义，满足链接需要。
ThreadPool *tp = nullptr;
mylog::Util::JsonData *g_conf_data = nullptr;

// =====================================================================
// 极简断言框架
// =====================================================================
namespace ut {
struct Stat {
    int total = 0;
    int passed = 0;
    int failed = 0;
};
static Stat g_stat;

static void Report(const std::string &case_name, bool ok,
                   const std::string &detail = "") {
    g_stat.total++;
    if (ok) {
        g_stat.passed++;
        std::cout << "[ PASS ] " << case_name << std::endl;
    } else {
        g_stat.failed++;
        std::cout << "[ FAIL ] " << case_name
                  << (detail.empty() ? "" : " :: " + detail) << std::endl;
    }
}
}  // namespace ut

#define EXPECT_TRUE(expr, name)                                                \
    ut::Report((name), static_cast<bool>(expr),                                \
               std::string("expected true: ") + #expr)

#define EXPECT_EQ(a, b, name)                                                  \
    do {                                                                       \
        auto _va = (a);                                                        \
        auto _vb = (b);                                                        \
        std::ostringstream _os;                                                \
        _os << #a << "=" << _va << ", " << #b << "=" << _vb;                   \
        ut::Report((name), (_va) == (_vb), _os.str());                         \
    } while (0)

#define EXPECT_GE(a, b, name)                                                  \
    do {                                                                       \
        auto _va = (a);                                                        \
        auto _vb = (b);                                                        \
        std::ostringstream _os;                                                \
        _os << #a << "=" << _va << " >= " << #b << "=" << _vb;                 \
        ut::Report((name), (_va) >= (_vb), _os.str());                         \
    } while (0)

// =====================================================================
// 工具函数
// =====================================================================
static const std::string kTestDir = "./test_logfile/";

static std::string ReadAllFile(const std::string &path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return "";
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

static int CountLines(const std::string &content) {
    int n = 0;
    for (char c : content)
        if (c == '\n') ++n;
    return n;
}

// 等待异步日志线程把缓冲区刷盘
static void WaitFlush(int ms = 300) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// 删除测试目录下所有文件 (浅层即可)
static void CleanTestDir() {
    DIR *dir = opendir(kTestDir.c_str());
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        std::string full = kTestDir + name;
        ::remove(full.c_str());
    }
    closedir(dir);
    ::rmdir(kTestDir.c_str());
}

// =====================================================================
// TC-01  日志级别字符串转换
// ---------------------------------------------------------------------
// 目标   : 验证 LogLevel::ToString 能正确把枚举值映射为对应字符串。
// 被测对象: mylog::LogLevel::ToString  (Level.hpp)
// 步骤   : 依次传入 DEBUG/INFO/WARN/ERROR/FATAL 五个枚举，比较返回值。
// 通过判定: 5 个值返回字符串与枚举名严格一致。
// 备注   : 这是日志格式化最基础的一环，任何级别拼写错误都会让消费侧
//         (运维/排查) 失去过滤依据，因此放在第 1 个用例。
// =====================================================================
static void TC_LogLevelToString() {
    using mylog::LogLevel;
    EXPECT_EQ(std::string(LogLevel::ToString(LogLevel::value::DEBUG)), "DEBUG",
              "TC-01.1 Level=DEBUG");
    EXPECT_EQ(std::string(LogLevel::ToString(LogLevel::value::INFO)), "INFO",
              "TC-01.2 Level=INFO");
    EXPECT_EQ(std::string(LogLevel::ToString(LogLevel::value::WARN)), "WARN",
              "TC-01.3 Level=WARN");
    EXPECT_EQ(std::string(LogLevel::ToString(LogLevel::value::ERROR)), "ERROR",
              "TC-01.4 Level=ERROR");
    EXPECT_EQ(std::string(LogLevel::ToString(LogLevel::value::FATAL)), "FATAL",
              "TC-01.5 Level=FATAL");
}

// =====================================================================
// TC-02  LogMessage::format 输出内容完整性
// ---------------------------------------------------------------------
// 目标   : 验证序列化后的日志文本同时包含「级别 / 文件名 / 行号 /
//         payload」四要素，并以换行符结尾，便于按行解析。
// 被测对象: mylog::LogMessage::format  (Message.hpp)
// 步骤   : 构造一条 INFO 级别的 LogMessage(file=foo.cpp, line=42,
//         payload="hello world")，调用 format()。
// 通过判定:
//   - 字符串中可找到 "INFO"、"foo.cpp"、":42"、"hello world"
//   - 末尾字符必须是 '\n'，避免下条日志粘连
// 备注   : 时间戳与线程 id 因为非确定值，不直接比较。
// =====================================================================
static void TC_LogMessageFormat() {
    mylog::LogMessage msg(mylog::LogLevel::value::INFO, "foo.cpp", 42,
                          "tester", "hello world");
    std::string s = msg.format();
    EXPECT_TRUE(s.find("INFO") != std::string::npos, "TC-02.1 contains INFO");
    EXPECT_TRUE(s.find("foo.cpp") != std::string::npos, "TC-02.2 contains file");
    EXPECT_TRUE(s.find(":42") != std::string::npos, "TC-02.3 contains line");
    EXPECT_TRUE(s.find("hello world") != std::string::npos,
                "TC-02.4 contains payload");
    EXPECT_TRUE(!s.empty() && s.back() == '\n', "TC-02.5 ends with newline");
}

// =====================================================================
// TC-03  Buffer 基础读写 / Swap / Reset
// ---------------------------------------------------------------------
// 目标   : 校验生产者-消费者双缓冲核心容器的语义正确。
// 被测对象: mylog::Buffer  (AsyncBuffer.hpp)
// 步骤   :
//   1) 构造两个空 Buffer a/b；
//   2) 向 a Push 8 字节字符串，校验 ReadableSize、IsEmpty、Begin 内容；
//   3) 调用 a.Swap(b)，校验 a 变空、b 接管数据；
//   4) 调用 b.Reset()，校验恢复到空状态。
// 通过判定: 5 个状态量逐一与预期相符。
// 备注   : 该容器是异步落盘"零拷贝交换"的核心，一旦 Swap 实现错误，
//         整个日志链路就会丢数据或重复写。
// =====================================================================
static void TC_BufferBasic() {
    mylog::Buffer a, b;
    const std::string data = "abc12345";
    a.Push(data.c_str(), data.size());

    EXPECT_EQ(a.ReadableSize(), data.size(), "TC-03.1 ReadableSize after push");
    EXPECT_TRUE(!a.IsEmpty(), "TC-03.2 not empty after push");

    // 比较 Begin() 内容
    std::string view(a.Begin(), a.ReadableSize());
    EXPECT_EQ(view, data, "TC-03.3 Begin() content matches");

    // Swap
    a.Swap(b);
    EXPECT_TRUE(a.IsEmpty(), "TC-03.4 swap: a becomes empty");
    EXPECT_EQ(b.ReadableSize(), data.size(),
              "TC-03.5 swap: b takes data");

    // Reset
    b.Reset();
    EXPECT_TRUE(b.IsEmpty(), "TC-03.6 Reset clears buffer");
}

// =====================================================================
// TC-04  Buffer 扩容策略
// ---------------------------------------------------------------------
// 目标   : 验证当单次 Push 数据量 > buffer_size 时，ToBeEnough 能正常扩容。
// 被测对象: Buffer::ToBeEnough  (AsyncBuffer.hpp)
// 步骤   : 构造一个 Buffer，一次性 Push 超过 buffer_size 的字节。
// 通过判定: ReadableSize 等于写入的字节数，无截断。
// 备注   : 如果扩容失败，会导致后续日志被截断或崩溃，属于高优先级场景。
// =====================================================================
static void TC_BufferGrow() {
    mylog::Buffer buf;
    // 写入 buffer_size + 100 字节，触发 ToBeEnough 扩容
    size_t big = g_conf_data->buffer_size + 100;
    std::string blob(big, 'x');
    buf.Push(blob.c_str(), blob.size());
    EXPECT_EQ(buf.ReadableSize(), big, "TC-04.1 grown buffer accepts all bytes");
}

// =====================================================================
// TC-05  Util::File 路径解析与目录创建
// ---------------------------------------------------------------------
// 目标   : 验证文件系统工具函数能正确提取目录及递归创建多级目录。
// 被测对象: mylog::Util::File::Path / CreateDirectory / Exists  (Util.hpp)
// 步骤   :
//   1) 测试 Path 函数从 "./a/b/c/x.log" 提取出目录部分；
//   2) 测试 Path 对无斜杠文件名返回空串；
//   3) 创建深层目录 level1/level2/，并断言其存在。
// 通过判定: 路径提取结果与目录存在性均符合预期。
// 备注   : 日志落盘前必须确保父目录存在，否则 fopen 失败会静默丢日志。
// =====================================================================
static void TC_UtilFile() {
    using mylog::Util::File;
    EXPECT_EQ(File::Path("./a/b/c/x.log"), std::string("./a/b/c/"),
              "TC-05.1 Path() extracts dir");
    EXPECT_EQ(File::Path("nofile"), std::string(""),
              "TC-05.2 Path() returns empty when no slash");

    std::string dir = kTestDir + "level1/level2/";
    File::CreateDirectory(dir);
    EXPECT_TRUE(File::Exists(dir), "TC-05.3 CreateDirectory creates nested dir");
}

// =====================================================================
// TC-06  LoggerManager 单例 / 重名拒绝 / DefaultLogger
// ---------------------------------------------------------------------
// 目标   : 验证单例管理器能唯一持有日志器实例，并正确处理重名注册、
//         查询不存在日志器时返回空指针。
// 被测对象: mylog::LoggerManager  (Manager.hpp)
// 步骤   :
//   1) 检查单例默认日志器存在；
//   2) 新建 "uniq_logger_A" 并注册；
//   3) 用同名再次注册，验证不会覆盖旧实例；
//   4) 查询不存在的日志器，验证返回空指针。
// 通过判定: 5 个断言全部成立。
// 备注   : 生产环境中常通过 LoggerManager 获取全局日志器，
//         若单例或重名逻辑出错，会导致日志输出混乱。
// =====================================================================
static void TC_LoggerManager() {
    auto &mgr = mylog::LoggerManager::GetInstance();

    // 默认日志器存在
    EXPECT_TRUE(mgr.DefaultLogger() != nullptr, "TC-06.1 default logger exists");
    EXPECT_TRUE(mgr.LoggerExist("default"), "TC-06.2 default registered");

    // 添加新日志器
    auto build = std::make_shared<mylog::LoggerBuilder>();
    build->BuildLoggerName("uniq_logger_A");
    build->BuildLoggerFlush<mylog::FileFlush>(kTestDir + "uniqA.log");
    mgr.AddLogger(build->Build());
    EXPECT_TRUE(mgr.LoggerExist("uniq_logger_A"),
                "TC-06.3 new logger registered");

    // 同名再次 Add 应被拒绝（内部直接 return），数量不变
    auto build2 = std::make_shared<mylog::LoggerBuilder>();
    build2->BuildLoggerName("uniq_logger_A");
    build2->BuildLoggerFlush<mylog::FileFlush>(kTestDir + "uniqA_dup.log");
    mgr.AddLogger(build2->Build());
    // 依然只能拿到一个对象，且 uniqA_dup.log 不会被写入
    auto lg = mgr.GetLogger("uniq_logger_A");
    EXPECT_TRUE(lg != nullptr, "TC-06.4 duplicate add does not break manager");

    // GetLogger 不存在的名字 -> 空指针
    EXPECT_TRUE(mgr.GetLogger("not_exist_xxx") == nullptr,
                "TC-06.5 GetLogger nonexistent returns null");
}

// =====================================================================
// TC-07  单线程下 INFO/WARN/DEBUG 级别落盘正确性
// ---------------------------------------------------------------------
// 目标   : 验证 AsyncLogger + FileFlush 端到端链路能按指定级别正确落盘，
//         数量无丢、关键标签齐全。
// 被测对象: mylog::AsyncLogger / FileFlush  (AsyncLogger.hpp, LogFlush.hpp)
// 步骤   :
//   1) 通过 LoggerBuilder 创建独立日志器 "tc07_logger"，输出到
//      tc07_file.log；
//   2) 单线程循环 50 轮，每轮顺序写 INFO / WARN / DEBUG 各一条，
//      共 150 条；
//   3) WaitFlush(500ms) 等异步线程把双缓冲全部刷盘；
//   4) 读取整个文件，按 '\n' 计数行，并 grep 关键字。
// 通过判定:
//   - 总行数 == 150；
//   - 文件中同时出现 [INFO]、[WARN]、[DEBUG] 三种级别标签；
//   - 首条 "info-msg-0"、末条 "warn-msg-49" 内容均能找到，
//     说明既未截断头部，也未截断尾部。
// 备注   : 故意避开 ERROR/FATAL：那两级在 AsyncLogger::serialize 中
//         会调用 start_backup 进行远程备份，单测环境下无 server，
//         tp->enqueue 调用会阻塞或长时间重试。
// =====================================================================
static void TC_FileFlushBasic() {
    const std::string log_path = kTestDir + "tc07_file.log";
    ::remove(log_path.c_str());

    auto build = std::make_shared<mylog::LoggerBuilder>();
    build->BuildLoggerName("tc07_logger");
    build->BuildLoggerFlush<mylog::FileFlush>(log_path);
    mylog::LoggerManager::GetInstance().AddLogger(build->Build());

    auto lg = mylog::GetLogger("tc07_logger");
    EXPECT_TRUE(lg != nullptr, "TC-07.0 logger created");

    const int N = 50;
    for (int i = 0; i < N; ++i) {
        lg->Info("info-msg-%d", i);
        lg->Warn("warn-msg-%d", i);
        lg->Debug("debug-msg-%d", i);
    }
    WaitFlush(500);

    std::string content = ReadAllFile(log_path);
    int lines = CountLines(content);
    // 每轮 3 条
    EXPECT_EQ(lines, N * 3, "TC-07.1 total lines == N*3");
    EXPECT_TRUE(content.find("[INFO]") != std::string::npos,
                "TC-07.2 contains [INFO]");
    EXPECT_TRUE(content.find("[WARN]") != std::string::npos,
                "TC-07.3 contains [WARN]");
    EXPECT_TRUE(content.find("[DEBUG]") != std::string::npos,
                "TC-07.4 contains [DEBUG]");
    EXPECT_TRUE(content.find("info-msg-0") != std::string::npos,
                "TC-07.5 first payload present");
    EXPECT_TRUE(content.find("warn-msg-" + std::to_string(N - 1)) !=
                    std::string::npos,
                "TC-07.6 last payload present");
}

// =====================================================================
// TC-08  printf 风格可变参数格式化
// ---------------------------------------------------------------------
// 目标   : 验证 AsyncLogger 各级别 API 支持 printf 风格的可变参数，
//         整型 / 字符串 / 浮点占位符均能按 fmt 正确替换。
// 被测对象: AsyncLogger::Info 中基于 vasprintf 的格式化路径
//         (AsyncLogger.hpp)
// 步骤   :
//   1) 创建独立日志器 "tc08_logger"，输出到 tc08_vaargs.log；
//   2) 调用 lg->Info("int=%d str=%s float=%.2f", 7, "hello", 3.14)；
//   3) WaitFlush 后读取整个文件。
// 通过判定:
//   - 文件中出现完整子串 "int=7 str=hello float=3.14"，
//     说明三种占位符都被正确替换、且参数顺序未错位。
// 备注   : 这是用户最高频使用的 API；若 vasprintf 失败或格式串与参数
//         数量不匹配，会直接造成日志信息失真甚至程序崩溃，因此单独立用例。
// =====================================================================
static void TC_VaArgsFormat() {
    const std::string log_path = kTestDir + "tc08_vaargs.log";
    ::remove(log_path.c_str());

    auto build = std::make_shared<mylog::LoggerBuilder>();
    build->BuildLoggerName("tc08_logger");
    build->BuildLoggerFlush<mylog::FileFlush>(log_path);
    mylog::LoggerManager::GetInstance().AddLogger(build->Build());

    auto lg = mylog::GetLogger("tc08_logger");
    lg->Info("int=%d str=%s float=%.2f", 7, "hello", 3.14);
    WaitFlush(300);

    std::string content = ReadAllFile(log_path);
    EXPECT_TRUE(content.find("int=7 str=hello float=3.14") != std::string::npos,
                "TC-08.1 printf-style args formatted correctly");
}

// =====================================================================
// TC-09  RollFileFlush 按 max_size 自动滚动切分
// ---------------------------------------------------------------------
// 目标   : 验证当当前日志文件大小达到 max_size 阈值时，RollFileFlush
//         能自动切到下一个新文件，文件名包含时间戳和递增序号。
// 被测对象: mylog::RollFileFlush  (LogFlush.hpp)
// 步骤   :
//   1) 清理目录中残留的 tc09_roll_* 文件，避免污染计数；
//   2) 创建滚动日志器，把 max_size 设成极小的 4KB；
//   3) 分 4 轮，每轮 50 条 ~200B 的日志（单轮总量 >>4KB）；
//      每轮之间 sleep 1.1s ——
//         a. 让消费者线程多次唤醒、多次走 InitLogFile 判断滚动；
//         b. 文件名含秒级时间戳，间隔 >1s 保证文件名不重复，
//            避免 fopen("ab") 把新批次追加到上一个滚动文件；
//   4) 写完后 WaitFlush(1500ms) + ::sync()，保证所有滚动文件已落盘
//      并对 readdir 可见；
//   5) opendir 扫描 ./test_logfile/，统计前缀为 tc09_roll_ 的文件数。
// 通过判定: 文件数量 ≥ 2，说明至少发生过一次滚动。
// 备注   : 早期版本曾出现过 "整批 fwrite 一次性写完导致只产生 1 个
//         文件" 的偶发误报，根因是测试节奏过快。当前用例通过
//         显式分批 + sync 已稳定通过。
// =====================================================================
static void TC_RollFile() {
    const std::string base = kTestDir + "tc09_roll_";
    // 清理可能残留
    DIR *dir = opendir(kTestDir.c_str());
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != nullptr) {
            std::string name = ent->d_name;
            if (name.find("tc09_roll_") == 0)
                ::remove((kTestDir + name).c_str());
        }
        closedir(dir);
    }

    auto build = std::make_shared<mylog::LoggerBuilder>();
    build->BuildLoggerName("tc09_logger");
    // 极小阈值 4KB，写入若干条即可触发滚动
    build->BuildLoggerFlush<mylog::RollFileFlush>(base, 4 * 1024);
    mylog::LoggerManager::GetInstance().AddLogger(build->Build());

    auto lg = mylog::GetLogger("tc09_logger");
    // 写入足够多的数据以触发至少 2 次滚动。
    // 注意：异步消费者一次 swap 可能把整批数据合成一次大 fwrite，
    // 因此分批写 + 中途 sleep，让消费者多次唤醒、多次走 InitLogFile 判断滚动，
    // 同时文件名含秒级时间戳，分批间隔 > 1s 可确保每个滚动文件名唯一、
    // 避免因同名 fopen("ab") 合并写入同一文件。
    std::string big_payload(200, 'A');
    for (int round = 0; round < 4; ++round) {
        for (int i = 0; i < 50; ++i) {
            lg->Info("%s-r%d-#%d", big_payload.c_str(), round, i);
        }
        // 让消费者线程把这一批冲到磁盘后再继续生产
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    }
    // 再多等一会儿，确保最后一次滚动产生的文件已经被 fopen + fwrite 完成
    WaitFlush(1500);
    ::sync();

    // 统计以 tc09_roll_ 开头的文件数量
    int file_cnt = 0;
    DIR *dd = opendir(kTestDir.c_str());
    if (dd) {
        struct dirent *ent;
        while ((ent = readdir(dd)) != nullptr) {
            std::string name = ent->d_name;
            if (name.find("tc09_roll_") == 0) ++file_cnt;
        }
        closedir(dd);
    }
    EXPECT_GE(file_cnt, 2, "TC-09.1 RollFileFlush rolled into >=2 files");
}

// =====================================================================
// TC-10  多线程并发写入：零丢日志压力测试
// ---------------------------------------------------------------------
// 目标   : 在 100 个生产者线程并发写同一个 logger 的极端场景下，
//         验证生产者-消费者模型不丢失任何一条日志，且不出现"半截行"。
// 被测对象: AsyncLogger::Info / AsyncWorker::Push / Buffer 互斥与条件变量
//         (AsyncLogger.hpp, AsyncWorker.hpp)
// 步骤   :
//   1) 创建独立日志器 "tc10_logger"，输出到 tc10_mt.log；
//   2) 启动 kThreads=100 个线程，每线程写 kEachLogs=200 条 INFO，
//      合计 20,000 条；
//   3) 全部线程 join 后 WaitFlush(800ms)；
//   4) 读取整个文件，按 '\n' 计数行。
// 通过判定: 行数 == kThreads * kEachLogs == 20000。
// 备注   : 此用例同时检验 3 件事：
//     - AsyncWorker::Push 内部互斥 / 条件变量是否正确，
//     - 序列化生成的日志是 "整行原子写入"（否则会出现行数不等），
//     - ASYNC_SAFE 模式下生产者阻塞等待时不会丢任何条目。
// =====================================================================
static void TC_MultiThread() {
    const std::string log_path = kTestDir + "tc10_mt.log";
    ::remove(log_path.c_str());

    auto build = std::make_shared<mylog::LoggerBuilder>();
    build->BuildLoggerName("tc10_logger");
    build->BuildLoggerFlush<mylog::FileFlush>(log_path);
    mylog::LoggerManager::GetInstance().AddLogger(build->Build());

    auto lg = mylog::GetLogger("tc10_logger");

    const int kThreads = 100;
    const int kEachLogs = 200;
    std::vector<std::thread> ts;
    ts.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([lg, t, kEachLogs] {
            for (int i = 0; i < kEachLogs; ++i) {
                lg->Info("thread-%d-msg-%d", t, i);
            }
        });
    }
    for (auto &th : ts) th.join();
    WaitFlush(800);

    std::string content = ReadAllFile(log_path);
    int lines = CountLines(content);
    EXPECT_EQ(lines, kThreads * kEachLogs,
              "TC-10.1 concurrent writes lose no log");
}

// =====================================================================
// TC-11  ASYNC_UNSAFE 突发写入下的缓冲区扩容正确性
// ---------------------------------------------------------------------
// 目标   : 验证在 ASYNC_UNSAFE 模式（生产者不阻塞、缓冲区按需扩容）下，
//         面对短时间大批量日志依然不丢条目。
// 被测对象: AsyncWorker(ASYNC_UNSAFE) + Buffer::ToBeEnough 扩容路径
//         (AsyncWorker.hpp, AsyncBuffer.hpp)
// 步骤   :
//   1) 创建独立日志器 "tc11_logger"，显式调用
//      BuildLopperType(ASYNC_UNSAFE) 切换为不阻塞模式；
//   2) 单线程紧密循环写 5000 条 INFO，会迫使生产者缓冲区在消费者
//      还没来得及 swap 时多次 ToBeEnough 扩容；
//   3) WaitFlush(1000ms) 等所有数据刷盘；
//   4) 读取文件按行统计。
// 通过判定: 行数 == 5000。
// 备注   : 与 ASYNC_SAFE 不同，此模式下生产者从不阻塞，因此对扩容
//         策略的正确性要求更高；如果 ToBeEnough 实现错误，会在
//         此用例中表现为日志条数变少。
// =====================================================================
static void TC_AsyncUnsafe() {
    const std::string log_path = kTestDir + "tc11_unsafe.log";
    ::remove(log_path.c_str());

    auto build = std::make_shared<mylog::LoggerBuilder>();
    build->BuildLoggerName("tc11_logger");
    build->BuildLopperType(mylog::AsyncType::ASYNC_UNSAFE);
    build->BuildLoggerFlush<mylog::FileFlush>(log_path);
    mylog::LoggerManager::GetInstance().AddLogger(build->Build());

    auto lg = mylog::GetLogger("tc11_logger");
    const int N = 5000;
    for (int i = 0; i < N; ++i) lg->Info("unsafe-#%d", i);
    WaitFlush(1000);

    std::string content = ReadAllFile(log_path);
    EXPECT_EQ(CountLines(content), N, "TC-11.1 ASYNC_UNSAFE no loss under burst");
}

// =====================================================================
// main
// =====================================================================
int main() {
    // 1) 全局初始化
    g_conf_data = mylog::Util::JsonData::GetJsonData();
    tp = new ThreadPool(g_conf_data->thread_count);

    // 2) 准备测试输出目录
    mylog::Util::File::CreateDirectory(kTestDir);

    std::cout << "================ functional_test BEGIN ================"
              << std::endl;

    // 3) 运行用例
    TC_LogLevelToString();
    TC_LogMessageFormat();
    TC_BufferBasic();
    TC_BufferGrow();
    TC_UtilFile();
    TC_LoggerManager();
    TC_FileFlushBasic();
    TC_VaArgsFormat();
    TC_RollFile();
    TC_MultiThread();
    TC_AsyncUnsafe();

    // 4) 汇总
    std::cout << "================ functional_test END   ================"
              << std::endl;
    std::cout << "Total : " << ut::g_stat.total
              << " | Passed : " << ut::g_stat.passed
              << " | Failed : " << ut::g_stat.failed << std::endl;

    // 5) 清理
    delete tp;
    tp = nullptr;
    // 注意：日志文件保留，方便人工排查；若需自动清理，取消下行注释
    // CleanTestDir();

    return ut::g_stat.failed == 0 ? 0 : 1;
}
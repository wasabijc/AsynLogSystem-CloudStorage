#define DEBUG_LOG
#include "Service.hpp"
#include <thread>
#include <csignal>
#include <atomic>
using namespace std;

storage::DataManager *data_;
ThreadPool* tp=nullptr;
mylog::Util::JsonData* g_conf_data;

// 全局 Service 指针，供信号处理函数触发平滑关闭
// atomic 指针保证信号处理里读取时的可见性与原子性
static std::atomic<storage::Service *> g_service{nullptr};

void service_module()
{
    storage::Service s;
    g_service.store(&s, std::memory_order_release);
    mylog::GetLogger("asynclogger")->Info("service step in RunModule");
    s.RunModule();
    // RunModule 返回意味着事件循环已退出，清理指针，避免悬挂访问
    g_service.store(nullptr, std::memory_order_release);
    mylog::GetLogger("asynclogger")->Info("service_module: RunModule returned, thread exiting");
}

// 信号处理函数：收到 SIGINT / SIGTERM 时平滑关闭
// 注意：函数内尽量只做信号安全的操作；event_base_loopexit 是信号安全的，
// 日志器内部是加锁的异步写入，短时间内调用一次风险可控，但不追加过多逻辑。
static void signal_handler(int sig)
{
    // 先打一条关闭起点日志，便于在日志中定位到关闭触发点
    mylog::GetLogger("asynclogger")->Info(
        "Shutdown signal received: %d, start graceful shutdown", sig);
    auto *svc = g_service.load(std::memory_order_acquire);
    if (svc != nullptr)
    {
        svc->Stop();
    }
    else
    {
        mylog::GetLogger("asynclogger")->Warn(
            "signal_handler: service not running, nothing to stop");
    }
}

void log_system_module_init()
{
    g_conf_data = mylog::Util::JsonData::GetJsonData();
    tp = new ThreadPool(g_conf_data->thread_count);
    std::shared_ptr<mylog::LoggerBuilder> Glb(new mylog::LoggerBuilder());
    Glb->BuildLoggerName("asynclogger");
    Glb->BuildLoggerFlush<mylog::RollFileFlush>("./logfile/RollFile_log",
                                              1024 * 1024);
    // LoggerManager 已构建完成，并由 LoggerManager 类的成员进行管理
    // 将 logger 交给托管对象，调用方通过调用单例托管对象来完成日志落地
    mylog::LoggerManager::GetInstance().AddLogger(Glb->Build());
}

int main()
{
    log_system_module_init();
    mylog::GetLogger("asynclogger")->Info("main: log system initialized");

    data_ = new storage::DataManager();
    mylog::GetLogger("asynclogger")->Info("main: DataManager created");

    // 注册信号处理，支持 Ctrl+C / kill 平滑关闭
    // SIGPIPE 忽略，防止客户端中断导致进程被杀
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);
    mylog::GetLogger("asynclogger")->Info("main: signal handlers registered (SIGINT/SIGTERM)");

    thread t1(service_module);
    mylog::GetLogger("asynclogger")->Info("main: service thread started, waiting for exit");

    t1.join();

    // ===== 关闭流程日志埋点（按资源创建的反向顺序释放） =====
    mylog::GetLogger("asynclogger")->Info("main: service thread joined, start cleanup");

    if (data_ != nullptr)
    {
        delete data_;
        data_ = nullptr;
        mylog::GetLogger("asynclogger")->Info("main: DataManager deleted");
    }

    if (tp != nullptr)
    {
        delete tp;
        tp = nullptr;
        mylog::GetLogger("asynclogger")->Info("main: ThreadPool deleted");
    }

    mylog::GetLogger("asynclogger")->Info("main: shutdown finish, process exit");
    return 0;
}
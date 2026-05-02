#define DEBUG_LOG
#include "Service.hpp"
#include <thread>
#include <csignal>
using namespace std;

storage::DataManager *data_;
ThreadPool* tp=nullptr;
mylog::Util::JsonData* g_conf_data;

void service_module()
{
    storage::Service s;
    mylog::GetLogger("asynclogger")->Info("service step in RunModule");
    // RunModule 内部自己注册 SIGINT/SIGTERM 为 libevent 原生信号事件，
    // 这样一按下 Ctrl+C 就能立刻唤醒 event_base_dispatch，而不用等
    // 新的 HTTP 请求打破 epoll_wait 的阻塞。
    s.RunModule();
    mylog::GetLogger("asynclogger")->Info("service_module: RunModule returned, thread exiting");
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

    // SIGPIPE 忽略，防止客户端中断导致服务端写 socket 时被默认 handler 杀进程。
    // SIGINT / SIGTERM 由 Service 内部通过 evsignal_new 注册到 event_base 上处理，
    // 这里不再用 std::signal 注册，避免 SA_RESTART 导致 epoll_wait 被自动重启
    // （之前的现象：Ctrl+C 后必须手动刷新网页才退出）。
    std::signal(SIGPIPE, SIG_IGN);
    mylog::GetLogger("asynclogger")->Info("main: SIGPIPE ignored; SIGINT/SIGTERM handled by libevent inside Service");

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
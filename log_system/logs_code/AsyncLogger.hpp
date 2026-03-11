#pragma once
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <memory>
#include <mutex>

#include "Level.hpp"
#include "AsyncWorker.hpp"
#include "Message.hpp"
#include "LogFlush.hpp"
#include "backlog/CliBackupLog.hpp"
#include "ThreadPoll.hpp"

extern ThreadPool *tp;

namespace mylog
{
    class AsyncLogger
    {
    public:
        using ptr = std::shared_ptr<AsyncLogger>;
        AsyncLogger(const std::string &logger_name, std::vector<LogFlush::ptr> &flushs, AsyncType type)
            : logger_name_(logger_name),//初始化日志器的名字
              flushs_(flushs.begin(), flushs.end()),//添加实例化方式给日志器，如日志输出到文件还是标准输出，可能有多种
              asyncworker(std::make_shared<AsyncWorker>(//启动异步工作器
                  std::bind(&AsyncLogger::RealFlush, this, std::placeholders::_1),
                  type)) {}
        virtual ~AsyncLogger() {};
        std::string Name() { return logger_name_; }
        //该函数则是特定日志级别的日志信息的格式化，当外部调用该日志器时，使用debug模式的日志就会进来
        //在serialize时把日志信息中的日志级别定义为DEBUG。
        void Debug(const std::string &file, size_t line, const std::string format,
                   ...)
        {
            // 获取可变参数列表中的格式
            va_list va;
            va_start(va, format);
            char *ret;
            int r = vasprintf(&ret, format.c_str(), va);
            if (r == -1)
                perror("vasprintf failed!!!: ");
            va_end(va); // 将va指针置空

            serialize(LogLevel::value::DEBUG, file, line,
                      ret); // 生成格式化日志信息并写文件

            free(ret);
            ret = nullptr;
        };
        void Info(const std::string &file, size_t line, const std::string format,
                  ...)
        {
            va_list va;
            va_start(va, format);
            char *ret;
            int r = vasprintf(&ret, format.c_str(), va);
            if (r == -1)
                perror("vasprintf failed!!!: ");
            va_end(va);

            serialize(LogLevel::value::INFO, file, line,
                      ret);

            free(ret);
            ret = nullptr;
        };

        void Warn(const std::string &file, size_t line, const std::string format,
                  ...)
        {
            va_list va;
            va_start(va, format);
            char *ret;
            int r = vasprintf(&ret, format.c_str(), va);
            if (r == -1)
                perror("vasprintf failed!!!: ");
            va_end(va);

            serialize(LogLevel::value::WARN, file, line,
                      ret);
            free(ret);
            ret = nullptr;
        };
        void Error(const std::string &file, size_t line, const std::string format,
                   ...)
        {
            va_list va;
            va_start(va, format);
            char *ret;
            int r = vasprintf(&ret, format.c_str(), va);
            if (r == -1)
                perror("vasprintf failed!!!: ");
            va_end(va);

            serialize(LogLevel::value::ERROR, file, line,
                      ret);

            free(ret);
            ret = nullptr;
        };
        void Fatal(const std::string &file, size_t line, const std::string format,
                   ...)
        {
            va_list va;
            va_start(va, format);
            char *ret;
            int r = vasprintf(&ret, format.c_str(), va);
            if (r == -1)
                perror("vasprintf failed!!!: ");
            va_end(va);

            serialize(LogLevel::value::FATAL, file, line,
                      ret);

            free(ret);
            ret = nullptr;
        };

    protected:
        //在这里将日志消息组织起来，并写入文件
        void serialize(LogLevel::value level, const std::string &file, size_t line,
                       char *ret)
        {
            // std::cout << "Debug:serialize begin\n";
            LogMessage msg(level, file, line, logger_name_, ret);
            std::string data = msg.format();
            if (level == LogLevel::value::FATAL ||
                level == LogLevel::value::ERROR)
            {
                try
                {
                    auto ret = tp->enqueue(start_backup, data);
                    ret.get();
                }
                catch (const std::runtime_error &e)
                {
                    // 该线程池没有把stop设置为true的逻辑，所以不做处理
                    std::cout << __FILE__ << __LINE__ << "thread pool closed" << std::endl;
                }
            }
             //获取到string类型的日志信息后就可以输出到异步缓冲区了，异步工作器后续会对其进行刷盘
            Flush(data.c_str(), data.size());

            // std::cout << "Debug:serialize Flush\n";
        }

        void Flush(const char *data, size_t len)
        {
            asyncworker->Push(data, len); // Push函数本身是线程安全的，这里不加锁
        }

        void RealFlush(Buffer &buffer)
        { // 由异步线程进行实际写文件
            if (flushs_.empty())
                return;
            for (auto &e : flushs_)
            {  //e是Flush这个类，即控制把日志输出到哪的类。
                e->Flush(buffer.Begin(), buffer.ReadableSize());
            }
        }

    protected:
        std::mutex mtx_;
        std::string logger_name_;
        std::vector<LogFlush::ptr> flushs_; // 输出到指定方向\
    std::vector<LogFlush> flush_;不能使用logflush作为元素类型，logflush是纯虚类，不能实例化
        mylog::AsyncWorker::ptr asyncworker;
    };

    // 日志器建造
    class LoggerBuilder
    {
    public:
        using ptr = std::shared_ptr<LoggerBuilder>;
        void BuildLoggerName(const std::string &name) { logger_name_ = name; }
        void BuildLopperType(AsyncType type) { async_type_ = type; }
        template <typename FlushType, typename... Args>
        void BuildLoggerFlush(Args &&...args)
        {
            flushs_.emplace_back(
                LogFlushFactory::CreateLog<FlushType>(std::forward<Args>(args)...));
        }
        AsyncLogger::ptr Build()
        {
            assert(logger_name_.empty() == false);// 必须有日志器名称
            
            // 如果写日志方式没有指定，那么采用默认的标准输出
            if (flushs_.empty())
                flushs_.emplace_back(std::make_shared<StdoutFlush>());
            return std::make_shared<AsyncLogger>(
                logger_name_, flushs_, async_type_);
        }

    protected:
        std::string logger_name_ = "async_logger"; // 日志器名称
        std::vector<mylog::LogFlush::ptr> flushs_; // 写日志方式
        AsyncType async_type_ = AsyncType::ASYNC_SAFE;//用于控制缓冲区是否增长
    };
} // namespace mylog
#pragma once

#include <memory>
#include <thread>

#include "Level.hpp"
#include "Util.hpp"

namespace mylog
{
  struct LogMessage
  {
    using ptr = std::shared_ptr<LogMessage>;
    LogMessage() = default;
    LogMessage(LogLevel::value level, std::string file, size_t line,
               std::string name, std::string payload)
        : name_(name),
          file_name_(file),
          payload_(payload),
          level_(level),
          line_(line),
          ctime_(Util::Date::Now()),
          tid_(std::this_thread::get_id()) {}

    std::string format()
    {
      std::stringstream ret;
      // 获取当前时间
      struct tm t;
      localtime_r(&ctime_, &t);
      char buf[128];
      strftime(buf, sizeof(buf), "%H:%M:%S", &t);
      std::string tmp1 = '[' + std::string(buf) + "][";
      std::string tmp2 = '[' + std::string(LogLevel::ToString(level_)) + "][" + name_ + "][" + file_name_ + ":" + std::to_string(line_) + "]\t" + payload_ + "\n";
      ret << tmp1 << tid_ << tmp2;
      return ret.str();
    }

    size_t line_;           // 行号
    time_t ctime_;          // 时间
    std::string file_name_; // 文件名
    std::string name_;      // 日志器名
    std::string payload_;   // 信息体
    std::thread::id tid_;   // 线程id
    LogLevel::value level_; // 等级
  };
} // namespace mylog
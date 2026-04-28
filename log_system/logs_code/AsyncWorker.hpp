#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>

#include "AsyncBuffer.hpp"

namespace mylog
{
    enum class AsyncType
    {
        ASYNC_SAFE,     // 生产者阻塞等待，直到消费者腾出空间，不扩容
        ASYNC_UNSAFE    // 生产者不阻塞，直接调用 ToBeEnough 扩容后写入
    };
    using functor = std::function<void(Buffer &)>;
    class AsyncWorker
    {
    public:
        using ptr = std::shared_ptr<AsyncWorker>;

        // 构造函数，传入回调函数和异步类型
        AsyncWorker(const functor &cb, AsyncType async_type = AsyncType::ASYNC_SAFE)
            : async_type_(async_type),
              callback_(cb),
              stop_(false),
              thread_(std::thread(&AsyncWorker::ThreadEntry, this)) {}
        ~AsyncWorker() { Stop(); }

        // 将数据写入生产者缓冲区
        void Push(const char *data, size_t len)
        {
            // 如果生产者队列不足以写下len长度数据，并且缓冲区是固定大小，那么阻塞
            std::unique_lock<std::mutex> lock(mtx_);
            if (AsyncType::ASYNC_SAFE == async_type_)
                cond_productor_.wait(lock, [&]()
                                     { return len <= buffer_productor_.WriteableSize(); });
            buffer_productor_.Push(data, len);
            cond_consumer_.notify_one();
        }

        // 停止工作器
        void Stop()
        {
            stop_ = true;
            cond_consumer_.notify_all(); // 所有线程把缓冲区内数据处理完就结束了
            thread_.join();
        }

    private:
        // 消费者线程入口函数，负责交换缓冲区和调用回调函数处理数据
        void ThreadEntry()
        {
            while (!stop_)
            {
                { // 缓冲区交换完就解锁，让productor继续写入数据
                    std::unique_lock<std::mutex> lock(mtx_);
                    cond_consumer_.wait(lock, [&]()
                                        { return stop_ || !buffer_productor_.IsEmpty(); });
                    buffer_productor_.Swap(buffer_consumer_);
                    // 固定容量的缓冲区才需要唤醒
                    if (async_type_ == AsyncType::ASYNC_SAFE)
                        cond_productor_.notify_one();
                }
                callback_(buffer_consumer_); // 调用回调函数对缓冲区中数据进行处理
                buffer_consumer_.Reset();
                if (stop_ && buffer_productor_.IsEmpty())
                    return;
            }
        }

    private:
        AsyncType async_type_;                      // 用于控制缓冲区是否增长
        std::atomic<bool> stop_;                    // 用于控制异步工作器的启动
        std::mutex mtx_;                            // 保护缓冲区交换和条件变量的互斥锁
        mylog::Buffer buffer_productor_;            // 生产者缓冲区
        mylog::Buffer buffer_consumer_;             // 消费者缓冲区
        std::condition_variable cond_productor_;    // 生产者条件变量，生产者线程在生产者缓冲区空间不足时等待，消费者线程在交换完缓冲区后通知生产者线程继续写入
        std::condition_variable cond_consumer_;     // 消费者条件变量，消费者线程在生产者缓冲区没有数据可处理时等待，生产者线程在写入数据后通知消费者线程进行处理
        functor callback_;                          // 回调，由 AsyncLogger 构造时传入，负责将 buffer_consumer_ 的数据写到 LogFlush指定的目的地
        std::thread thread_;                        // 异步工作器的线程，负责从生产者缓冲区交换数据到消费者缓冲区，并调用回调函数处理数据
    };
} // namespace mylog

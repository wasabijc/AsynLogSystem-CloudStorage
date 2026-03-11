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
        ASYNC_SAFE,
        ASYNC_UNSAFE
    };
    using functor = std::function<void(Buffer &)>;
    class AsyncWorker
    {
    public:
        using ptr = std::shared_ptr<AsyncWorker>;
        AsyncWorker(const functor &cb, AsyncType async_type = AsyncType::ASYNC_SAFE)
            : async_type_(async_type),
              callback_(cb),
              stop_(false),
              thread_(std::thread(&AsyncWorker::ThreadEntry, this)) {}
        ~AsyncWorker() { Stop(); }
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
        void Stop()
        {
            stop_ = true;
            cond_consumer_.notify_all(); // 所有线程把缓冲区内数据处理完就结束了
            thread_.join();
        }

    private:
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
        AsyncType async_type_;
        std::atomic<bool> stop_; // 用于控制异步工作器的启动
        std::mutex mtx_;
        mylog::Buffer buffer_productor_;
        mylog::Buffer buffer_consumer_;
        std::condition_variable cond_productor_;
        std::condition_variable cond_consumer_;
        functor callback_; // 回调函数，用来告知工作器如何落地
        std::thread thread_;
    };
} // namespace mylog

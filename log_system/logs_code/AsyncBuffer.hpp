/*日志缓冲区类设计*/
#pragma once
#include <cassert>
#include <string>
#include <vector>
#include "Util.hpp"

extern mylog::Util::JsonData* g_conf_data;

namespace mylog{
    class Buffer{
    public:
        // 构造函数，初始化缓冲区大小，读写位置
        Buffer() : write_pos_(0), read_pos_(0) {
            buffer_.resize(g_conf_data->buffer_size);
        }

        // 写入数据到缓冲区
        void Push(const char *data, size_t len)
        {
            ToBeEnough(len); // 确保容量足够
            // 开始写入
            std::copy(data, data + len, &buffer_[write_pos_]);
            write_pos_ += len;
        }

        // 获取可读数据的起始指针
        char *ReadBegin(int len)
        {
            assert(len <= ReadableSize());
            return &buffer_[read_pos_];
        }

        // 判断缓冲区是否为空
        bool IsEmpty() { return write_pos_ == read_pos_; }

        // 交换缓冲区内容，实现零拷贝交换生产者和消费者缓冲区
        void Swap(Buffer &buf)
        {
            buffer_.swap(buf.buffer_);
            std::swap(read_pos_, buf.read_pos_);
            std::swap(write_pos_, buf.write_pos_);
        }

        // 获取写空间剩余容量
        size_t WriteableSize()
        { 
            return buffer_.size() - write_pos_;
        }
        // 获取读空间剩余容量
        size_t ReadableSize()
        { 
            return write_pos_ - read_pos_;
        }

        // 获取可读数据的起始指针
        const char *Begin() { return &buffer_[read_pos_]; }
        // 移动写指针
        void MoveWritePos(int len)
        {
            assert(len <= WriteableSize());
            write_pos_ += len;
        }
        // 移动读指针
        void MoveReadPos(int len)
        {
            assert(len <= ReadableSize());
            read_pos_ += len;
        }

        // 重置缓冲区
        void Reset()
        {
            write_pos_ = 0;
            read_pos_ = 0;
        }

    protected:
        // 扩容函数，确保缓冲区容量足够写入len长度数据
        void ToBeEnough(size_t len)
        {
            int buffersize = buffer_.size();
            if (len >= WriteableSize())
            {
                if (buffer_.size() < g_conf_data->threshold)
                {
                    buffer_.resize(2 * buffer_.size() + buffersize);
                }
                else
                {
                    buffer_.resize(g_conf_data->linear_growth + buffersize);
                }
            }
        }

    protected:
        std::vector<char> buffer_; // 缓冲区
        size_t write_pos_;         // 写指针
        size_t read_pos_;          // 读指针
    };
} // namespace mylog
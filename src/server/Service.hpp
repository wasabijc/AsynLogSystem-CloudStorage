#pragma once
#include "DataManager.hpp"

#include <sys/queue.h>
#include <event.h>
// for http
#include <evhttp.h>
#include <event2/http.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

#include <regex>
#include <algorithm>

#include "base64.h" // 来自 cpp-base64 库

extern storage::DataManager *data_;
namespace storage
{
    class Service
    {
    public:
        Service()
        {
#ifdef DEBUG_LOG
            mylog::GetLogger("asynclogger")->Debug("Service start(Construct)");
#endif
            server_port_ = Config::GetInstance()->GetServerPort();
            server_ip_ = Config::GetInstance()->GetServerIp();
            download_prefix_ = Config::GetInstance()->GetDownloadPrefix();
#ifdef DEBUG_LOG
            mylog::GetLogger("asynclogger")->Debug("Service end(Construct)");
#endif
        }
        bool RunModule()
        {
            // 初始化环境，创建event_base事件循环
            event_base *base = event_base_new();
            if (base == NULL)
            {
                mylog::GetLogger("asynclogger")->Fatal("event_base_new err!");
                return false;
            }
            // 设置监听的端口和地址
            sockaddr_in sin;
            memset(&sin, 0, sizeof(sin));
            sin.sin_family = AF_INET;
            sin.sin_port = htons(server_port_);
            // http 服务器,创建evhttp上下文
            evhttp *httpd = evhttp_new(base);
            // 绑定端口和ip
            if (evhttp_bind_socket(httpd, "0.0.0.0", server_port_) != 0)
            {
                mylog::GetLogger("asynclogger")->Fatal("evhttp_bind_socket failed!");
                return false;
            }
            // 设定回调函数
            // 指定generic callback，也可以为特定的URI指定callback
            evhttp_set_gencb(httpd, GenHandler, NULL);

            if (base)
            {
#ifdef DEBUG_LOG
                mylog::GetLogger("asynclogger")->Debug("event_base_dispatch");
#endif
                if (-1 == event_base_dispatch(base))// 启动事件循环，监听请求
                {
                    mylog::GetLogger("asynclogger")->Debug("event_base_dispatch err");
                }
            }
            if (base)
                event_base_free(base);
            if (httpd)
                evhttp_free(httpd);
            return true;
        }

    private:
        uint16_t server_port_;
        std::string server_ip_;
        std::string download_prefix_;

    private:
        static void GenHandler(struct evhttp_request *req, void *arg)
        {
            std::string path = evhttp_uri_get_path(evhttp_request_get_evhttp_uri(req));
            path = UrlDecode(path);
            mylog::GetLogger("asynclogger")->Info("get req, uri: %s", path.c_str());

            // 根据请求中的内容判断是什么请求
            // 下载请求
            if (path.find("/download/") != std::string::npos)
            {
                Download(req, arg);
            }
            // 上传请求
            else if (path == "/upload")
            {
                Upload(req, arg);
            }
            // 显示已存储文件列表，返回一个html页面给浏览器
            else if (path == "/")
            {
                ListShow(req, arg);
            }
            // 其他请求返回404
            else
            {
                evhttp_send_reply(req, HTTP_NOTFOUND, "Not Found", NULL);
            }
        }

        // 将 evbuffer 中剩余的数据以分块方式流式写入已打开的文件描述符 fd
        // 返回 true 表示全部写入成功，false 表示失败
        // 该函数避免一次性在内存中分配整个文件大小的缓冲，防止大文件场景下 OOM
        static bool StreamEvbufferToFd(struct evbuffer *buf, int fd)
        {
            // 64KB 固定分块，写多少拿多少，拿完就落盘
            constexpr size_t kChunkSize = 64 * 1024;
            unsigned char chunk[kChunkSize];
            while (size_t remain = evbuffer_get_length(buf))
            {
                size_t want = remain < kChunkSize ? remain : kChunkSize;
                // evbuffer_remove 会把数据从 evbuffer 中搬走（而不是拷贝一份），
                // 这样 evbuffer 自身也会随写入进程不断释放内存
                int n = evbuffer_remove(buf, chunk, want);
                if (n <= 0)
                {
                    mylog::GetLogger("asynclogger")->Error("evbuffer_remove error, n=%d", n);
                    return false;
                }
                // 可能的部分写入，需要循环直至把这一块写完
                ssize_t written = 0;
                while (written < n)
                {
                    ssize_t w = ::write(fd, chunk + written, n - written);
                    if (w < 0)
                    {
                        if (errno == EINTR) continue;
                        mylog::GetLogger("asynclogger")->Error("write fd error: %s", strerror(errno));
                        return false;
                    }
                    written += w;
                }
            }
            return true;
        }

        static void Upload(struct evhttp_request *req, void *arg)
        {
            mylog::GetLogger("asynclogger")->Info("Upload start");
            // 约定：请求中包含"low_storage"，说明请求中存在文件数据,并希望普通存储\
                包含"deep_storage"字段则压缩后存储
            // 获取请求体内容
            struct evbuffer *buf = evhttp_request_get_input_buffer(req);
            if (buf == nullptr)
            {
                mylog::GetLogger("asynclogger")->Error("evhttp_request_get_input_buffer is empty");
                return;
            }

            size_t len = evbuffer_get_length(buf); // 获取请求体的长度
            mylog::GetLogger("asynclogger")->Info("evbuffer_get_length is %zu", len);
            if (0 == len)
            {
                evhttp_send_reply(req, HTTP_BADREQUEST, "file empty", NULL);
                mylog::GetLogger("asynclogger")->Error("request body is empty");
                return;
            }

            // 获取文件名
            const char* raw_filename = evhttp_find_header(req->input_headers, "FileName");
            if (raw_filename == nullptr) {
                mylog::GetLogger("asynclogger")->Error("Missing FileName header");
                evhttp_send_reply(req, HTTP_BADREQUEST, "Missing FileName header", NULL);
                return;
            }
            std::string filename = raw_filename;
            // 解码文件名
            filename = base64_decode(filename);

            // 获取存储类型，客户端自定义请求头 StorageType
            const char* storage_typeptr = evhttp_find_header(req->input_headers, "StorageType");
            if (storage_typeptr == nullptr) {
                mylog::GetLogger("asynclogger")->Error("Missing File StorageType");
                evhttp_send_reply(req, HTTP_BADREQUEST, "Missing File StorageType", NULL);
                return;
            }
            std::string storage_type = storage_typeptr;
            // 组织存储路径
            std::string storage_path;
            if (storage_type == "low")
            {
                storage_path = Config::GetInstance()->GetLowStorageDir();
            }
            else if (storage_type == "deep")
            {
                storage_path = Config::GetInstance()->GetDeepStorageDir();
            }
            else
            {
                mylog::GetLogger("asynclogger")->Error("evhttp_send_reply: HTTP_BADREQUEST");
                evhttp_send_reply(req, HTTP_BADREQUEST, "Illegal storage type", NULL);
                return;
            }

            // 如果不存在就创建low或deep目录
            FileUtil dirCreate(storage_path);
            dirCreate.CreateDirectory();

            // 目录创建后加可以加上文件名，这个就是最终要写入的文件路径
            storage_path += filename;
#ifdef DEBUG_LOG
            mylog::GetLogger("asynclogger")->Debug("storage_path:%s", storage_path.c_str());
#endif

            // 看路径里是low还是deep存储，是deep就压缩，是low就直接流式写入
            if (storage_path.find("low_storage") != std::string::npos)
            {
                // ============ low_storage：分块流式落盘，避免 OOM ============
                // 直接从 evbuffer 中按 64KB 为单位 remove 并写入磁盘，
                // 峰值内存只有单个 chunk 大小（64KB），哪怕上传几十 GB 也不会把进程撑爆。
                int fd = ::open(storage_path.c_str(),
                                O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd == -1)
                {
                    mylog::GetLogger("asynclogger")->Error("open %s error: %s",
                                                           storage_path.c_str(), strerror(errno));
                    evhttp_send_reply(req, HTTP_INTERNAL, "server error", NULL);
                    return;
                }
                bool ok = StreamEvbufferToFd(buf, fd);
                ::close(fd);
                if (!ok)
                {
                    // 写失败，清理已经写了一半的残留文件
                    ::remove(storage_path.c_str());
                    mylog::GetLogger("asynclogger")->Error("low_storage stream write fail, evhttp_send_reply: HTTP_INTERNAL");
                    evhttp_send_reply(req, HTTP_INTERNAL, "server error", NULL);
                    return;
                }
                mylog::GetLogger("asynclogger")->Info("low_storage success (streamed, size=%zu)", len);
            }
            else
            {
                // ============ deep_storage：压缩存储 ============
                // 压缩算法（bundle::pack）需要完整的数据视图，因此必须先把整块数据装进内存。
                // 为避免超大文件把服务器打爆，这里设置一个最大可压缩长度上限，
                // 超过该阈值直接拒绝并建议客户端改用普通存储。
                constexpr size_t kMaxCompressSize = 256ULL * 1024 * 1024; // 256MB
                if (len > kMaxCompressSize)
                {
                    mylog::GetLogger("asynclogger")->Error(
                        "deep_storage refused: file too large (%zu > %zu), use low storage instead",
                        len, kMaxCompressSize);
                    evhttp_send_reply(req, 413,
                                      "file too large for deep storage, please use low storage",
                                      NULL);
                    return;
                }

                // 使用 evbuffer_remove 将数据一次性搬运（而非拷贝）到 content，
                // 搬运完 evbuffer 内部内存即可释放，相较 copyout 可减少约一半峰值内存。
                std::string content;
                try {
                    content.resize(len);
                } catch (const std::bad_alloc &e) {
                    mylog::GetLogger("asynclogger")->Error(
                        "deep_storage bad_alloc when resize(%zu): %s", len, e.what());
                    evhttp_send_reply(req, HTTP_INTERNAL, "out of memory", NULL);
                    return;
                }
                if (-1 == evbuffer_remove(buf, &content[0], len))
                {
                    mylog::GetLogger("asynclogger")->Error("evbuffer_remove error");
                    evhttp_send_reply(req, HTTP_INTERNAL, NULL, NULL);
                    return;
                }

                FileUtil fu(storage_path);
                if (fu.Compress(content, Config::GetInstance()->GetBundleFormat()) == false)
                {
                    mylog::GetLogger("asynclogger")->Error("deep_storage fail, evhttp_send_reply: HTTP_INTERNAL");
                    evhttp_send_reply(req, HTTP_INTERNAL, "server error", NULL);
                    return;
                }
                mylog::GetLogger("asynclogger")->Info("deep_storage success");
            }

            // 添加存储文件信息，交由数据管理类进行管理
            StorageInfo info;
            info.NewStorageInfo(storage_path); // 组织存储的文件信息
            data_->Insert(info);               // 向数据管理模块添加存储的文件信息

            evhttp_send_reply(req, HTTP_OK, "Success", NULL);
            mylog::GetLogger("asynclogger")->Info("upload finish:success");
        }

        static std::string TimetoStr(time_t t)
        {
            std::string tmp = std::ctime(&t);
            return tmp;
        }

        // 前端代码处理函数
        // 在渲染函数中直接处理StorageInfo
        static std::string generateModernFileList(const std::vector<StorageInfo> &files)
        {
            std::stringstream ss;
            ss << "<div class='file-list'><h3>已上传文件</h3>";

            for (const auto &file : files)
            {
                std::string filename = FileUtil(file.storage_path_).FileName();

                // 从路径中解析存储类型（示例逻辑，需根据实际路径规则调整）
                std::string storage_type = "low";
                if (file.storage_path_.find("deep") != std::string::npos)
                {
                    storage_type = "deep";
                }

                ss << "<div class='file-item'>"
                   << "<div class='file-info'>"
                   << "<span>📄" << filename << "</span>"
                   << "<span class='file-type'>"
                   << (storage_type == "deep" ? "深度存储" : "普通存储")
                   << "</span>"
                   << "<span>" << formatSize(file.fsize_) << "</span>"
                   << "<span>" << TimetoStr(file.mtime_) << "</span>"
                   << "</div>"
                   << "<button onclick=\"window.location='" << file.url_ << "'\">⬇️ 下载</button>"
                   << "</div>";
            }

            ss << "</div>";
            return ss.str();
        }

        // 文件大小格式化函数
        static std::string formatSize(uint64_t bytes)
        {
            const char *units[] = {"B", "KB", "MB", "GB"};
            int unit_index = 0;
            double size = bytes;

            while (size >= 1024 && unit_index < 3)
            {
                size /= 1024;
                unit_index++;
            }

            std::stringstream ss;
            ss << std::fixed << std::setprecision(2) << size << " " << units[unit_index];
            return ss.str();
        }
        static void ListShow(struct evhttp_request *req, void *arg)
        {
            mylog::GetLogger("asynclogger")->Info("ListShow()");
            // 1. 获取所有的文件存储信息
            std::vector<StorageInfo> arry;
            data_->GetAll(&arry);

            // 按最后修改时间降序排序，使最新上传/修改的文件显示在最上方
            std::sort(arry.begin(), arry.end(),
                      [](const StorageInfo &a, const StorageInfo &b) {
                          return a.mtime_ > b.mtime_;
                      });

            // 读取模板文件
            std::ifstream templateFile("index.html");
            std::string templateContent(
                (std::istreambuf_iterator<char>(templateFile)),
                std::istreambuf_iterator<char>());

            // 替换html文件中的占位符
            //替换文件列表进html
            templateContent = std::regex_replace(templateContent,
                                                 std::regex("\\{\\{FILE_LIST\\}\\}"),
                                                 generateModernFileList(arry));
            //替换服务器地址进html
            templateContent = std::regex_replace(templateContent,
                                                 std::regex("\\{\\{BACKEND_URL\\}\\}"),
                                                "http://"+storage::Config::GetInstance()->GetServerIp()+":"+std::to_string(storage::Config::GetInstance()->GetServerPort()));
            // 获取请求的输出evbuffer
            struct evbuffer *buf = evhttp_request_get_output_buffer(req);
            auto response_body = templateContent;
            // 把前面的html数据给到evbuffer，然后设置响应头部字段，最后返回给浏览器
            evbuffer_add(buf, (const void *)response_body.c_str(), response_body.size());
            evhttp_add_header(req->output_headers, "Content-Type", "text/html;charset=utf-8");
            evhttp_send_reply(req, HTTP_OK, NULL, NULL);
            mylog::GetLogger("asynclogger")->Info("ListShow() finish");
        }
        static std::string GetETag(const StorageInfo &info)
        {
            // 自定义etag :  filename-fsize-mtime
            FileUtil fu(info.storage_path_);
            std::string etag = fu.FileName();
            etag += "-";
            etag += std::to_string(info.fsize_);
            etag += "-";
            etag += std::to_string(info.mtime_);
            return etag;
        }

        static void Download(struct evhttp_request *req, void *arg)
        {
            mylog::GetLogger("asynclogger")->Info("Download start");
            // 1. 获取客户端请求的资源路径path   req.path
            // 2. 根据资源路径，获取StorageInfo
            StorageInfo info;
            std::string resource_path = evhttp_uri_get_path(evhttp_request_get_evhttp_uri(req));
            resource_path = UrlDecode(resource_path);
            data_->GetOneByURL(resource_path, &info);
            mylog::GetLogger("asynclogger")->Info("request resource_path:%s", resource_path.c_str());

            std::string download_path = info.storage_path_;
            // 2.如果是深度存储的文件就先解压再给用户下载
            if (info.storage_path_.find(Config::GetInstance()->GetLowStorageDir()) == std::string::npos)
            {
                mylog::GetLogger("asynclogger")->Info("uncompressing:%s", info.storage_path_.c_str());
                FileUtil fu(info.storage_path_);
                download_path = Config::GetInstance()->GetLowStorageDir() +
                                std::string(download_path.begin() + download_path.find_last_of('/') + 1, download_path.end());
                FileUtil dirCreate(Config::GetInstance()->GetLowStorageDir());
                dirCreate.CreateDirectory();
                fu.UnCompress(download_path); // 将文件解压到low_storage下
            }
            mylog::GetLogger("asynclogger")->Info("request download_path:%s", download_path.c_str());
            FileUtil fu(download_path);
            if (fu.Exists() == false && info.storage_path_.find("deep_storage") != std::string::npos)
            {
                // 如果是压缩文件，且解压失败，是服务端的错误
                mylog::GetLogger("asynclogger")->Error("evhttp_send_reply: 500 - UnCompress failed");
                evhttp_send_reply(req, HTTP_INTERNAL, NULL, NULL);
            }
            else if (fu.Exists() == false && info.storage_path_.find("low_storage") == std::string::npos)
            {
                // 如果是普通文件，且文件不存在，是客户端的错误
                mylog::GetLogger("asynclogger")->Error("evhttp_send_reply: 400 - bad request,file not exists");
                evhttp_send_reply(req, HTTP_BADREQUEST, "file not exists", NULL);
            }

            // 3.确认文件是否需要断点续传
            bool retrans = false;
            std::string old_etag;
            auto if_range = evhttp_find_header(req->input_headers, "If-Range");
            if (NULL != if_range)
            {
                old_etag = if_range;
                // 有If-Range字段且，这个字段的值与请求文件的最新etag一致则符合断点续传
                if (old_etag == GetETag(info))
                {
                    retrans = true;
                    mylog::GetLogger("asynclogger")->Info("%s need breakpoint continuous transmission", download_path.c_str());
                }
            }

            // 4. 读取文件数据，放入rsp.body中
            if (fu.Exists() == false)
            {
                mylog::GetLogger("asynclogger")->Error("%s not exists", download_path.c_str());
                download_path += "not exists";
                evhttp_send_reply(req, 404, download_path.c_str(), NULL);
                return;
            }
            evbuffer *outbuf = evhttp_request_get_output_buffer(req);
            int fd = open(download_path.c_str(), O_RDONLY);
            if (fd == -1)
            {
                mylog::GetLogger("asynclogger")->Error("open file error: %s -- %s", download_path.c_str(), strerror(errno));
                evhttp_send_reply(req, HTTP_INTERNAL, strerror(errno), NULL);
                return;
            }
            // 5. evbuffer_add_file将文件数据添加到响应体中
            if (-1 == evbuffer_add_file(outbuf, fd, 0, fu.FileSize()))
            {
                mylog::GetLogger("asynclogger")->Error("evbuffer_add_file: %d -- %s -- %s", fd, download_path.c_str(), strerror(errno));
            }
            // 6. 设置响应头部字段： ETag， Accept-Ranges: bytes
            evhttp_add_header(req->output_headers, "Accept-Ranges", "bytes");
            evhttp_add_header(req->output_headers, "ETag", GetETag(info).c_str());
            evhttp_add_header(req->output_headers, "Content-Type", "application/octet-stream");
            if (retrans == false)
            {
                evhttp_send_reply(req, HTTP_OK, "Success", NULL);
                mylog::GetLogger("asynclogger")->Info("evhttp_send_reply: HTTP_OK");
            }
            else
            {
                evhttp_send_reply(req, 206, "breakpoint continuous transmission", NULL); // 区间请求响应的是206
                mylog::GetLogger("asynclogger")->Info("evhttp_send_reply: 206");
            }
            // 7. 如果是深度存储的文件，且解压后生成了普通文件，则删除解压生成的普通文件
            if (download_path != info.storage_path_)
            {
                remove(download_path.c_str());
            }
        }
    };
}

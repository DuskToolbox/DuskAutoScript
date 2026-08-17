#pragma once

#include <das/_autogen/idl/abi/DasResult.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace Das::Plugins::DasMaaPi
{
    // Parsed HTTP proxy target (http://[user:pass@]host:port). Credentials are
    // optional; a socks5:// proxy URL is rejected up front (D-89-08).
    struct ProxyTarget
    {
        std::string                scheme;
        std::string                host;
        std::uint16_t              port = 0;
        std::optional<std::string> username;
        std::optional<std::string> password;
    };

    // Parse an HTTP proxy URL. Proxy-scheme rejection (missing scheme,
    // socks5://, or any non-http scheme) returns DAS_E_MAAPI_PROXY_UNSUPPORTED
    // so callers can distinguish it from structural URL errors
    // (empty host/port), which still return DAS_E_INVALID_ARGUMENT.
    DasResult ParseProxyUrl(std::string_view url, ProxyTarget& out);

    // 日志用代理 URL 打码：空串输出 "direct"（无代理直连），解析成功输出
    // scheme://host:port（认证字段剥离），解析失败输出 "unparsed(result=…)"
    // 而不回落原文（原文可能携带认证信息）。排查「代理到底有没有生效」全靠
    // 这条能与配置对账。
    std::string RedactProxyUrlForLog(std::string_view proxy_url);

    // 流式下载进度。total_bytes 仅在响应携带 Content-Length 时填充；缺失
    // （chunked / 连接关闭定界）时为空，调用侧显示阶段态「正在下载…」而非
    // 估算百分比。
    struct DownloadProgress
    {
        std::uint64_t                bytes_received = 0;
        std::optional<std::uint64_t> total_bytes;
    };

    using ProgressCallback = std::function<void(const DownloadProgress&)>;

    // Outbound HTTP seam (89-RESEARCH Pitfall 2). PiUpdateService depends on
    // this interface rather than a concrete client, so tests can inject a local
    // HTTP mock server while production uses the Boost.Beast implementation.
    class PiHttpClient
    {
    public:
        virtual ~PiHttpClient() = default;

        // Perform a GET and collect the response body + status code. On success
        // returns DAS_S_OK and fills out_body / out_status.
        // proxy_url empty => direct connection. Otherwise the request is routed
        // through the HTTP proxy: https targets use a CONNECT tunnel, http
        // targets use an absolute-form request line (D-89-08).
        virtual DasResult Get(
            const std::string& url,
            std::string&       out_body,
            int&               out_status,
            const std::string& proxy_url = {}) = 0;

        // Perform a HEAD and collect the status code only.
        virtual DasResult Head(
            const std::string& url,
            int&               out_status,
            const std::string& proxy_url = {}) = 0;

        // 流式下载 seam。把响应体边下边写入 out_path（不得整体缓冲进内存），
        // 逐块调 on_progress。测试注入 mock 模拟进度；生产传 BeastHttpClient。
        // out_path 由版本号拼接，调用侧负责目录穿越防护。
        virtual DasResult DownloadStream(
            const std::string&           url,
            const std::filesystem::path& out_path,
            const ProgressCallback&      on_progress,
            int&                         out_status,
            const std::string&           proxy_url = {}) = 0;

        // 请求取消 seam：请求在途时尽快中断（进程退出收尾用，使 join 有
        // 界）。默认 no-op——具体实现按自身 I/O 模型覆盖；测试 fake 覆盖它
        // 释放阻塞 latch。
        virtual void RequestCancel() {}
    };

    // Production Boost.Beast implementation of PiHttpClient. All network I/O is
    // asynchronous with a per-operation timeout so a dead peer cannot hang the
    // caller forever. https targets negotiate TLS via beast::ssl_stream; the
    // TLS certificate is verified against the URL host by default.
    class BeastHttpClient final : public PiHttpClient
    {
    public:
        explicit BeastHttpClient(
            bool                      verify_tls_certificate = true,
            std::chrono::milliseconds timeout = std::chrono::seconds(15));

        DasResult Get(
            const std::string& url,
            std::string&       out_body,
            int&               out_status,
            const std::string& proxy_url = {}) override;

        DasResult Head(
            const std::string& url,
            int&               out_status,
            const std::string& proxy_url = {}) override;

        DasResult DownloadStream(
            const std::string&           url,
            const std::filesystem::path& out_path,
            const ProgressCallback&      on_progress,
            int&                         out_status,
            const std::string&           proxy_url = {}) override;

        // 取消在途请求：stop() 其 io_context，阻塞中的 run() 尽快返回，
        // 协程被放弃、调用以默认失败码收尾。线程安全——可与执行请求的
        // 线程并发调用（进程退出收尾经 registry 在非 io 线程触达）。
        void RequestCancel() override;

    private:
        // 每请求独立的 I/O 状态。定义在实现文件中，Boost.Asio 依赖不进
        // 本头文件；active_ 弱引用使 RequestCancel 能从其它线程触达挂起
        // 请求的 io_context。
        struct RequestState;

        DasResult RunRequest(
            const std::string& url,
            const std::string& proxy_url,
            bool               head_only,
            std::string&       out_body,
            int&               out_status);

        DasResult RunStreamRequest(
            const std::string&           url,
            const std::string&           proxy_url,
            const std::filesystem::path& out_path,
            const ProgressCallback&      on_progress,
            int&                         out_status);

        bool                        verify_tls_certificate_;
        std::chrono::milliseconds   timeout_;
        std::mutex                  cancel_mutex_;
        std::weak_ptr<RequestState> active_;
    };
} // namespace Das::Plugins::DasMaaPi

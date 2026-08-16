#include "PiHttpClient.h"

#include <das/DasApi.h>
#include <das/Plugins/DasMaaPi/MaaPiErrorCodes.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/DasResult.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <openssl/ssl.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Das::Plugins::DasMaaPi
{
    namespace
    {
        // 响应体不设上限：完整 PI 更新包（MaaFW/MaaToolkit DLLs）经同一
        // Get seam（PiUpdateService::DownloadZip）下载，真实包大小可能远超
        // 固定上限，故 body_limit(boost::none) 不限制。
        constexpr std::string_view USER_AGENT =
            "DasMaaPi/0.1 (+DuskAutoScript)";

        struct UrlParts
        {
            std::string scheme;
            std::string host;
            uint16_t    port = 0;
            std::string path = "/";
            std::string host_and_port;
        };

        // Minimal "scheme://host[:port][/path]" parser. Rejects unknown schemes
        // and malformed ports. Only http / https are meaningful for this seam.
        bool ParseUrl(std::string_view url, UrlParts& out)
        {
            const auto scheme_end = url.find("://");
            if (scheme_end == std::string_view::npos)
            {
                return false;
            }
            const auto scheme = url.substr(0, scheme_end);
            if (scheme != "http" && scheme != "https")
            {
                return false;
            }

            const auto rest = url.substr(scheme_end + 3);
            const auto path_start = rest.find('/');
            const auto authority = (path_start == std::string_view::npos)
                                       ? rest
                                       : rest.substr(0, path_start);
            const auto path = (path_start == std::string_view::npos)
                                  ? std::string_view("/")
                                  : rest.substr(path_start);

            out.scheme = std::string(scheme);
            out.path = std::string(path);
            out.port = (scheme == "https") ? 443 : 80;

            const auto colon = authority.rfind(':');
            if (colon != std::string_view::npos)
            {
                const auto port_str = authority.substr(colon + 1);
                if (port_str.empty())
                {
                    return false;
                }
                std::uint32_t parsed = 0;
                for (const char c : port_str)
                {
                    if (c < '0' || c > '9')
                    {
                        return false;
                    }
                    parsed = parsed * 10 + static_cast<std::uint32_t>(c - '0');
                    if (parsed > 65535)
                    {
                        return false;
                    }
                }
                if (parsed == 0)
                {
                    return false;
                }
                out.port = static_cast<std::uint16_t>(parsed);
                out.host = std::string(authority.substr(0, colon));
            }
            else
            {
                out.host = std::string(authority);
            }
            if (out.host.empty())
            {
                return false;
            }
            out.host_and_port = out.host + ":" + std::to_string(out.port);
            return true;
        }

        // Redact a URL for logging: keep scheme://host:port, drop path/query so
        // sensitive query parameters never reach the log (T-89-01-02).
        std::string RedactUrlForLog(std::string_view url)
        {
            UrlParts parts;
            if (ParseUrl(url, parts))
            {
                return parts.scheme + "://" + parts.host_and_port;
            }
            return std::string(url);
        }

        struct RequestOutcome
        {
            DasResult          result = DAS_E_FAIL;
            std::exception_ptr error;
        };

        // Map a boost/beast error_code raised by co_await to a DasResult.
        DasResult MapSystemError(const boost::system::error_code& ec)
        {
            if (ec == boost::asio::error::operation_aborted
                || ec == boost::system::errc::timed_out
                || ec == boost::asio::error::timed_out)
            {
                return DAS_E_TIMEOUT;
            }
            if (ec == boost::beast::http::error::body_limit)
            {
                return DAS_E_MAYBE_OVERFLOW;
            }
            return DAS_E_FAIL;
        }

        // Plugin-facing diagnostics go through the exported DasLog* C ABI
        // (DasCoreApi.generated.h), which takes a single pre-formatted string.
        // DAS_CORE_LOG_* is unavailable here because DasCore::g_logger is not
        // exported from DasCore.dll.
        void LogDiagnosticError(const std::string& message)
        {
            DAS_LOG_ERROR(message.c_str());
        }

        void LogDiagnosticWarning(const std::string& message)
        {
            DAS_LOG_WARNING(message.c_str());
        }

        // Minimal base64 (standard alphabet) for Proxy-Authorization Basic.
        std::string Base64Encode(std::string_view input)
        {
            constexpr std::string_view CHARS =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out;
            out.reserve(((input.size() + 2) / 3) * 4);
            std::size_t i = 0;
            while (i + 3 <= input.size())
            {
                const auto n = (static_cast<std::uint32_t>(
                                    static_cast<std::uint8_t>(input[i]))
                                << 16)
                               | (static_cast<std::uint32_t>(
                                      static_cast<std::uint8_t>(input[i + 1]))
                                  << 8)
                               | static_cast<std::uint32_t>(
                                   static_cast<std::uint8_t>(input[i + 2]));
                out.push_back(CHARS[(n >> 18) & 63]);
                out.push_back(CHARS[(n >> 12) & 63]);
                out.push_back(CHARS[(n >> 6) & 63]);
                out.push_back(CHARS[n & 63]);
                i += 3;
            }
            const auto rem = input.size() - i;
            if (rem == 1)
            {
                const auto n = static_cast<std::uint32_t>(
                                   static_cast<std::uint8_t>(input[i]))
                               << 16;
                out.push_back(CHARS[(n >> 18) & 63]);
                out.push_back(CHARS[(n >> 12) & 63]);
                out.push_back('=');
                out.push_back('=');
            }
            else if (rem == 2)
            {
                const auto n = (static_cast<std::uint32_t>(
                                    static_cast<std::uint8_t>(input[i]))
                                << 16)
                               | (static_cast<std::uint32_t>(
                                      static_cast<std::uint8_t>(input[i + 1]))
                                  << 8);
                out.push_back(CHARS[(n >> 18) & 63]);
                out.push_back(CHARS[(n >> 12) & 63]);
                out.push_back(CHARS[(n >> 6) & 63]);
                out.push_back('=');
            }
            return out;
        }

        // Re-arm the per-operation timeout. beast::tcp_stream exposes
        // expires_after directly; beast::ssl_stream forwards through
        // next_layer.
        template <typename Stream>
        void ArmStreamTimeout(Stream& stream, std::chrono::milliseconds timeout)
        {
            if constexpr (requires { stream.expires_after(timeout); })
            {
                stream.expires_after(timeout);
            }
            else
            {
                stream.next_layer().expires_after(timeout);
            }
        }

        // Write the request and read the bounded response over an established
        // (optionally TLS) stream. absolute_form is used for plain-http targets
        // routed through a proxy: the request line carries the full URL
        // (RFC 7230 5.3.2) while the Host header stays the target host.
        // out_location, when non-null, receives the response Location header
        // (empty when absent) so the caller can follow 3xx redirects (CR-01).
        template <typename Stream>
        boost::asio::awaitable<DasResult> SendRequest(
            Stream&                   stream,
            const UrlParts&           target,
            bool                      head_only,
            bool                      absolute_form,
            std::chrono::milliseconds timeout,
            std::string&              out_body,
            int&                      out_status,
            std::string*              out_location)
        {
            boost::beast::http::request<boost::beast::http::empty_body> req;
            req.method(
                head_only ? boost::beast::http::verb::head
                          : boost::beast::http::verb::get);
            if (absolute_form)
            {
                req.target(
                    target.scheme + "://" + target.host_and_port + target.path);
            }
            else
            {
                req.target(target.path);
            }
            req.version(11);
            req.set(boost::beast::http::field::host, target.host_and_port);
            req.set(boost::beast::http::field::user_agent, USER_AGENT);
            req.set(boost::beast::http::field::accept, "*/*");

            boost::beast::http::response_parser<boost::beast::http::string_body>
                parser;
            parser.body_limit(boost::none);
            parser.skip(head_only);

            boost::beast::flat_buffer buffer;
            ArmStreamTimeout(stream, timeout);
            co_await boost::beast::http::async_write(
                stream,
                req,
                boost::asio::use_awaitable);
            ArmStreamTimeout(stream, timeout);
            co_await boost::beast::http::async_read(
                stream,
                buffer,
                parser,
                boost::asio::use_awaitable);

            auto resp = parser.release();
            out_status = static_cast<int>(resp.result_int());
            out_body = std::move(resp.body());
            if (out_location)
            {
                const auto loc = resp.find(boost::beast::http::field::location);
                if (loc != resp.end())
                {
                    *out_location = std::string(loc->value());
                }
            }
            co_return DAS_S_OK;
        }

        // Streaming GET that writes the response body straight to out_path as
        // it arrives (no whole-body buffering). The caller's on_progress
        // callback receives byte counts; total_bytes is filled only when the
        // response carries a Content-Length header (Content-Length absent =>
        // stage-only progress). Response headers use
        // response_parser<buffer_body>
        // + async_read_header so the header (and thus Content-Length) is known
        // before the first body byte is written to disk.
        template <typename Stream>
        boost::asio::awaitable<DasResult> SendStreamRequest(
            Stream&                      stream,
            const UrlParts&              target,
            bool                         absolute_form,
            std::chrono::milliseconds    timeout,
            const std::filesystem::path& out_path,
            const ProgressCallback&      on_progress,
            int&                         out_status,
            std::string*                 out_location)
        {
            boost::beast::http::request<boost::beast::http::empty_body> req;
            req.method(boost::beast::http::verb::get);
            if (absolute_form)
            {
                req.target(
                    target.scheme + "://" + target.host_and_port + target.path);
            }
            else
            {
                req.target(target.path);
            }
            req.version(11);
            req.set(boost::beast::http::field::host, target.host_and_port);
            req.set(boost::beast::http::field::user_agent, USER_AGENT);
            req.set(boost::beast::http::field::accept, "*/*");

            boost::beast::http::response_parser<boost::beast::http::buffer_body>
                parser;
            parser.body_limit(boost::none);

            boost::beast::flat_buffer buffer;
            ArmStreamTimeout(stream, timeout);
            co_await boost::beast::http::async_write(
                stream,
                req,
                boost::asio::use_awaitable);
            ArmStreamTimeout(stream, timeout);
            co_await boost::beast::http::async_read_header(
                stream,
                buffer,
                parser,
                boost::asio::use_awaitable);

            auto resp = parser.get();
            out_status = static_cast<int>(resp.result_int());
            if (out_location)
            {
                const auto loc = resp.find(boost::beast::http::field::location);
                if (loc != resp.end())
                {
                    *out_location = std::string(loc->value());
                }
            }

            // 一次性打开输出文件（binary append/truncate）；写入路径由版本号
            // 拼接，调用侧负责目录穿越防护。
            std::ofstream ofs(out_path, std::ios::binary | std::ios::trunc);
            if (!ofs)
            {
                LogDiagnosticError(
                    "BeastHttpClient cannot open download path: "
                    + out_path.string());
                co_return DAS_E_FAIL;
            }

            // Content-Length（存在时）→ total_bytes；缺失（chunked / 连接关闭
            // 定界）→ total_bytes 空，调用侧显示阶段态。
            std::optional<std::uint64_t> total_bytes;
            if (const auto cl =
                    resp.find(boost::beast::http::field::content_length);
                cl != resp.end())
            {
                std::uint64_t value = 0;
                bool          ok = !cl->value().empty();
                for (const char c : cl->value())
                {
                    if (c < '0' || c > '9')
                    {
                        ok = false;
                        break;
                    }
                    value = value * 10 + static_cast<std::uint64_t>(c - '0');
                }
                if (ok)
                {
                    total_bytes = value;
                }
            }

            std::uint64_t               bytes_received = 0;
            std::array<char, 64 * 1024> buf{};
            for (;;)
            {
                auto& body = parser.get().body();
                body.data = buf.data();
                body.size = buf.size();
                const std::size_t requested = buf.size();

                boost::system::error_code ec;
                co_await boost::beast::http::async_read_some(
                    stream,
                    buffer,
                    parser,
                    boost::asio::redirect_error(
                        boost::asio::use_awaitable,
                        ec));

                // buffer_body 消费后 body.size 为剩余缓冲空间，
                // requested - body.size 即本次写入 body 缓冲的字节数。
                const std::size_t written = requested - body.size;
                if (written > 0)
                {
                    ofs.write(
                        buf.data(),
                        static_cast<std::streamsize>(written));
                    if (!ofs)
                    {
                        LogDiagnosticError(
                            "BeastHttpClient write to disk failed: "
                            + out_path.string());
                        co_return DAS_E_FAIL;
                    }
                    bytes_received += written;
                    if (on_progress)
                    {
                        on_progress(
                            DownloadProgress{bytes_received, total_bytes});
                    }
                }
                if (ec)
                {
                    if (ec == boost::beast::http::error::need_buffer)
                    {
                        // 缓冲区已满且仍有 body 剩余 → 提供新缓冲区继续。
                        continue;
                    }
                    throw boost::system::system_error(ec);
                }
                // chunked 传输时 async_read_some 可能只消费 framing（分块头）
                // 而不写 body 字节、且未完成——不能因 written==0 提前结束，
                // 继续读直到 parser.is_done()。
                if (parser.is_done())
                {
                    break;
                }
            }
            co_return DAS_S_OK;
        }

        // Establish a CONNECT tunnel to the target through the HTTP proxy
        // (D-89-08). After the 200 Connection Established the same socket is
        // reused for the TLS handshake.
        boost::asio::awaitable<DasResult> DoConnectTunnel(
            boost::beast::tcp_stream& stream,
            const UrlParts&           target,
            const ProxyTarget&        proxy,
            std::chrono::milliseconds timeout)
        {
            const auto connect_target =
                target.host + ":" + std::to_string(target.port);

            boost::beast::http::request<boost::beast::http::empty_body> req;
            req.method(boost::beast::http::verb::connect);
            req.target(connect_target);
            req.version(11);
            req.set(boost::beast::http::field::host, connect_target);
            if (proxy.username && proxy.password)
            {
                const auto credential = *proxy.username + ":" + *proxy.password;
                req.set(
                    boost::beast::http::field::proxy_authorization,
                    "Basic " + Base64Encode(credential));
            }

            // A 2xx CONNECT response has no body (the tunnel starts
            // immediately) and real proxies omit Content-Length, so a full
            // message read would demand a body until connection close and fail
            // with unexpected_body. A skip(true) parser ends after the header.
            boost::beast::http::response_parser<boost::beast::http::empty_body>
                parser;
            parser.skip(true);
            boost::beast::flat_buffer buffer;
            ArmStreamTimeout(stream, timeout);
            co_await boost::beast::http::async_write(
                stream,
                req,
                boost::asio::use_awaitable);
            ArmStreamTimeout(stream, timeout);
            co_await boost::beast::http::async_read(
                stream,
                buffer,
                parser,
                boost::asio::use_awaitable);
            const auto resp = parser.release();
            if (resp.result() != boost::beast::http::status::ok)
            {
                LogDiagnosticError(
                    "proxy CONNECT rejected: host=" + target.host + " status="
                    + std::to_string(static_cast<int>(resp.result_int())));
                co_return DAS_E_FAIL;
            }
            co_return DAS_S_OK;
        }

        boost::asio::awaitable<DasResult> DoRequestImpl(
            bool                      verify_tls,
            std::chrono::milliseconds timeout,
            const UrlParts&           target,
            const ProxyTarget*        proxy,
            bool                      head_only,
            std::string&              out_body,
            int&                      out_status,
            std::string*              out_location)
        {
            const bool via_proxy = (proxy != nullptr);
            const bool use_tls = (target.scheme == "https");

            // Connect to the proxy when one is configured, else the target.
            const auto connect_host = via_proxy ? proxy->host : target.host;
            const auto connect_port = via_proxy ? std::to_string(proxy->port)
                                                : std::to_string(target.port);

            auto executor = co_await boost::asio::this_coro::executor;
            boost::asio::ip::tcp::resolver resolver(executor);

            // DNS resolution has no per-operation timeout of its own
            // (ArmStreamTimeout covers TCP connect/read/write only). Wrap
            // async_resolve in a steady_timer so a hung resolver cannot block
            // the authoring thread forever. The timer lives in the shared
            // state: after a timeout we cancel the resolver and return
            // DAS_E_TIMEOUT, and the resolve completion handler (which still
            // runs with operation_aborted) keeps the state alive — no dangling
            // reference into the coroutine frame.
            struct ResolveState
            {
                std::optional<boost::system::error_code> ec;
                std::optional<boost::asio::ip::tcp::resolver::results_type>
                                          result;
                boost::asio::steady_timer timer;
                explicit ResolveState(boost::asio::any_io_executor executor)
                    : timer(std::move(executor))
                {
                }
            };
            auto state = std::make_shared<ResolveState>(executor);
            state->timer.expires_after(timeout);
            resolver.async_resolve(
                connect_host,
                connect_port,
                [state](
                    const boost::system::error_code&             ec,
                    boost::asio::ip::tcp::resolver::results_type result)
                {
                    state->ec = ec;
                    state->result = std::move(result);
                    // basic_waitable_timer::cancel() 在此 Boost 版本只接受零参
                    // （可能抛 system_error）；失败可忽略——timer 随后以自然到期
                    // 结束，仅增加最多 timeout 的等待，不崩溃。
                    try
                    {
                        state->timer.cancel();
                    }
                    catch (const boost::system::system_error&)
                    {
                    }
                });
            // async_wait 经 redirect_error 捕获错误：resolver 先完成时 handler
            // cancel timer → wait 以 operation_aborted 结束，这是"解析已就绪"
            // 的正常信号而非失败，忽略 wait_ec，以 state->ec 为准；timer 自然
            // 到期才走超时分支。
            boost::system::error_code wait_ec;
            co_await state->timer.async_wait(
                boost::asio::redirect_error(
                    boost::asio::use_awaitable,
                    wait_ec));
            if (!state->ec)
            {
                // Timer fired before resolution completed → cancel and fail.
                resolver.cancel();
                LogDiagnosticError(
                    "BeastHttpClient DNS resolution timed out: host="
                    + connect_host);
                co_return DAS_E_TIMEOUT;
            }
            if (*state->ec)
            {
                throw boost::system::system_error(*state->ec);
            }
            const auto endpoints = std::move(*state->result);

            boost::beast::tcp_stream stream(executor);
            ArmStreamTimeout(stream, timeout);
            co_await stream.async_connect(
                endpoints,
                boost::asio::use_awaitable);

            // https targets through a proxy go over a CONNECT tunnel; the TLS
            // handshake runs inside it. http targets use absolute-form instead.
            if (via_proxy && use_tls)
            {
                const auto tunnel_result =
                    co_await DoConnectTunnel(stream, target, *proxy, timeout);
                if (IsFailed(tunnel_result))
                {
                    co_return tunnel_result;
                }
            }

            if (use_tls)
            {
                boost::asio::ssl::context ctx(
                    boost::asio::ssl::context::tls_client);
                if (verify_tls)
                {
                    ctx.set_verify_mode(boost::asio::ssl::verify_peer);
                    // 加载 OpenSSL 默认 CA（POSIX 走 /etc/ssl；Windows 上
                    // clang64 libcrypto 基于自身位置解析 OPENSSLDIR，实测
                    // 原生进程可定位到 ca-bundle）。set_verify_mode 只打开
                    // 校验开关，不调这个的话信任存储为空，任何 HTTPS 目标
                    // 都会 certificate verify failed。
                    ctx.set_default_verify_paths();
                    ctx.set_verify_callback(
                        boost::asio::ssl::host_name_verification(target.host));
                }
                else
                {
                    ctx.set_verify_mode(boost::asio::ssl::verify_none);
                }
                boost::beast::ssl_stream<boost::beast::tcp_stream> tls(
                    std::move(stream),
                    ctx);
                if (verify_tls)
                {
                    SSL_set_tlsext_host_name(
                        tls.native_handle(),
                        target.host.c_str());
                }
                ArmStreamTimeout(tls, timeout);
                co_await tls.async_handshake(
                    boost::asio::ssl::stream_base::client,
                    boost::asio::use_awaitable);
                co_return co_await SendRequest(
                    tls,
                    target,
                    head_only,
                    /*absolute_form=*/false,
                    timeout,
                    out_body,
                    out_status,
                    out_location);
            }

            co_return co_await SendRequest(
                stream,
                target,
                head_only,
                /*absolute_form=*/via_proxy,
                timeout,
                out_body,
                out_status,
                out_location);
        }

        // Connect-and-read coroutine for DownloadStream. Mirrors
        // DoRequestImpl's DNS-with-timeout / CONNECT tunnel / TLS /
        // absolute-form logic but terminates in SendStreamRequest (body
        // streamed to disk) instead of the whole-body SendRequest.
        boost::asio::awaitable<DasResult> DoStreamRequestImpl(
            bool                         verify_tls,
            std::chrono::milliseconds    timeout,
            const UrlParts&              target,
            const ProxyTarget*           proxy,
            const std::filesystem::path& out_path,
            const ProgressCallback&      on_progress,
            int&                         out_status,
            std::string*                 out_location)
        {
            const bool via_proxy = (proxy != nullptr);
            const bool use_tls = (target.scheme == "https");

            // Connect to the proxy when one is configured, else the target.
            const auto connect_host = via_proxy ? proxy->host : target.host;
            const auto connect_port = via_proxy ? std::to_string(proxy->port)
                                                : std::to_string(target.port);

            auto executor = co_await boost::asio::this_coro::executor;
            boost::asio::ip::tcp::resolver resolver(executor);

            // DNS resolution has no per-operation timeout of its own
            // (ArmStreamTimeout covers TCP connect/read/write only). Wrap
            // async_resolve in a steady_timer so a hung resolver cannot block
            // the authoring thread forever (same pattern as DoRequestImpl).
            struct ResolveState
            {
                std::optional<boost::system::error_code> ec;
                std::optional<boost::asio::ip::tcp::resolver::results_type>
                                          result;
                boost::asio::steady_timer timer;
                explicit ResolveState(boost::asio::any_io_executor executor)
                    : timer(std::move(executor))
                {
                }
            };
            auto state = std::make_shared<ResolveState>(executor);
            state->timer.expires_after(timeout);
            resolver.async_resolve(
                connect_host,
                connect_port,
                [state](
                    const boost::system::error_code&             ec,
                    boost::asio::ip::tcp::resolver::results_type result)
                {
                    state->ec = ec;
                    state->result = std::move(result);
                    try
                    {
                        state->timer.cancel();
                    }
                    catch (const boost::system::system_error&)
                    {
                    }
                });
            boost::system::error_code wait_ec;
            co_await state->timer.async_wait(
                boost::asio::redirect_error(
                    boost::asio::use_awaitable,
                    wait_ec));
            if (!state->ec)
            {
                resolver.cancel();
                LogDiagnosticError(
                    "BeastHttpClient DNS resolution timed out: host="
                    + connect_host);
                co_return DAS_E_TIMEOUT;
            }
            if (*state->ec)
            {
                throw boost::system::system_error(*state->ec);
            }
            const auto endpoints = std::move(*state->result);

            boost::beast::tcp_stream stream(executor);
            ArmStreamTimeout(stream, timeout);
            co_await stream.async_connect(
                endpoints,
                boost::asio::use_awaitable);

            // https targets through a proxy go over a CONNECT tunnel; the TLS
            // handshake runs inside it. http targets use absolute-form instead.
            if (via_proxy && use_tls)
            {
                const auto tunnel_result =
                    co_await DoConnectTunnel(stream, target, *proxy, timeout);
                if (IsFailed(tunnel_result))
                {
                    co_return tunnel_result;
                }
            }

            if (use_tls)
            {
                boost::asio::ssl::context ctx(
                    boost::asio::ssl::context::tls_client);
                if (verify_tls)
                {
                    ctx.set_verify_mode(boost::asio::ssl::verify_peer);
                    ctx.set_default_verify_paths();
                    ctx.set_verify_callback(
                        boost::asio::ssl::host_name_verification(target.host));
                }
                else
                {
                    ctx.set_verify_mode(boost::asio::ssl::verify_none);
                }
                boost::beast::ssl_stream<boost::beast::tcp_stream> tls(
                    std::move(stream),
                    ctx);
                if (verify_tls)
                {
                    SSL_set_tlsext_host_name(
                        tls.native_handle(),
                        target.host.c_str());
                }
                ArmStreamTimeout(tls, timeout);
                co_await tls.async_handshake(
                    boost::asio::ssl::stream_base::client,
                    boost::asio::use_awaitable);
                co_return co_await SendStreamRequest(
                    tls,
                    target,
                    /*absolute_form=*/false,
                    timeout,
                    out_path,
                    on_progress,
                    out_status,
                    out_location);
            }

            co_return co_await SendStreamRequest(
                stream,
                target,
                /*absolute_form=*/via_proxy,
                timeout,
                out_path,
                on_progress,
                out_status,
                out_location);
        }

        // 3xx status codes whose Location header is followed by the redirect
        // loop. 301/302/303 and 307/308 both map to a re-issued GET/HEAD here:
        // the seam only ever issues GET or HEAD, so no method/body rewrite is
        // ever required (303's POST→GET rule is moot without POST).
        bool IsRedirectStatus(int status)
        {
            switch (status)
            {
            case 301: // Moved Permanently
            case 302: // Found
            case 303: // See Other
            case 307: // Temporary Redirect
            case 308: // Permanent Redirect
                return true;
            default:
                return false;
            }
        }

        // Resolve a Location header against the current target URL (RFC 3986
        // §5 relative resolution). Handles absolute URLs, protocol-relative
        // (//host[:port][/path]), root-relative (/path) and path-relative
        // locations. Returns nullopt for malformed / empty locations.
        std::optional<std::string> ResolveRedirectUrl(
            const UrlParts&  current,
            std::string_view location)
        {
            if (location.empty())
            {
                return std::nullopt;
            }
            // Absolute URL.
            if (location.find("://") != std::string_view::npos)
            {
                UrlParts parsed;
                if (!ParseUrl(location, parsed))
                {
                    return std::nullopt;
                }
                return std::string(location);
            }
            // Protocol-relative "//host[:port][/path]".
            if (location.substr(0, 2) == "//")
            {
                return current.scheme + ":" + std::string(location);
            }
            // Root-relative "/path".
            if (location.front() == '/')
            {
                return current.scheme + "://" + current.host_and_port
                       + std::string(location);
            }
            // Path-relative: resolve against the current path's directory.
            const auto        slash = current.path.rfind('/');
            const std::string base_dir =
                (slash == std::string::npos)
                    ? std::string("/")
                    : current.path.substr(0, slash + 1);
            return current.scheme + "://" + current.host_and_port + base_dir
                   + std::string(location);
        }

        boost::asio::awaitable<void> DoRequest(
            const std::string&        url,
            const std::string&        proxy_url,
            bool                      head_only,
            bool                      verify_tls,
            std::chrono::milliseconds timeout,
            std::string&              out_body,
            int&                      out_status,
            RequestOutcome&           outcome)
        {
            try
            {
                UrlParts target;
                if (!ParseUrl(url, target))
                {
                    outcome.result = DAS_E_INVALID_URL;
                    LogDiagnosticError(
                        "BeastHttpClient invalid url: url="
                        + RedactUrlForLog(url));
                    co_return;
                }

                // Resolve the proxy up front; an invalid proxy URL (including
                // socks5) fails here instead of silently connecting directly.
                ProxyTarget        proxy;
                const ProxyTarget* proxy_ptr = nullptr;
                if (!proxy_url.empty())
                {
                    const auto parse_result = ParseProxyUrl(proxy_url, proxy);
                    if (IsFailed(parse_result))
                    {
                        outcome.result = parse_result;
                        co_return;
                    }
                    proxy_ptr = &proxy;
                }

                // CR-01: follow up to MAX_REDIRECTS 3xx responses. GitHub
                // release download URLs 302 to
                // release-assets.githubusercontent.com, so a single-shot GET
                // would fail the DownloadZip status==200 check on every
                // real-network applyUpdate. Each hop re-issues the request
                // against the resolved Location; the proxy (if any) is applied
                // per hop inside DoRequestImpl.
                constexpr int MAX_REDIRECTS = 5;
                std::string   current_url = url;
                std::string   location;
                for (int hop = 0; hop <= MAX_REDIRECTS; ++hop)
                {
                    if (!ParseUrl(current_url, target))
                    {
                        outcome.result = DAS_E_INVALID_URL;
                        LogDiagnosticError(
                            "BeastHttpClient invalid redirect url: url="
                            + RedactUrlForLog(current_url));
                        co_return;
                    }
                    location.clear();
                    outcome.result = co_await DoRequestImpl(
                        verify_tls,
                        timeout,
                        target,
                        proxy_ptr,
                        head_only,
                        out_body,
                        out_status,
                        &location);
                    if (IsFailed(outcome.result))
                    {
                        co_return;
                    }
                    if (!IsRedirectStatus(out_status))
                    {
                        co_return;
                    }
                    if (location.empty())
                    {
                        // 3xx without a Location header is surfaced as-is so
                        // the caller sees the redirect status.
                        co_return;
                    }
                    if (hop == MAX_REDIRECTS)
                    {
                        LogDiagnosticWarning(
                            "BeastHttpClient redirect limit reached: url="
                            + RedactUrlForLog(url)
                            + " hops=" + std::to_string(MAX_REDIRECTS));
                        co_return;
                    }
                    const auto resolved = ResolveRedirectUrl(target, location);
                    if (!resolved)
                    {
                        LogDiagnosticError(
                            "BeastHttpClient invalid redirect location: url="
                            + RedactUrlForLog(current_url));
                        outcome.result = DAS_E_FAIL;
                        co_return;
                    }
                    current_url = *resolved;
                }
            }
            catch (const boost::system::system_error& e)
            {
                outcome.result = MapSystemError(e.code());
                LogDiagnosticError(
                    "BeastHttpClient request failed: url="
                    + RedactUrlForLog(url) + " error=" + e.code().message());
            }
            catch (const std::exception& e)
            {
                outcome.result = DAS_E_FAIL;
                LogDiagnosticError(
                    "BeastHttpClient request exception: url="
                    + RedactUrlForLog(url) + " error=" + e.what());
            }
        }

        boost::asio::awaitable<void> DoStreamRequest(
            const std::string&           url,
            const std::string&           proxy_url,
            bool                         verify_tls,
            std::chrono::milliseconds    timeout,
            const std::filesystem::path& out_path,
            const ProgressCallback&      on_progress,
            int&                         out_status,
            RequestOutcome&              outcome)
        {
            try
            {
                UrlParts target;
                if (!ParseUrl(url, target))
                {
                    outcome.result = DAS_E_INVALID_URL;
                    LogDiagnosticError(
                        "BeastHttpClient invalid url: url="
                        + RedactUrlForLog(url));
                    co_return;
                }

                // Resolve the proxy up front; an invalid proxy URL (including
                // socks5) fails here instead of silently connecting directly.
                ProxyTarget        proxy;
                const ProxyTarget* proxy_ptr = nullptr;
                if (!proxy_url.empty())
                {
                    const auto parse_result = ParseProxyUrl(proxy_url, proxy);
                    if (IsFailed(parse_result))
                    {
                        outcome.result = parse_result;
                        co_return;
                    }
                    proxy_ptr = &proxy;
                }

                // Follow up to MAX_REDIRECTS 3xx responses. Each hop
                // re-issues the request against the resolved Location; each hop
                // truncates out_path, so the file ends up with only the final
                // 200 body.
                constexpr int MAX_REDIRECTS = 5;
                std::string   current_url = url;
                std::string   location;
                for (int hop = 0; hop <= MAX_REDIRECTS; ++hop)
                {
                    if (!ParseUrl(current_url, target))
                    {
                        outcome.result = DAS_E_INVALID_URL;
                        LogDiagnosticError(
                            "BeastHttpClient invalid redirect url: url="
                            + RedactUrlForLog(current_url));
                        co_return;
                    }
                    location.clear();
                    outcome.result = co_await DoStreamRequestImpl(
                        verify_tls,
                        timeout,
                        target,
                        proxy_ptr,
                        out_path,
                        on_progress,
                        out_status,
                        &location);
                    if (IsFailed(outcome.result))
                    {
                        co_return;
                    }
                    if (!IsRedirectStatus(out_status))
                    {
                        co_return;
                    }
                    if (location.empty())
                    {
                        // 3xx without a Location header is surfaced as-is so
                        // the caller sees the redirect status.
                        co_return;
                    }
                    if (hop == MAX_REDIRECTS)
                    {
                        LogDiagnosticWarning(
                            "BeastHttpClient redirect limit reached: url="
                            + RedactUrlForLog(url)
                            + " hops=" + std::to_string(MAX_REDIRECTS));
                        co_return;
                    }
                    const auto resolved = ResolveRedirectUrl(target, location);
                    if (!resolved)
                    {
                        LogDiagnosticError(
                            "BeastHttpClient invalid redirect location: url="
                            + RedactUrlForLog(current_url));
                        outcome.result = DAS_E_FAIL;
                        co_return;
                    }
                    current_url = *resolved;
                }
            }
            catch (const boost::system::system_error& e)
            {
                outcome.result = MapSystemError(e.code());
                LogDiagnosticError(
                    "BeastHttpClient stream request failed: url="
                    + RedactUrlForLog(url) + " error=" + e.code().message());
            }
            catch (const std::exception& e)
            {
                outcome.result = DAS_E_FAIL;
                LogDiagnosticError(
                    "BeastHttpClient stream request exception: url="
                    + RedactUrlForLog(url) + " error=" + e.what());
            }
        }

        DasResult RunRequest(
            bool                      verify_tls,
            std::chrono::milliseconds timeout,
            const std::string&        url,
            const std::string&        proxy_url,
            bool                      head_only,
            std::string&              out_body,
            int&                      out_status)
        {
            boost::asio::io_context io;
            RequestOutcome          outcome;
            boost::asio::co_spawn(
                io,
                DoRequest(
                    url,
                    proxy_url,
                    head_only,
                    verify_tls,
                    timeout,
                    out_body,
                    out_status,
                    outcome),
                [&](std::exception_ptr e) { outcome.error = std::move(e); });
            io.run();
            if (outcome.error)
            {
                try
                {
                    std::rethrow_exception(outcome.error);
                }
                catch (const boost::system::system_error& e)
                {
                    outcome.result = MapSystemError(e.code());
                    LogDiagnosticError(
                        "BeastHttpClient request aborted: url="
                        + RedactUrlForLog(url)
                        + " error=" + e.code().message());
                }
                catch (...)
                {
                    outcome.result = DAS_E_FAIL;
                }
            }
            return outcome.result;
        }
    } // namespace

    DasResult ParseProxyUrl(std::string_view url, ProxyTarget& out)
    {
        const auto scheme_end = url.find("://");
        if (scheme_end == std::string_view::npos)
        {
            LogDiagnosticWarning("proxy url missing scheme");
            return DAS_E_MAAPI_PROXY_UNSUPPORTED;
        }
        const auto scheme = url.substr(0, scheme_end);
        if (scheme == "socks5" || scheme == "socks5h")
        {
            // Explicit unsupported error rather than a silent downgrade
            // (D-89-08). Converged to DAS_E_MAAPI_PROXY_UNSUPPORTED in 89-02.
            LogDiagnosticWarning("socks5 proxy is not supported yet");
            return DAS_E_MAAPI_PROXY_UNSUPPORTED;
        }
        if (scheme != "http")
        {
            LogDiagnosticWarning(
                "unsupported proxy scheme: scheme=" + std::string(scheme));
            return DAS_E_MAAPI_PROXY_UNSUPPORTED;
        }

        auto             authority = url.substr(scheme_end + 3);
        std::string_view userinfo;
        if (const auto at = authority.rfind('@'); at != std::string_view::npos)
        {
            userinfo = authority.substr(0, at);
            authority = authority.substr(at + 1);
        }

        std::string_view host;
        std::string_view port_str;
        if (const auto colon = authority.rfind(':');
            colon != std::string_view::npos)
        {
            host = authority.substr(0, colon);
            port_str = authority.substr(colon + 1);
        }
        else
        {
            host = authority;
        }
        if (host.empty() || port_str.empty())
        {
            LogDiagnosticWarning(
                "proxy url must include host and port: scheme="
                + std::string(scheme));
            return DAS_E_INVALID_ARGUMENT;
        }
        std::uint32_t port = 0;
        for (const char c : port_str)
        {
            if (c < '0' || c > '9')
            {
                return DAS_E_INVALID_ARGUMENT;
            }
            port = port * 10 + static_cast<std::uint32_t>(c - '0');
            if (port > 65535)
            {
                return DAS_E_INVALID_ARGUMENT;
            }
        }
        if (port == 0)
        {
            return DAS_E_INVALID_ARGUMENT;
        }

        out.scheme = std::string(scheme);
        out.host = std::string(host);
        out.port = static_cast<std::uint16_t>(port);
        out.username.reset();
        out.password.reset();
        if (!userinfo.empty())
        {
            if (const auto sep = userinfo.find(':');
                sep != std::string_view::npos)
            {
                out.username = std::string(userinfo.substr(0, sep));
                out.password = std::string(userinfo.substr(sep + 1));
            }
            else
            {
                out.username = std::string(userinfo);
            }
        }
        return DAS_S_OK;
    }

    // 每请求独立的 io_context 状态：io.run() 阻塞期间经 BeastHttpClient::
    // active_ 弱引用可达，RequestCancel 据此 stop()。stop 后 run() 尽快返回，
    // 已排队未调用的 handler（含被放弃协程的帧）随 io_context 析构销毁。
    struct BeastHttpClient::RequestState
    {
        boost::asio::io_context io;
    };

    BeastHttpClient::BeastHttpClient(
        bool                      verify_tls_certificate,
        std::chrono::milliseconds timeout)
        : verify_tls_certificate_(verify_tls_certificate), timeout_(timeout)
    {
    }

    DasResult BeastHttpClient::Get(
        const std::string& url,
        std::string&       out_body,
        int&               out_status,
        const std::string& proxy_url)
    {
        return RunRequest(
            verify_tls_certificate_,
            timeout_,
            url,
            proxy_url,
            /*head_only=*/false,
            out_body,
            out_status);
    }

    DasResult BeastHttpClient::Head(
        const std::string& url,
        int&               out_status,
        const std::string& proxy_url)
    {
        std::string unused_body;
        return RunRequest(
            verify_tls_certificate_,
            timeout_,
            url,
            proxy_url,
            /*head_only=*/true,
            unused_body,
            out_status);
    }

    DasResult BeastHttpClient::DownloadStream(
        const std::string&           url,
        const std::filesystem::path& out_path,
        const ProgressCallback&      on_progress,
        int&                         out_status,
        const std::string&           proxy_url)
    {
        return RunStreamRequest(
            url,
            proxy_url,
            out_path,
            on_progress,
            out_status);
    }

    void BeastHttpClient::RequestCancel()
    {
        const std::lock_guard<std::mutex> cancel_lock(cancel_mutex_);
        if (const auto state = active_.lock())
        {
            state->io.stop();
        }
    }

    DasResult BeastHttpClient::RunStreamRequest(
        const std::string&           url,
        const std::string&           proxy_url,
        const std::filesystem::path& out_path,
        const ProgressCallback&      on_progress,
        int&                         out_status)
    {
        const auto state = std::make_shared<RequestState>();
        {
            const std::lock_guard<std::mutex> registration_lock(cancel_mutex_);
            active_ = state;
        }
        RequestOutcome outcome;
        boost::asio::co_spawn(
            state->io,
            DoStreamRequest(
                url,
                proxy_url,
                verify_tls_certificate_,
                timeout_,
                out_path,
                on_progress,
                out_status,
                outcome),
            [&](std::exception_ptr e) { outcome.error = std::move(e); });
        state->io.run();
        {
            const std::lock_guard<std::mutex> registration_lock(cancel_mutex_);
            active_.reset();
        }
        if (outcome.error)
        {
            try
            {
                std::rethrow_exception(outcome.error);
            }
            catch (const boost::system::system_error& e)
            {
                outcome.result = MapSystemError(e.code());
                LogDiagnosticError(
                    "BeastHttpClient stream request aborted: url="
                    + RedactUrlForLog(url) + " error=" + e.code().message());
            }
            catch (...)
            {
                outcome.result = DAS_E_FAIL;
            }
        }
        return outcome.result;
    }
} // namespace Das::Plugins::DasMaaPi

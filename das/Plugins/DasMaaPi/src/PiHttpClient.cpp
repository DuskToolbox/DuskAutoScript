#include "PiHttpClient.h"

#include <das/DasApi.h>
#include <das/_autogen/idl/abi/DasResult.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <openssl/ssl.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace Das::Plugins::DasMaaPi
{
    namespace
    {
        // Response body cap (T-89-01-03 DoS mitigation). Streamed downloads are
        // handled by the 89-02 update flow; the seam only ever buffers API
        // responses that fit well under this bound.
        constexpr std::size_t      kMaxResponseBodyBytes = 64u * 1024u * 1024u;
        constexpr std::string_view kUserAgent =
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
        // (optionally TLS) stream.
        template <typename Stream>
        boost::asio::awaitable<DasResult> SendRequest(
            Stream&                   stream,
            const UrlParts&           target,
            bool                      head_only,
            std::chrono::milliseconds timeout,
            std::string&              out_body,
            int&                      out_status)
        {
            boost::beast::http::request<boost::beast::http::empty_body> req;
            req.method(
                head_only ? boost::beast::http::verb::head
                          : boost::beast::http::verb::get);
            req.target(target.path);
            req.version(11);
            req.set(boost::beast::http::field::host, target.host_and_port);
            req.set(boost::beast::http::field::user_agent, kUserAgent);
            req.set(boost::beast::http::field::accept, "*/*");

            boost::beast::http::response_parser<boost::beast::http::string_body>
                parser;
            parser.body_limit(kMaxResponseBodyBytes);
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
            co_return DAS_S_OK;
        }

        boost::asio::awaitable<DasResult> DoRequestImpl(
            bool                      verify_tls,
            std::chrono::milliseconds timeout,
            const UrlParts&           target,
            bool                      head_only,
            std::string&              out_body,
            int&                      out_status)
        {
            auto executor = co_await boost::asio::this_coro::executor;
            boost::asio::ip::tcp::resolver resolver(executor);
            const auto endpoints = co_await resolver.async_resolve(
                target.host,
                std::to_string(target.port),
                boost::asio::use_awaitable);

            boost::beast::tcp_stream stream(executor);
            ArmStreamTimeout(stream, timeout);
            co_await stream.async_connect(
                endpoints,
                boost::asio::use_awaitable);

            if (target.scheme == "https")
            {
                boost::asio::ssl::context ctx(
                    boost::asio::ssl::context::tls_client);
                if (verify_tls)
                {
                    ctx.set_verify_mode(boost::asio::ssl::verify_peer);
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
                    timeout,
                    out_body,
                    out_status);
            }

            co_return co_await SendRequest(
                stream,
                target,
                head_only,
                timeout,
                out_body,
                out_status);
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
                // Proxy routing lands with the CONNECT tunnel task; the seam
                // signature already reserves the parameter (D-89-08).
                if (!proxy_url.empty())
                {
                    outcome.result = DAS_E_INVALID_ARGUMENT;
                    LogDiagnosticWarning(
                        "BeastHttpClient proxy support not yet implemented");
                    co_return;
                }
                UrlParts target;
                if (!ParseUrl(url, target))
                {
                    outcome.result = DAS_E_INVALID_URL;
                    LogDiagnosticError(
                        "BeastHttpClient invalid url: url="
                        + RedactUrlForLog(url));
                    co_return;
                }
                outcome.result = co_await DoRequestImpl(
                    verify_tls,
                    timeout,
                    target,
                    head_only,
                    out_body,
                    out_status);
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
} // namespace Das::Plugins::DasMaaPi

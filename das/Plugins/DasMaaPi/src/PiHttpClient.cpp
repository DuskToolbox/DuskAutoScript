#include "PiHttpClient.h"

#include <das/DasApi.h>
#include <das/Utils/DasJsonCore.h>
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
#include <boost/dll.hpp>
#include <openssl/ssl.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iterator>
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

        // Minimal base64 (standard alphabet) for Proxy-Authorization Basic.
        std::string Base64Encode(std::string_view input)
        {
            constexpr std::string_view kChars =
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
                out.push_back(kChars[(n >> 18) & 63]);
                out.push_back(kChars[(n >> 12) & 63]);
                out.push_back(kChars[(n >> 6) & 63]);
                out.push_back(kChars[n & 63]);
                i += 3;
            }
            const auto rem = input.size() - i;
            if (rem == 1)
            {
                const auto n = static_cast<std::uint32_t>(
                                   static_cast<std::uint8_t>(input[i]))
                               << 16;
                out.push_back(kChars[(n >> 18) & 63]);
                out.push_back(kChars[(n >> 12) & 63]);
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
                out.push_back(kChars[(n >> 18) & 63]);
                out.push_back(kChars[(n >> 12) & 63]);
                out.push_back(kChars[(n >> 6) & 63]);
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
        template <typename Stream>
        boost::asio::awaitable<DasResult> SendRequest(
            Stream&                   stream,
            const UrlParts&           target,
            bool                      head_only,
            bool                      absolute_form,
            std::chrono::milliseconds timeout,
            std::string&              out_body,
            int&                      out_status)
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
            int&                      out_status)
        {
            const bool via_proxy = (proxy != nullptr);
            const bool use_tls = (target.scheme == "https");

            // Connect to the proxy when one is configured, else the target.
            const auto connect_host = via_proxy ? proxy->host : target.host;
            const auto connect_port = via_proxy ? std::to_string(proxy->port)
                                                : std::to_string(target.port);

            auto executor = co_await boost::asio::this_coro::executor;
            boost::asio::ip::tcp::resolver resolver(executor);
            const auto endpoints = co_await resolver.async_resolve(
                connect_host,
                connect_port,
                boost::asio::use_awaitable);

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
                    out_status);
            }

            co_return co_await SendRequest(
                stream,
                target,
                head_only,
                /*absolute_form=*/via_proxy,
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

                outcome.result = co_await DoRequestImpl(
                    verify_tls,
                    timeout,
                    target,
                    proxy_ptr,
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

    DasResult ParseProxyUrl(std::string_view url, ProxyTarget& out)
    {
        const auto scheme_end = url.find("://");
        if (scheme_end == std::string_view::npos)
        {
            LogDiagnosticWarning("proxy url missing scheme");
            return DAS_E_INVALID_ARGUMENT;
        }
        const auto scheme = url.substr(0, scheme_end);
        if (scheme == "socks5" || scheme == "socks5h")
        {
            // Explicit unsupported error rather than a silent downgrade
            // (D-89-08). Converged to DAS_E_MAAPI_PROXY_UNSUPPORTED in 89-02.
            LogDiagnosticWarning("socks5 proxy is not supported yet");
            return DAS_E_INVALID_ARGUMENT;
        }
        if (scheme != "http")
        {
            LogDiagnosticWarning(
                "unsupported proxy scheme: scheme=" + std::string(scheme));
            return DAS_E_INVALID_ARGUMENT;
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

    std::string ReadManifestProxyUrl(const std::filesystem::path& manifest_path)
    {
        std::filesystem::path manifest = manifest_path;
        if (manifest.empty())
        {
            try
            {
                // this_line_location() returns a boost::filesystem::path;
                // convert explicitly so the manifest path is std::filesystem.
                manifest = std::filesystem::path(
                    (boost::dll::this_line_location().parent_path()
                     / "DasMaaPi.json")
                        .string());
            }
            catch (const std::exception&)
            {
                // this_line_location() can fail on exotic platforms; treat the
                // manifest as absent (direct connection).
                return {};
            }
        }

        std::ifstream file(manifest, std::ios::binary);
        if (!file.is_open())
        {
            return {};
        }
        const std::string text(
            (std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>());

        const auto json = Das::Utils::ParseYyjsonFromString(text);
        if (!json)
        {
            return {};
        }
        const auto obj = json->as_object();
        if (!obj)
        {
            return {};
        }
        // Iterate rather than operator[] because cpp_yyjson operator[] throws
        // std::out_of_range on a missing key; an absent proxyUrl means direct.
        for (auto it = obj->begin(); it != obj->end(); ++it)
        {
            if (it->first == "proxyUrl" && it->second.is_string())
            {
                return std::string(it->second.as_string().value_or(""));
            }
        }
        return {};
    }

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

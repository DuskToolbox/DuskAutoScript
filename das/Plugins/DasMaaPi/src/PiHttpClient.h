#pragma once

#include <das/_autogen/idl/abi/DasResult.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
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

    // Parse an HTTP proxy URL. Returns DAS_E_INVALID_ARGUMENT with a diagnostic
    // for socks5:// and for malformed URLs (missing scheme/host/port) instead
    // of silently connecting directly.
    DasResult ParseProxyUrl(std::string_view url, ProxyTarget& out);

    // Read the top-level "proxyUrl" field from the co-located DasMaaPi.json
    // manifest (boost::dll::this_line_location().parent_path()). Empty string
    // means direct connection. When manifest_path is given it is used instead
    // (tests); a missing/unreadable manifest or absent field returns "".
    std::string ReadManifestProxyUrl(
        const std::filesystem::path& manifest_path = {});

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

    private:
        bool                      verify_tls_certificate_;
        std::chrono::milliseconds timeout_;
    };
} // namespace Das::Plugins::DasMaaPi

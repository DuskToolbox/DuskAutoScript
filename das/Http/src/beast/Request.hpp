#ifndef DAS_HTTP_BEAST_REQUEST_HPP
#define DAS_HTTP_BEAST_REQUEST_HPP

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <das/IDasBase.h>
#include <map>
#include <nlohmann/json.hpp>
#include <string>

namespace Das::Http::Beast
{

    namespace http = boost::beast::http;
    using tcp = boost::asio::ip::tcp;

    // HTTP请求封装
    class HttpRequest
    {
    public:
        using request_type = http::request<http::string_body>;

        HttpRequest(request_type&& req) : request_(std::move(req))
        {
            ParseBody();
        }

        const std::string& Method() const noexcept
        {
            return request_.method_string();
        }

        const std::string& Target() const noexcept { return request_.target(); }

        const std::string& Body() const noexcept { return request_.body(); }

        const nlohmann::json& JsonBody() const noexcept { return json_body_; }

        const request_type& RawRequest() const noexcept { return request_; }

        std::string GetHeader(const std::string& name) const
        {
            auto it = request_.find(name);
            if (it != request_.end())
            {
                return std::string(it->value());
            }
            return "";
        }

    private:
        void ParseBody()
        {
            try
            {
                if (!request_.body().empty())
                {
                    json_body_ = nlohmann::json::parse(request_.body());
                }
            }
            catch (const nlohmann::json::exception&)
            {
                // JSON解析失败，json_body_为空对象
                json_body_ = nlohmann::json::object();
            }
        }

        request_type   request_;
        nlohmann::json json_body_;
    };

    // HTTP响应封装
    class HttpResponse
    {
    public:
        using response_type = http::response<http::string_body>;

        HttpResponse(
            http::status status = http::status::ok,
            unsigned int version = 11)
            : response_(status, version)
        {
            response_.set(http::field::server, BOOST_BEAST_VERSION_STRING);
            response_.set(http::field::content_type, "application/json");
        }

        void SetBody(const std::string& body)
        {
            response_.body() = body;
            response_.content_length(body.size());
        }

        void SetBody(const nlohmann::json& json)
        {
            response_.body() = json.dump();
            response_.content_length(response_.body().size());
        }

        void SetHeader(const std::string& name, const std::string& value)
        {
            response_.set(name, value);
        }

        response_type&& Release() noexcept { return std::move(response_); }

        static HttpResponse CreateErrorResponse(
            DasResult          error_code,
            const std::string& message)
        {
            HttpResponse   response;
            nlohmann::json body;
            body["code"] = error_code;
            body["message"] = message;
            body["data"] = nullptr;
            response.SetBody(body);
            return response;
        }

        static HttpResponse CreateSuccessResponse(
            const nlohmann::json& data = nullptr)
        {
            HttpResponse   response;
            nlohmann::json body;
            body["code"] = DAS_S_OK;
            body["message"] = "";
            body["data"] = data;
            response.SetBody(body);
            return response;
        }

    private:
        response_type response_;
    };

} // namespace Das::Http::Beast

#endif // DAS_HTTP_BEAST_REQUEST_HPP

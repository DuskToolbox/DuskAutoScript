#ifndef DAS_HTTP_BEAST_REQUEST_HPP
#define DAS_HTTP_BEAST_REQUEST_HPP

#include "JsonUtils.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <cpp_yyjson.hpp>
#include <das/IDasBase.h>
#include <das/Utils/DasJsonCore.h>
#include <map>
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

        std::string Method() const noexcept
        {
            return std::string{request_.method_string()};
        }

        std::string Target() const noexcept
        {
            return std::string{request_.target()};
        }

        const std::string& Body() const noexcept { return request_.body(); }

        const yyjson::writer::detail::value& JsonBody() const noexcept
        {
            return json_body_;
        }

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

        // Path parameter access
        std::string GetPathParameter(const std::string& name) const
        {
            auto it = path_params_.find(name);
            if (it != path_params_.end())
            {
                return it->second;
            }
            return "";
        }

        void SetPathParameter(const std::string& name, const std::string& value)
            const
        {
            path_params_[name] = value;
        }

    private:
        void ParseBody()
        {
            if (!request_.body().empty())
            {
                auto parsed =
                    Das::Utils::ParseYyjsonFromString(request_.body());
                if (parsed)
                {
                    json_body_ = std::move(parsed.value());
                    return;
                }
            }
            json_body_ = Das::Utils::MakeYyjsonObject();
        }

        request_type                               request_;
        yyjson::writer::detail::value              json_body_;
        mutable std::map<std::string, std::string> path_params_;
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

        void SetBody(const yyjson::writer::detail::value& json)
        {
            auto opt_str = Das::Utils::SerializeYyjsonValue(json);
            if (opt_str)
            {
                response_.body() = std::move(opt_str.value());
                response_.content_length(response_.body().size());
            }
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
            HttpResponse response;
            auto         json_body =
                JsonUtils::CreateErrorResponse(error_code, message);
            response.SetBody(json_body);
            return response;
        }

        static HttpResponse CreateSuccessResponse(
            const yyjson::writer::detail::value& data = {})
        {
            HttpResponse response;
            auto         json_body = JsonUtils::CreateSuccessResponse(data);
            response.SetBody(json_body);
            return response;
        }

        static HttpResponse CreateSuccessResponse(
            DasResult                            code,
            const std::string&                   message,
            const yyjson::writer::detail::value& data = {})
        {
            HttpResponse response;
            auto         json_body = JsonUtils::CreateSuccessResponse(data);
            auto         obj_opt = json_body.as_object();
            if (obj_opt)
            {
                auto& obj = obj_opt.value();
                obj["Code"] = static_cast<int64_t>(code);
                obj["Message"] = std::string{message};
            }
            response.SetBody(json_body);
            return response;
        }

    private:
        response_type response_;
    };

} // namespace Das::Http::Beast

#endif // DAS_HTTP_BEAST_REQUEST_HPP

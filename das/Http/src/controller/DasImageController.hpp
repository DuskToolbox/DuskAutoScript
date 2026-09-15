#ifndef DAS_HTTP_CONTROLLER_IMAGECONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_IMAGECONTROLLER_HPP

#include <cassert>

#include "Config.h"
#include "beast/Request.hpp"
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/Utils/PathText.h>
#include <das/_autogen/idl/abi/IDasLogRecorder.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace Das::Http
{
    // 静态图片服务：GET /logs/{profile}/images/{file} 从当前会话目录回 JPG
    // 字节（Content-Type image/jpeg）。路径穿越防护：file 与 session_dir
    // 拼接后 lexically_normal()，断言结果仍落在 session_dir 内，且 file
    // 不含 ".." 段 / 不是绝对路径 / 不带盘符。
    // session 由 App.cpp 注入（替代原 LogRecorderRuntime::SessionDir
    // 静态调用）。 改为实例方法：handler 经 shared_ptr 持有 controller，session
    // 在 ctor 时 由 App.cpp 传入。SanitizeImagePath 保持纯函数（含穿越防护）。
    class DasImageController
    {
    public:
        Beast::HttpResponse GetImage(const Beast::HttpRequest& request)
        {
            if (request.GetPathParameter("profile") != "0")
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Profile ID must be 0 in v1.2");
            }

            if (!session_)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_OBJECT_NOT_INIT,
                    "Log recorder session not initialized");
            }

            Das::DasPtr<IDasReadOnlyString> directory_str;
            if (DAS::IsFailed(session_->GetSessionDir(directory_str.Put()))
                || !directory_str)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_OBJECT_NOT_INIT,
                    "Log recorder session not initialized");
            }
            const char* directory_utf8 = nullptr;
            if (DAS::IsFailed(directory_str->GetUtf8(&directory_utf8))
                || directory_utf8 == nullptr)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_OBJECT_NOT_INIT,
                    "Log recorder session not initialized");
            }
            const auto session_dir = std::filesystem::path(
                reinterpret_cast<const char8_t*>(directory_utf8));

            const auto safe_path = SanitizeImagePath(
                request.GetPathParameter("file"),
                session_dir);
            if (!safe_path)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_FILE_NOT_FOUND,
                    "Image not found");
            }

            std::ifstream input{*safe_path, std::ios::binary};
            if (!input)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_FILE_NOT_FOUND,
                    "Image not found");
            }
            std::string bytes{
                std::istreambuf_iterator<char>{input},
                std::istreambuf_iterator<char>{}};
            if (bytes.empty())
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_FILE_NOT_FOUND,
                    "Image not found");
            }

            Beast::HttpResponse response;
            response.SetHeader("Content-Type", "image/jpeg");
            response.SetBody(std::move(bytes));
            return response;
        }

        // 路径规范化 + 穿越防护：file 拼到 session_dir 后必须仍落在
        // session_dir 目录内。不通过返回 nullopt。
        static std::optional<std::filesystem::path> SanitizeImagePath(
            const std::string&           file,
            const std::filesystem::path& session_dir)
        {
            if (file.empty())
            {
                return std::nullopt;
            }
            const std::filesystem::path rel = Das::Utils::PathFromUtf8(file);
            // 绝对路径或带盘符（如 C:foo）直接拒绝——盘符路径拼接会覆盖
            // session_dir 的根。
            if (rel.is_absolute() || rel.has_root_name())
            {
                return std::nullopt;
            }
            for (const auto& part : rel)
            {
                if (part == DAS_PATH_LITERAL(".."))
                {
                    return std::nullopt;
                }
            }

            const auto  base = session_dir.lexically_normal();
            const auto  resolved = (base / rel).lexically_normal();
            std::string base_str = Das::Utils::PathToUtf8(base);
            if (!base_str.empty()
                && base_str.back()
                       != std::filesystem::path::preferred_separator)
            {
                base_str.push_back(std::filesystem::path::preferred_separator);
            }
            const auto resolved_str = Das::Utils::PathToUtf8(resolved);
            if (resolved_str.size() <= base_str.size()
                || !resolved_str.starts_with(base_str))
            {
                return std::nullopt;
            }
            return resolved;
        }

        void SetSession(Das::ExportInterface::IDasLogSession* session)
        {
            session_ =
                Das::DasPtr<Das::ExportInterface::IDasLogSession>(session);
        }

    private:
        Das::DasPtr<Das::ExportInterface::IDasLogSession> session_;
    };
} // namespace Das::Http

#endif // DAS_HTTP_CONTROLLER_IMAGECONTROLLER_HPP

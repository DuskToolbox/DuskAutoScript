#include "AdbTouch.h"
#include <AutoStarRail/ExportInterface/AsrLogger.h>
#include <AutoStarRail/Utils/QueryInterface.hpp>
#include <AutoStarRail/Utils/StringUtils.h>
#include <AutoStarRail/Utils/fmt.h>

#if defined(_WIN32) || defined(__CYGWIN__)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif // WIN32_LEAN_AND_MEAN

#include <Windows.h>
#endif // WIN32

ASR_DISABLE_WARNING_BEGIN
ASR_IGNORE_UNUSED_PARAMETER

#ifdef _MSC_VER
ASR_PRAGMA(warning(disable : 4189 4245))
#endif // _MSC_VER

#include <boost/asio.hpp>
#include <boost/process/v2.hpp>

ASR_DISABLE_WARNING_END
#include <chrono>

using namespace std::literals;

ASR_NS_BEGIN

ASR_NS_ANONYMOUS_DETAILS_BEGIN

// {ECD62252-7058-4E61-AD29-53D4579812D3}
const AsrGuid ASR_IID_ADB_TOUCH = {
    0xecd62252,
    0x7058,
    0x4e61,
    {0xad, 0x29, 0x53, 0xd4, 0x57, 0x98, 0x12, 0xd3}};

class ProcessExecutor
{
    boost::asio::io_context          ctx_;
    boost::asio::steady_timer        timeout_timer_;
    boost::asio::cancellation_signal sig_;
    std::chrono::milliseconds        timeout_in_ms_;
    std::string                      command_;
    AsrResult                        result_;

public:
    ProcessExecutor(
        std::string_view          cmd,
        std::chrono::milliseconds timeout_in_ms)
        : ctx_{}, timeout_timer_{ctx_, timeout_in_ms}, sig_{},
          timeout_in_ms_{timeout_in_ms}, command_{cmd},
          result_{ASR_E_UNDEFINED_RETURN_VALUE}
    {
        boost::process::v2::async_execute(
            boost::process::v2::process{ctx_, cmd, {}},
            boost::asio::bind_cancellation_slot(
                sig_.slot(),
                [this](boost::system::error_code ec, int exit_code)
                {
                    const auto info =
                        fmt::format("{} return {}.", command_, exit_code);
                    if (ec)
                    {
                        ASR_LOG_ERROR(info.c_str());
                        const auto error_message = fmt::format(
                            "Error happened when executing command {}. Message = {}.",
                            command_,
                            ec.message());
                        ASR_LOG_ERROR(error_message.c_str());
                        if (result_ != ASR_E_TIMEOUT)
                        {
                            result_ = ASR_E_INTERNAL_FATAL_ERROR;
                        }
                        return;
                    }
                    else [[likely]]
                    {
                        ASR_LOG_INFO(info.c_str());
                        result_ = ASR_S_OK;
                    }

                    timeout_timer_.cancel(); // we're done earlier
                }));
        timeout_timer_.async_wait(
            [this](boost::system::error_code ec)
            {
                if (ec) // we were cancelled, do nothing
                {
                    return;
                }
                result_ = ASR_E_TIMEOUT;
                const auto error_message = fmt::format(
                    "Timeout detected when executing command {}.",
                    command_);
                ASR_LOG_ERROR(error_message.c_str());
                sig_.emit(boost::asio::cancellation_type::partial);
                // request exit first, but terminate after another
                // timeout_in_ms_
                timeout_timer_.expires_after(this->timeout_in_ms_);
                timeout_timer_.async_wait(
                    [this](boost::system::error_code timer_ec)
                    {
                        if (!timer_ec)
                        {
                            sig_.emit(boost::asio::cancellation_type::terminal);
                        }
                    });
            });
    }

    AsrResult Run()
    {
        ctx_.run();
        return result_;
    }
};

ASR_NS_ANONYMOUS_DETAILS_END

AdbTouch::AdbTouch(std::string_view adb_path, std::string_view adb_serial)
    : adb_cmd_{fmt::format("{} -s {} ", adb_path, adb_serial)}
{
}

ASR_IMPL AdbTouch::QueryInterface(const AsrGuid& iid, void** pp_out_object)
{
    return Utils::QueryInterface<IAsrTouch>(this, iid, pp_out_object);
}

ASR_IMPL AdbTouch::GetGuid(AsrGuid* p_out_guid)
{
    ASR_UTILS_CHECK_POINTER_FOR_PLUGIN(p_out_guid)

    *p_out_guid = Details::ASR_IID_ADB_TOUCH;
    return ASR_S_OK;
}

ASR_IMPL AdbTouch::GetRuntimeClassName(IAsrReadOnlyString** pp_out_name)
{
    const auto name = ASR_UTILS_STRINGUTILS_DEFINE_U8STR("ASR::AsrAdbTouch");

    return ::CreateIAsrReadOnlyStringFromUtf8(name, pp_out_name);
}

ASR_IMPL AdbTouch::Click(int32_t x, int32_t y)
{
    const auto cmd = fmt::format("{} shell input tap {} {}", adb_cmd_, x, y);
    Details::ProcessExecutor executor{cmd, 5000ms};
    return executor.Run();
}

ASR_IMPL AdbTouch::Swipe(AsrPoint from, AsrPoint to, int32_t duration_ms)
{
    const auto cmd = fmt::format(
        "{} shell input swipe {} {} {} {}",
        adb_cmd_,
        from.x,
        from.y,
        to.x,
        to.y);
    Details::ProcessExecutor executor{
        cmd,
        std::chrono::milliseconds{duration_ms}};
    return executor.Run();
}

ASR_NS_END

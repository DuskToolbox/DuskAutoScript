#include "AdbTouch.h"
#include <DAS/_autogen/idl/abi/DasLogger.h>
#include <das/Core/Logger/Logger.h>
#include <das/Utils/StringUtils.h>
#include <das/Utils/fmt.h>

#if defined(_WIN32) || defined(__CYGWIN__)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif // WIN32_LEAN_AND_MEAN

#include <Windows.h>
#endif // WIN32

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_UNUSED_PARAMETER

#ifdef _MSC_VER
DAS_PRAGMA(warning(disable : 4189 4245))
#endif // _MSC_VER

#include <boost/asio.hpp>
#include <boost/process/v2/execute.hpp>
#include <boost/process/v2/process.hpp>

DAS_DISABLE_WARNING_END
#include <chrono>

using namespace std::literals;

DAS_NS_BEGIN

DAS_NS_ANONYMOUS_DETAILS_BEGIN

// {ECD62252-7058-4E61-AD29-53D4579812D3}
const DasGuid DAS_IID_ADB_TOUCH = {
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
    DasResult                        result_;

public:
    ProcessExecutor(
        std::string_view          cmd,
        std::chrono::milliseconds timeout_in_ms)
        : ctx_{}, timeout_timer_{ctx_, timeout_in_ms}, sig_{},
          timeout_in_ms_{timeout_in_ms}, command_{cmd},
          result_{DAS_E_UNDEFINED_RETURN_VALUE}
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
                        DAS_LOG_ERROR(info.c_str());
                        const auto error_message = fmt::format(
                            "Error happened when executing command {}. Message = {}.",
                            command_,
                            ec.message());
                        DAS_LOG_ERROR(error_message.c_str());
                        if (result_ != DAS_E_TIMEOUT)
                        {
                            result_ = DAS_E_INTERNAL_FATAL_ERROR;
                        }
                        return;
                    }
                    else [[likely]]
                    {
                        DAS_LOG_INFO(info.c_str());
                        result_ = DAS_S_OK;
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
                result_ = DAS_E_TIMEOUT;
                const auto error_message = fmt::format(
                    "Timeout detected when executing command {}.",
                    command_);
                DAS_LOG_ERROR(error_message.c_str());
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

    DasResult Run()
    {
        ctx_.run();
        return result_;
    }
};

DAS_NS_ANONYMOUS_DETAILS_END

AdbTouch::AdbTouch(std::string_view adb_path, std::string_view adb_serial)
    : adb_cmd_{fmt::format("{} -s {} ", adb_path, adb_serial)}
{
}

DasResult AdbTouch::QueryInterface(const DasGuid& iid, void** pp_out_object)
{
    if (pp_out_object == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    // 检查IID_IDasTouch
    if (iid == DasIidOf<PluginInterface::IDasTouch>())
    {
        *pp_out_object = static_cast<PluginInterface::IDasTouch*>(this);
        this->AddRef();
        return DAS_S_OK;
    }

    // 检查IID_IDasTypeInfo
    if (iid == DAS_IID_TYPE_INFO)
    {
        *pp_out_object = static_cast<IDasTypeInfo*>(this);
        this->AddRef();
        return DAS_S_OK;
    }

    // 检查IID_IDasBase
    if (iid == DAS_IID_BASE)
    {
        *pp_out_object = static_cast<IDasBase*>(this);
        this->AddRef();
        return DAS_S_OK;
    }

    *pp_out_object = nullptr;
    return DAS_E_NO_INTERFACE;
}
DAS_IMPL AdbTouch::GetGuid(DasGuid* p_out_guid)
{
    DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_out_guid)

    *p_out_guid = Details::DAS_IID_ADB_TOUCH;
    return DAS_S_OK;
}

DAS_IMPL AdbTouch::GetRuntimeClassName(IDasReadOnlyString** pp_out_name)
{
    const auto name = DAS_UTILS_STRINGUTILS_DEFINE_U8STR("DAS::DasAdbTouch");

    return ::CreateIDasReadOnlyStringFromUtf8(name, pp_out_name);
}

DAS_IMPL AdbTouch::Click(int32_t x, int32_t y)
{
    const auto cmd = fmt::format("{} shell input tap {} {}", adb_cmd_, x, y);
    Details::ProcessExecutor executor{cmd, 5000ms};
    return executor.Run();
}

DAS_IMPL AdbTouch::Swipe(DasPoint from, DasPoint to, int32_t duration_ms)
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

DAS_NS_END

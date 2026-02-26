#include <das/Core/IPC/IpcTimeoutConfig.h>
#include <das/DasApi.h>

#include <das/Core/IPC/Config.h>

namespace
{
    // 线程局部存储的超时值（毫秒）
    // 0 表示无限超时（默认值）
    thread_local uint32_t t_timeout_ms = 0;
} // anonymous namespace

DAS_CORE_IPC_NS_BEGIN
namespace IpcTimeoutConfig
{

    std::chrono::milliseconds FetchAndResetTimeout()
    {
        uint32_t timeout = t_timeout_ms;
        t_timeout_ms = 0; // 重置为无限
        return std::chrono::milliseconds(timeout);
    }

    void SetTimeout(uint32_t timeout_ms) { t_timeout_ms = timeout_ms; }

    uint32_t GetTimeout() { return t_timeout_ms; }

} // namespace IpcTimeoutConfig
DAS_CORE_IPC_NS_END

//=============================================================================
// DAS C API 实现（DAS_C_API 已包含 extern "C"）
//=============================================================================

DasResult DasSetIpcTimeout(uint32_t timeout_ms)
{
    DAS::Core::IPC::IpcTimeoutConfig::SetTimeout(timeout_ms);
    return DAS_S_OK;
}

DasResult DasGetIpcTimeout(uint32_t* p_out_timeout_ms)
{
    if (!p_out_timeout_ms)
    {
        return DAS_E_INVALID_POINTER;
    }
    *p_out_timeout_ms = DAS::Core::IPC::IpcTimeoutConfig::GetTimeout();
    return DAS_S_OK;
}

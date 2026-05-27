/**
 * @file IHostLauncher.h
 * @brief Host 进程启动器接口
 *
 * 负责启动 Host 进程、执行四次握手协议、管理生命周期。
 * 由 IIpcContext::CreateHostLauncher() 创建。
 */

#ifndef DAS_CORE_IPC_MAINPROCESS_IHOST_LAUNCHER_H
#define DAS_CORE_IPC_MAINPROCESS_IHOST_LAUNCHER_H

#include <das/Core/IPC/Config.h>
#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/IDasAsyncHandshakeOperation.h>
#include <das/IDasBase.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

DAS_CORE_IPC_NS_BEGIN

struct HostLaunchDesc
{
    IDasReadOnlyString*        p_executable_path;
    IDasReadOnlyString* const* pp_args;
    size_t                     arg_count;
    IDasReadOnlyString*        p_working_directory;
    IDasReadOnlyString*        p_environment_config;
};

DAS_INTERFACE IHostLauncher : public IDasBase
{
public:
    virtual ~IHostLauncher() = default;

    virtual DasResult StartAsync(
        const std::string&            host_exe_path,
        IDasAsyncHandshakeOperation** pp_out_operation) = 0;

    virtual DasResult Start(
        const std::string& host_exe_path,
        uint16_t&          out_session_id,
        uint32_t           timeout_ms) = 0;

    virtual DasResult StartWithDesc(
        const HostLaunchDesc* p_desc,
        uint32_t              timeout_ms,
        uint16_t*             p_out_session_id) = 0;

    virtual void Stop() = 0;

    [[nodiscard]]
    virtual bool IsRunning() const = 0;

    [[nodiscard]]
    virtual uint32_t GetPid() const = 0;

    [[nodiscard]]
    virtual uint16_t GetSessionId() const = 0;

protected:
    IHostLauncher() = default;
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_MAINPROCESS_IHOST_LAUNCHER_H

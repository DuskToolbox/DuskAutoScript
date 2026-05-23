#include <das/Core/ForeignInterfaceHost/NativeIpcRuntime.h>

#include <das/DasPtr.hpp>
#include <das/DasString.hpp>

#include <cstdint>
#include <string>
#include <tl/expected.hpp>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define DAS_CURRENT_PROCESS_ID() GetCurrentProcessId()
#else
#include <unistd.h>
#define DAS_CURRENT_PROCESS_ID() getpid()
#endif

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

namespace
{
    auto MakeReadOnlyString(const std::string& value)
        -> DAS::Utils::Expected<DAS::DasPtr<IDasReadOnlyString>>
    {
        DAS::DasPtr<IDasReadOnlyString> result;
        const auto create_result =
            CreateIDasReadOnlyStringFromUtf8(value.c_str(), result.Put());
        if (DAS::IsFailed(create_result))
        {
            return tl::make_unexpected(create_result);
        }
        return result;
    }
} // namespace

NativeIpcRuntime::NativeIpcRuntime(
    std::filesystem::path              host_exe_path,
    std::unique_ptr<IRemotePluginHost> remote_plugin_host,
    std::chrono::milliseconds          timeout)
    : host_exe_path_{std::move(host_exe_path)},
      remote_plugin_host_{std::move(remote_plugin_host)}, timeout_{timeout}
{
}

auto NativeIpcRuntime::LoadPlugin(const RuntimeLoadRequest& request)
    -> DAS::Utils::Expected<RuntimeLoadResult>
{
    if (host_exe_path_.empty())
    {
        return tl::make_unexpected(DAS_E_NO_IMPLEMENTATION);
    }
    if (!remote_plugin_host_)
    {
        return tl::make_unexpected(DAS_E_OBJECT_NOT_INIT);
    }

    auto executable = MakeReadOnlyString(host_exe_path_.string());
    if (!executable)
    {
        return tl::make_unexpected(executable.error());
    }

    const auto main_pid =
        static_cast<uint32_t>(DAS_CURRENT_PROCESS_ID());
    auto arg_name = MakeReadOnlyString("--main-pid");
    if (!arg_name)
    {
        return tl::make_unexpected(arg_name.error());
    }
    auto arg_value = MakeReadOnlyString(std::to_string(main_pid));
    if (!arg_value)
    {
        return tl::make_unexpected(arg_value.error());
    }

    std::vector<DAS::DasPtr<IDasReadOnlyString>> arg_storage;
    arg_storage.push_back(std::move(arg_name.value()));
    arg_storage.push_back(std::move(arg_value.value()));

    std::vector<IDasReadOnlyString*> args;
    args.reserve(arg_storage.size());
    for (const auto& arg : arg_storage)
    {
        args.push_back(arg.Get());
    }

    RemotePluginLoadRequest remote_request{};
    remote_request.launch_desc.p_executable_path = executable->Get();
    remote_request.launch_desc.pp_args = args.data();
    remote_request.launch_desc.arg_count = args.size();
    remote_request.manifest_path = request.manifest_path;
    remote_request.plugin_guid = request.plugin_guid;
    remote_request.timeout = timeout_;

    return remote_plugin_host_->LoadPlugin(remote_request);
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

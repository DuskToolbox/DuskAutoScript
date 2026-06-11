#pragma once

#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
#include <das/Utils/Expected.h>

#include <filesystem>
#include <functional>

namespace Das::Core::IPC::Host
{
    struct IIpcContext;

    using HostPluginLoader =
        std::function<DAS::Utils::Expected<DAS::DasPtr<IDasBase>>(
            const std::filesystem::path& manifest_path)>;

    struct HostCommandHandlerOptions
    {
        HostPluginLoader      load_plugin;
        std::filesystem::path plugin_dir; // LIST_FILE/READ_FILE 的工作目录
    };

    DAS_API DasResult RegisterHostCommandHandlers(
        IIpcContext*              ctx,
        HostCommandHandlerOptions options);
} // namespace Das::Core::IPC::Host

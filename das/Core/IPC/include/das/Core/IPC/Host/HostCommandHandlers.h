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

    using HostPluginLoader = std::function<
        DAS::Utils::Expected<DAS::DasPtr<IDasBase>>(
            const std::filesystem::path& manifest_path)>;

    struct HostCommandHandlerOptions
    {
        HostPluginLoader load_plugin;
    };

    DAS_API DasResult RegisterHostCommandHandlers(
        IIpcContext*                ctx,
        HostCommandHandlerOptions   options);
} // namespace Das::Core::IPC::Host

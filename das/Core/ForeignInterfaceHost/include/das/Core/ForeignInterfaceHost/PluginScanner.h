#ifndef DAS_CORE_FOREIGNINTERFACEHOST_PLUGINSCANNER_H
#define DAS_CORE_FOREIGNINTERFACEHOST_PLUGINSCANNER_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/IDasBase.h>

#include <filesystem>
#include <vector>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

std::vector<PluginPackageDesc> ScanPlugins(
    const std::filesystem::path& plugin_dir);

void CleanupMarkedPlugins(const std::filesystem::path& plugin_dir);

DasResult MarkForDeletion(
    const std::filesystem::path& plugin_dir,
    const DasGuid&               guid);

std::filesystem::path FindManifest(
    const std::filesystem::path& plugin_dir_entry);

nlohmann::json PluginPackageDescToJson(const PluginPackageDesc& desc);

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_PLUGINSCANNER_H

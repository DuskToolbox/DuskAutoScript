#ifndef DAS_CORE_FOREIGNINTERFACEHOST_PLUGINSCANNER_H
#define DAS_CORE_FOREIGNINTERFACEHOST_PLUGINSCANNER_H

#include <cassert>
#include <cpp_yyjson.hpp>
#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/DasExport.h>
#include <das/IDasBase.h>

#include <filesystem>
#include <vector>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DAS_EXPORT std::vector<PluginPackageDesc> ScanPlugins(
    const std::filesystem::path& plugin_dir);

DAS_EXPORT void CleanupMarkedPlugins(const std::filesystem::path& plugin_dir);

DAS_EXPORT DasResult
MarkForDeletion(const std::filesystem::path& plugin_dir, const DasGuid& guid);

DAS_EXPORT std::filesystem::path FindManifest(
    const std::filesystem::path& plugin_dir_entry);

DAS_EXPORT yyjson::value PluginPackageDescToJson(const PluginPackageDesc& desc);

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_PLUGINSCANNER_H

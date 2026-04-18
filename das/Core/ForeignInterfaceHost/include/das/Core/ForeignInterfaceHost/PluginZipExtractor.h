#ifndef DAS_CORE_FOREIGNINTERFACEHOST_PLUGINZIPEXTRACTOR_H
#define DAS_CORE_FOREIGNINTERFACEHOST_PLUGINZIPEXTRACTOR_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/DasExport.h>
#include <das/IDasBase.h>

#include <filesystem>
#include <string_view>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DAS_EXPORT DasResult InstallPlugin(
    const std::filesystem::path& plugin_dir,
    std::string_view             zip_data);

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_PLUGINZIPEXTRACTOR_H

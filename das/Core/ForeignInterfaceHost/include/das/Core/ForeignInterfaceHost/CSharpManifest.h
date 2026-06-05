#ifndef DAS_CORE_FOREIGNINTERFACEHOST_CSHARPMANIFEST_H
#define DAS_CORE_FOREIGNINTERFACEHOST_CSHARPMANIFEST_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/DasExport.h>
#include <das/DasTypes.hpp>
#include <das/Utils/Expected.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

enum class CSharpTargetFrameworkFamily
{
    ModernDotNet,
    NetFx
};

struct CSharpEntryPoint
{
    std::string type_name;
    std::string method_name;
};

struct CSharpManifest
{
    std::filesystem::path                manifest_path;
    std::filesystem::path                package_root;
    std::filesystem::path                plugin_binary_path;
    std::string                          name;
    std::string                          plugin_filename_extension;
    std::string                          target_framework;
    CSharpTargetFrameworkFamily          target_framework_family;
    CSharpEntryPoint                     entry_point;
    std::optional<std::filesystem::path> runtime_config_path;
};

DAS_EXPORT Das::Utils::Expected<CSharpTargetFrameworkFamily>
ClassifyCSharpTargetFramework(std::string_view target_framework);

DAS_EXPORT Das::Utils::Expected<CSharpEntryPoint> SplitCSharpEntryPoint(
    std::string_view entry_point);

DAS_EXPORT Das::Utils::Expected<CSharpManifest> ParseCSharpManifest(
    const std::filesystem::path& manifest_path,
    std::string_view             manifest_json);

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_CSHARPMANIFEST_H

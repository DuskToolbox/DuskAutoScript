#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <gtest/gtest.h>

TEST(CppPluginLoadTest, LoadIpcTestPlugin)
{
    using namespace DAS::Core::ForeignInterfaceHost;

    ForeignLanguageRuntimeFactoryDesc desc{};
    desc.language = ForeignInterfaceLanguage::Cpp;

    auto runtime_result = CreateForeignLanguageRuntime(desc);
    ASSERT_TRUE(runtime_result.has_value()) << "Failed to create Cpp runtime";

    auto& runtime = runtime_result.value();

    const auto plugin_path = "C:/vmbuild/bin/Debug/plugins/IpcTestPlugin.dll";
    auto       plugin_result = runtime->LoadPlugin(plugin_path);

    ASSERT_TRUE(plugin_result.has_value())
        << "Failed to load IpcTestPlugin.dll from " << plugin_path;
}

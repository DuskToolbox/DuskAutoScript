#include <IpcTestConfig.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <gtest/gtest.h>

TEST(CppPluginLoadTest, LoadIpcTestPlugin1)
{
    using namespace DAS::Core::ForeignInterfaceHost;

    ForeignLanguageRuntimeFactoryDesc desc{};
    desc.language = ForeignInterfaceLanguage::Cpp;

    auto runtime_result = CreateForeignLanguageRuntime(desc);
    ASSERT_TRUE(runtime_result.has_value()) << "Failed to create Cpp runtime";

    auto& runtime = runtime_result.value();

    std::filesystem::path plugin_path =
        std::filesystem::path{IpcTestConfig::GetPluginDir()}
        / "IpcTestPlugin1.dll";
    auto plugin_result = runtime->LoadPlugin(plugin_path.string());

    ASSERT_TRUE(plugin_result.has_value())
        << "Failed to load IpcTestPlugin1.dll from " << plugin_path;
}

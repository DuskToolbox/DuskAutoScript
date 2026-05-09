#include <IpcTestConfig.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
    std::filesystem::path PluginDir()
    {
        return std::filesystem::path{IpcTestConfig::GetPluginDir()};
    }

    std::filesystem::path IpcTestPlugin1ManifestPath()
    {
        return PluginDir() / "IpcTestPlugin1.json";
    }

    std::filesystem::path IpcTestPlugin1LibPath()
    {
        return PluginDir() / "libIpcTestPlugin1.dll";
    }

    std::filesystem::path IpcTestPlugin1StemDllPath()
    {
        return PluginDir() / "IpcTestPlugin1.dll";
    }

    Das::DasPtr<DAS::Core::ForeignInterfaceHost::IForeignLanguageRuntime>
    CreateCppRuntimeForTest()
    {
        using namespace DAS::Core::ForeignInterfaceHost;

        ForeignLanguageRuntimeFactoryDesc desc{};
        desc.language = ForeignInterfaceLanguage::Cpp;

        auto runtime_result = CreateForeignLanguageRuntime(desc);
        if (!runtime_result.has_value())
        {
            return nullptr;
        }

        return std::move(runtime_result.value());
    }
} // namespace

TEST(CppPluginLoadTest, IpcTestPlugin1ArtifactsUseLibPrefixedBinaryContract)
{
    const auto manifest_path = IpcTestPlugin1ManifestPath();
    const auto lib_path = IpcTestPlugin1LibPath();
    const auto stem_dll_path = IpcTestPlugin1StemDllPath();

    ASSERT_TRUE(std::filesystem::exists(manifest_path));
    EXPECT_TRUE(std::filesystem::exists(lib_path));
    EXPECT_FALSE(std::filesystem::exists(stem_dll_path));

    std::ifstream manifest_file(manifest_path);
    ASSERT_TRUE(manifest_file.is_open());
    const std::string manifest_content(
        (std::istreambuf_iterator<char>(manifest_file)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(
        manifest_content.find(R"("name": "libIpcTestPlugin1")"),
        std::string::npos);
    EXPECT_NE(
        manifest_content.find(R"("pluginFilenameExtension": "dll")"),
        std::string::npos);
}

TEST(CppPluginLoadTest, RejectsDllPath)
{
    using namespace DAS::Core::ForeignInterfaceHost;

    auto runtime = CreateCppRuntimeForTest();
    ASSERT_NE(runtime, nullptr) << "Failed to create Cpp runtime";

    auto plugin_result = runtime->LoadPlugin(IpcTestPlugin1LibPath());

    ASSERT_FALSE(plugin_result.has_value());
    EXPECT_EQ(plugin_result.error(), DAS_E_INVALID_ARGUMENT);
}

TEST(CppPluginLoadTest, LoadIpcTestPlugin1)
{
    auto runtime = CreateCppRuntimeForTest();
    ASSERT_NE(runtime, nullptr) << "Failed to create Cpp runtime";

    const auto manifest_path = IpcTestPlugin1ManifestPath();
    auto       plugin_result = runtime->LoadPlugin(manifest_path);
    ASSERT_TRUE(plugin_result.has_value())
        << "Failed to load IpcTestPlugin1 from manifest " << manifest_path;
}

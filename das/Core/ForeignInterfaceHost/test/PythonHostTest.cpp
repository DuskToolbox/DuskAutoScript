#ifdef DAS_EXPORT_PYTHON

#include <das/Core/ForeignInterfaceHost/PythonHost.h>
#include <gtest/gtest.h>

#include <boost/nowide/quoted.hpp>
#include <das/Utils/StringUtils.h>
#include <filesystem>

#define DAS_VAR(x) #x ":", x

DAS_NS_ANONYMOUS_DETAILS_BEGIN

auto LogAndGetRelativePath(const std::filesystem::path& input)
    -> std::filesystem::path
{
    const auto relative_path = std::filesystem::relative(input);
    std::cout << DAS_VAR(boost::nowide::quoted(relative_path));
    return relative_path;
}

DAS_NS_ANONYMOUS_DETAILS_END

// ============================================================================
// 端到端集成测试
// ============================================================================

// 测试工厂函数创建 - 正常路径
TEST(PythonHostIntegration, CreatePythonRuntime_Success)
{
    using namespace DAS::Core::ForeignInterfaceHost;

    ForeignLanguageRuntimeFactoryDesc desc{};
    desc.language = ForeignInterfaceLanguage::Python;
    desc.p_user_data = nullptr;

    auto result = CreateForeignLanguageRuntime(desc);
    ASSERT_TRUE(result.has_value())
        << "CreateForeignLanguageRuntime should succeed for Python";
    EXPECT_NE(result.value().Get(), nullptr)
        << "Runtime pointer should not be null";
}

// 测试工厂函数语言验证 - 错误的语言
TEST(PythonHostIntegration, CreatePythonRuntime_InvalidLanguage)
{
    using namespace DAS::Core::ForeignInterfaceHost;

    ForeignLanguageRuntimeFactoryDesc desc{};
    desc.language = ForeignInterfaceLanguage::Java; // 错误的语言
    desc.p_user_data = nullptr;

    auto result = CreateForeignLanguageRuntime(desc);
    EXPECT_FALSE(result.has_value())
        << "CreateForeignLanguageRuntime should fail for Java in PythonHost test";
    // 注意：错误码可能是 DAS_E_NO_IMPLEMENTATION 或其他
}

// 端到端测试：加载 Python 插件
TEST(PythonHostIntegration, LoadPlugin_EndToEnd)
{
    using namespace DAS::Core::ForeignInterfaceHost;

    // 1. 创建运行时
    ForeignLanguageRuntimeFactoryDesc desc{};
    desc.language = ForeignInterfaceLanguage::Python;
    desc.p_user_data = nullptr;

    auto runtime_result = CreateForeignLanguageRuntime(desc);
    ASSERT_TRUE(runtime_result.has_value())
        << "Failed to create Python runtime";
    auto runtime = runtime_result.value();
    ASSERT_NE(runtime.Get(), nullptr);

    // 2. 构建插件 manifest 路径
    auto manifest_path = std::filesystem::current_path()
                         / "plugins/PythonTestPlugin/manifest.json";

    if (!std::filesystem::exists(manifest_path))
    {
        GTEST_SKIP() << "Test plugin not found at: " << manifest_path;
    }

    // 3. 加载插件
    auto plugin_result = runtime->LoadPlugin(manifest_path);
    ASSERT_TRUE(plugin_result.has_value()) << "LoadPlugin failed";
    auto plugin = plugin_result.value();
    ASSERT_NE(plugin.Get(), nullptr) << "Plugin pointer should not be null";

    // 4. 验证插件对象有效
    // 注意：详细的接口测试（EnumFeature, CreateFeatureInterface）需要通过
    // QueryInterface 获取 IDasPluginPackage 接口，这里只验证基础加载成功
}

// ============================================================================
// 注释掉的旧测试（保留用于参考）
// ============================================================================

// TEST(PythonHost, PathToPackageNameTest1)
// {
//     const auto current_path = std::filesystem::current_path();
//     const auto path = current_path / "test1.py";
//
//     const auto relative_path = Details::LogAndGetRelativePath(path);
//
//     const auto expected_package_name = DAS::Core::ForeignInterfaceHost::
//         PythonHost::PythonRuntime::ResolveClassName(relative_path);
//
//     EXPECT_TRUE(expected_package_name);
//
//     if (expected_package_name)
//     {
//         const auto package_name = reinterpret_cast<const char*>(
//             expected_package_name.value().c_str());
//         EXPECT_TRUE(
//             std::strcmp(
//                 package_name,
//                 DAS_UTILS_STRINGUTILS_DEFINE_U8STR("test1"))
//             == 0);
//     }
// }

// TEST(PythonHost, PathToPackageNameTest2)
// {
//     auto path = std::filesystem::current_path() / "test";
//     path /= "test2.py";
//
//     const auto expected_package_name =
//         DAS::Core::ForeignInterfaceHost::PythonHost::PythonRuntime::
//             ResolveClassName(Details::LogAndGetRelativePath(path));
//
//     EXPECT_TRUE(expected_package_name);
//
//     if (expected_package_name)
//     {
//         const auto package_name = reinterpret_cast<const char*>(
//             expected_package_name.value().c_str());
//         EXPECT_TRUE(
//             std::strcmp(
//                 package_name,
//                 DAS_UTILS_STRINGUTILS_DEFINE_U8STR("test.test2"))
//             == 0);
//     }
// }

#endif // DAS_EXPORT_PYTHON

#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace DAS::Core::ForeignInterfaceHost;

namespace
{
    std::string ReadSourceFile(const std::filesystem::path& path)
    {
        std::ifstream stream{path};
        std::string   content(
            (std::istreambuf_iterator<char>(stream)),
            std::istreambuf_iterator<char>());
        return content;
    }

    std::filesystem::path RepositoryRoot()
    {
        auto path = std::filesystem::path{__FILE__};
        while (!path.empty())
        {
            if (std::filesystem::exists(
                    path / "das" / "Host" / "src" / "main.cpp"))
            {
                return path;
            }
            path = path.parent_path();
        }
        return {};
    }
} // namespace

TEST(CSharpHostRouting, FactoryCreatesCSharpRuntimeWhenExportEnabled)
{
    ForeignLanguageRuntimeFactoryDesc desc{};
    desc.language = ForeignInterfaceLanguage::CSharp;

    auto runtime = CreateForeignLanguageRuntime(desc);

#ifdef DAS_EXPORT_CSHARP
    ASSERT_TRUE(runtime);
    EXPECT_NE(runtime.value().Get(), nullptr);
#else
    ASSERT_FALSE(runtime);
    EXPECT_EQ(runtime.error(), DAS_E_NO_IMPLEMENTATION);
#endif
}

TEST(CSharpHostRouting, DasHostSourceContainsExplicitCSharpBranch)
{
    const auto source =
        ReadSourceFile(RepositoryRoot() / "das" / "Host" / "src" / "main.cpp");

    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("lang_lower == \"csharp\""), std::string::npos);
    EXPECT_NE(
        source.find("ForeignInterfaceLanguage::CSharp"),
        std::string::npos);
}

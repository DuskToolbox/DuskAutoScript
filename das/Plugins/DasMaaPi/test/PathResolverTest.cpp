#include "../src/PathResolver.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    using namespace Das::Plugins::DasMaaPi;

    // 临时目录 fixture：每个用例独占目录，析构清理，避免用例间文件泄漏。
    class ScopedTempDirectory final
    {
    public:
        ScopedTempDirectory()
            : path_(
                  std::filesystem::temp_directory_path()
                  / ("das_maapi_path_resolver_" + std::to_string(++counter_)))
        {
            std::filesystem::create_directories(path_);
        }

        ~ScopedTempDirectory() { std::filesystem::remove_all(path_); }

        const std::filesystem::path& Get() const { return path_; }

    private:
        std::filesystem::path      path_;
        static inline unsigned int counter_ = 0;
    };

    void TouchFile(const std::filesystem::path& path)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary);
        stream << "";
    }
} // namespace

// 相对路径 + Windows 无扩展名写法（agent/go-service，实际文件
// go-service.exe）：解析结果补 .exe。Windows 进程创建对显式可执行路径
// 不做扩展名补全（boost::process v2 走该路径），必须在此处理。
TEST(PathResolverTest, WindowsExtensionlessExecResolvesToExeWhenPresent)
{
#ifdef _WIN32
    ScopedTempDirectory temp;
    TouchFile(temp.Get() / "agent" / "go-service.exe");

    const auto resolved =
        ResolveChildExec("agent/go-service", temp.Get().generic_string());

    EXPECT_EQ(
        resolved,
        (temp.Get() / "agent" / "go-service.exe").lexically_normal());
#else
    GTEST_SKIP() << "Windows-only extension resolution";
#endif
}

// 绝对路径同样补全（解析分支与相对路径一致）。
TEST(PathResolverTest, WindowsAbsoluteExtensionlessExecResolvesToExe)
{
#ifdef _WIN32
    ScopedTempDirectory temp;
    TouchFile(temp.Get() / "tool.exe");

    const auto resolved =
        ResolveChildExec((temp.Get() / "tool").generic_string(), "C:/unused");

    EXPECT_EQ(resolved, (temp.Get() / "tool.exe").lexically_normal());
#else
    GTEST_SKIP() << "Windows-only extension resolution";
#endif
}

// 显式存在的无扩展名文件（脚本等）不补全。
TEST(PathResolverTest, ExistingExtensionlessFileStaysUnchanged)
{
    ScopedTempDirectory temp;
    TouchFile(temp.Get() / "tools" / "run");

    const auto resolved =
        ResolveChildExec("tools/run", temp.Get().generic_string());

    EXPECT_EQ(resolved, (temp.Get() / "tools" / "run").lexically_normal());
}

// 既无同名文件也无 .exe：保留原解析结果，让 spawn 报真实错误（不静默
// 改写路径掩盖配置问题）。
TEST(PathResolverTest, MissingExecWithoutExeStaysUnchanged)
{
    ScopedTempDirectory temp;

    const auto resolved =
        ResolveChildExec("tools/missing", temp.Get().generic_string());

    EXPECT_EQ(resolved, (temp.Get() / "tools" / "missing").lexically_normal());
}

// 裸命令名（走 PATH）原样返回，不参与扩展名补全。
TEST(PathResolverTest, BareCommandStaysUnchanged)
{
    const auto resolved = ResolveChildExec("python", "C:/maa");
    EXPECT_EQ(resolved, std::filesystem::path("python"));
}

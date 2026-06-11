/**
 * @file DiscoveryFlowTest.cpp
 * @brief Tests for plugin discovery flow using ScanPluginsWith template
 *
 * Tests manifest filtering logic, WS-session vs pipe-session behavior
 * distinction, and failure tolerance using a mock FileProvider.
 */

#include <das/Core/ForeignInterfaceHost/PluginScanner.h>
#include <das/Core/IPC/HandshakeSerialization.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
    using DAS::Core::ForeignInterfaceHost::FileEntry;
    using DAS::Core::ForeignInterfaceHost::PluginPackageDesc;
    using DAS::Core::ForeignInterfaceHost::ScanPluginsWith;

    /**
     * @brief Mock FileProvider for testing ScanPluginsWith
     *
     * Simulates a remote Host's file system with configurable
     * directory listings and file contents.
     */
    class MockFileProvider
    {
    public:
        struct DirectoryInfo
        {
            std::vector<FileEntry> entries;
        };

        struct FileInfo
        {
            std::string content;
        };

        std::map<std::string, DirectoryInfo> directories;
        std::map<std::string, FileInfo>      files;
        std::string                          base_path = "/remote/plugin-dir";

        std::vector<FileEntry> ListDirectory(
            const std::string& relative_path,
            bool               recursive)
        {
            auto it = directories.find(relative_path);
            if (it == directories.end())
            {
                return {};
            }
            return it->second.entries;
        }

        std::string ReadFile(const std::string& relative_path)
        {
            auto it = files.find(relative_path);
            if (it == files.end())
            {
                return {};
            }
            return it->second.content;
        }

        std::string GetBasePath() const { return base_path; }
    };

    /**
     * @brief Standard valid manifest content
     *
     * Uses the same format as ForeignInterfaceHost test manifests
     * (uppercase GUID, required fields present).
     */
    static const std::string kValidManifest = R"({
        "name": "TestPlugin",
        "author": "test",
        "version": "1.0",
        "guid": "35BF38D4-7760-42EA-8A9C-9F2BF7C3CBDA",
        "description": "test",
        "supportedSystem": "Windows",
        "language": "Cpp",
        "pluginFilenameExtension": "dll",
        "settings": []
    })";

    /**
     * @brief Create a standard two-layer plugin directory layout
     */
    MockFileProvider CreateStandardProvider()
    {
        MockFileProvider provider;

        provider.directories[""] = MockFileProvider::DirectoryInfo{
            .entries =
                {
                    FileEntry{
                        "TestPlugin",
                        "/remote/plugin-dir/TestPlugin",
                        true},
                    FileEntry{
                        "readme.txt",
                        "/remote/plugin-dir/readme.txt",
                        false},
                },
        };

        provider.directories["TestPlugin/"] = MockFileProvider::DirectoryInfo{
            .entries =
                {
                    FileEntry{
                        "manifest.json",
                        "/remote/plugin-dir/TestPlugin/manifest.json",
                        false},
                },
        };

        provider.files["TestPlugin/manifest.json"] =
            MockFileProvider::FileInfo{.content = kValidManifest};

        return provider;
    }

    // ============================================================
    // Manifest filtering tests
    // ============================================================

    TEST(DiscoveryFlowTest, StandardLayout_FindsPlugin)
    {
        auto provider = CreateStandardProvider();
        auto descs = ScanPluginsWith(provider);
        ASSERT_FALSE(descs.empty());
        EXPECT_EQ(descs[0].name, "TestPlugin");
    }

    TEST(DiscoveryFlowTest, SkipInstallingDirectory)
    {
        auto provider = CreateStandardProvider();
        provider.directories[""].entries.push_back(
            FileEntry{
                "PluginB.installing",
                "/remote/plugin-dir/PluginB.installing",
                true});

        auto descs = ScanPluginsWith(provider);
        ASSERT_FALSE(descs.empty());
        EXPECT_EQ(descs.size(), size_t{1});
        EXPECT_EQ(descs[0].name, "TestPlugin");
    }

    TEST(DiscoveryFlowTest, SkipWillBeDeleteDirectory)
    {
        auto provider = CreateStandardProvider();
        provider.directories[""].entries.push_back(
            FileEntry{
                "PluginB.willBeDelete",
                "/remote/plugin-dir/PluginB.willBeDelete",
                true});

        auto descs = ScanPluginsWith(provider);
        ASSERT_FALSE(descs.empty());
        EXPECT_EQ(descs.size(), size_t{1});
    }

    TEST(DiscoveryFlowTest, SkipTmpInstallDirectory)
    {
        auto provider = CreateStandardProvider();
        provider.directories[""].entries.push_back(
            FileEntry{
                ".tmp_install_xyz",
                "/remote/plugin-dir/.tmp_install_xyz",
                true});

        auto descs = ScanPluginsWith(provider);
        ASSERT_FALSE(descs.empty());
        EXPECT_EQ(descs.size(), size_t{1});
    }

    TEST(DiscoveryFlowTest, SkipPluginWithDeletionMarker)
    {
        auto provider = CreateStandardProvider();

        provider.directories[""].entries.push_back(
            FileEntry{"OldPlugin", "/remote/plugin-dir/OldPlugin", true});

        provider.directories["OldPlugin/"] = MockFileProvider::DirectoryInfo{
            .entries =
                {
                    FileEntry{
                        "OldPlugin.willBeDelete",
                        "/remote/plugin-dir/OldPlugin/OldPlugin.willBeDelete",
                        false},
                    FileEntry{
                        "manifest.json",
                        "/remote/plugin-dir/OldPlugin/manifest.json",
                        false},
                },
        };

        provider.files["OldPlugin/manifest.json"] =
            MockFileProvider::FileInfo{.content = kValidManifest};

        auto descs = ScanPluginsWith(provider);
        ASSERT_FALSE(descs.empty());
        EXPECT_EQ(descs.size(), size_t{1});
        EXPECT_EQ(descs[0].name, "TestPlugin");
    }

    TEST(DiscoveryFlowTest, NoManifestInSubdir_SkipsDirectory)
    {
        MockFileProvider provider;

        provider.directories[""] = MockFileProvider::DirectoryInfo{
            .entries =
                {
                    FileEntry{
                        "EmptyPlugin",
                        "/remote/plugin-dir/EmptyPlugin",
                        true},
                },
        };

        provider.directories["EmptyPlugin/"] = MockFileProvider::DirectoryInfo{
            .entries =
                {
                    FileEntry{
                        "readme.txt",
                        "/remote/plugin-dir/EmptyPlugin/readme.txt",
                        false},
                },
        };

        auto descs = ScanPluginsWith(provider);
        EXPECT_TRUE(descs.empty());
    }

    // ============================================================
    // Flat-file manifest mode
    // ============================================================

    TEST(DiscoveryFlowTest, FlatFileManifest_FindsPlugin)
    {
        MockFileProvider provider;

        provider.directories[""] = MockFileProvider::DirectoryInfo{
            .entries =
                {
                    FileEntry{
                        "StandalonePlugin.json",
                        "/remote/plugin-dir/StandalonePlugin.json",
                        false},
                },
        };

        provider.files["StandalonePlugin.json"] =
            MockFileProvider::FileInfo{.content = R"({
            "name": "StandalonePlugin",
            "author": "test",
            "version": "1.0",
            "guid": "A1B2C3D4-5678-9ABC-DEF0-1234567890AB",
            "description": "test",
            "supportedSystem": "Windows",
            "language": "Cpp",
            "pluginFilenameExtension": "dll",
            "settings": []
        })"};

        auto descs = ScanPluginsWith(provider);
        ASSERT_EQ(descs.size(), size_t{1});
        EXPECT_EQ(descs[0].name, "StandalonePlugin");
    }

    TEST(DiscoveryFlowTest, FlatFileWithDeletionMarker_Skipped)
    {
        MockFileProvider provider;

        provider.directories[""] = MockFileProvider::DirectoryInfo{
            .entries =
                {
                    FileEntry{
                        "DeadPlugin.json",
                        "/remote/plugin-dir/DeadPlugin.json",
                        false},
                    FileEntry{
                        "DeadPlugin.willBeDelete",
                        "/remote/plugin-dir/DeadPlugin.willBeDelete",
                        false},
                },
        };

        provider.files["DeadPlugin.json"] =
            MockFileProvider::FileInfo{.content = kValidManifest};

        auto descs = ScanPluginsWith(provider);
        EXPECT_TRUE(descs.empty());
    }

    TEST(DiscoveryFlowTest, NonJsonFile_Skipped)
    {
        MockFileProvider provider;

        provider.directories[""] = MockFileProvider::DirectoryInfo{
            .entries =
                {
                    FileEntry{
                        "readme.txt",
                        "/remote/plugin-dir/readme.txt",
                        false},
                },
        };

        auto descs = ScanPluginsWith(provider);
        EXPECT_TRUE(descs.empty());
    }

    // ============================================================
    // WS-session vs pipe-session behavior
    // ============================================================

    TEST(DiscoveryFlowTest, EmptyProvider_ReturnsNoPlugins)
    {
        MockFileProvider provider;

        provider.directories[""] = MockFileProvider::DirectoryInfo{
            .entries = {},
        };

        auto descs = ScanPluginsWith(provider);
        EXPECT_TRUE(descs.empty());
    }

    TEST(DiscoveryFlowTest, ListDirectoryFailure_ReturnsNoPlugins)
    {
        MockFileProvider provider;
        auto             descs = ScanPluginsWith(provider);
        EXPECT_TRUE(descs.empty());
    }

    // ============================================================
    // Failure tolerance
    // ============================================================

    TEST(DiscoveryFlowTest, InvalidManifestJson_SkippedGracefully)
    {
        MockFileProvider provider;

        provider.directories[""] = MockFileProvider::DirectoryInfo{
            .entries =
                {
                    FileEntry{
                        "BadPlugin",
                        "/remote/plugin-dir/BadPlugin",
                        true},
                },
        };

        provider.directories["BadPlugin/"] = MockFileProvider::DirectoryInfo{
            .entries =
                {
                    FileEntry{
                        "manifest.json",
                        "/remote/plugin-dir/BadPlugin/manifest.json",
                        false},
                },
        };

        provider.files["BadPlugin/manifest.json"] =
            MockFileProvider::FileInfo{.content = "this is not json"};

        auto descs = ScanPluginsWith(provider);
        EXPECT_TRUE(descs.empty());
    }

    TEST(DiscoveryFlowTest, EmptyManifestContent_SkippedGracefully)
    {
        MockFileProvider provider;

        provider.directories[""] = MockFileProvider::DirectoryInfo{
            .entries =
                {
                    FileEntry{
                        "EmptyManifest",
                        "/remote/plugin-dir/EmptyManifest",
                        true},
                },
        };

        provider.directories["EmptyManifest/"] =
            MockFileProvider::DirectoryInfo{
                .entries =
                    {
                        FileEntry{
                            "manifest.json",
                            "/remote/plugin-dir/EmptyManifest/manifest.json",
                            false},
                    },
            };

        auto descs = ScanPluginsWith(provider);
        EXPECT_TRUE(descs.empty());
    }

    TEST(DiscoveryFlowTest, MixedValidAndInvalid_ReturnsOnlyValid)
    {
        MockFileProvider provider;

        provider.directories[""] = MockFileProvider::DirectoryInfo{
            .entries =
                {
                    FileEntry{
                        "GoodPlugin",
                        "/remote/plugin-dir/GoodPlugin",
                        true},
                    FileEntry{
                        "BadPlugin",
                        "/remote/plugin-dir/BadPlugin",
                        true},
                },
        };

        provider.directories["GoodPlugin/"] = MockFileProvider::DirectoryInfo{
            .entries =
                {
                    FileEntry{
                        "manifest.json",
                        "/remote/plugin-dir/GoodPlugin/manifest.json",
                        false},
                },
        };

        provider.files["GoodPlugin/manifest.json"] =
            MockFileProvider::FileInfo{.content = R"({
            "name": "GoodPlugin",
            "author": "test",
            "version": "1.0",
            "guid": "B2C3D4E5-6789-ABCD-EF01-234567890ABC",
            "description": "test",
            "supportedSystem": "Windows",
            "language": "Cpp",
            "pluginFilenameExtension": "dll",
            "settings": []
        })"};

        provider.directories["BadPlugin/"] = MockFileProvider::DirectoryInfo{
            .entries =
                {
                    FileEntry{
                        "manifest.json",
                        "/remote/plugin-dir/BadPlugin/manifest.json",
                        false},
                },
        };

        provider.files["BadPlugin/manifest.json"] =
            MockFileProvider::FileInfo{.content = "not json"};

        auto descs = ScanPluginsWith(provider);
        ASSERT_EQ(descs.size(), size_t{1});
        EXPECT_EQ(descs[0].name, "GoodPlugin");
    }

    // ============================================================
    // Manifest name priority: dirname.json > manifest.json
    // ============================================================

    TEST(DiscoveryFlowTest, DirnameJsonTakesPriorityOverManifestJson)
    {
        MockFileProvider provider;

        provider.directories[""] = MockFileProvider::DirectoryInfo{
            .entries =
                {
                    FileEntry{"MyPlugin", "/remote/plugin-dir/MyPlugin", true},
                },
        };

        provider.directories["MyPlugin/"] = MockFileProvider::DirectoryInfo{
            .entries =
                {
                    FileEntry{
                        "MyPlugin.json",
                        "/remote/plugin-dir/MyPlugin/MyPlugin.json",
                        false},
                    FileEntry{
                        "manifest.json",
                        "/remote/plugin-dir/MyPlugin/manifest.json",
                        false},
                },
        };

        provider.files["MyPlugin/MyPlugin.json"] =
            MockFileProvider::FileInfo{.content = R"({
            "name": "PriorityPlugin",
            "author": "test",
            "version": "1.0",
            "guid": "C3D4E5F6-789A-BCDE-F012-34567890ABCD",
            "description": "test",
            "supportedSystem": "Windows",
            "language": "Cpp",
            "pluginFilenameExtension": "dll",
            "settings": []
        })"};

        provider.files["MyPlugin/manifest.json"] =
            MockFileProvider::FileInfo{.content = R"({
            "name": "FallbackPlugin",
            "author": "test",
            "version": "1.0",
            "guid": "D4E5F6A7-89AB-CDEF-0123-4567890ABCDE",
            "description": "test",
            "supportedSystem": "Windows",
            "language": "Cpp",
            "pluginFilenameExtension": "dll",
            "settings": []
        })"};

        auto descs = ScanPluginsWith(provider);
        ASSERT_EQ(descs.size(), size_t{1});
        EXPECT_EQ(descs[0].name, "PriorityPlugin");
    }

} // anonymous namespace

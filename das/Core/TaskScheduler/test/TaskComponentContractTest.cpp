#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/GraphRuntime/GraphRuntime.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/Core/Utils/DasJsonImpl.h>
#include <das/DasString.hpp>
#include <das/Utils/BoostPathBridge.h>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/PathText.h>
#include <das/Utils/TaskComponentPorts.h>
#include <das/_autogen/idl/abi/IDasComponent.h>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponentCreator.Implements.hpp>
#include <gtest/gtest.h>

#include <boost/dll/runtime_symbol_info.hpp>

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_BOOST_PROCESS_WARNING

#include <boost/process/v2/environment.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

DAS_DISABLE_WARNING_END

#ifdef _WIN32
#include <windows.h>
#endif

#include "FlowControlErrorCodes.h"
#include "FlowControlI18n.h"
#include "FlowControlTaskComponents.h"
#include "LaunchOutputCapture.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <thread>

namespace
{
    using Das::DasPtr;
    using namespace Das::Plugins::DasFlowControl;
    using Das::Core::ForeignInterfaceHost::PluginManager;
    using Das::PluginInterface::IDasTaskComponent;

    struct OfficialFlowComponent
    {
        std::string_view name;
        std::string_view kind;
        std::string_view guid_text;
    };

    constexpr std::array<OfficialFlowComponent, 15> OFFICIAL_COMPONENTS{
        OfficialFlowComponent{
            "branch",
            "das.flow.branch",
            "68F10001-0000-4000-8000-000000000001"},
        OfficialFlowComponent{
            "delay",
            "das.flow.delay",
            "68F10003-0000-4000-8000-000000000003"},
        OfficialFlowComponent{
            "for",
            "das.flow.for",
            "68F10004-0000-4000-8000-000000000004"},
        OfficialFlowComponent{
            "while",
            "das.flow.while",
            "68F10005-0000-4000-8000-000000000005"},
        OfficialFlowComponent{
            "merge",
            "das.flow.merge",
            "68F10008-0000-4000-8000-000000000008"},
        OfficialFlowComponent{
            "constantBool",
            "das.flow.constant_bool",
            "68F10009-0000-4000-8000-000000000009"},
        OfficialFlowComponent{
            "constantInt64",
            "das.flow.constant_int64",
            "68F1000A-0000-4000-8000-00000000000A"},
        OfficialFlowComponent{
            "constantDouble",
            "das.flow.constant_double",
            "68F1000B-0000-4000-8000-00000000000B"},
        OfficialFlowComponent{
            "constantString",
            "das.flow.constant_string",
            "68F1000C-0000-4000-8000-00000000000C"},
        OfficialFlowComponent{
            "compareDouble",
            "das.flow.compare_double",
            "68F1000E-0000-4000-8000-00000000000E"},
        OfficialFlowComponent{
            "arithmeticNumber",
            "das.flow.arithmetic_number",
            "68F10010-0000-4000-8000-000000000010"},
        OfficialFlowComponent{
            "concatString",
            "das.flow.concat_string",
            "68F10011-0000-4000-8000-000000000011"},
        OfficialFlowComponent{
            "compareInt64",
            "das.flow.compare_int64",
            "68F1000D-0000-4000-8000-00000000000D"},
        OfficialFlowComponent{
            "compareString",
            "das.flow.compare_string",
            "68F1000F-0000-4000-8000-00000000000F"},
        OfficialFlowComponent{
            "launch",
            "das.flow.launch",
            "95FD2FFA-88F2-4F85-A536-C708D25B641F"},
    };

    constexpr std::string_view DELAY_COMPONENT_GUID =
        "68F10003-0000-4000-8000-000000000003";
    constexpr std::string_view FOR_COMPONENT_GUID =
        "68F10004-0000-4000-8000-000000000004";
    constexpr std::string_view WHILE_COMPONENT_GUID =
        "68F10005-0000-4000-8000-000000000005";
    constexpr std::string_view MERGE_COMPONENT_GUID =
        "68F10008-0000-4000-8000-000000000008";
    constexpr std::string_view LAUNCH_COMPONENT_GUID =
        "95FD2FFA-88F2-4F85-A536-C708D25B641F";
    constexpr std::string_view LAUNCH_KIND_TEXT = "das.flow.launch";

    std::filesystem::path UniqueSettingsDir()
    {
        static std::atomic<int> counter{0};
        std::random_device      rd;
        return std::filesystem::current_path()
               / ("task_component_contract_settings_" + std::to_string(rd())
                  + "_" + std::to_string(counter.fetch_add(1)));
    }

    DasGuid GuidFromText(std::string_view guid_text)
    {
        return Das::Core::ForeignInterfaceHost::MakeDasGuid(
            std::string{guid_text});
    }

    DasPtr<Das::Core::ForeignInterfaceHost::IForeignLanguageRuntime>
    CreateCppRuntime()
    {
        Das::Core::ForeignInterfaceHost::ForeignLanguageRuntimeFactoryDesc
            desc{};
        desc.language =
            Das::Core::ForeignInterfaceHost::ForeignInterfaceLanguage::Cpp;
        auto result =
            Das::Core::ForeignInterfaceHost::CreateForeignLanguageRuntime(desc);
        if (!result)
        {
            return nullptr;
        }
        return std::move(result.value());
    }

    std::filesystem::path FindDasFlowControlManifest()
    {
        if (const auto plugin_dir =
                Das::Utils::PathFromEnvironment("DAS_PLUGIN_DIR");
            plugin_dir && !plugin_dir->empty())
        {
            return *plugin_dir / DAS_PATH_LITERAL("DasFlowControl")
                   / DAS_PATH_LITERAL("DasFlowControl.json");
        }

        const auto cwd = std::filesystem::current_path();
        const std::array<std::filesystem::path, 3> candidates{
            cwd / DAS_PATH_LITERAL("plugins")
                / DAS_PATH_LITERAL("DasFlowControl")
                / DAS_PATH_LITERAL("DasFlowControl.json"),
            cwd.parent_path() / DAS_PATH_LITERAL("bin")
                / DAS_PATH_LITERAL("Debug") / DAS_PATH_LITERAL("plugins")
                / DAS_PATH_LITERAL("DasFlowControl")
                / DAS_PATH_LITERAL("DasFlowControl.json"),
            cwd / DAS_PATH_LITERAL("build") / DAS_PATH_LITERAL("mingw-debug")
                / DAS_PATH_LITERAL("bin") / DAS_PATH_LITERAL("Debug")
                / DAS_PATH_LITERAL("plugins")
                / DAS_PATH_LITERAL("DasFlowControl")
                / DAS_PATH_LITERAL("DasFlowControl.json"),
        };
        const auto found = std::ranges::find_if(
            candidates,
            [](const std::filesystem::path& candidate)
            { return std::filesystem::exists(candidate); });
        return found != candidates.end()
                   ? *found
                   : std::filesystem::path{
                         DAS_PATH_LITERAL("DasFlowControl.json")};
    }

    std::filesystem::path SourceRoot()
    {
        auto path = std::filesystem::path{DAS_PATH_LITERAL(__FILE__)};
        for (int i = 0; i < 5; ++i)
        {
            path = path.parent_path();
        }
        return path;
    }

    std::string ReadFileText(const std::filesystem::path& path)
    {
        const std::ifstream input(path);
        EXPECT_TRUE(input.is_open()) << Das::Utils::PathToUtf8(path);
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
    }

    yyjson::value ToJson(Das::ExportInterface::IDasJson* json)
    {
        DasPtr<IDasReadOnlyString> text;
        EXPECT_EQ(json->ToString(0, text.Put()), DAS_S_OK);
        const char* u8 = nullptr;
        EXPECT_EQ(text->GetUtf8(&u8), DAS_S_OK);
        auto parsed =
            Das::Utils::ParseYyjsonFromString(u8 != nullptr ? u8 : "");
        EXPECT_TRUE(parsed.has_value());
        return parsed ? std::move(*parsed) : yyjson::value{};
    }

    std::string ToJsonText(Das::ExportInterface::IDasJson* json)
    {
        if (json == nullptr)
        {
            return {};
        }

        DasPtr<IDasReadOnlyString> text;
        if (DAS::IsFailed(json->ToString(0, text.Put())) || !text)
        {
            return {};
        }

        const char* u8 = nullptr;
        if (DAS::IsFailed(text->GetUtf8(&u8)) || u8 == nullptr)
        {
            return {};
        }
        return std::string{u8};
    }

    DasPtr<Das::ExportInterface::IDasJson> Wrap(yyjson::value value)
    {
        return Das::MakeDasPtr<Das::Core::Utils::IDasJsonImpl>(
            std::move(value));
    }

    template <typename ObjectRef>
    std::string_view StringField(const ObjectRef& object, std::string_view key)
    {
        return object[key].as_string().value_or("");
    }

    yyjson::value ParseJson(std::string_view json)
    {
        auto parsed = Das::Utils::ParseYyjsonFromString(json);
        EXPECT_TRUE(parsed.has_value());
        return parsed ? std::move(*parsed) : yyjson::value{};
    }

    // Read the single signal port id this component emitted (or "" for none).
    // Port 读写一律经公共头（das/Utils/TaskComponentPorts.h）——本测试文件
    // 不再持有第四份 PortMap/Json 桥接拷贝。
    std::string FirstEmittedSignal(
        Das::ExportInterface::IDasReadOnlyPortMap* map)
    {
        auto j = Das::Utils::GetPortJson(map, "signals");
        if (!j || !j->is_array() || j->as_array()->empty())
        {
            return {};
        }
        return std::string((*j->as_array())[0].as_string().value_or(""));
    }

    // 直构 launch 组件具体类（测试 exe 静态编入源码；进程/窗口注入缝在
    // 具体类上，经工厂拿到的 DLL 内实例无法注入）。
    DasPtr<Das::DasFlowControlTaskComponent> MakeLaunchComponent()
    {
        return DasPtr<Das::DasFlowControlTaskComponent>{
            new Das::DasFlowControlTaskComponent(LAUNCH_KIND_TEXT)};
    }

    // 经 GraphRuntime 的 {"settings": S, "payload": null} 包装形投递节点
    // 配置（与生产 ApplyNodeSettings 投递形状一致）。
    void ApplyLaunchConfig(
        Das::DasFlowControlTaskComponent* component,
        std::string_view                  settings_json)
    {
        auto change = ParseJson(
            std::string{R"({"settings": )"} + std::string{settings_json}
            + R"(, "payload": null})");
        DasPtr<Das::ExportInterface::IDasJson> result;
        ASSERT_EQ(
            component->ApplySettingsChange(
                Wrap(std::move(change)).Get(),
                result.Put()),
            DAS_S_OK);
    }

    // Read one string field ("code"/"message") from the structured
    // "diagnostic" port (JSON object) of a failed component result.
    std::string DiagnosticField(
        Das::ExportInterface::IDasReadOnlyPortMap* map,
        std::string_view                           field)
    {
        auto diagnostic = Das::Utils::GetPortJson(map, "diagnostic");
        if (!diagnostic || !diagnostic->is_object())
        {
            return {};
        }
        auto obj = diagnostic->as_object();
        if (!obj)
        {
            return {};
        }
        return std::string((*obj)[field].as_string().value_or(""));
    }

    // 构造可编程填充的 i18n 替身表，同一 key 同时登记 en 与 zh-cn（Translate
    // 默认读进程级 GlobalLocale，缺失回退 en——两处都写保证任意默认 locale
    // 下可命中）。
    DasPtr<Das::ExportInterface::IDasMutableI18n> MakeI18nWithTranslation(
        std::string_view key,
        std::string_view text)
    {
        DasPtr<Das::ExportInterface::IDasMutableI18n> i18n;
        if (DAS::IsFailed(CreateIDasMutableI18n(i18n.Put())) || !i18n)
        {
            return {};
        }
        for (const std::string_view locale : {"en", "zh-cn"})
        {
            DasPtr<IDasReadOnlyString> locale_str;
            DasPtr<IDasReadOnlyString> key_str;
            DasPtr<IDasReadOnlyString> text_str;
            if (DAS::IsFailed(CreateIDasReadOnlyStringFromUtf8(
                    std::string{locale}.c_str(),
                    locale_str.Put()))
                || DAS::IsFailed(CreateIDasReadOnlyStringFromUtf8(
                    std::string{key}.c_str(),
                    key_str.Put()))
                || DAS::IsFailed(CreateIDasReadOnlyStringFromUtf8(
                    std::string{text}.c_str(),
                    text_str.Put())))
            {
                return {};
            }
            if (DAS::IsFailed(i18n->AddTranslation(
                    locale_str.Get(),
                    key_str.Get(),
                    text_str.Get())))
            {
                return {};
            }
        }
        return i18n;
    }

    // i18n 替身表作用域守卫：测试结束（含断言早退）恢复默认装载路径，
    // 避免跨用例污染。
    struct ScopedFlowControlI18nOverride
    {
        explicit ScopedFlowControlI18nOverride(
            Das::ExportInterface::IDasI18n* i18n)
        {
            Das::Plugins::DasFlowControl::SetFlowControlI18nForTest(i18n);
        }
        ~ScopedFlowControlI18nOverride()
        {
            Das::Plugins::DasFlowControl::SetFlowControlI18nForTest(nullptr);
        }
    };

    // Escape a filesystem path for embedding in a JSON string literal
    // (backslash doubling is sufficient for Windows paths).
    std::string JsonEscapePath(std::string_view text)
    {
        std::string escaped;
        for (const char character : text)
        {
            if (character == '\\' || character == '"')
            {
                escaped.push_back('\\');
            }
            escaped.push_back(character);
        }
        return escaped;
    }

    // 捕获日志目录缝作用域守卫：断言早退也恢复生产默认，避免跨用例污染。
    struct ScopedLaunchCaptureDirectoryOverride
    {
        explicit ScopedLaunchCaptureDirectoryOverride(
            const std::filesystem::path& directory)
        {
            Das::SetLaunchCaptureLogDirectoryForTest(
                Das::Utils::PathToUtf8(directory).c_str());
        }

        ~ScopedLaunchCaptureDirectoryOverride()
        {
            Das::SetLaunchCaptureLogDirectoryForTest(nullptr);
        }
    };

    // 整文件读回（二进制零转换）——捕获落盘断言用。
    std::string ReadFileTextBinary(const std::filesystem::path& file_path)
    {
        std::ifstream      input(file_path, std::ios::binary);
        std::ostringstream content;
        content << input.rdbuf();
        return content.str();
    }

    // 文本中存在与 line 完全相等的原始行（按 '\n' 拆行比对）——比 contains
    // 更强：带任何前缀（时间戳/流标签）的行不会命中。
    bool HasExactRawLine(const std::string& text, std::string_view line)
    {
        std::size_t start = 0;
        while (start <= text.size())
        {
            const auto newline = text.find('\n', start);
            const auto end =
                newline == std::string::npos ? text.size() : newline;
            if (std::string_view(text.data() + start, end - start) == line)
            {
                return true;
            }
            if (newline == std::string::npos)
            {
                break;
            }
            start = newline + 1;
        }
        return false;
    }

#ifdef _WIN32
    // spawn 的 exe 形参需要可定位文件——bare 名不做 PATH 搜索（进程启动
    // 走 lpApplicationName 语义），bp2 的 find_executable 走 PATH/PATHEXT
    // 找全路径 python。找不到返回空（测试侧 ASSERT 拦截）。
    std::string ResolvePythonExecutablePath()
    {
        const auto executable_path =
            boost::process::environment::find_executable(
                DAS_PATH_LITERAL("python"));
        if (executable_path.empty())
        {
            return {};
        }
        return Das::Utils::PathToUtf8(Das::Utils::ToStdPath(executable_path));
    }
#endif

    class FakeStopToken final : public Das::PluginInterface::IDasStopToken
    {
    public:
        explicit FakeStopToken(bool requested = false) : requested_(requested)
        {
        }

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            const auto count = --ref_count_;
            if (count == 0)
            {
                delete this;
            }
            return count;
        }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out) override
        {
            if (pp_out == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DasIidOf<IDasBase>())
            {
                *pp_out = static_cast<IDasBase*>(this);
                AddRef();
                return DAS_S_OK;
            }
            if (iid == DasIidOf<Das::PluginInterface::IDasStopToken>())
            {
                *pp_out =
                    static_cast<Das::PluginInterface::IDasStopToken*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL StopRequested(bool* p_requested) override
        {
            if (p_requested == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *p_requested = requested_.load(std::memory_order_acquire);
            return DAS_S_OK;
        }

        // Thread-safe: a watchdog thread may flip stop mid-Do() (delay cancel).
        void RequestStop()
        {
            requested_.store(true, std::memory_order_release);
        }

    private:
        std::atomic<bool>     requested_{false};
        std::atomic<uint32_t> ref_count_{0};
    };

    constexpr std::string_view DELAY_KIND_TEXT = "das.flow.delay";

    // GraphRuntime 直驱用 host：创建测试 exe 内静态编入的真实 FlowControl
    // 组件（与 DLL 内副本同源码；直驱用例需要真实组件行为，不经工厂注册表）。
    class StaticFlowControlComponentHost final
        : public Das::PluginInterface::DasTaskComponentCreatorImplBase<
              StaticFlowControlComponentHost>
    {
    public:
        DasResult CreateTaskComponent(
            const DasGuid&                            component_guid,
            Das::PluginInterface::IDasTaskComponent** pp_out_component) override
        {
            if (pp_out_component == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_component = nullptr;
            if (component_guid != GuidFromText(DELAY_COMPONENT_GUID))
            {
                return DAS_E_NOT_FOUND;
            }

            auto* component =
                new Das::DasFlowControlTaskComponent(DELAY_KIND_TEXT);
            component->AddRef();
            *pp_out_component = component;
            return DAS_S_OK;
        }

        DasResult CreateAuthoringSession(
            const DasGuid&                                   task_guid,
            Das::ExportInterface::IDasJson*                  p_context_json,
            Das::PluginInterface::IDasTaskAuthoringSession** pp_out_session)
            override
        {
            if (pp_out_session == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_session = nullptr;
            return DAS_E_NO_IMPLEMENTATION;
        }
    };

    class TaskComponentContractTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            settings_dir_ = UniqueSettingsDir();
            settings_manager_ =
                std::make_unique<Das::Core::SettingsManager::SettingsManager>(
                    settings_dir_);
            ipc_sp_ = DAS::Core::IPC::MainProcess::CreateIpcContext(false);
            plugin_manager_ = std::make_unique<PluginManager>(
                *settings_manager_,
                Das::DasRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                    ipc_sp_.Get()));

            ASSERT_EQ(plugin_manager_->Initialize(1), DAS_S_OK);

            manifest_path_ = FindDasFlowControlManifest();
            ASSERT_TRUE(std::filesystem::exists(manifest_path_))
                << "DasFlowControl manifest not found at "
                << Das::Utils::PathToUtf8(manifest_path_);
            ASSERT_EQ(plugin_manager_->LoadPlugin(manifest_path_), DAS_S_OK);
        }

        void TearDown() override
        {
            if (plugin_manager_)
            {
                plugin_manager_->Shutdown();
            }
            plugin_manager_.reset();
            settings_manager_.reset();
            std::filesystem::remove_all(settings_dir_);
        }

        DasPtr<IDasTaskComponent> CreateComponent(std::string_view guid_text)
        {
            DasPtr<IDasTaskComponent> component;
            EXPECT_EQ(
                plugin_manager_->GetTaskComponentFactoryManager()
                    .CreateComponent(GuidFromText(guid_text), component.Put()),
                DAS_S_OK);
            EXPECT_NE(component.Get(), nullptr);
            return component;
        }

        std::filesystem::path settings_dir_;
        std::filesystem::path manifest_path_;
        std::unique_ptr<Das::Core::SettingsManager::SettingsManager>
                                                              settings_manager_;
        DAS::DasPtr<DAS::Core::IPC::MainProcess::IIpcContext> ipc_sp_;
        std::unique_ptr<PluginManager>                        plugin_manager_;
    };

} // namespace

TEST_F(
    TaskComponentContractTest,
    DasFlowControlManifestDefinitionsComeFromManager)
{
    auto definitions = plugin_manager_->GetTaskComponentFactoryManager()
                           .EnumerateDefinitions();
    ASSERT_EQ(definitions.size(), OFFICIAL_COMPONENTS.size());

    for (const auto& expected : OFFICIAL_COMPONENTS)
    {
        const auto expected_guid = GuidFromText(expected.guid_text);
        const auto found = std::ranges::find_if(
            definitions,
            [&expected_guid](const auto& definition)
            { return definition.component_guid == expected_guid; });
        ASSERT_NE(found, definitions.end()) << expected.name;

        auto obj = found->definition.as_object();
        ASSERT_TRUE(obj.has_value()) << expected.name;
        EXPECT_EQ(StringField(*obj, "componentGuid"), expected.guid_text)
            << expected.name;
        EXPECT_EQ(StringField(*obj, "kind"), expected.kind) << expected.name;
        EXPECT_TRUE((*obj)[std::string_view("inputs")].is_array())
            << expected.name;
        EXPECT_TRUE((*obj)[std::string_view("outputs")].is_array())
            << expected.name;
        EXPECT_TRUE((*obj)[std::string_view("config")].is_object())
            << expected.name;
        EXPECT_TRUE((*obj)[std::string_view("diagnostics")].is_array())
            << expected.name;
    }
}

// 三份真实 manifest（DasFlowControl/DasMaaPi/DasGraphTask）的 label map
// 完整账本：每个 definition 顶层 label、每个 inputs/outputs/signals 端口
// 条目的 label、每个 config 声明字段的 label 均为 object 且含非空 "en" 与
// "zh-cn" 键。label map 是 catalog 本地化显示的唯一数据源，文案回退丢失
// （map 缺失/键缺失/空值）在此即红；总量随 manifest 演进，以 ManifestSpec
// 数字为准，不在注释里重复维护。
TEST_F(TaskComponentContractTest, RealManifestsCarryLocalizedLabelMaps)
{
    struct ManifestSpec
    {
        std::string_view plugin_dir_name;
        size_t           definition_count;
        size_t           port_count;
        size_t           config_field_count;
    };
    // port_count = inputs + outputs + signals 条目总和（DasFlowControl 无
    // signals 键，DasMaaPi 的 signals 条目形状为 {id,label}）；
    // config_field_count = config 对象的声明字段数（空 config 计 0）。
    const std::array<ManifestSpec, 3> manifests{
        ManifestSpec{"DasFlowControl", 15, 42, 26},
        ManifestSpec{"DasMaaPi", 1, 9, 2},
        // execute 组件 config 补 repositoryRef（taskRepositoryRef 类型，
        // 编译期绑定判定 + UI setValue 键）。
        ManifestSpec{"DasGraphTask", 4, 7, 6},
    };

    const auto expect_label_map =
        [](const yyjson::value& label_map, std::string_view context)
    {
        auto map_obj = label_map.as_object();
        ASSERT_TRUE(map_obj.has_value()) << context;
        for (const std::string_view locale : {"en", "zh-cn"})
        {
            ASSERT_TRUE(map_obj->contains(locale))
                << context << " lacks locale " << locale;
            const auto text = (*map_obj)[locale].as_string();
            ASSERT_TRUE(text.has_value() && !text->empty())
                << context << " has empty " << locale;
        }
    };

    size_t total_definitions = 0;
    size_t total_ports = 0;
    size_t total_config_fields = 0;
    for (const auto& spec : manifests)
    {
        const auto manifest_path =
            (SourceRoot() / DAS_PATH_LITERAL("das")
             / DAS_PATH_LITERAL("Plugins")
             / Das::Utils::PathFromUtf8(std::string(spec.plugin_dir_name)))
            / Das::Utils::PathFromUtf8(
                std::string(spec.plugin_dir_name) + ".json");
        auto manifest = ParseJson(ReadFileText(manifest_path));
        auto manifest_obj = manifest.as_object();
        ASSERT_TRUE(manifest_obj.has_value()) << spec.plugin_dir_name;
        auto task_components =
            (*manifest_obj)[std::string_view("taskComponents")].as_object();
        ASSERT_TRUE(task_components.has_value()) << spec.plugin_dir_name;
        auto components =
            (*task_components)[std::string_view("components")].as_object();
        ASSERT_TRUE(components.has_value()) << spec.plugin_dir_name;

        size_t definitions_with_label = 0;
        size_t ports_with_label = 0;
        size_t config_fields_with_label = 0;
        for (const auto& [guid_text, entry] : *components)
        {
            auto entry_obj = entry.as_object();
            ASSERT_TRUE(entry_obj.has_value()) << guid_text;
            auto definition =
                (*entry_obj)[std::string_view("definition")].as_object();
            ASSERT_TRUE(definition.has_value()) << guid_text;

            ASSERT_TRUE(definition->contains(std::string_view("label")))
                << guid_text << " definition lacks label map";
            expect_label_map(
                (*definition)[std::string_view("label")],
                guid_text);
            ++definitions_with_label;

            for (const std::string_view list_key :
                 {"inputs", "outputs", "signals"})
            {
                if (!definition->contains(list_key))
                {
                    continue;
                }
                auto list = (*definition)[list_key].as_array();
                ASSERT_TRUE(list.has_value()) << guid_text << ' ' << list_key;
                for (const auto& port : *list)
                {
                    auto port_obj = port.as_object();
                    ASSERT_TRUE(port_obj.has_value()) << guid_text;
                    const auto port_id = StringField(*port_obj, "id");
                    ASSERT_TRUE(port_obj->contains(std::string_view("label")))
                        << guid_text << '/' << port_id << " lacks label map";
                    expect_label_map(
                        (*port_obj)[std::string_view("label")],
                        std::string(guid_text) + "/" + std::string(port_id));
                    ++ports_with_label;
                }
            }

            // config 声明字段（{field_name: {type,label?,…}}）：节点编辑窗
            // fields[] 的显示文案源，同样要求双语 map 全覆盖。
            auto config = (*definition)[std::string_view("config")].as_object();
            ASSERT_TRUE(config.has_value()) << guid_text;
            for (const auto& [field_name, declaration] : *config)
            {
                auto declaration_obj = declaration.as_object();
                ASSERT_TRUE(declaration_obj.has_value())
                    << guid_text << '/' << field_name;
                ASSERT_TRUE(
                    declaration_obj->contains(std::string_view("label")))
                    << guid_text << '/' << field_name << " lacks label map";
                expect_label_map(
                    (*declaration_obj)[std::string_view("label")],
                    std::string(guid_text) + "/" + std::string(field_name));
                ++config_fields_with_label;
            }
        }
        EXPECT_EQ(definitions_with_label, spec.definition_count)
            << spec.plugin_dir_name;
        EXPECT_EQ(ports_with_label, spec.port_count) << spec.plugin_dir_name;
        EXPECT_EQ(config_fields_with_label, spec.config_field_count)
            << spec.plugin_dir_name;
        total_definitions += definitions_with_label;
        total_ports += ports_with_label;
        total_config_fields += config_fields_with_label;
    }
    EXPECT_EQ(total_definitions, 20u);
    EXPECT_EQ(total_ports, 58u);
    EXPECT_EQ(total_config_fields, 34u);
}

TEST_F(
    TaskComponentContractTest,
    DasFlowControlFeatureFactoryCreatesManifestComponents)
{
    auto runtime = CreateCppRuntime();
    ASSERT_NE(runtime, nullptr);
    auto plugin_result = runtime->LoadPlugin(manifest_path_);
    ASSERT_TRUE(plugin_result.has_value())
        << "Failed to load DasFlowControl from "
        << Das::Utils::PathToUtf8(manifest_path_);

    auto plugin_base = plugin_result.value();
    DasPtr<Das::PluginInterface::IDasPluginPackage> package;
    ASSERT_EQ(plugin_base.As(package.Put()), DAS_S_OK);

    bool     found_factory_feature = false;
    uint64_t task_component_feature_index = 0;
    for (uint64_t index = 0;; ++index)
    {
        Das::PluginInterface::DasPluginFeature feature{};
        if (package->EnumFeature(index, &feature) != DAS_S_OK)
        {
            break;
        }
        if (feature
            == Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY)
        {
            found_factory_feature = true;
            task_component_feature_index = index;
            break;
        }
    }
    ASSERT_TRUE(found_factory_feature);

    IDasBase* factory_base_raw = nullptr;
    ASSERT_EQ(
        package->CreateFeatureInterface(
            task_component_feature_index,
            &factory_base_raw),
        DAS_S_OK);
    ASSERT_NE(factory_base_raw, nullptr);
    const DasPtr<IDasBase> factory_base =
        DasPtr<IDasBase>::Attach(factory_base_raw);

    DasPtr<Das::PluginInterface::IDasTaskComponentFactory> factory;
    ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);

    for (const auto& expected : OFFICIAL_COMPONENTS)
    {
        DasPtr<Das::PluginInterface::IDasTaskComponent> component;
        EXPECT_EQ(
            factory->CreateComponent(
                GuidFromText(expected.guid_text),
                component.Put()),
            DAS_S_OK)
            << expected.name;
        EXPECT_NE(component.Get(), nullptr) << expected.name;
    }
}

TEST_F(TaskComponentContractTest, CreatesAllOfficialFlowComponentsByGuid)
{
    for (const auto& expected : OFFICIAL_COMPONENTS)
    {
        auto component = CreateComponent(expected.guid_text);
        ASSERT_NE(component.Get(), nullptr) << expected.name;

        DasPtr<IDasTypeInfo> type_info;
        EXPECT_EQ(
            component->QueryInterface(
                DasIidOf<IDasTypeInfo>(),
                reinterpret_cast<void**>(type_info.Put())),
            DAS_S_OK)
            << expected.name;
    }
}

TEST_F(TaskComponentContractTest, ApplySettingsChangeReturnsAcceptedSettings)
{
    auto component = CreateComponent(OFFICIAL_COMPONENTS[0].guid_text);
    ASSERT_NE(component.Get(), nullptr);

    auto change = Das::Utils::ParseYyjsonFromString(R"json({
        "kind": "setValue",
        "payload": {"path": "label", "value": "choose route"}
    })json");
    ASSERT_TRUE(change.has_value());
    auto request = Wrap(std::move(*change));

    DasPtr<Das::ExportInterface::IDasJson> result;
    ASSERT_EQ(
        component->ApplySettingsChange(request.Get(), result.Put()),
        DAS_S_OK);

    auto json = ToJson(result.Get());
    EXPECT_EQ(
        (*(*json.as_object())[std::string_view("acceptedSettings")]
              .as_object())[std::string_view("label")]
            .as_string()
            .value(),
        "choose route");
}

// GraphRuntime 的 settings-change wire 是 {"settings": S, "payload": P} 包装：
// compiled_settings 是节点当前配置的全量投影，组件以替换语义接收。修复前
// 该形状被忽略——formSequence item 的配置在运行期从未真正到达组件。
TEST_F(TaskComponentContractTest, ApplySettingsChangeAcceptsSettingsWrapper)
{
    auto component = CreateComponent(DELAY_COMPONENT_GUID);
    ASSERT_NE(component.Get(), nullptr);

    auto change = Das::Utils::ParseYyjsonFromString(R"json({
        "settings": {"delay_ms": 2500},
        "payload": null
    })json");
    ASSERT_TRUE(change.has_value());
    auto request = Wrap(std::move(*change));

    DasPtr<Das::ExportInterface::IDasJson> result;
    ASSERT_EQ(
        component->ApplySettingsChange(request.Get(), result.Put()),
        DAS_S_OK);

    auto json = ToJson(result.Get());
    auto accepted =
        (*json.as_object())[std::string_view("acceptedSettings")].as_object();
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(
        (*accepted)[std::string_view("delay_ms")].as_int().value_or(0),
        2500);
}

// 包装形状的 acceptedSettings 响应（经协议 builder 组装）与手拼
// {"acceptedSettings": ...} wire 逐字节等价。
TEST_F(
    TaskComponentContractTest,
    SettingsWrapperAcceptedResponseMatchesHandAssembledWire)
{
    auto component = CreateComponent(DELAY_COMPONENT_GUID);
    ASSERT_NE(component.Get(), nullptr);

    auto change = ParseJson(R"json({
        "settings": {"delay_ms": 2500},
        "payload": null
    })json");
    auto request = Wrap(std::move(change));

    DasPtr<Das::ExportInterface::IDasJson> result;
    ASSERT_EQ(
        component->ApplySettingsChange(request.Get(), result.Put()),
        DAS_S_OK);

    DasPtr<IDasReadOnlyString> response_text;
    ASSERT_EQ(result->ToString(-1, response_text.Put()), DAS_S_OK);
    const char* response_u8 = nullptr;
    ASSERT_EQ(response_text->GetUtf8(&response_u8), DAS_S_OK);
    ASSERT_NE(response_u8, nullptr);

    auto hand_assembled = Das::Utils::MakeYyjsonObject();
    auto hand_settings = Das::Utils::MakeYyjsonObject();
    (*hand_settings.as_object())[std::string_view("delay_ms")] = 2500;
    (*hand_assembled.as_object())[std::string_view("acceptedSettings")] =
        std::move(hand_settings);
    const auto hand_serialized =
        Das::Utils::SerializeYyjsonValue(std::move(hand_assembled));
    ASSERT_TRUE(hand_serialized.has_value());

    EXPECT_EQ(std::string{response_u8}, *hand_serialized);
}

// F1 运行期真实生效回归：真实 GraphRuntime 驱动 delay 节点，非默认
// delay_ms 经 {"settings": ...} 包装真实到达组件并生效（计时断言）。
// 修复前 settings 从未到达——delay_ms 恒 0、节点零等待完成。
TEST_F(
    TaskComponentContractTest,
    GraphRuntimeDeliversNodeSettingsToDelayComponent)
{
    Das::Core::GraphRuntime::Dto::CompiledGraphPlanDto plan;
    plan.source_fingerprint = "fp_delay_settings";

    Das::Core::GraphRuntime::Dto::CompiledNodeSnapshotDto node;
    node.node_id = "31000000-0000-0000-0000-000000000001";
    node.component_guid = std::string{DELAY_COMPONENT_GUID};
    node.compiled_settings = ParseJson(R"json({"delay_ms": 250})json");
    plan.node_snapshots.push_back(std::move(node));
    plan.execution_order.push_back("31000000-0000-0000-0000-000000000001");

    auto          host = StaticFlowControlComponentHost::Make();
    FakeStopToken stop_token;

    Das::Core::GraphRuntime::GraphRuntime runtime;
    const auto start = std::chrono::steady_clock::now();
    const auto hr =
        runtime.RunWithHost(plan, "fp_delay_settings", &stop_token, host.Get());
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count();

    EXPECT_EQ(hr, DAS_S_OK);
    EXPECT_GE(elapsed_ms, 200);  // 配置的 250ms 真实生效（0 = wire 断线）
    EXPECT_LT(elapsed_ms, 3000); // 未 hang
}

// ---- das.flow.launch：进程启动 + 可选等窗口（全注入缝驱动，业务中立）----

// 等窗字段缺省合法：spawn 成功即完成；规范口 signal_out 的发射归运行时
// 兜底，组件不再自发。
TEST_F(TaskComponentContractTest, LaunchDoSpawnsAndCompletesWithoutWaitWindow)
{
    auto component = MakeLaunchComponent();
    ASSERT_NE(component.Get(), nullptr);

    bool spawn_called = false;
    component->SetProcessSpawnerForTest(
        [&spawn_called](
            const std::string&,
            const std::vector<std::string>&,
            const std::string&,
            bool) -> bool
        {
            spawn_called = true;
            return true;
        });

    ApplyLaunchConfig(
        component.Get(),
        R"json({"exePath": "C:/test/app.exe"})json");

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result;
    ASSERT_EQ(component->Do(nullptr, nullptr, result.Put()), DAS_S_OK);

    EXPECT_TRUE(spawn_called);
    EXPECT_EQ(Das::Utils::GetPortString(result.Get(), "status"), "completed");
    EXPECT_EQ(FirstEmittedSignal(result.Get()), "");
}

// 等窗正则配置后轮询窗口枚举：首枚举 0 匹配、次枚举 1 匹配 → 完成。
TEST_F(TaskComponentContractTest, LaunchDoWaitsForSingleWindowMatch)
{
    auto component = MakeLaunchComponent();
    ASSERT_NE(component.Get(), nullptr);

    component->SetProcessSpawnerForTest(
        [](const std::string&,
           const std::vector<std::string>&,
           const std::string&,
           bool) -> bool { return true; });

    int enumeration_count = 0;
    component->SetWindowEnumeratorForTest(
        [&enumeration_count]()
        {
            ++enumeration_count;
            if (enumeration_count == 1)
            {
                return std::vector<Das::FlowControlWindowInfo>{};
            }
            return std::vector<Das::FlowControlWindowInfo>{
                Das::FlowControlWindowInfo{
                    nullptr,
                    "TestWindowClass",
                    "Test Window Title"}};
        });

    ApplyLaunchConfig(
        component.Get(),
        R"json({
            "exePath": "C:/test/app.exe",
            "waitWindowClassRegex": "TestWindowClass",
            "waitWindowWindowRegex": "",
            "waitWindowTimeoutMs": 5000
        })json");

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result;
    ASSERT_EQ(component->Do(nullptr, nullptr, result.Put()), DAS_S_OK);

    EXPECT_GT(enumeration_count, 1); // 轮询真实发生（0 匹配后重试）
    EXPECT_EQ(Das::Utils::GetPortString(result.Get(), "status"), "completed");
    EXPECT_EQ(FirstEmittedSignal(result.Get()), "");
}

// 轮询超时：枚举恒 0 匹配、waitWindowTimeoutMs 小值 → 专用执行失败错误码
// + launch-window-timeout 诊断载荷；消息经 i18n 表解析（注入替身表验证
// key 链路）。
TEST_F(TaskComponentContractTest, LaunchDoTimesOutWaitingForWindow)
{
    constexpr std::string_view translated_timeout_text =
        "ZZ-launch-window-timeout-translated-ZZ";
    auto injected_i18n = MakeI18nWithTranslation(
        "flow.diagnostic.launch-window-timeout",
        translated_timeout_text);
    ASSERT_NE(injected_i18n.Get(), nullptr);
    const ScopedFlowControlI18nOverride i18n_override{injected_i18n.Get()};

    auto component = MakeLaunchComponent();
    ASSERT_NE(component.Get(), nullptr);

    component->SetProcessSpawnerForTest(
        [](const std::string&,
           const std::vector<std::string>&,
           const std::string&,
           bool) -> bool { return true; });
    component->SetWindowEnumeratorForTest(
        []() -> std::vector<Das::FlowControlWindowInfo> { return {}; });

    ApplyLaunchConfig(
        component.Get(),
        R"json({
            "exePath": "C:/test/app.exe",
            "waitWindowClassRegex": "Anything",
            "waitWindowTimeoutMs": 300
        })json");

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result;
    ASSERT_EQ(
        component->Do(nullptr, nullptr, result.Put()),
        DAS_E_FLOW_LAUNCH_WINDOW_TIMEOUT);

    EXPECT_EQ(DiagnosticField(result.Get(), "code"), "launch-window-timeout");
    EXPECT_EQ(
        DiagnosticField(result.Get(), "message"),
        std::string{translated_timeout_text});
}

// 枚举恒返回多于 1 个匹配 → 专用错误码 + launch-window-ambiguous 载荷。
TEST_F(TaskComponentContractTest, LaunchDoFailsOnAmbiguousWindowMatch)
{
    auto component = MakeLaunchComponent();
    ASSERT_NE(component.Get(), nullptr);

    component->SetProcessSpawnerForTest(
        [](const std::string&,
           const std::vector<std::string>&,
           const std::string&,
           bool) -> bool { return true; });
    component->SetWindowEnumeratorForTest(
        []() -> std::vector<Das::FlowControlWindowInfo>
        {
            return std::vector<Das::FlowControlWindowInfo>{
                Das::FlowControlWindowInfo{nullptr, "TestWindowClass", "A"},
                Das::FlowControlWindowInfo{nullptr, "TestWindowClass", "B"}};
        });

    ApplyLaunchConfig(
        component.Get(),
        R"json({
            "exePath": "C:/test/app.exe",
            "waitWindowClassRegex": "TestWindowClass",
            "waitWindowTimeoutMs": 5000
        })json");

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result;
    ASSERT_EQ(
        component->Do(nullptr, nullptr, result.Put()),
        DAS_E_FLOW_LAUNCH_WINDOW_AMBIGUOUS);

    EXPECT_EQ(DiagnosticField(result.Get(), "code"), "launch-window-ambiguous");
}

// spawn 缝失败 → 专用错误码 + launch-spawn-failed 载荷（显性失败，无静默
// 兜底）。
TEST_F(TaskComponentContractTest, LaunchDoFailsWhenSpawnFails)
{
    auto component = MakeLaunchComponent();
    ASSERT_NE(component.Get(), nullptr);

    component->SetProcessSpawnerForTest(
        [](const std::string&,
           const std::vector<std::string>&,
           const std::string&,
           bool) -> bool { return false; });

    ApplyLaunchConfig(
        component.Get(),
        R"json({"exePath": "C:/test/app.exe"})json");

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result;
    ASSERT_EQ(
        component->Do(nullptr, nullptr, result.Put()),
        DAS_E_FLOW_LAUNCH_SPAWN_FAILED);

    EXPECT_EQ(DiagnosticField(result.Get(), "code"), "launch-spawn-failed");
}

// UAC 被拒（spawner 返回 ElevationDeclined）→ 专用错误码 +
// launch-elevation-declined 载荷；消息经 i18n 表解析（替身表注入验证
// key 链路）。
TEST_F(TaskComponentContractTest, LaunchDoFailsWhenElevationDeclined)
{
    constexpr std::string_view translated_declined_text =
        "ZZ-launch-elevation-declined-translated-ZZ";
    auto injected_i18n = MakeI18nWithTranslation(
        "flow.diagnostic.launch-elevation-declined",
        translated_declined_text);
    ASSERT_NE(injected_i18n.Get(), nullptr);
    const ScopedFlowControlI18nOverride i18n_override{injected_i18n.Get()};

    auto component = MakeLaunchComponent();
    ASSERT_NE(component.Get(), nullptr);

    component->SetProcessSpawnerForTest(
        [](const std::string&,
           const std::vector<std::string>&,
           const std::string&,
           bool) -> Das::FlowControlSpawnResult
        {
            return Das::FlowControlSpawnResult{
                Das::FlowControlSpawnStatus::ElevationDeclined};
        });

    ApplyLaunchConfig(
        component.Get(),
        R"json({"exePath": "C:/test/app.exe"})json");

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result;
    ASSERT_EQ(
        component->Do(nullptr, nullptr, result.Put()),
        DAS_E_FLOW_LAUNCH_ELEVATION_DECLINED);

    EXPECT_EQ(
        DiagnosticField(result.Get(), "code"),
        "launch-elevation-declined");
    EXPECT_EQ(
        DiagnosticField(result.Get(), "message"),
        std::string{translated_declined_text});
}

// ---- 提权回退编排（740 分类/拒绝/成功/非 740 跳过，双注入缝直测）----

namespace
{

    // 主启动体替身：抛指定错误值的 boost system_error（生产 bp2 构造失败
    // 语义）。
    Das::FlowControlPrimarySpawnBody BodyThrowingBoostSystemError(
        int error_value)
    {
        return [error_value](
                   const std::string&,
                   const std::vector<std::string>&,
                   const std::string&,
                   bool) -> std::uint32_t
        {
            throw boost::system::system_error{boost::system::error_code{
                error_value,
                boost::system::system_category()}};
        };
    }

    // 提权启动缝替身：固定三态 + 可选调用旗标（验证非 740 不触发回退）。
    Das::FlowControlElevatedLauncher LauncherReturning(
        Das::FlowControlElevatedLaunchStatus status,
        std::uint32_t                        process_id,
        bool*                                call_flag)
    {
        return [status, process_id, call_flag](
                   const std::string&,
                   const std::vector<std::string>&,
                   const std::string&) -> Das::FlowControlElevatedLaunchOutcome
        {
            if (call_flag != nullptr)
            {
                *call_flag = true;
            }
            Das::FlowControlElevatedLaunchOutcome outcome;
            outcome.status = status;
            outcome.process_id = process_id;
            return outcome;
        };
    }

} // namespace

TEST(SpawnWithElevationFallback, ReturnsSpawnedWhenPrimaryBodySucceeds)
{
    const auto result = Das::SpawnProcessWithElevationFallback(
        [](const std::string&,
           const std::vector<std::string>&,
           const std::string&,
           bool) -> std::uint32_t { return 100; },
        LauncherReturning(
            Das::FlowControlElevatedLaunchStatus::Failed,
            0,
            nullptr),
        "C:/test/app.exe",
        {},
        "",
        false);
    EXPECT_EQ(result.status, Das::FlowControlSpawnStatus::Spawned);
}

// 非 740 失败不触发回退（保持通用 spawn 失败语义）。
TEST(SpawnWithElevationFallback, SkipsFallbackForNonElevationError)
{
    bool       launcher_called = false;
    const auto result = Das::SpawnProcessWithElevationFallback(
        BodyThrowingBoostSystemError(2),
        LauncherReturning(
            Das::FlowControlElevatedLaunchStatus::Spawned,
            4242,
            &launcher_called),
        "C:/test/app.exe",
        {},
        "",
        false);
    EXPECT_EQ(result.status, Das::FlowControlSpawnStatus::Failed);
    EXPECT_FALSE(launcher_called);
}

#ifdef _WIN32
// 740（ERROR_ELEVATION_REQUIRED）→ 回退提权启动成功 → 结果 Spawned。
TEST(SpawnWithElevationFallback, ElevatesOnElevationRequiredAndSucceeds)
{
    const auto result = Das::SpawnProcessWithElevationFallback(
        BodyThrowingBoostSystemError(ERROR_ELEVATION_REQUIRED),
        LauncherReturning(
            Das::FlowControlElevatedLaunchStatus::Spawned,
            4242,
            nullptr),
        "C:/test/app.exe",
        {},
        "",
        false);
    EXPECT_EQ(result.status, Das::FlowControlSpawnStatus::Spawned);
}

// 740 → 回退启动的 UAC 请求被用户拒绝 → 结果 ElevationDeclined。
TEST(SpawnWithElevationFallback, DeclinesWhenUserRejectsElevation)
{
    const auto result = Das::SpawnProcessWithElevationFallback(
        BodyThrowingBoostSystemError(ERROR_ELEVATION_REQUIRED),
        LauncherReturning(
            Das::FlowControlElevatedLaunchStatus::Declined,
            0,
            nullptr),
        "C:/test/app.exe",
        {},
        "",
        false);
    EXPECT_EQ(result.status, Das::FlowControlSpawnStatus::ElevationDeclined);
}

// 740 → 回退启动自身失败 → 结果 Failed。
TEST(SpawnWithElevationFallback, FailsWhenElevatedLauncherFails)
{
    const auto result = Das::SpawnProcessWithElevationFallback(
        BodyThrowingBoostSystemError(ERROR_ELEVATION_REQUIRED),
        LauncherReturning(
            Das::FlowControlElevatedLaunchStatus::Failed,
            0,
            nullptr),
        "C:/test/app.exe",
        {},
        "",
        false);
    EXPECT_EQ(result.status, Das::FlowControlSpawnStatus::Failed);
}
#endif // _WIN32（740 是 Win32 错误码，回退分支仅 Win32 编译）

// ---- ACP→UTF-8 转码通道（spawn 失败日志中文段乱码修复）----

#ifdef _WIN32
// 机器 ACP 编不了 CJK 时往返必假阴性（936/950 是 CJK 代码页）。
bool MachineAcpCanEncodeChinese()
{
    const unsigned int active_code_page = GetACP();
    return active_code_page == 936 || active_code_page == 950;
}

// UTF-8 中文串经 path.string() 制出 CP_ACP 字节（模拟 FormatMessageA 的
// 本地化消息源）→ AcpTextToUtf8 后应还原为原 UTF-8 串。
TEST(AcpTextToUtf8, RoundTripsChineseTextFromAcpBytes)
{
    if (!MachineAcpCanEncodeChinese())
    {
        GTEST_SKIP() << "machine ACP cannot encode CJK; round trip is vacuous";
    }
    const std::string utf8_text = "启动失败：无法定位程序输入点";
    const std::string acp_bytes = Das::Utils::PathFromUtf8(utf8_text).string();
    EXPECT_EQ(Das::Utils::AcpTextToUtf8(acp_bytes), utf8_text);
}

// 混合串的 ASCII 尾巴（boost what() 的 [system:...] 段）转码无损。
TEST(AcpTextToUtf8, PreservesAsciiTailInMixedText)
{
    if (!MachineAcpCanEncodeChinese())
    {
        GTEST_SKIP() << "machine ACP cannot encode CJK; round trip is vacuous";
    }
    const std::string utf8_text =
        "中文段 [system:740 at D:\\source\\spawn.cpp:123]";
    const std::string acp_bytes = Das::Utils::PathFromUtf8(utf8_text).string();
    EXPECT_EQ(Das::Utils::AcpTextToUtf8(acp_bytes), utf8_text);
}
#endif // _WIN32

#ifndef _WIN32
// POSIX 窄文本本就是 UTF-8，任意字节串原样透传（Windows 构建上该用例
// 编译为空，保双平台可编译）。
TEST(AcpTextToUtf8, PassesThroughBytesOnPosix)
{
    const std::string arbitrary_bytes = "café 中";
    EXPECT_EQ(Das::Utils::AcpTextToUtf8(arbitrary_bytes), arbitrary_bytes);
}
#endif

// exePath 缺省/空 → 专用错误码 + launch-exe-path-missing 载荷，不触碰
// spawn 缝。
TEST_F(TaskComponentContractTest, LaunchDoFailsWhenExePathMissing)
{
    auto component = MakeLaunchComponent();
    ASSERT_NE(component.Get(), nullptr);

    bool spawn_called = false;
    component->SetProcessSpawnerForTest(
        [&spawn_called](
            const std::string&,
            const std::vector<std::string>&,
            const std::string&,
            bool) -> bool
        {
            spawn_called = true;
            return true;
        });

    ApplyLaunchConfig(component.Get(), R"json({"args": ["--flag"]})json");

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result;
    ASSERT_EQ(
        component->Do(nullptr, nullptr, result.Put()),
        DAS_E_FLOW_LAUNCH_EXE_PATH_MISSING);

    EXPECT_FALSE(spawn_called);
    EXPECT_EQ(DiagnosticField(result.Get(), "code"), "launch-exe-path-missing");
}

// 轮询期间 stop 置位 → cancelled（与 delay 组件同取消语义）。
TEST_F(TaskComponentContractTest, LaunchDoCancelsDuringWindowPoll)
{
    auto component = MakeLaunchComponent();
    ASSERT_NE(component.Get(), nullptr);

    component->SetProcessSpawnerForTest(
        [](const std::string&,
           const std::vector<std::string>&,
           const std::string&,
           bool) -> bool { return true; });

    FakeStopToken stop_token;
    component->SetWindowEnumeratorForTest(
        [&stop_token]() -> std::vector<Das::FlowControlWindowInfo>
        {
            stop_token.RequestStop(); // 首枚举即请求停止
            return {};
        });

    ApplyLaunchConfig(
        component.Get(),
        R"json({
            "exePath": "C:/test/app.exe",
            "waitWindowClassRegex": "Anything",
            "waitWindowTimeoutMs": 5000
        })json");

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result;
    ASSERT_EQ(component->Do(&stop_token, nullptr, result.Put()), DAS_S_OK);

    EXPECT_EQ(Das::Utils::GetPortString(result.Get(), "status"), "cancelled");
}

// config 端到端可达：spawn 缝收到 exePath/args/workingDirectory 原值。
TEST_F(TaskComponentContractTest, LaunchDoPassesConfigToSpawner)
{
    auto component = MakeLaunchComponent();
    ASSERT_NE(component.Get(), nullptr);

    std::string              spawned_exe_path;
    std::vector<std::string> spawned_arguments;
    std::string              spawned_working_directory;
    bool                     spawned_capture_output = true;
    component->SetProcessSpawnerForTest(
        [&spawned_exe_path,
         &spawned_arguments,
         &spawned_working_directory,
         &spawned_capture_output](
            const std::string&              exe_path,
            const std::vector<std::string>& arguments,
            const std::string&              working_directory,
            bool                            capture_output) -> bool
        {
            spawned_exe_path = exe_path;
            spawned_arguments = arguments;
            spawned_working_directory = working_directory;
            spawned_capture_output = capture_output;
            return true;
        });

    ApplyLaunchConfig(
        component.Get(),
        R"json({
            "exePath": "C:/test/app.exe",
            "args": ["--flag", "value"],
            "workingDirectory": "C:/test/work"
        })json");

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result;
    ASSERT_EQ(component->Do(nullptr, nullptr, result.Put()), DAS_S_OK);

    EXPECT_EQ(spawned_exe_path, "C:/test/app.exe");
    ASSERT_EQ(spawned_arguments.size(), 2u);
    EXPECT_EQ(spawned_arguments[0], "--flag");
    EXPECT_EQ(spawned_arguments[1], "value");
    EXPECT_EQ(spawned_working_directory, "C:/test/work");
    EXPECT_FALSE(spawned_capture_output); // 缺省 captureOutput = 关
}

// D-A9：args string 形态经 boost::process v2::shell 解析（MSVCRT/
// CommandLineToArgvW 规则）：引号包裹的含空格路径保形为单 token，恰 3
// 参——解析正确性是 boost 的责任，本用例锚定接线（token 序列原样到
// spawn 缝）。
TEST_F(TaskComponentContractTest, LaunchDoParsesStringArgsViaShellTokenizer)
{
    auto component = MakeLaunchComponent();
    ASSERT_NE(component.Get(), nullptr);

    std::vector<std::string> spawned_arguments;
    component->SetProcessSpawnerForTest(
        [&spawned_arguments](
            const std::string&,
            const std::vector<std::string>& arguments,
            const std::string&,
            bool) -> bool
        {
            spawned_arguments = arguments;
            return true;
        });

    ApplyLaunchConfig(
        component.Get(),
        R"json({
            "exePath": "C:/test/app.exe",
            "args": "--path \"C:\\Program Files\\x\" -v"
        })json");

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result;
    ASSERT_EQ(component->Do(nullptr, nullptr, result.Put()), DAS_S_OK);

    ASSERT_EQ(spawned_arguments.size(), 3u);
    EXPECT_EQ(spawned_arguments[0], "--path");
    EXPECT_EQ(spawned_arguments[1], "C:\\Program Files\\x");
    EXPECT_EQ(spawned_arguments[2], "-v");
}

// 逗号通道退役锚点：含逗号参数是单 token（旧 array<string> UI 逗号切分
// 会破坏 a,b.csv 这类值；string 形态 + shell 解析下保形）。
TEST_F(TaskComponentContractTest, LaunchDoKeepsCommaArgumentAsSingleToken)
{
    auto component = MakeLaunchComponent();
    ASSERT_NE(component.Get(), nullptr);

    std::vector<std::string> spawned_arguments;
    component->SetProcessSpawnerForTest(
        [&spawned_arguments](
            const std::string&,
            const std::vector<std::string>& arguments,
            const std::string&,
            bool) -> bool
        {
            spawned_arguments = arguments;
            return true;
        });

    ApplyLaunchConfig(
        component.Get(),
        R"json({
            "exePath": "C:/test/app.exe",
            "args": "-o a,b.csv"
        })json");

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result;
    ASSERT_EQ(component->Do(nullptr, nullptr, result.Put()), DAS_S_OK);

    ASSERT_EQ(spawned_arguments.size(), 2u);
    EXPECT_EQ(spawned_arguments[0], "-o");
    EXPECT_EQ(spawned_arguments[1], "a,b.csv");
}

// D-A9 边界：args 空串 = 空 token 序列（行为等价空 vector，不落哑参）。
TEST_F(TaskComponentContractTest, LaunchDoTreatsEmptyStringArgsAsNoArguments)
{
    auto component = MakeLaunchComponent();
    ASSERT_NE(component.Get(), nullptr);

    std::vector<std::string> spawned_arguments;
    // 故意预置哨兵值：空串形态应清空为 0 参而非保留预置。
    spawned_arguments.emplace_back("stale");
    component->SetProcessSpawnerForTest(
        [&spawned_arguments](
            const std::string&,
            const std::vector<std::string>& arguments,
            const std::string&,
            bool) -> bool
        {
            spawned_arguments = arguments;
            return true;
        });

    ApplyLaunchConfig(
        component.Get(),
        R"json({
            "exePath": "C:/test/app.exe",
            "args": ""
        })json");

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result;
    ASSERT_EQ(component->Do(nullptr, nullptr, result.Put()), DAS_S_OK);

    EXPECT_TRUE(spawned_arguments.empty());
}

// 宽容读锚点：array<string> 时期写入的存量条目（数据不迁移）逐元素兼容
// 读，等价 vector——与 LaunchDoPassesConfigToSpawner 同形态独立锚定。
TEST_F(TaskComponentContractTest, LaunchDoReadsLegacyArrayArgsForm)
{
    auto component = MakeLaunchComponent();
    ASSERT_NE(component.Get(), nullptr);

    std::vector<std::string> spawned_arguments;
    component->SetProcessSpawnerForTest(
        [&spawned_arguments](
            const std::string&,
            const std::vector<std::string>& arguments,
            const std::string&,
            bool) -> bool
        {
            spawned_arguments = arguments;
            return true;
        });

    ApplyLaunchConfig(
        component.Get(),
        R"json({
            "exePath": "C:/test/app.exe",
            "args": ["--flag", "value with space"]
        })json");

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result;
    ASSERT_EQ(component->Do(nullptr, nullptr, result.Put()), DAS_S_OK);

    ASSERT_EQ(spawned_arguments.size(), 2u);
    EXPECT_EQ(spawned_arguments[0], "--flag");
    EXPECT_EQ(spawned_arguments[1], "value with space");
}

// 生产 spawn 冒烟（无注入缝，走真实 boost::process 路径）：以「自 spawn
// 本测试 exe 的子进程形态」启动一个 filter 命中 0 例的短命进程并让它把
// gtest XML 结果写到落盘文件。断言 Do 返回后子进程仍存活并最终自行完成
// 落盘（文件出现为证）——同时覆盖 spawn 真链路不崩、detach 后零句柄持有
// （ASAN 冒烟观察项）。真窗口等待路径由注入缝行为用例覆盖，不含任何
// 窗口类/系统组件名硬编码。
TEST_F(TaskComponentContractTest, LaunchDoProductionSpawnSmoke)
{
    auto component = MakeLaunchComponent();
    ASSERT_NE(component.Get(), nullptr);

    const auto self_path =
        Das::Utils::ToStdPath(boost::dll::program_location());
    const std::string self_exe = Das::Utils::PathToUtf8(self_path);

    const auto marker_path =
        std::filesystem::current_path()
        / Das::Utils::PathFromUtf8(
            "launch_spawn_smoke_child_" + std::to_string(std::random_device{}())
            + ".xml");
    const std::string marker_path_text = Das::Utils::PathToUtf8(marker_path);
    std::error_code   remove_error;
    std::filesystem::remove(marker_path, remove_error);

    const std::string settings_json =
        std::string{R"json({"exePath": ")json"} + JsonEscapePath(self_exe)
        + R"json(", "args": ["--gtest_filter=NoLaunchSmokeChildTests", )json"
        + R"json("--gtest_output=xml:)json" + JsonEscapePath(marker_path_text)
        + R"json("]})json";

    ApplyLaunchConfig(component.Get(), settings_json);

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result;
    ASSERT_EQ(component->Do(nullptr, nullptr, result.Put()), DAS_S_OK);

    EXPECT_EQ(Das::Utils::GetPortString(result.Get(), "status"), "completed");
    EXPECT_EQ(FirstEmittedSignal(result.Get()), "");

    // 子进程落盘佐证：Do 返回（detach 已完成）后子进程仍在运行并最终写出
    // gtest XML——detach 语义失败的形态（析构杀子进程）不会出现本文件。
    // 预算 90s：子进程与本体同为大体积 ASAN 二进制，静态初始化/收尾墙钟
    // 可达数十秒（负载相关），15s 级预算会产生机器慢即红的假失败。
    bool marker_seen = false;
    for (int attempt = 0; attempt < 900 && !marker_seen; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::error_code probe_error;
        marker_seen = std::filesystem::exists(marker_path, probe_error);
    }
    EXPECT_TRUE(marker_seen) << "spawned child never wrote its result file";
    std::filesystem::remove(marker_path, remove_error);
}

// 无控制台宿主根修回归锚点：GUI 子系统进程（无控制台）的 stdin 句柄为
// NULL，spawn 若继承父进程 stdin（stdio 默认构造位）会在 CreateProcessW
// 失败。SetStdHandle(STD_INPUT_HANDLE, NULL) 模拟该宿主形态，断言生产
// spawner（零注入缝）仍能 spawn 短命 python 子进程并完成——修复前该场
// 景 spawn 失败返回执行失败错误码。
TEST_F(TaskComponentContractTest, ProductionSpawnerWorksWithoutConsoleStdin)
{
#ifndef _WIN32
    GTEST_SKIP() << "SetStdHandle stdin 模拟为 Windows 宿主形态专用";
#else
    const std::string python_path = ResolvePythonExecutablePath();
    ASSERT_FALSE(python_path.empty())
        << "python.exe not found on PATH (find_executable)";

    auto component = MakeLaunchComponent();
    ASSERT_NE(component.Get(), nullptr);

    const std::string settings_json = std::string{R"json({"exePath": ")json"}
                                      + JsonEscapePath(python_path)
                                      + R"json(", "args": ["-c", "pass"]})json";
    ApplyLaunchConfig(component.Get(), settings_json);

    const HANDLE saved_stdin = GetStdHandle(STD_INPUT_HANDLE);
    SetStdHandle(STD_INPUT_HANDLE, NULL);

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result;
    const DasResult                                do_result = [&]()
    {
        try
        {
            return component->Do(nullptr, nullptr, result.Put());
        }
        catch (...)
        {
            return DAS_E_FAIL;
        }
    }();
    SetStdHandle(STD_INPUT_HANDLE, saved_stdin);

    ASSERT_EQ(do_result, DAS_S_OK);
    EXPECT_EQ(Das::Utils::GetPortString(result.Get(), "status"), "completed");
#endif
}

// captureOutput 开（生产 spawn 路径，零注入缝）：子进程 stdout/stderr 原始
// 行落盘到注入目录的两个日志文件；文件名 {yyyyMMdd-HHmmss}-{进程名}-
// {pid}-{out|err}.log（进程名=exe 文件名去扩展名）；行内容零加工（无时
// 间戳/流标签前缀）；会话 io 线程异步写，轮询等待内容齐（行级 flush 保证
// 已写行可见即已落盘）。
TEST_F(TaskComponentContractTest, LaunchCaptureWritesRawLinesToPerProcessLogs)
{
#ifndef _WIN32
    GTEST_SKIP() << "捕获落盘用例经 ResolvePythonExecutablePath 定位解释器";
#else
    const std::string python_path = ResolvePythonExecutablePath();
    ASSERT_FALSE(python_path.empty())
        << "python.exe not found on PATH (find_executable)";

    const auto log_directory =
        std::filesystem::current_path()
        / Das::Utils::PathFromUtf8(
            "launch_capture_logs_" + std::to_string(std::random_device{}()));
    std::error_code remove_error;
    std::filesystem::remove_all(log_directory, remove_error);
    const ScopedLaunchCaptureDirectoryOverride capture_directory_override{
        log_directory};

    auto component = MakeLaunchComponent();
    ASSERT_NE(component.Get(), nullptr);

    // exePath 用裸命令名（PATH 解析链一并验证：解析后形态驱动启动与捕获
    // 文件名进程名）；-u 关 stdout 缓冲；stderr 经显式 \n 换行——两行都走
    // 真实管道。
    ApplyLaunchConfig(
        component.Get(),
        R"json({"exePath": "python", "args": ["-u", "-c", "print('cap-out-line'); import sys; sys.stderr.write('cap-err-line\\n')"], "captureOutput": true})json");

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result;
    ASSERT_EQ(component->Do(nullptr, nullptr, result.Put()), DAS_S_OK);
    EXPECT_EQ(Das::Utils::GetPortString(result.Get(), "status"), "completed");

    const std::regex file_name_pattern{
        R"(^\d{8}-\d{6}-python-\d+-(out|err)\.log$)"};
    std::string out_text;
    std::string err_text;
    for (int attempt = 0; attempt < 200
                          && (!HasExactRawLine(out_text, "cap-out-line")
                              || !HasExactRawLine(err_text, "cap-err-line"));
         ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::error_code list_error;
        for (const auto& entry :
             std::filesystem::directory_iterator(log_directory, list_error))
        {
            const auto file_name =
                Das::Utils::PathToUtf8(entry.path().filename());
            if (!std::regex_match(file_name, file_name_pattern))
            {
                continue;
            }
            if (file_name.ends_with("-out.log"))
            {
                out_text = ReadFileTextBinary(entry.path());
            }
            else if (file_name.ends_with("-err.log"))
            {
                err_text = ReadFileTextBinary(entry.path());
            }
        }
    }

    ASSERT_TRUE(HasExactRawLine(out_text, "cap-out-line"))
        << "stdout capture content: '" << out_text << "'";
    ASSERT_TRUE(HasExactRawLine(err_text, "cap-err-line"))
        << "stderr capture content: '" << err_text << "'";

    std::filesystem::remove_all(log_directory, remove_error);
#endif
}

// 裸命令名 PATH 解析未命中：exePath 为裸名且 PATH 无此命令 → 专用错误码
// + launch-exe-not-found 诊断（显性失败，不触碰 spawn 缝）。测试进程无
// i18n 目录，诊断消息走组件硬编码英文 fallback（确定性）。
TEST_F(
    TaskComponentContractTest,
    LaunchBareExeNameNotFoundOnPathFailsExplicitly)
{
    auto component = MakeLaunchComponent();
    ASSERT_NE(component.Get(), nullptr);

    bool spawn_called = false;
    component->SetProcessSpawnerForTest(
        [&spawn_called](
            const std::string&,
            const std::vector<std::string>&,
            const std::string&,
            bool) -> bool
        {
            spawn_called = true;
            return true;
        });

    ApplyLaunchConfig(
        component.Get(),
        R"json({"exePath": "definitely-not-exist-xyz"})json");

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result;
    ASSERT_EQ(
        component->Do(nullptr, nullptr, result.Put()),
        DAS_E_FLOW_LAUNCH_EXE_NOT_FOUND);

    EXPECT_FALSE(spawn_called);
    EXPECT_EQ(DiagnosticField(result.Get(), "code"), "launch-exe-not-found");
    EXPECT_EQ(
        DiagnosticField(result.Get(), "message"),
        "The configured executable was not found on PATH.");
}

// captureOutput 默认关（缺省与显式 false 两形态）：spawner 缝收到的第 4 参
// 均 false，且缝指向的目录零创建——关闭形态不产生任何捕获副作用。
TEST_F(TaskComponentContractTest, LaunchCaptureDefaultsToDisabled)
{
    const auto log_directory = std::filesystem::current_path()
                               / Das::Utils::PathFromUtf8(
                                   "launch_capture_disabled_"
                                   + std::to_string(std::random_device{}()));
    std::error_code remove_error;
    std::filesystem::remove_all(log_directory, remove_error);
    const ScopedLaunchCaptureDirectoryOverride capture_directory_override{
        log_directory};

    auto component = MakeLaunchComponent();
    ASSERT_NE(component.Get(), nullptr);

    bool spawned_capture_output = true;
    component->SetProcessSpawnerForTest(
        [&spawned_capture_output](
            const std::string&,
            const std::vector<std::string>&,
            const std::string&,
            bool capture_output) -> bool
        {
            spawned_capture_output = capture_output;
            return true;
        });

    ApplyLaunchConfig(
        component.Get(),
        R"json({"exePath": "C:/test/app.exe"})json");
    {
        DAS::DasPtr<Das::ExportInterface::IDasPortMap> result;
        ASSERT_EQ(component->Do(nullptr, nullptr, result.Put()), DAS_S_OK);
        EXPECT_FALSE(spawned_capture_output);
    }

    ApplyLaunchConfig(
        component.Get(),
        R"json({"exePath": "C:/test/app.exe", "captureOutput": false})json");
    {
        DAS::DasPtr<Das::ExportInterface::IDasPortMap> result;
        ASSERT_EQ(component->Do(nullptr, nullptr, result.Put()), DAS_S_OK);
        EXPECT_FALSE(spawned_capture_output);
    }

    std::error_code probe_error;
    EXPECT_FALSE(std::filesystem::exists(log_directory, probe_error))
        << "capture 默认关不应创建任何日志目录或文件";
}

TEST_F(TaskComponentContractTest, BranchDoReturnsTrueAndFalseSignals)
{
    auto component = CreateComponent(OFFICIAL_COMPONENTS[0].guid_text);
    ASSERT_NE(component.Get(), nullptr);

    const std::array<std::pair<bool, std::string_view>, 2> cases{
        std::pair{true, std::string_view{"true"}},
        std::pair{false, std::string_view{"false"}},
    };

    for (const auto& [condition, expected_signal] : cases)
    {
        DAS::DasPtr<Das::ExportInterface::IDasPortMap> input_map;
        ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
        input_map->SetBool(DasReadOnlyString("condition").Get(), condition);

        DAS::DasPtr<Das::ExportInterface::IDasPortMap> result_map;
        ASSERT_EQ(
            component->Do(nullptr, input_map.Get(), result_map.Put()),
            DAS_S_OK);

        auto status = Das::Utils::GetPortString(result_map.Get(), "status");
        EXPECT_EQ(status, "completed");
        auto outputs_json =
            Das::Utils::GetPortJson(result_map.Get(), "outputs");
        ASSERT_TRUE(outputs_json.has_value());
        EXPECT_TRUE(outputs_json->is_object());
        auto signals_json =
            Das::Utils::GetPortJson(result_map.Get(), "signals");
        ASSERT_TRUE(signals_json.has_value());
        auto signals = signals_json->as_array();
        ASSERT_TRUE(signals.has_value());
        ASSERT_EQ(signals->size(), 1u);
        EXPECT_EQ((*signals)[0].as_string().value(), expected_signal);
    }
}

TEST_F(TaskComponentContractTest, TaskComponentDoesNotExposeDynamicComponent)
{
    auto component = CreateComponent(OFFICIAL_COMPONENTS[0].guid_text);
    ASSERT_NE(component.Get(), nullptr);

    void* out = nullptr;
    EXPECT_EQ(
        component->QueryInterface(
            DasIidOf<Das::PluginInterface::IDasComponent>(),
            &out),
        DAS_E_NO_INTERFACE);
    EXPECT_EQ(out, nullptr);
}

// DAS-60 Stage 4: for is a counted loop head. It emits continue + the
// current index while in range, then break when exhausted. The counter
// persists across Do() calls on the same component instance.
TEST_F(TaskComponentContractTest, ForEmitsContinueAndIndexThenBreak)
{
    auto component = CreateComponent(FOR_COMPONENT_GUID);
    ASSERT_NE(component.Get(), nullptr);

    // 模拟运行期自环绑定：上圈 index 输出回填下圈 index 输入（首轮不填，
    // 缺键回落 start）。
    auto make_input = [&](std::optional<int64_t> carried_index)
    {
        DAS::DasPtr<Das::ExportInterface::IDasPortMap> input_map;
        EXPECT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
        Das::Utils::SetPortInt(input_map.Get(), "start", 0);
        Das::Utils::SetPortInt(input_map.Get(), "end", 3);
        Das::Utils::SetPortInt(input_map.Get(), "step", 1);
        if (carried_index.has_value())
        {
            Das::Utils::SetPortInt(input_map.Get(), "index", *carried_index);
        }
        return input_map;
    };

    // start=0, end=3, step=1 → continue with index 0,1,2.
    const std::array<int64_t, 3> expected_indices{0, 1, 2};
    std::optional<int64_t>       carried;
    for (auto idx : expected_indices)
    {
        auto                                           in = make_input(carried);
        DAS::DasPtr<Das::ExportInterface::IDasPortMap> result_map;
        ASSERT_EQ(component->Do(nullptr, in.Get(), result_map.Put()), DAS_S_OK);
        EXPECT_EQ(
            Das::Utils::GetPortString(result_map.Get(), "status"),
            "completed");
        EXPECT_EQ(FirstEmittedSignal(result_map.Get()), "continue");
        carried = Das::Utils::GetPortInt(result_map.Get(), "index");
        EXPECT_EQ(*carried, idx);
    }

    // 4th call: range exhausted → break, no index output.
    auto                                           in = make_input(carried);
    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result_map;
    ASSERT_EQ(component->Do(nullptr, in.Get(), result_map.Put()), DAS_S_OK);
    EXPECT_EQ(FirstEmittedSignal(result_map.Get()), "break");
}

// DAS-60 Stage 4: while emits continue/break by the dynamic condition each
// call.
TEST_F(TaskComponentContractTest, WhileEmitsContinueOrBreakByCondition)
{
    auto component = CreateComponent(WHILE_COMPONENT_GUID);
    ASSERT_NE(component.Get(), nullptr);

    auto run = [&](bool cond)
    {
        DAS::DasPtr<Das::ExportInterface::IDasPortMap> input_map;
        EXPECT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
        input_map->SetBool(DasReadOnlyString("condition").Get(), cond);
        DAS::DasPtr<Das::ExportInterface::IDasPortMap> result_map;
        EXPECT_EQ(
            component->Do(nullptr, input_map.Get(), result_map.Put()),
            DAS_S_OK);
        return FirstEmittedSignal(result_map.Get());
    };

    EXPECT_EQ(run(true), "continue");
    EXPECT_EQ(run(false), "break");
}

// DAS-60 Stage 4: merge (φ-join) copies whichever branch value is present.
// The runtime guarantees only the taken branch's value_* reaches merge.
TEST_F(TaskComponentContractTest, MergeOutputsValueFromTakenBranch)
{
    auto component = CreateComponent(MERGE_COMPONENT_GUID);
    ASSERT_NE(component.Get(), nullptr);

    auto run = [&](std::string_view set_port, int64_t value)
    {
        DAS::DasPtr<Das::ExportInterface::IDasPortMap> input_map;
        EXPECT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
        Das::Utils::SetPortInt(input_map.Get(), set_port, value);
        DAS::DasPtr<Das::ExportInterface::IDasPortMap> result_map;
        EXPECT_EQ(
            component->Do(nullptr, input_map.Get(), result_map.Put()),
            DAS_S_OK);
        return Das::Utils::GetPortInt(result_map.Get(), "result");
    };

    EXPECT_EQ(run("value_true", 42), 42); // true branch taken
    EXPECT_EQ(run("value_false", 7), 7);  // false branch taken
}

// DAS-52: delay_ms <= 0 emits "next" immediately without waiting.
TEST_F(TaskComponentContractTest, DelayZeroMsEmitsNextImmediately)
{
    auto component = CreateComponent(DELAY_COMPONENT_GUID);
    ASSERT_NE(component.Get(), nullptr);

    auto                                   change = ParseJson(R"json({
        "kind": "setValue",
        "payload": {"path": "delay_ms", "value": 0}
    })json");
    auto                                   request = Wrap(std::move(change));
    DasPtr<Das::ExportInterface::IDasJson> settings_result;
    ASSERT_EQ(
        component->ApplySettingsChange(request.Get(), settings_result.Put()),
        DAS_S_OK);

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);

    const auto start = std::chrono::steady_clock::now();
    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result_map;
    ASSERT_EQ(
        component->Do(nullptr, input_map.Get(), result_map.Put()),
        DAS_S_OK);
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count();

    EXPECT_EQ(
        Das::Utils::GetPortString(result_map.Get(), "status"),
        "completed");
    EXPECT_EQ(FirstEmittedSignal(result_map.Get()), "next");
    EXPECT_LT(elapsed_ms, 500); // no wait
}

// DAS-52: delay_ms > 0 actually waits (≈delay_ms), then emits "next".
TEST_F(TaskComponentContractTest, DelayWaitsThenEmitsNext)
{
    auto component = CreateComponent(DELAY_COMPONENT_GUID);
    ASSERT_NE(component.Get(), nullptr);

    auto                                   change = ParseJson(R"json({
        "kind": "setValue",
        "payload": {"path": "delay_ms", "value": 150}
    })json");
    auto                                   request = Wrap(std::move(change));
    DasPtr<Das::ExportInterface::IDasJson> settings_result;
    ASSERT_EQ(
        component->ApplySettingsChange(request.Get(), settings_result.Put()),
        DAS_S_OK);

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);

    const auto start = std::chrono::steady_clock::now();
    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result_map;
    ASSERT_EQ(
        component->Do(nullptr, input_map.Get(), result_map.Put()),
        DAS_S_OK);
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count();

    EXPECT_EQ(
        Das::Utils::GetPortString(result_map.Get(), "status"),
        "completed");
    EXPECT_EQ(FirstEmittedSignal(result_map.Get()), "next");
    EXPECT_GE(elapsed_ms, 100);  // actually waited (~150ms)
    EXPECT_LT(elapsed_ms, 3000); // did not hang
}

// DAS-52: a stop requested during the wait returns "cancelled" promptly,
// proving the wait is interruptible (not a blocking sleep_for).
TEST_F(TaskComponentContractTest, DelayIsCancellableByStopToken)
{
    auto component = CreateComponent(DELAY_COMPONENT_GUID);
    ASSERT_NE(component.Get(), nullptr);

    auto                                   change = ParseJson(R"json({
        "kind": "setValue",
        "payload": {"path": "delay_ms", "value": 10000}
    })json");
    auto                                   request = Wrap(std::move(change));
    DasPtr<Das::ExportInterface::IDasJson> settings_result;
    ASSERT_EQ(
        component->ApplySettingsChange(request.Get(), settings_result.Put()),
        DAS_S_OK);

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);

    FakeStopToken stop_token; // not stopped initially
    // Watchdog flips stop mid-wait; the cancellable wait must observe it.
    std::thread watchdog(
        [&stop_token]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            stop_token.RequestStop();
        });

    const auto start = std::chrono::steady_clock::now();
    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result_map;
    ASSERT_EQ(
        component->Do(&stop_token, input_map.Get(), result_map.Put()),
        DAS_S_OK);
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count();
    watchdog.join();

    EXPECT_EQ(
        Das::Utils::GetPortString(result_map.Get(), "status"),
        "cancelled");
    EXPECT_LT(elapsed_ms,
              3000); // far below the 10s delay → was interrupted
}

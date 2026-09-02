#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/GraphRuntime.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/Core/TaskScheduler/RepositoryInvokeDtos.h>
#include <das/Core/Utils/DasJsonImpl.h>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/TaskComponentPorts.h>
#include <das/_autogen/idl/abi/IDasComponent.h>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponentHost.Implements.hpp>
#include <gtest/gtest.h>

#include <boost/dll/runtime_symbol_info.hpp>

#include "FlowControlErrorCodes.h"
#include "FlowControlI18n.h"
#include "FlowControlTaskComponents.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

namespace
{
    using Das::DasPtr;
    using namespace Das::Plugins::DasFlowControl;
    using Das::Core::ForeignInterfaceHost::PluginManager;
    using Das::Core::TaskScheduler::RepositoryInvoke::Dto::
        ChildExecutionSnapshotDto;
    using Das::Core::TaskScheduler::RepositoryInvoke::Dto::
        InvokeRepositoryTaskInputDto;
    using Das::PluginInterface::IDasTaskComponent;

    struct OfficialFlowComponent
    {
        std::string_view name;
        std::string_view kind;
        std::string_view guid_text;
    };

    constexpr std::array<OfficialFlowComponent, 7> OFFICIAL_COMPONENTS{
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
            "repositoryInvoke",
            "das.flow.invokeRepository",
            "68F10007-0000-4000-8000-000000000007"},
        OfficialFlowComponent{
            "merge",
            "das.flow.merge",
            "68F10008-0000-4000-8000-000000000008"},
        OfficialFlowComponent{
            "launch",
            "das.flow.launch",
            "95FD2FFA-88F2-4F85-A536-C708D25B641F"},
    };

    constexpr std::string_view REPOSITORY_INVOKE_COMPONENT_GUID =
        "68F10007-0000-4000-8000-000000000007";
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
    constexpr std::string_view FAKE_CHILD_PLUGIN_GUID =
        "68F19998-0000-4000-8000-000000000001";
    constexpr std::string_view FAKE_CHILD_FACTORY_GUID =
        "68F19998-0000-4000-8000-000000000002";
    constexpr std::string_view FAKE_CHILD_COMPONENT_GUID =
        "68F19999-0000-4000-8000-000000000001";
    constexpr std::string_view MISSING_CHILD_COMPONENT_GUID =
        "68F19999-0000-4000-8000-000000000099";
    constexpr std::string_view MAAPI_RUN_TASK_COMPONENT_GUID =
        "69F20008-0000-4000-8000-000000000001";

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
        if (const char* plugin_dir = std::getenv("DAS_PLUGIN_DIR");
            plugin_dir != nullptr && plugin_dir[0] != '\0')
        {
            return std::filesystem::path{plugin_dir} / "DasFlowControl.json";
        }

        const auto cwd = std::filesystem::current_path();
        const std::array<std::filesystem::path, 3> candidates{
            cwd / "plugins" / "DasFlowControl.json",
            cwd.parent_path() / "bin" / "Debug" / "plugins"
                / "DasFlowControl.json",
            cwd / "build" / "mingw-debug" / "bin" / "Debug" / "plugins"
                / "DasFlowControl.json",
        };
        const auto found = std::ranges::find_if(
            candidates,
            [](const std::filesystem::path& candidate)
            { return std::filesystem::exists(candidate); });
        return found != candidates.end()
                   ? *found
                   : std::filesystem::path{"DasFlowControl.json"};
    }

    std::filesystem::path SourceRoot()
    {
        auto path = std::filesystem::path(__FILE__);
        for (int i = 0; i < 5; ++i)
        {
            path = path.parent_path();
        }
        return path;
    }

    std::string ReadFileText(const std::filesystem::path& path)
    {
        const std::ifstream input(path);
        EXPECT_TRUE(input.is_open()) << path.string();
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

    yyjson::value MakeDefinition(
        std::string_view component_guid,
        std::string_view kind)
    {
        auto definition = Das::Utils::MakeYyjsonObject();
        auto obj = *definition.as_object();
        obj[std::string_view("schemaVersion")] = 1;
        obj[std::string_view("componentGuid")] = component_guid;
        obj[std::string_view("kind")] = kind;
        obj[std::string_view("inputs")] = Das::Utils::MakeYyjsonArray();
        obj[std::string_view("outputs")] = Das::Utils::MakeYyjsonArray();
        obj[std::string_view("config")] = Das::Utils::MakeYyjsonObject();
        obj[std::string_view("diagnostics")] = Das::Utils::MakeYyjsonArray();
        return definition;
    }

    Das::Core::ForeignInterfaceHost::TaskComponentsManifestDesc
    MakeChildManifest()
    {
        Das::Core::ForeignInterfaceHost::TaskComponentsManifestDesc manifest;
        manifest.factories =
            std::vector<std::string>{std::string{FAKE_CHILD_FACTORY_GUID}};

        Das::Core::ForeignInterfaceHost::TaskComponentManifestEntryDesc entry;
        entry.factory_guid = std::string{FAKE_CHILD_FACTORY_GUID};
        entry.definition =
            MakeDefinition(FAKE_CHILD_COMPONENT_GUID, "test.child.recording");

        std::unordered_map<
            std::string,
            Das::Core::ForeignInterfaceHost::TaskComponentManifestEntryDesc>
            components;
        components.emplace(
            std::string{FAKE_CHILD_COMPONENT_GUID},
            std::move(entry));
        manifest.components = std::move(components);
        return manifest;
    }

    yyjson::value MakeRepositoryInvokeInput(
        std::string_view component_guid,
        int32_t          snapshot_version = 1)
    {
        ChildExecutionSnapshotDto snapshot;
        snapshot.version = snapshot_version;
        snapshot.source_entry_id = 42;
        snapshot.source_revision = 7;
        snapshot.source_fingerprint = "source-hash";
        snapshot.plugin_guid = std::string{FAKE_CHILD_PLUGIN_GUID};
        snapshot.task_type_guid = "68F19997-0000-4000-8000-000000000001";
        snapshot.component_guid = std::string{component_guid};
        snapshot.execution_input = ParseJson(R"json({
            "childInput": "from-snapshot",
            "providerPrivate": {"exactKey": true}
        })json");

        InvokeRepositoryTaskInputDto input;
        input.compiled_snapshot = std::move(snapshot);
        input.runtime_inputs = ParseJson(R"json({"callerValue": 9})json");

        auto serialized = yyjson::object(input);
        auto text = serialized.write(yyjson::WriteFlag::NoFlag);
        auto parsed = Das::Utils::ParseYyjsonFromString(
            std::string_view(text.data(), text.size()));
        EXPECT_TRUE(parsed.has_value());
        return parsed ? std::move(*parsed) : yyjson::value{};
    }

    std::string_view DiagnosticCode(const yyjson::value& result)
    {
        auto obj = result.as_object();
        if (!obj)
        {
            return {};
        }
        auto diagnostics = (*obj)[std::string_view("diagnostics")].as_array();
        if (!diagnostics || diagnostics->empty())
        {
            return {};
        }
        auto diagnostic = (*diagnostics)[0].as_object();
        if (!diagnostic)
        {
            return {};
        }
        return (*diagnostic)[std::string_view("code")].as_string().value_or("");
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
            new Das::DasFlowControlTaskComponent(
                LAUNCH_KIND_TEXT,
                DasPtr<Das::PluginInterface::IDasTaskComponentHost>{})};
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

    struct RecordingChildState
    {
        int                                  do_call_count = 0;
        DasResult                            do_result = DAS_S_OK;
        Das::PluginInterface::IDasStopToken* last_stop_token = nullptr;
        std::string                          last_environment_json;
        std::string                          last_settings_json;
        std::string                          last_input_json;
        std::string                          result_json = R"json({
            "status": "completed",
            "outputs": {"value": 5},
            "diagnostics": [],
            "signals": {"succeeded": true}
        })json";
    };

    class RecordingChildTaskComponent final : public IDasTaskComponent
    {
    public:
        RecordingChildTaskComponent(
            DasGuid                              guid,
            std::shared_ptr<RecordingChildState> state)
            : guid_(guid), state_(std::move(state))
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
            if (iid == DasIidOf<IDasTypeInfo>())
            {
                *pp_out = static_cast<IDasTypeInfo*>(this);
                AddRef();
                return DAS_S_OK;
            }
            if (iid == DasIidOf<IDasTaskComponent>())
            {
                *pp_out = static_cast<IDasTaskComponent*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
        {
            if (p_out_guid == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *p_out_guid = guid_;
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            if (pp_out_name == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            return CreateIDasReadOnlyStringFromUtf8(
                "RecordingChildTaskComponent",
                pp_out_name);
        }

        DasResult DAS_STD_CALL ApplySettingsChange(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson**) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult DAS_STD_CALL
        Do(Das::PluginInterface::IDasStopToken*       stop_token,
           Das::ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
           Das::ExportInterface::IDasPortMap**        pp_out_port_map) override
        {
            if (pp_out_port_map == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_port_map = nullptr;
            ++state_->do_call_count;
            state_->last_stop_token = stop_token;

            if (p_input_port_map != nullptr)
            {
                state_->last_input_json = Das::Utils::GetPortString(
                    p_input_port_map,
                    "executionInput");
            }

            if (DAS::IsFailed(state_->do_result))
            {
                return state_->do_result;
            }

            auto result_parsed =
                Das::Utils::ParseYyjsonFromString(state_->result_json);
            if (!result_parsed)
            {
                return DAS_E_FAIL;
            }

            DAS::DasPtr<Das::ExportInterface::IDasPortMap> output_map;
            const DasResult hr = CreateIDasPortMap(output_map.Put());
            if (DAS::IsFailed(hr))
            {
                return hr;
            }

            if (result_parsed->is_object())
            {
                auto result_obj = result_parsed->as_object();
                auto status_val = (*result_obj)[std::string_view("status")]
                                      .as_string()
                                      .value_or("");
                Das::Utils::SetPortString(
                    output_map.Get(),
                    "status",
                    status_val);

                auto outputs = (*result_obj)[std::string_view("outputs")];
                if (!outputs.is_null())
                {
                    auto serialized = outputs.write(yyjson::WriteFlag::NoFlag);
                    Das::Utils::SetPortString(
                        output_map.Get(),
                        "outputs",
                        std::string_view(serialized));
                }
            }
            else
            {
                // result_json 不是 object（例如 malformed 空数组）→ 模拟 child
                // 自身报告失败，RepositoryInvoke 读到 status="failed" 后透传。
                Das::Utils::SetPortString(output_map.Get(), "status", "failed");
            }

            *pp_out_port_map = output_map.Get();
            (*pp_out_port_map)->AddRef();
            return DAS_S_OK;
        }

    private:
        DasGuid                              guid_{};
        std::shared_ptr<RecordingChildState> state_;
        std::atomic<uint32_t>                ref_count_{0};
    };

    class RecordingChildTaskComponentFactory final
        : public Das::PluginInterface::IDasTaskComponentFactory
    {
    public:
        explicit RecordingChildTaskComponentFactory(
            std::shared_ptr<RecordingChildState> state)
            : state_(std::move(state))
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
            if (iid == DasIidOf<IDasTypeInfo>())
            {
                *pp_out = static_cast<IDasTypeInfo*>(this);
                AddRef();
                return DAS_S_OK;
            }
            if (iid
                == DasIidOf<Das::PluginInterface::IDasTaskComponentFactory>())
            {
                *pp_out = static_cast<
                    Das::PluginInterface::IDasTaskComponentFactory*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
        {
            if (p_out_guid == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *p_out_guid = GuidFromText(FAKE_CHILD_FACTORY_GUID);
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            if (pp_out_name == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            return CreateIDasReadOnlyStringFromUtf8(
                "RecordingChildTaskComponentFactory",
                pp_out_name);
        }

        DasResult DAS_STD_CALL CreateComponent(
            const DasGuid&      component_guid,
            IDasTaskComponent** pp_out_component) override
        {
            if (pp_out_component == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_component = nullptr;
            if (component_guid != GuidFromText(FAKE_CHILD_COMPONENT_GUID))
            {
                return DAS_E_NOT_FOUND;
            }

            ++create_call_count;
            auto* component =
                new RecordingChildTaskComponent(component_guid, state_);
            component->AddRef();
            *pp_out_component = component;
            return DAS_S_OK;
        }

        int create_call_count = 0;

    private:
        std::shared_ptr<RecordingChildState> state_;
        std::atomic<uint32_t>                ref_count_{0};
    };

    constexpr std::string_view DELAY_KIND_TEXT = "das.flow.delay";

    // GraphRuntime 直驱用 host：创建测试 exe 内静态编入的真实 FlowControl
    // 组件（与 DLL 内副本同源码；直驱用例需要真实组件行为，不经工厂注册表）。
    class StaticFlowControlComponentHost final
        : public Das::PluginInterface::DasTaskComponentHostImplBase<
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

            auto* component = new Das::DasFlowControlTaskComponent(
                DELAY_KIND_TEXT,
                DasPtr<Das::PluginInterface::IDasTaskComponentHost>(this));
            component->AddRef();
            *pp_out_component = component;
            return DAS_S_OK;
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
                << manifest_path_.string();
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

        RecordingChildTaskComponentFactory* RegisterRecordingChildFactory(
            const std::shared_ptr<RecordingChildState>& state)
        {
            auto* factory = new RecordingChildTaskComponentFactory(state);
            factory->AddRef();

            Das::Core::ForeignInterfaceHost::FeatureInfo feature{};
            feature.feature_type =
                Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY;
            feature.interface_ptr = DasPtr<IDasBase>(static_cast<IDasBase*>(
                static_cast<Das::PluginInterface::IDasTaskComponentFactory*>(
                    factory)));

            Das::Core::ForeignInterfaceHost::FeatureInfo* feature_ptr =
                &feature;
            EXPECT_EQ(
                plugin_manager_->GetTaskComponentFactoryManager()
                    .OnPluginLoaded(
                        GuidFromText(FAKE_CHILD_PLUGIN_GUID),
                        {&feature_ptr, 1},
                        MakeChildManifest()),
                DAS_S_OK);
            return factory;
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

TEST_F(
    TaskComponentContractTest,
    RepositoryInvokeFinalCatalogHidesMaapiAgentRuntime)
{
    auto       definitions = plugin_manager_->GetTaskComponentFactoryManager()
                                 .EnumerateDefinitions();
    const auto repository_invoke_guid =
        GuidFromText(REPOSITORY_INVOKE_COMPONENT_GUID);
    auto repository_invoke = std::ranges::find_if(
        definitions,
        [&repository_invoke_guid](const auto& definition)
        { return definition.component_guid == repository_invoke_guid; });
    ASSERT_NE(repository_invoke, definitions.end());
    auto repository_definition = repository_invoke->definition.as_object();
    ASSERT_TRUE(repository_definition.has_value());
    EXPECT_EQ(
        StringField(*repository_definition, "kind"),
        std::string_view("das.flow.invokeRepository"));

    const auto maapi_manifest_path =
        SourceRoot() / "das" / "Plugins" / "DasMaaPi" / "DasMaaPi.json";
    const auto maapi_manifest_text = ReadFileText(maapi_manifest_path);
    EXPECT_EQ(
        maapi_manifest_text.find("das.maapi.agentRuntime"),
        std::string::npos);

    auto maapi_manifest = ParseJson(maapi_manifest_text);
    auto maapi_obj = maapi_manifest.as_object();
    ASSERT_TRUE(maapi_obj.has_value());
    auto task_components =
        (*maapi_obj)[std::string_view("taskComponents")].as_object();
    ASSERT_TRUE(task_components.has_value());
    auto components =
        (*task_components)[std::string_view("components")].as_object();
    ASSERT_TRUE(components.has_value());
    auto run_entry = (*components)[MAAPI_RUN_TASK_COMPONENT_GUID].as_object();
    ASSERT_TRUE(run_entry.has_value());
    auto definition = (*run_entry)[std::string_view("definition")].as_object();
    ASSERT_TRUE(definition.has_value());
    EXPECT_EQ(
        StringField(*definition, "kind"),
        std::string_view("das.maapi.run"));
}

// 三份真实 manifest（DasFlowControl/DasMaaPi/DasGraphTask）的 label map
// 完整账本：每个 definition 顶层 label、每个 inputs/outputs/signals 端口
// 条目的 label、每个 config 声明字段的 label 均为 object 且含非空 "en" 与
// "zh-cn" 键（共 9 个 definition map + 34 个端口 map + 9 个 config 字段
// map）。label map 是 catalog 本地化显示的唯一数据源，文案回退丢失（map
// 缺失/键缺失/空值）在此即红。
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
        ManifestSpec{"DasFlowControl", 7, 24, 7},
        ManifestSpec{"DasMaaPi", 1, 8, 2},
        ManifestSpec{"DasGraphTask", 1, 2, 0},
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
            (SourceRoot() / "das" / "Plugins"
             / std::string(spec.plugin_dir_name))
            / (std::string(spec.plugin_dir_name) + ".json");
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
    EXPECT_EQ(total_definitions, 9u);
    EXPECT_EQ(total_ports, 34u);
    EXPECT_EQ(total_config_fields, 9u);
}

TEST_F(
    TaskComponentContractTest,
    DasFlowControlFeatureFactoryCreatesManifestComponents)
{
    auto runtime = CreateCppRuntime();
    ASSERT_NE(runtime, nullptr);
    auto plugin_result = runtime->LoadPlugin(manifest_path_);
    ASSERT_TRUE(plugin_result.has_value())
        << "Failed to load DasFlowControl from " << manifest_path_.string();

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

// 等窗字段缺省合法：spawn 成功即完成，发规范信号口 signal_out。
TEST_F(TaskComponentContractTest, LaunchDoSpawnsAndEmitsSignalWithoutWaitWindow)
{
    auto component = MakeLaunchComponent();
    ASSERT_NE(component.Get(), nullptr);

    bool spawn_called = false;
    component->SetProcessSpawnerForTest(
        [&spawn_called](
            const std::string&,
            const std::vector<std::string>&,
            const std::string&) -> bool
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
    EXPECT_EQ(FirstEmittedSignal(result.Get()), "signal_out");
}

// 等窗正则配置后轮询窗口枚举：首枚举 0 匹配、次枚举 1 匹配 → 完成。
TEST_F(TaskComponentContractTest, LaunchDoWaitsForSingleWindowMatch)
{
    auto component = MakeLaunchComponent();
    ASSERT_NE(component.Get(), nullptr);

    component->SetProcessSpawnerForTest(
        [](const std::string&,
           const std::vector<std::string>&,
           const std::string&) -> bool { return true; });

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
    EXPECT_EQ(FirstEmittedSignal(result.Get()), "signal_out");
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
           const std::string&) -> bool { return true; });
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
           const std::string&) -> bool { return true; });
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
           const std::string&) -> bool { return false; });

    ApplyLaunchConfig(
        component.Get(),
        R"json({"exePath": "C:/test/app.exe"})json");

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result;
    ASSERT_EQ(
        component->Do(nullptr, nullptr, result.Put()),
        DAS_E_FLOW_LAUNCH_SPAWN_FAILED);

    EXPECT_EQ(DiagnosticField(result.Get(), "code"), "launch-spawn-failed");
}

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
            const std::string&) -> bool
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
           const std::string&) -> bool { return true; });

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
    component->SetProcessSpawnerForTest(
        [&spawned_exe_path, &spawned_arguments, &spawned_working_directory](
            const std::string&              exe_path,
            const std::vector<std::string>& arguments,
            const std::string&              working_directory) -> bool
        {
            spawned_exe_path = exe_path;
            spawned_arguments = arguments;
            spawned_working_directory = working_directory;
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

#ifdef _WIN32
    const std::filesystem::path self_path{
        boost::dll::program_location().wstring()};
#else
    const std::filesystem::path self_path{
        boost::dll::program_location().string()};
#endif
    const auto        self_exe_u8 = self_path.u8string();
    const std::string self_exe(self_exe_u8.begin(), self_exe_u8.end());

    const auto marker_u8 =
        (std::filesystem::current_path()
         / ("launch_spawn_smoke_child_" + std::to_string(std::random_device{}())
            + ".xml"))
            .u8string();
    const std::string marker_path(marker_u8.begin(), marker_u8.end());
    std::error_code   remove_error;
    std::filesystem::remove(marker_path, remove_error);

    const std::string settings_json =
        std::string{R"json({"exePath": ")json"} + JsonEscapePath(self_exe)
        + R"json(", "args": ["--gtest_filter=NoLaunchSmokeChildTests", )json"
        + R"json("--gtest_output=xml:)json" + JsonEscapePath(marker_path)
        + R"json("]})json";

    ApplyLaunchConfig(component.Get(), settings_json);

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result;
    ASSERT_EQ(component->Do(nullptr, nullptr, result.Put()), DAS_S_OK);

    EXPECT_EQ(Das::Utils::GetPortString(result.Get(), "status"), "completed");
    EXPECT_EQ(FirstEmittedSignal(result.Get()), "signal_out");

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

TEST_F(TaskComponentContractTest, RepositoryInvokeExecutesCompiledSnapshotChild)
{
    auto      state = std::make_shared<RecordingChildState>();
    auto*     child_factory = RegisterRecordingChildFactory(state);
    const int create_count_before = child_factory->create_call_count;

    auto component = CreateComponent(REPOSITORY_INVOKE_COMPONENT_GUID);
    ASSERT_NE(component.Get(), nullptr);

    FakeStopToken stop_token;
    auto input_json = MakeRepositoryInvokeInput(FAKE_CHILD_COMPONENT_GUID);
    auto input_obj = input_json.as_object();
    ASSERT_TRUE(input_obj.has_value());
    auto snapshot_val = (*input_obj)[std::string_view("compiledSnapshot")];
    auto snapshot_str = snapshot_val.write(yyjson::WriteFlag::NoFlag);

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
    Das::Utils::SetPortString(
        input_map.Get(),
        "compiledSnapshot",
        std::string_view(snapshot_str));

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result_map;
    ASSERT_EQ(
        component->Do(&stop_token, input_map.Get(), result_map.Put()),
        DAS_S_OK);

    EXPECT_EQ(child_factory->create_call_count, create_count_before + 1);
    EXPECT_EQ(state->do_call_count, 1);
    EXPECT_EQ(state->last_stop_token, &stop_token);

    auto child_input = ParseJson(state->last_input_json);
    auto child_input_obj = child_input.as_object();
    ASSERT_TRUE(child_input_obj.has_value());
    EXPECT_EQ(
        (*child_input_obj)[std::string_view("childInput")].as_string().value_or(
            ""),
        std::string_view("from-snapshot"));

    auto status = Das::Utils::GetPortString(result_map.Get(), "status");
    EXPECT_EQ(status, "completed");
    auto child_status =
        Das::Utils::GetPortString(result_map.Get(), "childStatus");
    EXPECT_EQ(child_status, "completed");
    auto child_outputs_json =
        Das::Utils::GetPortJson(result_map.Get(), "childOutputs");
    ASSERT_TRUE(child_outputs_json.has_value());
    auto child_outputs_obj = child_outputs_json->as_object();
    ASSERT_TRUE(child_outputs_obj.has_value());
    EXPECT_EQ(
        (*child_outputs_obj)[std::string_view("value")].as_int().value(),
        5);

    child_factory->Release();
}

TEST_F(TaskComponentContractTest, RepositoryInvokeRequiresCompiledSnapshot)
{
    auto component = CreateComponent(REPOSITORY_INVOKE_COMPONENT_GUID);
    ASSERT_NE(component.Get(), nullptr);

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result_map;
    ASSERT_EQ(
        component->Do(nullptr, input_map.Get(), result_map.Put()),
        DAS_S_OK);

    auto status = Das::Utils::GetPortString(result_map.Get(), "status");
    EXPECT_EQ(status, "failed");
}

TEST_F(TaskComponentContractTest, RepositoryInvokeFailsWhenHostUnavailable)
{
    auto runtime = CreateCppRuntime();
    ASSERT_NE(runtime, nullptr);
    auto plugin_result = runtime->LoadPlugin(manifest_path_);
    ASSERT_TRUE(plugin_result.has_value());

    DasPtr<Das::PluginInterface::IDasPluginPackage> package;
    ASSERT_EQ(plugin_result.value().As(package.Put()), DAS_S_OK);

    IDasBase* factory_base_raw = nullptr;
    ASSERT_EQ(package->CreateFeatureInterface(0, &factory_base_raw), DAS_S_OK);
    ASSERT_NE(factory_base_raw, nullptr);
    const DasPtr<IDasBase> factory_base =
        DasPtr<IDasBase>::Attach(factory_base_raw);

    DasPtr<Das::PluginInterface::IDasTaskComponentFactory> factory;
    ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);

    DasPtr<IDasTaskComponent> component;
    ASSERT_EQ(
        factory->CreateComponent(
            GuidFromText(REPOSITORY_INVOKE_COMPONENT_GUID),
            component.Put()),
        DAS_S_OK);

    auto input = MakeRepositoryInvokeInput(FAKE_CHILD_COMPONENT_GUID);
    auto input_obj = input.as_object();
    ASSERT_TRUE(input_obj.has_value());
    auto snapshot_val = (*input_obj)[std::string_view("compiledSnapshot")];
    auto snapshot_str = snapshot_val.write(yyjson::WriteFlag::NoFlag);

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
    Das::Utils::SetPortString(
        input_map.Get(),
        "compiledSnapshot",
        std::string_view(snapshot_str));

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result;
    ASSERT_EQ(component->Do(nullptr, input_map.Get(), result.Put()), DAS_S_OK);

    auto status = Das::Utils::GetPortString(result.Get(), "status");
    EXPECT_EQ(status, "failed");
}

TEST_F(TaskComponentContractTest, RepositoryInvokeReportsMissingChildRoute)
{
    auto component = CreateComponent(REPOSITORY_INVOKE_COMPONENT_GUID);
    ASSERT_NE(component.Get(), nullptr);

    auto input = MakeRepositoryInvokeInput(MISSING_CHILD_COMPONENT_GUID);
    auto input_obj = input.as_object();
    ASSERT_TRUE(input_obj.has_value());
    auto snapshot_val = (*input_obj)[std::string_view("compiledSnapshot")];
    auto snapshot_str = snapshot_val.write(yyjson::WriteFlag::NoFlag);

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
    Das::Utils::SetPortString(
        input_map.Get(),
        "compiledSnapshot",
        std::string_view(snapshot_str));

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result_map;
    ASSERT_EQ(
        component->Do(nullptr, input_map.Get(), result_map.Put()),
        DAS_S_OK);

    auto status = Das::Utils::GetPortString(result_map.Get(), "status");
    EXPECT_EQ(status, "failed");
}

TEST_F(TaskComponentContractTest, RepositoryInvokeReportsChildFailure)
{
    auto state = std::make_shared<RecordingChildState>();
    state->do_result = DAS_E_FAIL;
    auto* child_factory = RegisterRecordingChildFactory(state);

    auto component = CreateComponent(REPOSITORY_INVOKE_COMPONENT_GUID);
    auto input = MakeRepositoryInvokeInput(FAKE_CHILD_COMPONENT_GUID);
    auto input_obj = input.as_object();
    ASSERT_TRUE(input_obj.has_value());
    auto snapshot_val = (*input_obj)[std::string_view("compiledSnapshot")];
    auto snapshot_str = snapshot_val.write(yyjson::WriteFlag::NoFlag);

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
    Das::Utils::SetPortString(
        input_map.Get(),
        "compiledSnapshot",
        std::string_view(snapshot_str));

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result_map;
    ASSERT_EQ(
        component->Do(nullptr, input_map.Get(), result_map.Put()),
        DAS_S_OK);

    auto status = Das::Utils::GetPortString(result_map.Get(), "status");
    EXPECT_EQ(status, "failed");

    child_factory->Release();
}

TEST_F(TaskComponentContractTest, RepositoryInvokeReportsMalformedChildResult)
{
    auto state = std::make_shared<RecordingChildState>();
    state->result_json = R"json([])json";
    auto* child_factory = RegisterRecordingChildFactory(state);

    auto component = CreateComponent(REPOSITORY_INVOKE_COMPONENT_GUID);
    auto input = MakeRepositoryInvokeInput(FAKE_CHILD_COMPONENT_GUID);
    auto input_obj = input.as_object();
    ASSERT_TRUE(input_obj.has_value());
    auto snapshot_val = (*input_obj)[std::string_view("compiledSnapshot")];
    auto snapshot_str = snapshot_val.write(yyjson::WriteFlag::NoFlag);

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
    Das::Utils::SetPortString(
        input_map.Get(),
        "compiledSnapshot",
        std::string_view(snapshot_str));

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result_map;
    ASSERT_EQ(
        component->Do(nullptr, input_map.Get(), result_map.Put()),
        DAS_S_OK);

    auto status = Das::Utils::GetPortString(result_map.Get(), "status");
    EXPECT_EQ(status, "failed");

    child_factory->Release();
}

TEST_F(TaskComponentContractTest, RepositoryInvokeRejectsInvalidSnapshotVersion)
{
    auto component = CreateComponent(REPOSITORY_INVOKE_COMPONENT_GUID);
    ASSERT_NE(component.Get(), nullptr);

    auto input = MakeRepositoryInvokeInput(FAKE_CHILD_COMPONENT_GUID, 2);
    auto input_obj = input.as_object();
    ASSERT_TRUE(input_obj.has_value());
    auto snapshot_val = (*input_obj)[std::string_view("compiledSnapshot")];
    auto snapshot_str = snapshot_val.write(yyjson::WriteFlag::NoFlag);

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
    Das::Utils::SetPortString(
        input_map.Get(),
        "compiledSnapshot",
        std::string_view(snapshot_str));

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result_map;
    ASSERT_EQ(
        component->Do(nullptr, input_map.Get(), result_map.Put()),
        DAS_S_OK);

    auto status = Das::Utils::GetPortString(result_map.Get(), "status");
    EXPECT_EQ(status, "failed");
}

TEST_F(TaskComponentContractTest, RepositoryInvokeHonorsAlreadyRequestedStop)
{
    auto      state = std::make_shared<RecordingChildState>();
    auto*     child_factory = RegisterRecordingChildFactory(state);
    const int create_count_before = child_factory->create_call_count;

    auto          component = CreateComponent(REPOSITORY_INVOKE_COMPONENT_GUID);
    FakeStopToken stop_token(true);
    auto          input = MakeRepositoryInvokeInput(FAKE_CHILD_COMPONENT_GUID);
    auto          input_obj = input.as_object();
    ASSERT_TRUE(input_obj.has_value());
    auto snapshot_val = (*input_obj)[std::string_view("compiledSnapshot")];
    auto snapshot_str = snapshot_val.write(yyjson::WriteFlag::NoFlag);

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
    Das::Utils::SetPortString(
        input_map.Get(),
        "compiledSnapshot",
        std::string_view(snapshot_str));

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> result_map;
    ASSERT_EQ(
        component->Do(&stop_token, input_map.Get(), result_map.Put()),
        DAS_S_OK);

    auto status = Das::Utils::GetPortString(result_map.Get(), "status");
    EXPECT_EQ(status, "cancelled");
    EXPECT_EQ(child_factory->create_call_count, create_count_before);
    EXPECT_EQ(state->do_call_count, 0);

    child_factory->Release();
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

    auto make_input = [&]()
    {
        DAS::DasPtr<Das::ExportInterface::IDasPortMap> input_map;
        EXPECT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
        Das::Utils::SetPortInt(input_map.Get(), "start", 0);
        Das::Utils::SetPortInt(input_map.Get(), "end", 3);
        Das::Utils::SetPortInt(input_map.Get(), "step", 1);
        return input_map;
    };

    // start=0, end=3, step=1 → continue with index 0,1,2.
    const std::array<int64_t, 3> expected_indices{0, 1, 2};
    for (auto idx : expected_indices)
    {
        auto                                           in = make_input();
        DAS::DasPtr<Das::ExportInterface::IDasPortMap> result_map;
        ASSERT_EQ(component->Do(nullptr, in.Get(), result_map.Put()), DAS_S_OK);
        EXPECT_EQ(
            Das::Utils::GetPortString(result_map.Get(), "status"),
            "completed");
        EXPECT_EQ(FirstEmittedSignal(result_map.Get()), "continue");
        EXPECT_EQ(Das::Utils::GetPortInt(result_map.Get(), "index"), idx);
    }

    // 4th call: range exhausted → break, no index output.
    auto                                           in = make_input();
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

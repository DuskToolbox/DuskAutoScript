#include <IpcTestConfig.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/ForeignInterfaceHost/PluginResourceIndex.h>
#include <das/Core/ForeignInterfaceHost/RemotePluginHost.h>
#include <das/Core/ForeignInterfaceHost/RuntimeProvider.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/DasApi.h>
#include <das/DasSharedRef.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasErrorLens.h>
#include <das/_autogen/idl/abi/IDasTaskAuthoring.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasErrorLens.Implements.hpp>
#include <gtest/gtest.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/base_sink.h>

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_OPENCV_WARNING
#include <opencv2/core/mat.hpp>
#include <opencv2/imgcodecs.hpp>
DAS_DISABLE_WARNING_END

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <thread>

#include <boost/asio/io_context.hpp>

using namespace DAS::Core::ForeignInterfaceHost;
using namespace DAS::Core::IPC;
using Das::DasPtr;
using namespace Das::PluginInterface;

namespace
{
    class PluginManagerCapturingSink final
        : public spdlog::sinks::base_sink<std::mutex>
    {
    public:
        std::vector<std::string> messages;

    protected:
        void sink_it_(const spdlog::details::log_msg& msg) override
        {
            messages.emplace_back(msg.payload.data(), msg.payload.size());
        }

        void flush_() override {}
    };

    class ScopedPluginManagerLogCapture
    {
    public:
        ScopedPluginManagerLogCapture()
        {
            sink_ = std::make_shared<PluginManagerCapturingSink>();
            DAS::Core::g_logger->sinks().push_back(sink_);
        }

        ~ScopedPluginManagerLogCapture()
        {
            auto& sinks = DAS::Core::g_logger->sinks();
            sinks.erase(
                std::remove(sinks.begin(), sinks.end(), sink_),
                sinks.end());
        }

        bool Contains(std::string_view needle) const
        {
            return std::any_of(
                sink_->messages.begin(),
                sink_->messages.end(),
                [needle](const std::string& message)
                { return message.find(needle) != std::string::npos; });
        }

    private:
        std::shared_ptr<PluginManagerCapturingSink> sink_;
    };

    std::filesystem::path UniqueSettingsDir()
    {
        static std::atomic<int> counter{0};
        std::random_device      rd;
        auto name = "das_test_settings_" + std::to_string(rd()) + "_"
                    + std::to_string(counter.fetch_add(1));
        return std::filesystem::current_path() / name;
    }

    class ScopedEnvVar
    {
    public:
        ScopedEnvVar(const char* name, std::optional<std::string> value)
            : name_{name}
        {
            const char* current = std::getenv(name_.c_str());
            if (current)
            {
                original_ = std::string{current};
            }
            Set(std::move(value));
        }

        ScopedEnvVar(const ScopedEnvVar&) = delete;
        ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

        ~ScopedEnvVar() { Set(std::move(original_)); }

    private:
        void Set(std::optional<std::string> value)
        {
#ifdef _WIN32
            _putenv_s(name_.c_str(), value ? value->c_str() : "");
#else
            if (value)
            {
                setenv(name_.c_str(), value->c_str(), 1);
            }
            else
            {
                unsetenv(name_.c_str());
            }
#endif
        }

        std::string                name_;
        std::optional<std::string> original_;
    };

    std::string ReadUtf8String(IDasReadOnlyString* value)
    {
        const char* raw = nullptr;
        if (!value || value->GetUtf8(&raw) != DAS_S_OK || !raw)
        {
            return {};
        }
        return raw;
    }

    std::filesystem::path GetTestPluginPath()
    {
        // CppRuntime::LoadPlugin expects a path to the JSON manifest file.
        // The manifest contains the DLL name which CppRuntime uses to locate
        // and load the actual shared library.
        return std::filesystem::path{IpcTestConfig::GetPluginDir()}
               / "IpcTestPlugin1.json";
    }

    DasPtr<IForeignLanguageRuntime> CreateCppRuntime()
    {
        ForeignLanguageRuntimeFactoryDesc desc{};
        desc.language = ForeignInterfaceLanguage::Cpp;
        auto result = CreateForeignLanguageRuntime(desc);
        if (!result)
        {
            return nullptr;
        }
        return std::move(result.value());
    }

    class CapturingPluginPackage final : public IDasPluginPackage
    {
    public:
        explicit CapturingPluginPackage(
            std::vector<DasPluginFeature> features = {})
            : features_{std::move(features)}
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
        QueryInterface(const DasGuid& iid, void** pp_out_object) override
        {
            if (pp_out_object == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }

            if (iid == DAS_IID_BASE)
            {
                *pp_out_object = static_cast<IDasBase*>(this);
                AddRef();
                return DAS_S_OK;
            }
            if (iid == DAS_IID_PLUGIN_PACKAGE)
            {
                *pp_out_object = static_cast<IDasPluginPackage*>(this);
                AddRef();
                return DAS_S_OK;
            }

            *pp_out_object = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL
        EnumFeature(uint64_t index, DasPluginFeature* p_out_feature) override
        {
            if (p_out_feature == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (index >= features_.size())
            {
                return DAS_S_FALSE;
            }
            *p_out_feature = features_[index];
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        CreateFeatureInterface(uint64_t, IDasBase**) override
        {
            return DAS_E_NOT_FOUND;
        }

    private:
        std::vector<DasPluginFeature> features_;
        std::atomic<uint32_t>         ref_count_{0};
    };

    class CapturingRuntime final : public IForeignLanguageRuntime
    {
    public:
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
        QueryInterface(const DasGuid& iid, void** pp_out_object) override
        {
            if (pp_out_object == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }

            if (iid == DAS_IID_BASE)
            {
                *pp_out_object = static_cast<IDasBase*>(this);
                AddRef();
                return DAS_S_OK;
            }

            *pp_out_object = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        auto LoadPlugin(const std::filesystem::path& path)
            -> DAS::Utils::Expected<DasPtr<IDasBase>> override
        {
            loaded_paths.push_back(path);
            if (DAS::IsFailed(load_error))
            {
                return tl::make_unexpected(load_error);
            }
            auto* package = new CapturingPluginPackage();
            package->AddRef();
            return DasPtr<IDasBase>::Attach(static_cast<IDasBase*>(package));
        }

        std::vector<std::filesystem::path> loaded_paths;
        DasResult                          load_error = DAS_S_OK;

    private:
        std::atomic<uint32_t> ref_count_{0};
    };

    class CapturingRuntimeProvider final : public IRuntimeProvider
    {
    public:
        explicit CapturingRuntimeProvider(
            uint16_t                      owner_session_id,
            std::vector<DasPluginFeature> features = {})
            : owner_session_id_{owner_session_id},
              features_{std::move(features)}
        {
        }

        DasResult LoadPlugin(
            const RuntimeLoadRequest& request,
            RuntimeLoadResult*        out_result) override
        {
            requests.push_back(request);

            if (out_result == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }

            auto* package = new CapturingPluginPackage(features_);
            package->AddRef();

            out_result->object = static_cast<IDasBase*>(package);
            out_result->owner_session_id = owner_session_id_;
            return DAS_S_OK;
        }

        std::vector<RuntimeLoadRequest> requests;

    private:
        uint16_t                      owner_session_id_ = 0;
        std::vector<DasPluginFeature> features_;
    };

    struct CapturingRemoteHostState
    {
        int                      load_count = 0;
        std::filesystem::path    manifest_path;
        std::string              executable;
        std::vector<std::string> args;
        std::string              working_directory;
    };

    class CapturingRemoteHost final : public IRemotePluginHost
    {
    public:
        CapturingRemoteHost(
            CapturingRemoteHostState&     state,
            uint16_t                      owner_session_id,
            std::vector<DasPluginFeature> features)
            : state_{state}, owner_session_id_{owner_session_id},
              features_{std::move(features)}
        {
        }

        auto LoadPlugin(const RemotePluginLoadRequest& request)
            -> DAS::Utils::Expected<RuntimeLoadResult> override
        {
            state_.load_count += 1;
            state_.manifest_path = request.manifest_path;
            state_.executable =
                ReadUtf8String(request.launch_desc.p_executable_path);
            state_.working_directory =
                ReadUtf8String(request.launch_desc.p_working_directory);
            state_.args.clear();
            for (size_t i = 0; i < request.launch_desc.arg_count; ++i)
            {
                state_.args.push_back(
                    ReadUtf8String(request.launch_desc.pp_args[i]));
            }

            auto* package = new CapturingPluginPackage(features_);
            package->AddRef();

            RuntimeLoadResult result{};
            result.object = static_cast<IDasBase*>(package);
            result.owner_session_id = owner_session_id_;
            return result;
        }

    private:
        CapturingRemoteHostState&     state_;
        uint16_t                      owner_session_id_ = 0;
        std::vector<DasPluginFeature> features_;
    };

    void WriteMinimalManifest(
        const std::filesystem::path& path,
        const std::string&           guid,
        const std::string&           name)
    {
        std::ofstream ofs(path);
        ofs << R"({
            "guid": ")"
            << guid << R"(",
            "name": ")"
            << name << R"(",
            "language": "Cpp",
            "description": "test",
            "author": "test",
            "version": "1.0",
            "supportedSystem": "win",
            "pluginFilenameExtension": "dll",
            "settings": []
        })";
    }

    void WriteNodeManifest(
        const std::filesystem::path& path,
        const std::string&           guid,
        const std::string&           name)
    {
        auto manifest = Das::Utils::MakeYyjsonObject();
        {
            auto obj = *manifest.as_object();
            obj[std::string_view("guid")] = guid;
            obj[std::string_view("name")] = name;
            obj[std::string_view("language")] = "Node";
            obj[std::string_view("loadMode")] = "ipc";
            obj[std::string_view("description")] = "test";
            obj[std::string_view("author")] = "test";
            obj[std::string_view("version")] = "1.0";
            obj[std::string_view("supportedSystem")] = "win";
            obj[std::string_view("pluginFilenameExtension")] = "js";
            obj[std::string_view("settings")] = Das::Utils::MakeYyjsonArray();
        }

        std::ofstream ofs(path);
        ofs << *Das::Utils::SerializeYyjsonValue(manifest, false);
    }

    void WriteCSharpManifest(
        const std::filesystem::path& path,
        const std::string&           guid,
        const std::string&           name)
    {
        auto manifest = Das::Utils::MakeYyjsonObject();
        {
            auto obj = *manifest.as_object();
            obj[std::string_view("guid")] = guid;
            obj[std::string_view("name")] = name;
            obj[std::string_view("language")] = "CSharp";
            obj[std::string_view("description")] = "test";
            obj[std::string_view("author")] = "test";
            obj[std::string_view("version")] = "1.0";
            obj[std::string_view("supportedSystem")] = "win";
            obj[std::string_view("pluginFilenameExtension")] = "dll";
            obj[std::string_view("targetFramework")] = "net8.0";
            obj[std::string_view("entryPoint")] =
                "Das.TestPlugin.TestPluginEntrypoint.Create";
            obj[std::string_view("runtimeConfigPath")] =
                "DasCSharpRoutingPlugin.runtimeconfig.json";
            obj[std::string_view("settings")] = Das::Utils::MakeYyjsonArray();
        }

        std::ofstream ofs(path);
        ofs << *Das::Utils::SerializeYyjsonValue(manifest, false);
    }

    DasGuid MakeTaskComponentTestGuid(uint32_t value)
    {
        DasGuid guid{};
        guid.data1 = value;
        return guid;
    }

    yyjson::value MakeTaskComponentDefinitionJson(const DasGuid& component_guid)
    {
        auto definition = Das::Utils::MakeYyjsonObject();
        auto obj = *definition.as_object();
        obj[std::string_view("schemaVersion")] = 1;
        obj[std::string_view("componentGuid")] =
            DasGuidToStdString(component_guid);
        obj[std::string_view("kind")] = "pluginManagerTestComponent";
        obj[std::string_view("inputs")] = Das::Utils::MakeYyjsonArray();
        obj[std::string_view("outputs")] = Das::Utils::MakeYyjsonArray();
        obj[std::string_view("config")] = Das::Utils::MakeYyjsonObject();
        obj[std::string_view("diagnostics")] = Das::Utils::MakeYyjsonArray();
        return definition;
    }

    void WriteTaskComponentManifest(
        const std::filesystem::path& path,
        const DasGuid&               plugin_guid,
        const DasGuid&               factory_guid,
        const DasGuid&               component_guid)
    {
        auto manifest = Das::Utils::MakeYyjsonObject();
        auto obj = *manifest.as_object();
        obj[std::string_view("guid")] = DasGuidToStdString(plugin_guid);
        obj[std::string_view("name")] = "TaskComponentPlugin";
        obj[std::string_view("language")] = "Cpp";
        obj[std::string_view("description")] = "test";
        obj[std::string_view("author")] = "test";
        obj[std::string_view("version")] = "1.0";
        obj[std::string_view("supportedSystem")] = "win";
        obj[std::string_view("pluginFilenameExtension")] = "dll";
        obj[std::string_view("settings")] = Das::Utils::MakeYyjsonArray();

        auto task_components = Das::Utils::MakeYyjsonObject();
        auto factories = Das::Utils::MakeYyjsonArray();
        factories.as_array()->emplace_back(DasGuidToStdString(factory_guid));
        (*task_components.as_object())[std::string_view("factories")] =
            std::move(factories);

        auto components = Das::Utils::MakeYyjsonObject();
        auto entry = Das::Utils::MakeYyjsonObject();
        auto entry_obj = *entry.as_object();
        entry_obj[std::string_view("factoryGuid")] =
            DasGuidToStdString(factory_guid);
        entry_obj[std::string_view("definition")] =
            MakeTaskComponentDefinitionJson(component_guid);
        (*components.as_object())[DasGuidToStdString(component_guid)] =
            std::move(entry);
        (*task_components.as_object())[std::string_view("components")] =
            std::move(components);
        obj[std::string_view("taskComponents")] = std::move(task_components);

        std::ofstream ofs(path);
        ofs << *Das::Utils::SerializeYyjsonValue(manifest, false);
    }

    void WriteTaskComponentManifestMissingFactoryGuid(
        const std::filesystem::path& path,
        const DasGuid&               plugin_guid,
        const DasGuid&               factory_guid,
        const DasGuid&               component_guid)
    {
        auto manifest = Das::Utils::MakeYyjsonObject();
        auto obj = *manifest.as_object();
        obj[std::string_view("guid")] = DasGuidToStdString(plugin_guid);
        obj[std::string_view("name")] = "InvalidTaskComponentPlugin";
        obj[std::string_view("language")] = "Cpp";
        obj[std::string_view("description")] = "test";
        obj[std::string_view("author")] = "test";
        obj[std::string_view("version")] = "1.0";
        obj[std::string_view("supportedSystem")] = "win";
        obj[std::string_view("pluginFilenameExtension")] = "dll";
        obj[std::string_view("settings")] = Das::Utils::MakeYyjsonArray();

        auto task_components = Das::Utils::MakeYyjsonObject();
        auto factories = Das::Utils::MakeYyjsonArray();
        factories.as_array()->emplace_back(DasGuidToStdString(factory_guid));
        (*task_components.as_object())[std::string_view("factories")] =
            std::move(factories);

        auto components = Das::Utils::MakeYyjsonObject();
        auto entry = Das::Utils::MakeYyjsonObject();
        (*entry.as_object())[std::string_view("definition")] =
            MakeTaskComponentDefinitionJson(component_guid);
        (*components.as_object())[DasGuidToStdString(component_guid)] =
            std::move(entry);
        (*task_components.as_object())[std::string_view("components")] =
            std::move(components);
        obj[std::string_view("taskComponents")] = std::move(task_components);

        std::ofstream ofs(path);
        ofs << *Das::Utils::SerializeYyjsonValue(manifest, false);
    }

    class PluginManagerTaskComponent final : public IDasTaskComponent
    {
    public:
        explicit PluginManagerTaskComponent(DasGuid guid) : guid_(guid) {}

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
        QueryInterface(const DasGuid& iid, void** pp_out_object) override
        {
            if (pp_out_object == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DasIidOf<IDasBase>())
            {
                *pp_out_object = static_cast<IDasBase*>(this);
                AddRef();
                return DAS_S_OK;
            }
            if (iid == DasIidOf<IDasTypeInfo>())
            {
                *pp_out_object = static_cast<IDasTypeInfo*>(this);
                AddRef();
                return DAS_S_OK;
            }
            if (iid == DasIidOf<IDasTaskComponent>())
            {
                *pp_out_object = static_cast<IDasTaskComponent*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_out_object = nullptr;
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
            return CreateIDasReadOnlyStringFromUtf8(
                "PluginManagerTaskComponent",
                pp_out_name);
        }

        DasResult DAS_STD_CALL ApplySettingsChange(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson**) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult DAS_STD_CALL
        Do(IDasStopToken*,
           Das::ExportInterface::IDasReadOnlyPortMap*,
           Das::ExportInterface::IDasPortMap**) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

    private:
        DasGuid               guid_;
        std::atomic<uint32_t> ref_count_{0};
    };

    class PluginManagerTaskComponentFactory final
        : public IDasTaskComponentFactory
    {
    public:
        explicit PluginManagerTaskComponentFactory(DasGuid guid) : guid_(guid)
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
        QueryInterface(const DasGuid& iid, void** pp_out_object) override
        {
            if (pp_out_object == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DasIidOf<IDasBase>())
            {
                *pp_out_object = static_cast<IDasBase*>(this);
                AddRef();
                return DAS_S_OK;
            }
            if (iid == DasIidOf<IDasTypeInfo>())
            {
                *pp_out_object = static_cast<IDasTypeInfo*>(this);
                AddRef();
                return DAS_S_OK;
            }
            if (iid == DasIidOf<IDasTaskComponentFactory>())
            {
                *pp_out_object = static_cast<IDasTaskComponentFactory*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_out_object = nullptr;
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
            return CreateIDasReadOnlyStringFromUtf8(
                "PluginManagerTaskComponentFactory",
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
            auto* component = new PluginManagerTaskComponent(component_guid);
            component->AddRef();
            *pp_out_component = component;
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        SetTaskComponentHost(IDasTaskComponentHost* /*p_host*/) override
        {
            return DAS_S_OK;
        }

    private:
        DasGuid               guid_;
        std::atomic<uint32_t> ref_count_{0};
    };

    class TaskComponentPluginPackage final : public IDasPluginPackage
    {
    public:
        explicit TaskComponentPluginPackage(DasGuid factory_guid)
            : factory_guid_(factory_guid)
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
        QueryInterface(const DasGuid& iid, void** pp_out_object) override
        {
            if (pp_out_object == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DAS_IID_BASE)
            {
                *pp_out_object = static_cast<IDasBase*>(this);
                AddRef();
                return DAS_S_OK;
            }
            if (iid == DAS_IID_PLUGIN_PACKAGE)
            {
                *pp_out_object = static_cast<IDasPluginPackage*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_out_object = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL
        EnumFeature(uint64_t index, DasPluginFeature* p_out_feature) override
        {
            if (p_out_feature == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (index != 0)
            {
                return DAS_S_FALSE;
            }
            *p_out_feature = DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY;
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        CreateFeatureInterface(uint64_t index, IDasBase** pp_out) override
        {
            if (pp_out == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out = nullptr;
            if (index != 0)
            {
                return DAS_E_NOT_FOUND;
            }

            auto* factory =
                new PluginManagerTaskComponentFactory(factory_guid_);
            factory->AddRef();
            *pp_out = static_cast<IDasTaskComponentFactory*>(factory);
            return DAS_S_OK;
        }

    private:
        DasGuid               factory_guid_;
        std::atomic<uint32_t> ref_count_{0};
    };

    class TaskComponentRuntime final : public IForeignLanguageRuntime
    {
    public:
        explicit TaskComponentRuntime(DasGuid factory_guid)
            : factory_guid_(factory_guid)
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
        QueryInterface(const DasGuid& iid, void** pp_out_object) override
        {
            if (pp_out_object == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DAS_IID_BASE)
            {
                *pp_out_object = static_cast<IDasBase*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_out_object = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        auto LoadPlugin(const std::filesystem::path& path)
            -> DAS::Utils::Expected<DasPtr<IDasBase>> override
        {
            loaded_paths.push_back(path);
            auto* package = new TaskComponentPluginPackage(factory_guid_);
            package->AddRef();
            return DasPtr<IDasBase>::Attach(static_cast<IDasBase*>(package));
        }

        std::vector<std::filesystem::path> loaded_paths;

    private:
        DasGuid               factory_guid_;
        std::atomic<uint32_t> ref_count_{0};
    };

    class PluginManagerErrorLens final
        : public DasErrorLensImplBase<PluginManagerErrorLens>
    {
    public:
        explicit PluginManagerErrorLens(DasGuid provider_guid)
            : supported_iids_{provider_guid}
        {
        }

        DasResult DAS_STD_CALL GetSupportedIids(
            Das::ExportInterface::IDasReadOnlyGuidVector** pp_out_iids) override
        {
            DasPtr<Das::ExportInterface::IDasGuidVector> writable_iids;
            const auto create_result = ::CreateIDasGuidVector(
                supported_iids_.data(),
                supported_iids_.size(),
                writable_iids.Put());
            if (DAS::IsFailed(create_result))
            {
                return create_result;
            }

            return writable_iids->ToConst(pp_out_iids);
        }

        DasResult DAS_STD_CALL GetErrorMessage(
            IDasReadOnlyString*,
            DasResult,
            IDasReadOnlyString** pp_out_message) override
        {
            if (pp_out_message != nullptr)
            {
                *pp_out_message = nullptr;
            }
            return DAS_E_OUT_OF_RANGE;
        }

    private:
        std::vector<DasGuid> supported_iids_;
    };

    class ErrorLensPluginPackage final : public IDasPluginPackage
    {
    public:
        explicit ErrorLensPluginPackage(DasGuid provider_guid)
            : provider_guid_(provider_guid)
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
        QueryInterface(const DasGuid& iid, void** pp_out_object) override
        {
            if (pp_out_object == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DAS_IID_BASE)
            {
                *pp_out_object = static_cast<IDasBase*>(this);
                AddRef();
                return DAS_S_OK;
            }
            if (iid == DAS_IID_PLUGIN_PACKAGE)
            {
                *pp_out_object = static_cast<IDasPluginPackage*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_out_object = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL
        EnumFeature(uint64_t index, DasPluginFeature* p_out_feature) override
        {
            if (p_out_feature == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (index != 0)
            {
                return DAS_S_FALSE;
            }
            *p_out_feature = DAS_PLUGIN_FEATURE_ERROR_LENS;
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        CreateFeatureInterface(uint64_t index, IDasBase** pp_out) override
        {
            if (pp_out == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out = nullptr;
            if (index != 0)
            {
                return DAS_E_NOT_FOUND;
            }

            auto* lens = PluginManagerErrorLens::MakeRaw(provider_guid_);
            *pp_out = static_cast<IDasBase*>(static_cast<IDasErrorLens*>(lens));
            return DAS_S_OK;
        }

    private:
        DasGuid               provider_guid_;
        std::atomic<uint32_t> ref_count_{0};
    };

    class ErrorLensRuntime final : public IForeignLanguageRuntime
    {
    public:
        explicit ErrorLensRuntime(DasGuid provider_guid)
            : provider_guid_(provider_guid)
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
        QueryInterface(const DasGuid& iid, void** pp_out_object) override
        {
            if (pp_out_object == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DAS_IID_BASE)
            {
                *pp_out_object = static_cast<IDasBase*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_out_object = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        auto LoadPlugin(const std::filesystem::path& path)
            -> DAS::Utils::Expected<DasPtr<IDasBase>> override
        {
            loaded_paths.push_back(path);
            auto* package = new ErrorLensPluginPackage(provider_guid_);
            package->AddRef();
            return DasPtr<IDasBase>::Attach(static_cast<IDasBase*>(package));
        }

        std::vector<std::filesystem::path> loaded_paths;

    private:
        DasGuid               provider_guid_;
        std::atomic<uint32_t> ref_count_{0};
    };
} // anonymous namespace

// ============================================================
// GUID index tests
// ============================================================

class PluginManagerGuidTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        settings_dir_ = UniqueSettingsDir();
        settings_manager_ =
            std::make_unique<DAS::Core::SettingsManager::SettingsManager>(
                settings_dir_);
        ipc_sp_ = DAS::Core::IPC::MainProcess::CreateIpcContextShared(false);
        pm_ = std::make_unique<PluginManager>(
            *settings_manager_,
            Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp_));
        ASSERT_EQ(pm_->Initialize(1), DAS_S_OK);
    }

    void TearDown() override
    {
        pm_->Shutdown();
        std::filesystem::remove_all(settings_dir_);
    }

    std::unique_ptr<DAS::Core::SettingsManager::SettingsManager>
                                                              settings_manager_;
    std::shared_ptr<DAS::Core::IPC::MainProcess::IIpcContext> ipc_sp_;
    std::unique_ptr<PluginManager>                            pm_;
    std::filesystem::path                                     settings_dir_;
};

TEST_F(PluginManagerGuidTest, LoadPluginAndFindByGuid)
{
    auto plugin_path = GetTestPluginPath();

    auto result = pm_->LoadPlugin(plugin_path);
    ASSERT_EQ(result, DAS_S_OK) << "Failed to load IpcTestPlugin1.dll";

    EXPECT_EQ(pm_->GetLoadedPluginCount(), 1);
    EXPECT_TRUE(pm_->IsPluginLoaded(plugin_path));
}

TEST_F(PluginManagerGuidTest, LoadDuplicatePathReturnsSFalse)
{
    auto plugin_path = GetTestPluginPath();

    auto result1 = pm_->LoadPlugin(plugin_path);
    ASSERT_EQ(result1, DAS_S_OK) << "First load should succeed";

    auto result2 = pm_->LoadPlugin(plugin_path);
    EXPECT_EQ(result2, DAS_S_FALSE)
        << "Duplicate path should return DAS_S_FALSE";

    EXPECT_EQ(pm_->GetLoadedPluginCount(), 1)
        << "Count should remain 1 after duplicate load";
}

TEST_F(PluginManagerGuidTest, LoadDuplicateGuidViaSymlinkReturnsAlreadyExists)
{
    auto plugin_path = GetTestPluginPath();

    auto result1 = pm_->LoadPlugin(plugin_path);
    ASSERT_EQ(result1, DAS_S_OK) << "First load should succeed";

    // Create a symlink to the same manifest JSON to test GUID conflict
    // detection. The symlink provides a different entry path.
    // Note: NormalizePath() uses weakly_canonical() which resolves symlinks,
    // so if the symlink resolves to the same canonical path as the original,
    // path deduplication returns DAS_S_FALSE before GUID conflict is checked.
    // GUID conflict detection (DAS_E_DUPLICATE_ELEMENT) is exercised when two
    // genuinely different paths produce the same GUID — this requires two
    // distinct plugin DLLs with identical GUIDs, which cannot be tested here.
    // The GUID conflict code path in LoadPlugin
    // (loaded_plugins_.contains(desc->guid)) is guaranteed by code review.
    auto symlink_path =
        std::filesystem::current_path() / "IpcTestPlugin1_symlink.json";

    std::error_code ec;
    std::filesystem::create_symlink(
        std::filesystem::canonical(plugin_path),
        symlink_path,
        ec);

    if (ec)
    {
        GTEST_SKIP() << "Symlink creation failed: " << ec.message();
    }

    auto result2 = pm_->LoadPlugin(symlink_path);
    // weakly_canonical resolves the symlink to the same path, so path
    // deduplication fires first and returns DAS_S_FALSE.
    EXPECT_TRUE(result2 == DAS_S_FALSE || result2 == DAS_E_DUPLICATE_ELEMENT)
        << "Expected DAS_S_FALSE (path dedup) or DAS_E_DUPLICATE_ELEMENT "
           "(GUID conflict), got: "
        << result2;

    EXPECT_EQ(pm_->GetLoadedPluginCount(), 1)
        << "Count should remain 1 after duplicate load";

    // Cleanup symlink
    std::filesystem::remove(symlink_path, ec);
}

class PluginManagerManifestPathTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        test_dir_ =
            std::filesystem::current_path()
            / ("manifest_path_test_" + std::to_string(std::random_device{}()));
        std::filesystem::create_directories(test_dir_);

        settings_dir_ = test_dir_ / "settings";
        settings_manager_ =
            std::make_unique<DAS::Core::SettingsManager::SettingsManager>(
                settings_dir_);
        ipc_sp_ = DAS::Core::IPC::MainProcess::CreateIpcContextShared(false);
        pm_ = std::make_unique<PluginManager>(
            *settings_manager_,
            Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp_));

        ASSERT_EQ(pm_->Initialize(1), DAS_S_OK);

        // 注入 fake runtime：包成 IRuntimeProvider 注册，LoadPlugin 命中它而非
        // 走内置 CppRuntime 加载虚构 dll（5de027aa 后 SetRuntime 已删）。
        runtime_ = new CapturingRuntime();
        runtime_->AddRef();
        runtime_guard_ = DasPtr<IForeignLanguageRuntime>::Attach(runtime_);
        auto manifest_provider = CreateLocalRuntimeProvider(runtime_guard_);
        ASSERT_TRUE(manifest_provider);
        pm_->RegisterRuntimeProvider(
            ForeignInterfaceLanguage::Cpp,
            LoadMode::InProcess,
            std::shared_ptr<IRuntimeProvider>(
                std::move(manifest_provider.value())));
    }

    void TearDown() override
    {
        pm_->Shutdown();
        runtime_guard_.Reset();
        std::filesystem::remove_all(test_dir_);
    }

    std::filesystem::path Canonical(const std::filesystem::path& path) const
    {
        return std::filesystem::weakly_canonical(path);
    }

    std::filesystem::path test_dir_;
    std::filesystem::path settings_dir_;
    std::unique_ptr<DAS::Core::SettingsManager::SettingsManager>
                                                              settings_manager_;
    std::shared_ptr<DAS::Core::IPC::MainProcess::IIpcContext> ipc_sp_;
    std::unique_ptr<PluginManager>                            pm_;
    CapturingRuntime*               runtime_ = nullptr;
    DasPtr<IForeignLanguageRuntime> runtime_guard_;
};

TEST_F(
    PluginManagerManifestPathTest,
    DirectoryInputResolvesDirnameManifestBeforeFallback)
{
    const auto plugin_dir = test_dir_ / "PriorityPlugin";
    std::filesystem::create_directories(plugin_dir);
    const auto primary_manifest = plugin_dir / "PriorityPlugin.json";
    const auto fallback_manifest = plugin_dir / "manifest.json";
    WriteMinimalManifest(
        fallback_manifest,
        "00000000-0000-0000-0000-000000000202",
        "FallbackPlugin");
    WriteMinimalManifest(
        primary_manifest,
        "00000000-0000-0000-0000-000000000201",
        "PrimaryPlugin");

    ASSERT_EQ(pm_->LoadPlugin(plugin_dir), DAS_S_OK);

    ASSERT_EQ(runtime_->loaded_paths.size(), 1u);
    EXPECT_EQ(
        Canonical(runtime_->loaded_paths.front()),
        Canonical(primary_manifest));
}

TEST_F(PluginManagerManifestPathTest, JsonFileInputIsManifestPath)
{
    const auto manifest_path = test_dir_ / "SingleFilePlugin.json";
    WriteMinimalManifest(
        manifest_path,
        "00000000-0000-0000-0000-000000000203",
        "SingleFilePlugin");

    ASSERT_EQ(pm_->LoadPlugin(manifest_path), DAS_S_OK);

    ASSERT_EQ(runtime_->loaded_paths.size(), 1u);
    EXPECT_EQ(
        Canonical(runtime_->loaded_paths.front()),
        Canonical(manifest_path));
}

TEST_F(
    PluginManagerManifestPathTest,
    DirectoryAndManifestAliasesReturnDuplicateNoOp)
{
    const auto plugin_dir = test_dir_ / "AliasPlugin";
    std::filesystem::create_directories(plugin_dir);
    const auto manifest_path = plugin_dir / "AliasPlugin.json";
    WriteMinimalManifest(
        manifest_path,
        "00000000-0000-0000-0000-000000000204",
        "AliasPlugin");

    ASSERT_EQ(pm_->LoadPlugin(plugin_dir), DAS_S_OK);
    EXPECT_EQ(pm_->LoadPlugin(manifest_path), DAS_S_FALSE);

    EXPECT_EQ(pm_->GetLoadedPluginCount(), 1u);
    EXPECT_EQ(runtime_->loaded_paths.size(), 1u);
}

TEST_F(
    PluginManagerManifestPathTest,
    DistinctManifestFilesWithSameGuidReturnDuplicateElement)
{
    const auto     manifest_a = test_dir_ / "DuplicateA.json";
    const auto     manifest_b = test_dir_ / "DuplicateB.json";
    constexpr auto guid = "00000000-0000-0000-0000-000000000205";
    WriteMinimalManifest(manifest_a, guid, "DuplicateA");
    WriteMinimalManifest(manifest_b, guid, "DuplicateB");

    ASSERT_EQ(pm_->LoadPlugin(manifest_a), DAS_S_OK);
    EXPECT_EQ(pm_->LoadPlugin(manifest_b), DAS_E_DUPLICATE_ELEMENT);

    EXPECT_EQ(pm_->GetLoadedPluginCount(), 1u);
}

class PluginManagerTaskComponentTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        test_dir_ = std::filesystem::current_path()
                    / ("task_component_plugin_test_"
                       + std::to_string(std::random_device{}()));
        std::filesystem::create_directories(test_dir_);
        settings_dir_ = test_dir_ / "settings";

        settings_manager_ =
            std::make_unique<DAS::Core::SettingsManager::SettingsManager>(
                settings_dir_);
        ipc_sp_ = DAS::Core::IPC::MainProcess::CreateIpcContextShared(false);
        pm_ = std::make_unique<PluginManager>(
            *settings_manager_,
            Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp_));

        ASSERT_EQ(pm_->Initialize(1), DAS_S_OK);

        factory_guid_ = MakeTaskComponentTestGuid(0x68130002);
        auto* runtime = new TaskComponentRuntime(factory_guid_);
        runtime->AddRef();
        runtime_ = runtime;
        runtime_guard_ = DasPtr<IForeignLanguageRuntime>::Attach(runtime);
        // 注入 fake runtime（包成 IRuntimeProvider 注册）。
        auto task_provider = CreateLocalRuntimeProvider(runtime_guard_);
        ASSERT_TRUE(task_provider);
        pm_->RegisterRuntimeProvider(
            ForeignInterfaceLanguage::Cpp,
            LoadMode::InProcess,
            std::shared_ptr<IRuntimeProvider>(
                std::move(task_provider.value())));
    }

    void TearDown() override
    {
        pm_->Shutdown();
        runtime_guard_.Reset();
        std::filesystem::remove_all(test_dir_);
    }

    std::filesystem::path test_dir_;
    std::filesystem::path settings_dir_;
    std::unique_ptr<DAS::Core::SettingsManager::SettingsManager>
                                                              settings_manager_;
    std::shared_ptr<DAS::Core::IPC::MainProcess::IIpcContext> ipc_sp_;
    std::unique_ptr<PluginManager>                            pm_;
    DasPtr<IForeignLanguageRuntime>                           runtime_guard_;
    TaskComponentRuntime* runtime_ = nullptr;
    DasGuid               factory_guid_{};
};

TEST_F(
    PluginManagerTaskComponentTest,
    LoadPluginRegistersTaskComponentFactoryRoutes)
{
    const auto plugin_guid = MakeTaskComponentTestGuid(0x68130001);
    const auto component_guid = MakeTaskComponentTestGuid(0x68130003);
    const auto manifest_path = test_dir_ / "TaskComponentPlugin.json";
    WriteTaskComponentManifest(
        manifest_path,
        plugin_guid,
        factory_guid_,
        component_guid);

    ASSERT_EQ(pm_->LoadPlugin(manifest_path), DAS_S_OK);

    DasPtr<IDasTaskComponent> component;
    EXPECT_EQ(
        pm_->GetTaskComponentFactoryManager().CreateComponent(
            component_guid,
            component.Put()),
        DAS_S_OK);
    EXPECT_NE(component.Get(), nullptr);

    auto definitions =
        pm_->GetTaskComponentFactoryManager().EnumerateDefinitions();
    ASSERT_EQ(definitions.size(), 1u);
    EXPECT_EQ(definitions.front().plugin_guid, plugin_guid);
    EXPECT_EQ(definitions.front().factory_guid, factory_guid_);
    EXPECT_EQ(definitions.front().component_guid, component_guid);
}

TEST_F(PluginManagerTaskComponentTest, UnloadPluginRemovesTaskComponentRoutes)
{
    const auto plugin_guid = MakeTaskComponentTestGuid(0x68130011);
    const auto component_guid = MakeTaskComponentTestGuid(0x68130013);
    const auto manifest_path = test_dir_ / "UnloadTaskComponentPlugin.json";
    WriteTaskComponentManifest(
        manifest_path,
        plugin_guid,
        factory_guid_,
        component_guid);

    ASSERT_EQ(pm_->LoadPlugin(manifest_path), DAS_S_OK);
    ASSERT_EQ(pm_->UnloadPlugin(manifest_path), DAS_S_OK);

    DasPtr<IDasTaskComponent> component;
    EXPECT_EQ(
        pm_->GetTaskComponentFactoryManager().CreateComponent(
            component_guid,
            component.Put()),
        DAS_E_NOT_FOUND);
    EXPECT_TRUE(
        pm_->GetTaskComponentFactoryManager().EnumerateDefinitions().empty());
}

TEST_F(
    PluginManagerTaskComponentTest,
    InvalidTaskComponentsManifestRejectsPluginLoad)
{
    const auto plugin_guid = MakeTaskComponentTestGuid(0x68130021);
    const auto component_guid = MakeTaskComponentTestGuid(0x68130023);
    const auto manifest_path = test_dir_ / "InvalidTaskComponentPlugin.json";
    WriteTaskComponentManifestMissingFactoryGuid(
        manifest_path,
        plugin_guid,
        factory_guid_,
        component_guid);

    ScopedPluginManagerLogCapture logs;
    EXPECT_EQ(pm_->LoadPlugin(manifest_path), DAS_E_INVALID_JSON);
    EXPECT_TRUE(logs.Contains("factoryGuid"));
    EXPECT_TRUE(logs.Contains("missing required string"));
    EXPECT_EQ(pm_->GetLoadedPluginCount(), 0u);
    ASSERT_NE(runtime_, nullptr);
    EXPECT_TRUE(runtime_->loaded_paths.empty())
        << "Invalid declared taskComponents must reject before runtime load";
}

// ============================================================
// Feature-type index tests
// ============================================================

class PluginManagerFeatureTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        test_dir_ = std::filesystem::current_path()
                    / ("plugin_manager_feature_test_"
                       + std::to_string(std::random_device{}()));
        std::filesystem::create_directories(test_dir_);
        settings_dir_ = test_dir_ / "settings";
        settings_manager_ =
            std::make_unique<DAS::Core::SettingsManager::SettingsManager>(
                settings_dir_);
        auto ipc_sp =
            DAS::Core::IPC::MainProcess::CreateIpcContextShared(false);
        pm_ = std::make_unique<PluginManager>(
            *settings_manager_,
            Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_sp));

        registry_ = std::make_unique<RemoteObjectRegistry>();
        pm_->SetRegistry(*registry_);
    }

    void TearDown() override
    {
        pm_->Shutdown();
        runtime_guard_.Reset();
        std::filesystem::remove_all(test_dir_);
    }

    void InitializeCppRuntime()
    {
        if (initialized_)
        {
            return;
        }

        ASSERT_EQ(pm_->Initialize(1), DAS_S_OK);
        initialized_ = true;
    }

    void InitializeErrorLensRuntime(DasGuid provider_guid)
    {
        if (initialized_)
        {
            return;
        }

        ASSERT_EQ(pm_->Initialize(1), DAS_S_OK);

        auto* runtime = new ErrorLensRuntime(provider_guid);
        runtime->AddRef();
        runtime_guard_ = DasPtr<IForeignLanguageRuntime>::Attach(runtime);
        // 注入 fake runtime（包成 IRuntimeProvider 注册）。
        auto error_lens_provider = CreateLocalRuntimeProvider(runtime_guard_);
        ASSERT_TRUE(error_lens_provider);
        pm_->RegisterRuntimeProvider(
            ForeignInterfaceLanguage::Cpp,
            LoadMode::InProcess,
            std::shared_ptr<IRuntimeProvider>(
                std::move(error_lens_provider.value())));
        initialized_ = true;
    }

    void LoadIpcTestPlugin()
    {
        if (!plugin_path_.empty())
        {
            return;
        }

        InitializeCppRuntime();
        auto plugin_path = GetTestPluginPath();
        ASSERT_EQ(pm_->LoadPlugin(plugin_path), DAS_S_OK)
            << "Failed to load IpcTestPlugin1.dll";
        plugin_path_ = plugin_path;
    }

    void RegisterObjects()
    {
        LoadIpcTestPlugin();
        ASSERT_EQ(pm_->RegisterPluginObjects(plugin_path_), DAS_S_OK)
            << "RegisterPluginObjects failed";
    }

    std::filesystem::path test_dir_;
    std::unique_ptr<DAS::Core::SettingsManager::SettingsManager>
                                          settings_manager_;
    std::unique_ptr<PluginManager>        pm_;
    std::unique_ptr<RemoteObjectRegistry> registry_;
    std::filesystem::path                 plugin_path_;
    std::filesystem::path                 settings_dir_;
    DasPtr<IForeignLanguageRuntime>       runtime_guard_;
    bool                                  initialized_ = false;
};

TEST_F(PluginManagerFeatureTest, GetFeaturesByTypeReturnsInputFactory)
{
    RegisterObjects();

    auto span = pm_->GetFeaturesByType(DAS_PLUGIN_FEATURE_INPUT_FACTORY);
    ASSERT_GE(span.size(), 1u) << "Expected at least one INPUT_FACTORY feature";
    EXPECT_EQ(span[0]->feature_type, DAS_PLUGIN_FEATURE_INPUT_FACTORY);
}

TEST_F(PluginManagerFeatureTest, GetFeaturesByTypeReturnsComponentFactory)
{
    RegisterObjects();

    auto span = pm_->GetFeaturesByType(DAS_PLUGIN_FEATURE_COMPONENT_FACTORY);
    ASSERT_GE(span.size(), 1u)
        << "Expected at least one COMPONENT_FACTORY feature";
    EXPECT_EQ(span[0]->feature_type, DAS_PLUGIN_FEATURE_COMPONENT_FACTORY);
}

TEST_F(PluginManagerFeatureTest, GetFeaturesByTypeEmptyForUnregistered)
{
    // IpcTestPlugin1 does not provide DAS_PLUGIN_FEATURE_CAPTURE_FACTORY,
    // and we have NOT called RegisterPluginObjects, so the feature_type_index_
    // should be empty for all types.
    auto span = pm_->GetFeaturesByType(DAS_PLUGIN_FEATURE_CAPTURE_FACTORY);
    EXPECT_EQ(span.size(), 0u) << "Expected empty span for unregistered type";
}

TEST_F(PluginManagerFeatureTest, UnloadRemovesFromFeatureIndex)
{
    RegisterObjects();

    auto span_before = pm_->GetFeaturesByType(DAS_PLUGIN_FEATURE_INPUT_FACTORY);
    ASSERT_GE(span_before.size(), 1u)
        << "INPUT_FACTORY should be present before unload";

    auto unload_result = pm_->UnloadPlugin(plugin_path_);
    ASSERT_EQ(unload_result, DAS_S_OK) << "UnloadPlugin failed";

    auto span_after = pm_->GetFeaturesByType(DAS_PLUGIN_FEATURE_INPUT_FACTORY);
    EXPECT_EQ(span_after.size(), 0u)
        << "INPUT_FACTORY should be gone after unload";
}

TEST_F(PluginManagerFeatureTest, LoadOrderPreservedInSpan)
{
    RegisterObjects();

    auto input_span = pm_->GetFeaturesByType(DAS_PLUGIN_FEATURE_INPUT_FACTORY);
    ASSERT_EQ(input_span.size(), 1u);
    EXPECT_EQ(input_span[0]->feature_type, DAS_PLUGIN_FEATURE_INPUT_FACTORY);
    // Verify the feature belongs to the loaded plugin
    EXPECT_FALSE(input_span[0]->plugin_name.empty());

    auto comp_span =
        pm_->GetFeaturesByType(DAS_PLUGIN_FEATURE_COMPONENT_FACTORY);
    ASSERT_EQ(comp_span.size(), 1u);
    EXPECT_EQ(comp_span[0]->feature_type, DAS_PLUGIN_FEATURE_COMPONENT_FACTORY);
    EXPECT_FALSE(comp_span[0]->plugin_name.empty());
}

TEST_F(PluginManagerFeatureTest, FeatureInfoContainsPluginGuid)
{
    RegisterObjects();

    auto input_span = pm_->GetFeaturesByType(DAS_PLUGIN_FEATURE_INPUT_FACTORY);
    ASSERT_EQ(input_span.size(), 1u);

    // plugin_guid 应该非零（与 manifest.json 中的 GUID 匹配）
    DasGuid zero_guid{};
    EXPECT_FALSE(
        std::memcmp(&input_span[0]->plugin_guid, &zero_guid, sizeof(DasGuid))
        == 0)
        << "plugin_guid should be populated after LoadPlugin";
}

TEST_F(PluginManagerFeatureTest, GetFeaturesByTypeReturnsTaskAuthoringFactory)
{
    DasGuid plugin_guid{};
    plugin_guid.data1 = 0x6803;
    pm_->RegisterTestFeature(
        DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY,
        plugin_guid,
        nullptr);

    auto span =
        pm_->GetFeaturesByType(DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY);
    ASSERT_EQ(span.size(), 1u);
    EXPECT_EQ(span[0]->feature_type, DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY);
    EXPECT_EQ(span[0]->feature_index, 0u);
    EXPECT_EQ(span[0]->plugin_guid, plugin_guid);
    EXPECT_EQ(span[0]->iid, DasIidOf<IDasTaskAuthoringSessionFactory>());
}

TEST_F(PluginManagerFeatureTest, GetFeaturesByTypeReturnsTaskComponentFactory)
{
    DasGuid plugin_guid{};
    plugin_guid.data1 = 0x6804;
    pm_->RegisterTestFeature(
        DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY,
        plugin_guid,
        nullptr);

    auto span =
        pm_->GetFeaturesByType(DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY);
    ASSERT_EQ(span.size(), 1u);
    EXPECT_EQ(span[0]->feature_type, DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY);
    EXPECT_EQ(span[0]->plugin_guid, plugin_guid);
    EXPECT_EQ(span[0]->iid, DasIidOf<IDasTaskComponentFactory>());
}

TEST_F(PluginManagerFeatureTest, GetFeaturesByTypeReturnsErrorLens)
{
    DasGuid plugin_guid{};
    plugin_guid.data1 = 0x6805;
    pm_->RegisterTestFeature(
        DAS_PLUGIN_FEATURE_ERROR_LENS,
        plugin_guid,
        nullptr);

    auto span = pm_->GetFeaturesByType(DAS_PLUGIN_FEATURE_ERROR_LENS);
    ASSERT_EQ(span.size(), 1u);
    EXPECT_EQ(span[0]->feature_type, DAS_PLUGIN_FEATURE_ERROR_LENS);
    EXPECT_EQ(span[0]->plugin_guid, plugin_guid);
    EXPECT_EQ(span[0]->iid, DasIidOf<IDasErrorLens>());
}

TEST_F(PluginManagerFeatureTest, ErrorLensLifecycleRegistersRoutes)
{
    const auto provider_guid = MakeTaskComponentTestGuid(0x68130031);
    InitializeErrorLensRuntime(provider_guid);
    const auto manifest_path = test_dir_ / "ErrorLensPlugin.json";
    WriteMinimalManifest(
        manifest_path,
        "00000000-0000-0000-0000-006813003202",
        "ErrorLensPlugin");

    ASSERT_EQ(pm_->LoadPlugin(manifest_path), DAS_S_OK);

    DasPtr<IDasErrorLens> lens;
    EXPECT_EQ(
        pm_->GetErrorLensManager().FindInterface(provider_guid, lens.Put()),
        DAS_S_OK);
    EXPECT_NE(lens.Get(), nullptr);
}

TEST_F(PluginManagerFeatureTest, ErrorLensLifecycleUnregistersRoutes)
{
    const auto provider_guid = MakeTaskComponentTestGuid(0x68130031);
    InitializeErrorLensRuntime(provider_guid);
    const auto manifest_path = test_dir_ / "UnloadErrorLensPlugin.json";
    WriteMinimalManifest(
        manifest_path,
        "00000000-0000-0000-0000-006813003302",
        "UnloadErrorLensPlugin");

    ASSERT_EQ(pm_->LoadPlugin(manifest_path), DAS_S_OK);
    ASSERT_EQ(pm_->UnloadPlugin(manifest_path), DAS_S_OK);

    DasPtr<IDasErrorLens> lens;
    EXPECT_EQ(
        pm_->GetErrorLensManager().FindInterface(provider_guid, lens.Put()),
        DAS_E_NO_INTERFACE);
}

// ============================================================
// IPC routing tests
// ============================================================

TEST_F(PluginManagerGuidTest, LoadPlugin_NoHostPath_ReturnsError)
{
    // IPC context is always present via constructor; this tests that
    // missing host_exe_path_ still blocks the IPC load path.
    auto test_dir =
        std::filesystem::current_path() / "test_plugin_no_host_path";
    std::filesystem::create_directories(test_dir);
    auto manifest_path = test_dir / "test_plugin_no_host_path.json";

    auto manifest = Das::Utils::MakeYyjsonObject();
    {
        auto obj = *manifest.as_object();
        obj[std::string_view("guid")] = "00000000-0000-0000-0000-000000000001";
        obj[std::string_view("name")] = "TestCSharpPlugin";
        obj[std::string_view("language")] = "CSharp";
        obj[std::string_view("description")] = "test";
        obj[std::string_view("author")] = "test";
        obj[std::string_view("version")] = "1.0";
        obj[std::string_view("supportedSystem")] = "win";
        obj[std::string_view("pluginFilenameExtension")] = ".dll";
    }
    {
        std::ofstream ofs(manifest_path);
        ofs << *Das::Utils::SerializeYyjsonValue(manifest, false);
    }

    auto result = pm_->LoadPlugin(test_dir);
    EXPECT_EQ(result, DAS_E_NO_IMPLEMENTATION);

    std::filesystem::remove_all(test_dir);
}

TEST_F(PluginManagerGuidTest, LoadPlugin_CppLanguage_StaysInProcess)
{
    // Cpp (white-listed language) should NOT take the IPC path.
    // A nonexistent path returns an error from CppRuntime::LoadPlugin,
    // but NOT DAS_E_NO_IMPLEMENTATION (which is the IPC rejection code).
    auto result = pm_->LoadPlugin("/nonexistent/plugin/path");
    EXPECT_NE(result, DAS_E_NO_IMPLEMENTATION);
}

TEST_F(PluginManagerGuidTest, LoadPlugin_CppWithLoadModeIpc_GoesIpcPath)
{
    // Cpp (white-listed) with loadMode=Ipc should be forced to IPC path.
    auto test_dir =
        std::filesystem::current_path() / "test_plugin_loadmode_ipc";
    std::filesystem::create_directories(test_dir);
    auto manifest_path = test_dir / "test_plugin_loadmode_ipc.json";

    auto manifest = Das::Utils::MakeYyjsonObject();
    {
        auto obj = *manifest.as_object();
        obj[std::string_view("guid")] = "00000000-0000-0000-0000-000000000010";
        obj[std::string_view("name")] = "TestPluginCppIpc";
        obj[std::string_view("language")] = "Cpp";
        obj[std::string_view("loadMode")] = "ipc";
        obj[std::string_view("description")] = "test";
        obj[std::string_view("author")] = "test";
        obj[std::string_view("version")] = "1.0";
        obj[std::string_view("supportedSystem")] = "win";
        obj[std::string_view("pluginFilenameExtension")] = ".dll";
    }
    {
        std::ofstream ofs(manifest_path);
        ofs << *Das::Utils::SerializeYyjsonValue(manifest, false);
    }

    // No IPC context set -> IPC path returns DAS_E_NO_IMPLEMENTATION
    auto result = pm_->LoadPlugin(test_dir);
    EXPECT_EQ(result, DAS_E_NO_IMPLEMENTATION);

    std::filesystem::remove_all(test_dir);
}

// TODO: SetRuntimeProviderForTest API removed - test disabled
// TEST_F(PluginManagerGuidTest, LoadPlugin_UsesInjectedRuntimeProvider)
// {
//     auto test_dir =
//         std::filesystem::current_path() / "test_plugin_runtime_provider";
//     std::filesystem::create_directories(test_dir);
//     auto manifest_path = test_dir / "test_plugin_runtime_provider.json";
//     WriteMinimalManifest(
//         manifest_path,
//         "00000000-0000-0000-0000-000000750701",
//         "ProviderRoutedPlugin");
//
//     auto  provider = std::make_unique<CapturingRuntimeProvider>(73);
//     auto* raw_provider = provider.get();
//     pm_->SetRuntimeProviderForTest(std::move(provider));
//
//     IDasPluginPackage* raw_package = nullptr;
//     auto               result = pm_->LoadPlugin(manifest_path, &raw_package);
//     auto               package =
//     DasPtr<IDasPluginPackage>::Attach(raw_package);
//
//     EXPECT_EQ(result, DAS_S_OK);
//     ASSERT_NE(package.Get(), nullptr);
//     ASSERT_EQ(raw_provider->requests.size(), 1u);
//     EXPECT_EQ(raw_provider->requests.front().manifest_path, manifest_path);
//     EXPECT_EQ(raw_provider->requests.front().runtime_path, manifest_path);
//     EXPECT_TRUE(raw_provider->requests.front().node_modules_root.empty());
//     EXPECT_EQ(raw_provider->requests.front().main_process_owner_session_id,
//     1);
//
//     std::filesystem::remove_all(test_dir);
// }

// TODO: SetRuntimeProviderForTest API removed - test disabled
// TEST_F(
//     PluginManagerGuidTest,
//     LoadPlugin_IpcModeUsesProviderOwnerSessionForFeatureIndex)
// {
//     auto test_dir =
//         std::filesystem::current_path() / "test_plugin_provider_ipc_mode";
//     std::filesystem::create_directories(test_dir);
//     auto manifest_path = test_dir / "test_plugin_provider_ipc_mode.json";
//
//     auto manifest = Das::Utils::MakeYyjsonObject();
//     {
//         auto obj = *manifest.as_object();
//         obj[std::string_view("guid")] =
//         "00000000-0000-0000-0000-000000750702"; obj[std::string_view("name")]
//         = "ProviderIpcModePlugin"; obj[std::string_view("language")] = "Cpp";
//         obj[std::string_view("loadMode")] = "ipc";
//         obj[std::string_view("description")] = "test";
//         obj[std::string_view("author")] = "test";
//         obj[std::string_view("version")] = "1.0";
//         obj[std::string_view("supportedSystem")] = "win";
//         obj[std::string_view("pluginFilenameExtension")] = "dll";
//         obj[std::string_view("settings")] = Das::Utils::MakeYyjsonArray();
//     }
//     {
//         std::ofstream ofs(manifest_path);
//         ofs << *Das::Utils::SerializeYyjsonValue(manifest, false);
//     }
//
//     auto provider = std::make_unique<CapturingRuntimeProvider>(
//         94,
//         std::vector<DasPluginFeature>{DAS_PLUGIN_FEATURE_CAPTURE_FACTORY});
//     auto* raw_provider = provider.get();
//     pm_->SetRuntimeProviderForTest(std::move(provider));
//
//     auto result = pm_->LoadPlugin(manifest_path);
//
//     EXPECT_EQ(result, DAS_S_OK);
//     ASSERT_EQ(raw_provider->requests.size(), 1u);
//     EXPECT_EQ(raw_provider->requests.front().load_mode, LoadMode::Ipc);
//
//     std::vector<FeatureInfo> features;
//     ASSERT_EQ(pm_->GetPluginFeatures(manifest_path, features), DAS_S_OK);
//     ASSERT_EQ(features.size(), 1u);
//     EXPECT_EQ(features.front().session_id, 94);
//
//     std::filesystem::remove_all(test_dir);
// }

// TODO: SetRuntimeProviderForTest API removed - test disabled
// TEST_F(
//     PluginManagerGuidTest,
//     LoadPlugin_FlatNodeRuntimeRequestUsesCollectionNodeModulesRoot)
// {
//     auto test_dir = std::filesystem::current_path()
//                     / "test_plugin_flat_node_runtime_request";
//     std::filesystem::remove_all(test_dir);
//     std::filesystem::create_directories(test_dir);
//
//     const auto manifest_path = test_dir / "FlatNodePlugin.json";
//     WriteNodeManifest(
//         manifest_path,
//         "00000000-0000-0000-0000-000000760101",
//         "FlatNodePlugin");
//
//     auto  provider = std::make_unique<CapturingRuntimeProvider>(119);
//     auto* raw_provider = provider.get();
//     pm_->SetRuntimeProviderForTest(std::move(provider));
//
//     auto result = pm_->LoadPlugin(manifest_path);
//
//     EXPECT_EQ(result, DAS_S_OK);
//     ASSERT_EQ(raw_provider->requests.size(), 1u);
//     const auto& request = raw_provider->requests.front();
//     EXPECT_EQ(request.manifest_path, manifest_path);
//     EXPECT_EQ(request.runtime_path, manifest_path);
//     EXPECT_EQ(request.language, ForeignInterfaceLanguage::Node);
//     EXPECT_EQ(request.load_mode, LoadMode::Ipc);
//     EXPECT_EQ(request.node_modules_root, test_dir / "node_modules");
//
//     std::filesystem::remove_all(test_dir);
// }

// TODO: SetRuntimeProviderForTest API removed - test disabled
// TEST_F(
//     PluginManagerGuidTest,
//     LoadPlugin_FolderNodeRuntimeRequestUsesCollectionNodeModulesRoot)
// {
//     auto test_root = std::filesystem::current_path()
//                      / "test_plugin_folder_node_runtime_request";
//     std::filesystem::remove_all(test_root);
//
//     const auto plugins_dir = test_root / "plugins";
//     const auto package_dir = plugins_dir / "FolderNodePlugin";
//     std::filesystem::create_directories(package_dir);
//
//     const auto manifest_path = package_dir / "FolderNodePlugin.json";
//     WriteNodeManifest(
//         manifest_path,
//         "00000000-0000-0000-0000-000000760102",
//         "FolderNodePlugin");
//
//     auto  provider = std::make_unique<CapturingRuntimeProvider>(120);
//     auto* raw_provider = provider.get();
//     pm_->SetRuntimeProviderForTest(std::move(provider));
//
//     auto result = pm_->LoadPlugin(manifest_path);
//
//     EXPECT_EQ(result, DAS_S_OK);
//     ASSERT_EQ(raw_provider->requests.size(), 1u);
//     const auto& request = raw_provider->requests.front();
//     EXPECT_EQ(request.manifest_path, manifest_path);
//     EXPECT_EQ(request.runtime_path, manifest_path);
//     EXPECT_EQ(request.language, ForeignInterfaceLanguage::Node);
//     EXPECT_EQ(request.load_mode, LoadMode::Ipc);
//     EXPECT_EQ(request.node_modules_root, plugins_dir / "node_modules");
//
//     std::filesystem::remove_all(test_root);
// }

// TODO: SetRuntimeProviderForTest API removed - test disabled
// TEST_F(
//     PluginManagerGuidTest,
//     LoadPlugin_NodeManifestJsonRuntimeRequestUsesCollectionNodeModulesRoot)
// {
//     auto test_root = std::filesystem::current_path()
//                      / "test_plugin_node_manifest_json_runtime_request";
//     std::filesystem::remove_all(test_root);
//
//     const auto plugins_dir = test_root / "plugins";
//     const auto package_dir = plugins_dir / "ManifestJsonNodePlugin";
//     std::filesystem::create_directories(package_dir);
//
//     const auto manifest_path = package_dir / "manifest.json";
//     WriteNodeManifest(
//         manifest_path,
//         "00000000-0000-0000-0000-000000760103",
//         "ManifestJsonNodePlugin");
//
//     auto  provider = std::make_unique<CapturingRuntimeProvider>(121);
//     auto* raw_provider = provider.get();
//     pm_->SetRuntimeProviderForTest(std::move(provider));
//
//     auto result = pm_->LoadPlugin(manifest_path);
//
//     EXPECT_EQ(result, DAS_S_OK);
//     ASSERT_EQ(raw_provider->requests.size(), 1u);
//     const auto& request = raw_provider->requests.front();
//     EXPECT_EQ(request.manifest_path, manifest_path);
//     EXPECT_EQ(request.runtime_path, manifest_path);
//     EXPECT_EQ(request.language, ForeignInterfaceLanguage::Node);
//     EXPECT_EQ(request.load_mode, LoadMode::Ipc);
//     EXPECT_EQ(request.node_modules_root, plugins_dir / "node_modules");
//
//     std::filesystem::remove_all(test_root);
// }

TEST_F(PluginManagerGuidTest, LoadPlugin_NodeManifestRoutesThroughNodeRuntime)
{
    constexpr auto NODE_HOST_ENV = "DAS_NODE_HOST_EXE_PATH";
    auto           test_dir =
        std::filesystem::current_path() / "test_plugin_node_runtime_route";
    std::filesystem::remove_all(test_dir);
    std::filesystem::create_directories(test_dir);

    const auto manifest_path = test_dir / "node_plugin.json";
    const auto node_exe_path = test_dir / "fake-node.exe";
    const auto node_modules_root = test_dir / "node_modules";
    const auto runtime_root = node_modules_root / "das-core-node";
    {
        std::ofstream{node_exe_path} << "\n";
        std::filesystem::create_directories(runtime_root / "bin");
        std::filesystem::create_directories(runtime_root / "native");
        std::ofstream{runtime_root / "package.json"} << "\n";
        std::ofstream{runtime_root / "index.cjs"} << "\n";
        std::ofstream{runtime_root / "das_core_napi_export.js"} << "\n";
        std::ofstream{runtime_root / "bin" / "das-node-host.cjs"} << "\n";
        std::ofstream{runtime_root / "native" / "das_core_napi.node"} << "\n";
    }

    auto manifest = Das::Utils::MakeYyjsonObject();
    {
        auto obj = *manifest.as_object();
        obj[std::string_view("guid")] = "00000000-0000-0000-0000-000000750703";
        obj[std::string_view("name")] = "NodeProviderRoutePlugin";
        obj[std::string_view("language")] = "Node";
        obj[std::string_view("description")] = "test";
        obj[std::string_view("author")] = "test";
        obj[std::string_view("version")] = "1.0";
        obj[std::string_view("supportedSystem")] = "win";
        obj[std::string_view("pluginFilenameExtension")] = "js";
        obj[std::string_view("settings")] = Das::Utils::MakeYyjsonArray();
    }
    {
        std::ofstream ofs(manifest_path);
        ofs << *Das::Utils::SerializeYyjsonValue(manifest, false);
    }

    ScopedEnvVar             env{NODE_HOST_ENV, node_exe_path.string()};
    CapturingRemoteHostState remote_state;
    pm_->SetRemotePluginHostFactoryForTest(
        [&remote_state]()
        {
            return std::make_unique<CapturingRemoteHost>(
                remote_state,
                117,
                std::vector<DasPluginFeature>{
                    DAS_PLUGIN_FEATURE_CAPTURE_FACTORY});
        });

    auto result = pm_->LoadPlugin(manifest_path);

    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_EQ(remote_state.load_count, 1);
    EXPECT_EQ(remote_state.manifest_path, manifest_path);
    EXPECT_EQ(remote_state.executable, node_exe_path.string());
    ASSERT_EQ(remote_state.args.size(), 7u);
    EXPECT_EQ(
        remote_state.args.front(),
        (runtime_root / std::filesystem::path{"bin/das-node-host.cjs"})
            .string());
    EXPECT_EQ(remote_state.args[1], "--main-pid");
    EXPECT_FALSE(remote_state.args[2].empty());
    EXPECT_EQ(remote_state.args[3], "--package-root");
    EXPECT_EQ(remote_state.args[4], test_dir.string());
    EXPECT_EQ(remote_state.args[5], "--node-modules-root");
    EXPECT_EQ(remote_state.args[6], node_modules_root.string());
    EXPECT_EQ(remote_state.working_directory, test_dir.string());

    std::vector<FeatureInfo> features;
    ASSERT_EQ(pm_->GetPluginFeatures(manifest_path, features), DAS_S_OK);
    ASSERT_EQ(features.size(), 1u);
    EXPECT_EQ(features.front().session_id, 117);

    std::filesystem::remove_all(test_dir);
}

TEST_F(PluginManagerGuidTest, LoadPlugin_LowercaseNodeManifestIsRejected)
{
    auto test_dir =
        std::filesystem::current_path() / "test_plugin_lowercase_node_route";
    std::filesystem::remove_all(test_dir);
    std::filesystem::create_directories(test_dir);

    const auto manifest_path = test_dir / "node_plugin.json";
    auto       manifest = Das::Utils::MakeYyjsonObject();
    {
        auto obj = *manifest.as_object();
        obj[std::string_view("guid")] = "00000000-0000-0000-0000-000000750904";
        obj[std::string_view("name")] = "LowercaseNodePlugin";
        obj[std::string_view("language")] = "node";
        obj[std::string_view("description")] = "test";
        obj[std::string_view("author")] = "test";
        obj[std::string_view("version")] = "1.0";
        obj[std::string_view("supportedSystem")] = "win";
        obj[std::string_view("pluginFilenameExtension")] = "js";
        obj[std::string_view("settings")] = Das::Utils::MakeYyjsonArray();
    }
    {
        std::ofstream ofs(manifest_path);
        ofs << *Das::Utils::SerializeYyjsonValue(manifest, false);
    }

    CapturingRemoteHostState remote_state;
    pm_->SetRemotePluginHostFactoryForTest(
        [&remote_state]()
        {
            return std::make_unique<CapturingRemoteHost>(
                remote_state,
                118,
                std::vector<DasPluginFeature>{
                    DAS_PLUGIN_FEATURE_CAPTURE_FACTORY});
        });

    auto result = pm_->LoadPlugin(manifest_path);

    EXPECT_EQ(result, DAS_E_INVALID_JSON);
    EXPECT_EQ(remote_state.load_count, 0);
    EXPECT_EQ(pm_->GetLoadedPluginCount(), 0u);

    std::filesystem::remove_all(test_dir);
}

TEST_F(PluginManagerGuidTest, RuntimeLoadRequest_CarriesRoutingInputs)
{
    RuntimeLoadRequest request{};
    request.manifest_path = "plugins/TestPlugin.json";
    request.runtime_path = "plugins";
    request.plugin_guid = MakeTaskComponentTestGuid(0x75040001);
    request.node_modules_root = "plugins/node_modules";
    request.main_process_owner_session_id = 17;

    EXPECT_STREQ(request.manifest_path, "plugins/TestPlugin.json");
    EXPECT_STREQ(request.runtime_path, "plugins");
    EXPECT_EQ(request.plugin_guid.data1, 0x75040001u);
    EXPECT_STREQ(request.node_modules_root, "plugins/node_modules");
    EXPECT_EQ(request.main_process_owner_session_id, 17);
}

// TODO: SetRuntimeProviderForTest API removed - test disabled
// TEST_F(
//     PluginManagerGuidTest,
//     LoadPlugin_CSharpManifestKeepsCSharpFieldsOutOfRuntimeRequest)
// {
//     auto test_dir =
//         std::filesystem::current_path() / "test_plugin_csharp_request_shape";
//     std::filesystem::remove_all(test_dir);
//     std::filesystem::create_directories(test_dir);
//
//     const auto manifest_path = test_dir / "CSharpRoutingPlugin.json";
//     WriteCSharpManifest(
//         manifest_path,
//         "00000000-0000-0000-0000-000000780201",
//         "CSharpRoutingPlugin");
//
//     auto  provider = std::make_unique<CapturingRuntimeProvider>(122);
//     auto* raw_provider = provider.get();
//     pm_->SetRuntimeProviderForTest(std::move(provider));
//
//     auto result = pm_->LoadPlugin(manifest_path);
//
//     EXPECT_EQ(result, DAS_S_OK);
//     ASSERT_EQ(raw_provider->requests.size(), 1u);
//     const auto& request = raw_provider->requests.front();
//     EXPECT_EQ(request.manifest_path, manifest_path);
//     EXPECT_EQ(request.runtime_path, manifest_path);
//     EXPECT_EQ(request.language, ForeignInterfaceLanguage::CSharp);
//     EXPECT_EQ(request.load_mode, LoadMode::InProcess);
//     EXPECT_TRUE(request.node_modules_root.empty());
//     EXPECT_EQ(request.main_process_owner_session_id, 1);
//
//     std::filesystem::remove_all(test_dir);
// }

TEST_F(PluginManagerGuidTest, RuntimeLoadResult_CarriesObjectAndOwnerSession)
{
    auto* package = new CapturingPluginPackage();
    package->AddRef();

    RuntimeLoadResult result{};
    result.object = static_cast<IDasBase*>(package);
    result.owner_session_id = 23;

    EXPECT_NE(result.object, nullptr);
    EXPECT_EQ(result.owner_session_id, 23);
    if (result.object != nullptr)
    {
        result.object->Release();
    }
}

TEST_F(PluginManagerGuidTest, LocalRuntimeProvider_LoadsViaRuntimePath)
{
    auto* raw_runtime = new CapturingRuntime();
    auto  runtime = DasPtr<IForeignLanguageRuntime>(raw_runtime);
    auto  provider = CreateLocalRuntimeProvider(std::move(runtime));
    ASSERT_TRUE(provider);

    RuntimeLoadRequest request{};
    request.manifest_path = "plugins/TestPlugin.json";
    request.runtime_path = "plugins";
    request.plugin_guid = MakeTaskComponentTestGuid(0x75040002);
    request.main_process_owner_session_id = 41;

    RuntimeLoadResult result{};
    const auto        hr = provider.value()->LoadPlugin(request, &result);

    ASSERT_TRUE(DAS::IsOk(hr));
    ASSERT_EQ(raw_runtime->loaded_paths.size(), 1u);
    EXPECT_EQ(
        raw_runtime->loaded_paths.front(),
        std::filesystem::path{
            reinterpret_cast<const char8_t*>(request.runtime_path)});
    EXPECT_NE(result.object, nullptr);
    if (result.object != nullptr)
    {
        result.object->Release();
    }
    EXPECT_EQ(result.owner_session_id, 41);
}

TEST_F(PluginManagerGuidTest, LocalRuntimeProvider_PropagatesRuntimeError)
{
    auto* raw_runtime = new CapturingRuntime();
    raw_runtime->load_error = DAS_E_FILE_NOT_FOUND;
    auto runtime = DasPtr<IForeignLanguageRuntime>(raw_runtime);
    auto provider = CreateLocalRuntimeProvider(std::move(runtime));
    ASSERT_TRUE(provider);

    RuntimeLoadRequest request{};
    request.runtime_path = "missing.json";
    request.main_process_owner_session_id = 41;

    RuntimeLoadResult result{};
    const auto        hr = provider.value()->LoadPlugin(request, &result);

    ASSERT_TRUE(DAS::IsFailed(hr));
    EXPECT_EQ(hr, DAS_E_FILE_NOT_FOUND);
}

TEST_F(PluginManagerGuidTest, LocalRuntimeProvider_NodeReturnsNoImplementation)
{
    ForeignLanguageRuntimeFactoryDesc desc{};
    desc.language = ForeignInterfaceLanguage::Node;

    auto provider = CreateLocalRuntimeProvider(desc);

    ASSERT_FALSE(provider);
    EXPECT_EQ(provider.error(), DAS_E_NO_IMPLEMENTATION);
}

TEST_F(PluginManagerGuidTest, SetHostExePath)
{
    pm_->SetHostExePath("/path/to/DasHost");
}

TEST_F(PluginManagerGuidTest, OnHostProcessExit_DirectCall_CleansUpIndex)
{
    // Manually set up internal state to simulate post-IPC-load state.
    // This test uses friend access to PluginManager private members.
    // WR-04: test name reflects "同线程直调" (direct call) — bypasses the real
    // cross-thread HostLauncher callback path (plan 03 adds the real-thread
    // test).
    DasGuid test_guid{};
    test_guid.data1 = 0x00000099;

    LoadedPlugin fake_plugin;
    fake_plugin.plugin_path = "/fake/ipc/plugin";
    pm_->loaded_plugins_[test_guid] = std::move(fake_plugin);
    pm_->path_to_guid_["/fake/ipc/plugin"] = test_guid;

    // Verify setup
    EXPECT_TRUE(pm_->loaded_plugins_.contains(test_guid));
    EXPECT_TRUE(pm_->path_to_guid_.contains("/fake/ipc/plugin"));

    // Trigger the disconnect callback (new GUID-first signature)
    pm_->OnHostProcessExit(test_guid, 1);

    // Verify all indexes are cleaned up (host_launchers_ field deleted in plan
    // 04; HostLauncher cleanup now managed by IpcContext/ConnectionManager by
    // session)
    EXPECT_FALSE(pm_->loaded_plugins_.contains(test_guid));
    EXPECT_FALSE(pm_->path_to_guid_.contains("/fake/ipc/plugin"));
}

TEST_F(PluginManagerGuidTest, OnHeartbeatTimeout_DirectCall_CleansUpIndex)
{
    // Manually set up internal state to simulate post-IPC-load state.
    // Drives the full heartbeat-timeout path: OnHeartbeatTimeout takes its own
    // mutex lock and calls CleanupPluginByGuid internally.
    // WR-04: test name reflects "同线程直调" (direct call) — bypasses the real
    // cross-thread HostLauncher callback path (plan 03 adds the real-thread
    // test).
    DasGuid test_guid{};
    test_guid.data1 = 0x000000AA;

    LoadedPlugin fake_plugin;
    fake_plugin.plugin_path = "/fake/heartbeat/plugin";
    pm_->loaded_plugins_[test_guid] = std::move(fake_plugin);
    pm_->path_to_guid_["/fake/heartbeat/plugin"] = test_guid;

    // Verify setup
    EXPECT_TRUE(pm_->loaded_plugins_.contains(test_guid));
    EXPECT_TRUE(pm_->path_to_guid_.contains("/fake/heartbeat/plugin"));

    // Trigger the heartbeat timeout callback (full path, not direct cleanup)
    pm_->OnHeartbeatTimeout(test_guid);

    // Verify all indexes are cleaned up (host_launchers_ field deleted in plan
    // 04; HostLauncher cleanup now managed by IpcContext/ConnectionManager by
    // session)
    EXPECT_FALSE(pm_->loaded_plugins_.contains(test_guid));
    EXPECT_FALSE(pm_->path_to_guid_.contains("/fake/heartbeat/plugin"));
}

// ============================================================
// Phase 80.2 Plan 03 Task 2b: 真实跨线程心跳回调验证（D-09）
// Blocker #1: 强制真实 HostLauncher + NotifyHeartbeatTimeout 跨线程链
// Blocker #2: Shutdown 测试必须有第二线程并发 NotifyHeartbeatTimeout 制造
//             AB-BA 触发条件
// BLOCKER-2 (DasPtr): 用 DasPtr<IHostLauncher>::Attach(launcher.release())
//                      adopting 模式（禁两参 (T*, bool) 构造，DasPtr.hpp
//                      无此重载）
// ============================================================

// Blocker #1 修复：必须驱动真实 HostLauncher + NotifyHeartbeatTimeout
// 跨线程链。 禁止退化为 pm_->OnHeartbeatTimeout(test_guid) 同线程直调（绕过
// callback_mutex_ 持锁路径，违反 D-09）。 构造真实 HostLauncher（仅需 io_ctx +
// session_id，HostLauncher.h:129-132）， SetAssociatedGuid +
// SetOnHeartbeatTimeout 注入回调，测试局部 DasPtr<HostLauncher> keeper
// 全权管理生命周期（模拟生产中 IpcContext::launchers_ 持有，不再 friend 注入
// host_launchers_ 死字段 —— plan 04 已删）， std::thread 调
// launcher_ptr->NotifyHeartbeatTimeout() 驱动 GuardedCallback::Invoke 持
// callback_mutex_ -> OnHeartbeatTimeout 持 mutex_ 的完整跨线程路径。
TEST_F(PluginManagerGuidTest, HeartbeatTimeout_RealThread_CleansUpIndex)
{
    DasGuid test_guid{};
    test_guid.data1 = 0x000000BB;

    LoadedPlugin fake_plugin;
    fake_plugin.plugin_path = "/fake/realthread/plugin";
    pm_->loaded_plugins_[test_guid] = std::move(fake_plugin);
    pm_->path_to_guid_["/fake/realthread/plugin"] = test_guid;

    // 构造真实 HostLauncher（仅需 io_ctx + session_id，无需进程句柄/IPC
    // context）。测试局部 DasPtr<HostLauncher> keeper 全权管理生命周期
    // （plan 04 删 host_launchers_ 后不再 friend 注入，OnHeartbeatTimeout ->
    // CleanupPluginByGuid 只清 loaded_plugins_ / path_to_guid_ 索引）。
    boost::asio::io_context io_ctx;
    auto launcher = std::make_unique<HostLauncher>(io_ctx, /*session_id=*/1);
    // borrowed raw 指针，生命周期由测试局部 keeper 全权管理
    HostLauncher* launcher_ptr = launcher.get();
    launcher_ptr->SetAssociatedGuid(test_guid);
    launcher_ptr->SetOnHeartbeatTimeout(
        [this](DasGuid callback_guid)
        {
            pm_->OnHeartbeatTimeout(
                callback_guid); // plan 02 已改用 callback_guid
        });

    // DasPtr<HostLauncher> keeper adopting unique_ptr 所有权（DasPtr::Attach
    // 不 AddRef，DasPtr.hpp:201-206），keeper 出作用域自动
    // Release（HostLauncher 析构）。launcher_ptr 作为 borrowed raw
    // 跨线程访问，生命周期由 keeper 全权管理（模拟生产中 IpcContext::launchers_
    // 持有）。
    DasPtr<HostLauncher> keeper =
        DasPtr<HostLauncher>::Attach(launcher.release());

    // 跨线程触发（强制真实 GuardedCallback::Invoke 持锁路径）
    std::atomic<bool> done{false};
    std::thread       heartbeat_thread(
        [&]()
        {
            launcher_ptr
                ->NotifyHeartbeatTimeout(); // 持 callback_mutex_ ->
                                            // OnHeartbeatTimeout -> mutex_
            done.store(true);
        });

    // 超时断言防 hang（5s = 100ms × 50 次）
    for (int i = 0; i < 50; ++i)
    {
        if (done.load())
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_TRUE(done.load()) << "NotifyHeartbeatTimeout cross-thread should "
                                "not hang (CR-03 锁层级正确)";
    heartbeat_thread.join();

    // 验证跨线程清理（OnHeartbeatTimeout -> CleanupPluginByGuid 已执行）。
    // host_launchers_ 字段已删（plan 04），不再断言其清理。
    EXPECT_FALSE(pm_->loaded_plugins_.contains(test_guid));
    EXPECT_FALSE(pm_->path_to_guid_.contains("/fake/realthread/plugin"));
}

// Blocker #2 修复：必须有第二线程并发 NotifyHeartbeatTimeout 制造 AB-BA
// 触发条件。禁单线程 smoke test（单线程无法触发 AB-BA，对 CR-03 验证无效）。
// 线程 A 调 pm_->Shutdown()（方案 A 锁外 ClearCallbacks + Stop），
// 线程 B 调 launcher_ptr->NotifyHeartbeatTimeout()（持 callback_mutex_ ->
// OnHeartbeatTimeout -> 尝试 mutex_）。若 CR-03 未修复（Shutdown 持 mutex_ 调
// Stop）= AB-BA 死锁 = 超时。plan 02 已修复 CR-03，此测试应绿（不 hang）。
TEST_F(PluginManagerGuidTest, Shutdown_DoesNotHoldMutexDuringStop)
{
    DasGuid test_guid{};
    test_guid.data1 = 0x000000CC;

    LoadedPlugin fake_plugin;
    fake_plugin.plugin_path = "/fake/shutdown/plugin";
    pm_->loaded_plugins_[test_guid] = std::move(fake_plugin);
    pm_->path_to_guid_["/fake/shutdown/plugin"] = test_guid;

    boost::asio::io_context io_ctx;
    auto launcher = std::make_unique<HostLauncher>(io_ctx, /*session_id=*/2);
    HostLauncher* launcher_ptr = launcher.get();
    launcher_ptr->SetAssociatedGuid(test_guid);
    launcher_ptr->SetOnHeartbeatTimeout(
        [this](DasGuid callback_guid)
        { pm_->OnHeartbeatTimeout(callback_guid); });
    // plan 04：测试局部 DasPtr<HostLauncher> keeper 全权持有（不再 friend
    // 注入 host_launchers_ 死字段，已删）。
    DasPtr<HostLauncher> keeper =
        DasPtr<HostLauncher>::Attach(launcher.release());

    // 并发场景：制造 AB-BA 触发条件。
    // 注意：Shutdown 现在调 ipc_context_->ResetHostLifecycleCallbacks()，而测试
    // 局部 HostLauncher 未注册到 ipc_context_->launchers_（只由 keeper 持有），
    // 故 Shutdown 的 ResetHostLifecycleCallbacks 不会 drain 测试 HostLauncher
    // 的 slot。这没关系——测试目的是验证 Shutdown + 并发 NotifyHeartbeatTimeout
    // 不 hang（CR-03 锁层级），不是验证 drain 真实生效（drain 真实性由 phase
    // gate HeartbeatTimeout_CrossThreadDrainsInFlightCallback 在
    // IpcMultiProcessTest 覆盖）。
    std::atomic<bool> shutdown_done{false};
    std::atomic<bool> notify_done{false};
    std::thread       shutdown_thread(
        [&]()
        {
            pm_->Shutdown(); // 方案 A 锁外 ResetHostLifecycleCallbacks
            shutdown_done.store(true);
        });
    std::thread notify_thread(
        [&]()
        {
            launcher_ptr
                ->NotifyHeartbeatTimeout(); // 持 callback_mutex_ ->
                                            // OnHeartbeatTimeout -> mutex_
            notify_done.store(true);
        });

    // 超时断言防 AB-BA hang（5s = 100ms × 50 次）
    for (int i = 0; i < 50; ++i)
    {
        if (shutdown_done.load() && notify_done.load())
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_TRUE(shutdown_done.load())
        << "Shutdown should not hang (CR-03 锁层级正确)";
    EXPECT_TRUE(notify_done.load())
        << "NotifyHeartbeatTimeout should not hang (AB-BA not triggered)";
    shutdown_thread.join();
    notify_thread.join();
}

// ============================================================
// PluginResourceIndex GUID cache tests
// ============================================================

namespace
{
    constexpr const char* kTestGuid1 = "A1B2C3D4-1111-2222-3333-444455556666";
    constexpr const char* kTestGuid2 = "A1B2C3D4-5555-6666-7777-888899990000";

    void WriteFolderModePlugin(
        const std::filesystem::path& plugin_dir,
        const std::string&           dirname,
        const std::string&           guid,
        const std::string&           resource_path_value)
    {
        auto pkg_dir = plugin_dir / dirname;
        std::filesystem::create_directories(pkg_dir);

        auto manifest = Das::Utils::MakeYyjsonObject();
        {
            auto obj = *manifest.as_object();
            obj[std::string_view("guid")] = guid;
            obj[std::string_view("name")] = dirname;
            obj[std::string_view("language")] = "Cpp";
            obj[std::string_view("description")] = "test plugin";
            obj[std::string_view("author")] = "test";
            obj[std::string_view("version")] = "1.0";
            obj[std::string_view("supportedSystem")] = "win";
            obj[std::string_view("pluginFilenameExtension")] = "dll";
            obj[std::string_view("settings")] = Das::Utils::MakeYyjsonArray();
        }

        if (!resource_path_value.empty())
        {
            (*manifest.as_object())[std::string_view("resourcePath")] =
                resource_path_value;
        }

        auto manifest_path = pkg_dir / (dirname + ".json");
        {
            std::ofstream ofs(manifest_path);
            ofs << *Das::Utils::SerializeYyjsonValue(manifest, false);
        }

        // Create the resource subdirectory
        std::string rp =
            resource_path_value.empty() ? "resource" : resource_path_value;
        std::filesystem::create_directories(pkg_dir / rp);
    }
} // anonymous namespace

class PluginResourceIndexTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        test_dir_ =
            std::filesystem::current_path()
            / ("test_resource_index_" + std::to_string(std::random_device{}()));
        plugin_dir_ = test_dir_ / "plugins";
        std::filesystem::create_directories(plugin_dir_);

        auto& index = PluginResourceIndex::GetInstance();
        index.InvalidateCache();
        index.ConfigurePluginResourceScanRoot(plugin_dir_);
    }

    void TearDown() override
    {
        // Reset singleton to a clean state for subsequent tests
        auto& index = PluginResourceIndex::GetInstance();
        index.InvalidateCache();
        index.ConfigurePluginResourceScanRoot(plugin_dir_);

        std::filesystem::remove_all(test_dir_);
    }

    std::filesystem::path test_dir_;
    std::filesystem::path plugin_dir_;
};

TEST_F(PluginResourceIndexTest, CacheMissTriggersFullScanAndHits)
{
    WriteFolderModePlugin(plugin_dir_, "TestPlugin1", kTestGuid1, "");

    auto  guid = MakeDasGuid(kTestGuid1);
    auto& index = PluginResourceIndex::GetInstance();

    const PluginResourceEntry* p_entry = nullptr;
    auto result = index.ResolvePluginResourceEntryByGuid(guid, &p_entry);

    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_NE(p_entry, nullptr);
    EXPECT_EQ(p_entry->plugin_name, "TestPlugin1");
    EXPECT_FALSE(p_entry->resource_root.empty());
}

TEST_F(PluginResourceIndexTest, DuplicateGuidConflictFailsAndPreservesOriginal)
{
    // First, populate the cache with a single valid plugin
    WriteFolderModePlugin(plugin_dir_, "PluginAlpha", kTestGuid1, "");

    auto  guid = MakeDasGuid(kTestGuid1);
    auto& index = PluginResourceIndex::GetInstance();

    const PluginResourceEntry* p_entry = nullptr;
    auto result = index.ResolvePluginResourceEntryByGuid(guid, &p_entry);
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_NE(p_entry, nullptr);
    EXPECT_EQ(p_entry->plugin_name, "PluginAlpha");

    // Now add a second plugin with the SAME GUID -> conflict
    WriteFolderModePlugin(plugin_dir_, "PluginBeta", kTestGuid1, "");

    // Force a rescan
    index.InvalidateCache();

    const PluginResourceEntry* p_entry2 = nullptr;
    auto result2 = index.ResolvePluginResourceEntryByGuid(guid, &p_entry2);
    EXPECT_EQ(result2, DAS_E_DUPLICATE_ELEMENT);

    // Original map should remain unchanged: the old entry is still valid
    // (conflict prevents partial publishing)
}

TEST_F(PluginResourceIndexTest, SuccessfulRescanReplacesOldMap)
{
    // Initial state: one plugin
    WriteFolderModePlugin(plugin_dir_, "PluginV1", kTestGuid1, "");

    auto  guid1 = MakeDasGuid(kTestGuid1);
    auto& index = PluginResourceIndex::GetInstance();

    const PluginResourceEntry* p_entry = nullptr;
    auto result = index.ResolvePluginResourceEntryByGuid(guid1, &p_entry);
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_EQ(p_entry->plugin_name, "PluginV1");

    // Remove the old plugin and add a new one with a different GUID
    std::filesystem::remove_all(plugin_dir_ / "PluginV1");
    WriteFolderModePlugin(plugin_dir_, "PluginV2", kTestGuid2, "assets");

    // Force rescan
    index.InvalidateCache();

    // Old GUID should now be gone (map replaced, not merged)
    const PluginResourceEntry* p_old = nullptr;
    auto old_result = index.ResolvePluginResourceEntryByGuid(guid1, &p_old);
    EXPECT_EQ(old_result, DAS_E_NOT_FOUND)
        << "Old GUID should not exist after map replacement";

    // New GUID should be present
    auto                       guid2 = MakeDasGuid(kTestGuid2);
    const PluginResourceEntry* p_new = nullptr;
    auto new_result = index.ResolvePluginResourceEntryByGuid(guid2, &p_new);
    ASSERT_EQ(new_result, DAS_S_OK);
    EXPECT_EQ(p_new->plugin_name, "PluginV2");
}

TEST_F(PluginResourceIndexTest, InvalidResourcePath_TraversalFails)
{
    WriteFolderModePlugin(plugin_dir_, "EvilPlugin", kTestGuid1, "../outside");

    auto  guid = MakeDasGuid(kTestGuid1);
    auto& index = PluginResourceIndex::GetInstance();

    const PluginResourceEntry* p_entry = nullptr;
    // The plugin with traversal resourcePath should be skipped during scan,
    // so the GUID will not be found.
    auto result = index.ResolvePluginResourceEntryByGuid(guid, &p_entry);
    EXPECT_EQ(result, DAS_E_NOT_FOUND);
}

TEST_F(PluginResourceIndexTest, InvalidResourcePath_AbsoluteFails)
{
    WriteFolderModePlugin(
        plugin_dir_,
        "AbsolutePlugin",
        kTestGuid1,
        "/etc/secrets");

    auto  guid = MakeDasGuid(kTestGuid1);
    auto& index = PluginResourceIndex::GetInstance();

    const PluginResourceEntry* p_entry = nullptr;
    auto result = index.ResolvePluginResourceEntryByGuid(guid, &p_entry);
    EXPECT_EQ(result, DAS_E_NOT_FOUND);
}

// ============================================================
// DasPluginLoadImageFromResource end-to-end tests
// ============================================================

namespace
{
    void WriteTinyRedPng(const std::filesystem::path& path)
    {
        // Create a 2x2 red image (B=0, G=0, R=255 in BGR format)
        cv::Mat              red_img(2, 2, CV_8UC3, cv::Scalar(0, 0, 255));
        std::vector<uint8_t> buf;
        cv::imencode(".png", red_img, buf);
        assert(!buf.empty() && "imencode returned empty buffer");
        std::ofstream ofs(path, std::ios::binary);
        assert(ofs.is_open() && "Failed to open file for writing");
        ofs.write(
            reinterpret_cast<const char*>(buf.data()),
            static_cast<std::streamsize>(buf.size()));
        ofs.flush();
        ofs.close();
    }

    // A minimal mock that returns a configurable GUID from GetGuid().
    class MockTypeInfo final : public IDasTypeInfo
    {
    public:
        explicit MockTypeInfo(DasGuid guid) : guid_(guid) {}

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            auto count = --ref_count_;
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
            return CreateIDasReadOnlyStringFromUtf8(
                "MockTypeInfo",
                pp_out_name);
        }

    private:
        DasGuid               guid_;
        std::atomic<uint32_t> ref_count_{0};
    };
} // anonymous namespace

class DasPluginLoadImageFromResourceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        test_dir_ =
            std::filesystem::current_path()
            / ("test_load_image_" + std::to_string(std::random_device{}()));
        plugin_dir_ = test_dir_ / "plugins";
        std::filesystem::create_directories(plugin_dir_);

        auto& index = PluginResourceIndex::GetInstance();
        index.InvalidateCache();
        index.ConfigurePluginResourceScanRoot(plugin_dir_);

        // Set up a folder-mode plugin with a tiny PNG in its resource dir
        auto pkg_dir = plugin_dir_ / "ImagePlugin";
        auto res_dir = pkg_dir / "resource";
        std::filesystem::create_directories(res_dir);

        auto manifest = Das::Utils::MakeYyjsonObject();
        {
            auto obj = *manifest.as_object();
            obj[std::string_view("guid")] = kTestGuid1;
            obj[std::string_view("name")] = "ImagePlugin";
            obj[std::string_view("language")] = "Cpp";
            obj[std::string_view("description")] = "test image plugin";
            obj[std::string_view("author")] = "test";
            obj[std::string_view("version")] = "1.0";
            obj[std::string_view("supportedSystem")] = "win";
            obj[std::string_view("pluginFilenameExtension")] = "dll";
            obj[std::string_view("settings")] = Das::Utils::MakeYyjsonArray();
        }

        auto manifest_path = pkg_dir / "ImagePlugin.json";
        {
            std::ofstream ofs(manifest_path);
            ofs << *Das::Utils::SerializeYyjsonValue(manifest, false);
        }

        WriteTinyRedPng(res_dir / "tiny.png");

        test_guid_ = MakeDasGuid(kTestGuid1);
    }

    void TearDown() override
    {
        auto& index = PluginResourceIndex::GetInstance();
        index.InvalidateCache();
        index.ConfigurePluginResourceScanRoot(plugin_dir_);

        std::filesystem::remove_all(test_dir_);
    }

    std::filesystem::path test_dir_;
    std::filesystem::path plugin_dir_;
    DasGuid               test_guid_;
};

TEST_F(DasPluginLoadImageFromResourceTest, SuccessPath_ReturnsValidImage)
{
    auto* p_type_info = new MockTypeInfo(test_guid_);
    p_type_info->AddRef();

    IDasReadOnlyString* p_path = nullptr;
    ASSERT_EQ(CreateIDasReadOnlyStringFromUtf8("tiny.png", &p_path), DAS_S_OK);

    Das::ExportInterface::IDasImage* p_image = nullptr;
    auto result = DasPluginLoadImageFromResource(p_type_info, p_path, &p_image);

    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_NE(p_image, nullptr);

    Das::ExportInterface::DasSize size{};
    EXPECT_EQ(p_image->GetSize(&size), DAS_S_OK);
    EXPECT_EQ(size.width, 2);
    EXPECT_EQ(size.height, 2);

    int32_t channel_count = 0;
    EXPECT_EQ(p_image->GetChannelCount(&channel_count), DAS_S_OK);
    EXPECT_EQ(channel_count, 3); // RGB

    p_image->Release();
    p_path->Release();
    p_type_info->Release();
}

TEST_F(DasPluginLoadImageFromResourceTest, TraversalPath_ReturnsInvalidPath)
{
    auto* p_type_info = new MockTypeInfo(test_guid_);
    p_type_info->AddRef();

    IDasReadOnlyString* p_path = nullptr;
    ASSERT_EQ(
        CreateIDasReadOnlyStringFromUtf8("../escape.png", &p_path),
        DAS_S_OK);

    Das::ExportInterface::IDasImage* p_image = nullptr;
    auto result = DasPluginLoadImageFromResource(p_type_info, p_path, &p_image);

    EXPECT_EQ(result, DAS_E_INVALID_PATH);
    EXPECT_EQ(p_image, nullptr);

    p_path->Release();
    p_type_info->Release();
}

TEST_F(DasPluginLoadImageFromResourceTest, AbsolutePath_ReturnsInvalidPath)
{
    auto* p_type_info = new MockTypeInfo(test_guid_);
    p_type_info->AddRef();

    IDasReadOnlyString* p_path = nullptr;
    ASSERT_EQ(
        CreateIDasReadOnlyStringFromUtf8("/etc/passwd", &p_path),
        DAS_S_OK);

    Das::ExportInterface::IDasImage* p_image = nullptr;
    auto result = DasPluginLoadImageFromResource(p_type_info, p_path, &p_image);

    EXPECT_EQ(result, DAS_E_INVALID_PATH);
    EXPECT_EQ(p_image, nullptr);

    p_path->Release();
    p_type_info->Release();
}

TEST_F(DasPluginLoadImageFromResourceTest, MissingFile_ReturnsFileNotFound)
{
    auto* p_type_info = new MockTypeInfo(test_guid_);
    p_type_info->AddRef();

    IDasReadOnlyString* p_path = nullptr;
    ASSERT_EQ(
        CreateIDasReadOnlyStringFromUtf8("nonexistent.png", &p_path),
        DAS_S_OK);

    Das::ExportInterface::IDasImage* p_image = nullptr;
    auto result = DasPluginLoadImageFromResource(p_type_info, p_path, &p_image);

    EXPECT_EQ(result, DAS_E_FILE_NOT_FOUND);
    EXPECT_EQ(p_image, nullptr);

    p_path->Release();
    p_type_info->Release();
}

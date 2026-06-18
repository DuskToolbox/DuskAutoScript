#ifndef DAS_PLUGINS_SCHEDULERTESTPLUGIN_SCHEDULERTESTPLUGINIMPL_H
#define DAS_PLUGINS_SCHEDULERTESTPLUGIN_SCHEDULERTESTPLUGINIMPL_H

#include <das/Plugins/SchedulerTestPlugin/SchedulerTestSharedState.h>

#include <atomic>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <das/_autogen/idl/abi/IDasTask.h>
#include <das/_autogen/idl/abi/IDasTaskAuthoring.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>

DAS_NS_BEGIN

class FactoryBackedTask final : public PluginInterface::IDasTask
{
public:
    explicit FactoryBackedTask(FactoryTaskSharedState* state) : state_(state)
    {
        if (state_)
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            instance_id_ = ++state_->created_instance_count;
        }
    }

    uint32_t AddRef() override { return ++ref_count_; }

    uint32_t Release() override
    {
        auto count = --ref_count_;
        if (count == 0)
        {
            delete this;
        }
        return count;
    }

    DasResult QueryInterface(const DasGuid& iid, void** pp) override;
    DasResult GetGuid(DasGuid* p_out_guid) override;
    DasResult GetRuntimeClassName(IDasReadOnlyString** pp) override;

    DasResult Do(
        Das::PluginInterface::IDasStopToken*,
        Das::ExportInterface::IDasJson*,
        Das::ExportInterface::IDasJson* p_task_settings_json) override;

    DasResult GetNextExecutionTime(
        Das::ExportInterface::DasDate* p_out_date) override;

private:
    std::atomic<uint32_t>   ref_count_{0};
    FactoryTaskSharedState* state_ = nullptr;
    int                     instance_id_ = 0;
};

class FakeAuthoringSession final
    : public Das::PluginInterface::IDasTaskAuthoringSession
{
public:
    explicit FakeAuthoringSession(FactoryTaskSharedState* state) : state_(state)
    {
    }

    uint32_t AddRef() override { return ++ref_count_; }

    uint32_t Release() override
    {
        auto count = --ref_count_;
        if (count == 0)
        {
            delete this;
        }
        return count;
    }

    DasResult QueryInterface(const DasGuid& iid, void** pp) override;
    DasResult GetGuid(DasGuid* p_out_guid) override;
    DasResult GetRuntimeClassName(IDasReadOnlyString** pp) override;

    DasResult GetDocument(
        Das::ExportInterface::IDasJson*,
        Das::ExportInterface::IDasJson** pp_out_document_json) override;
    DasResult ApplyChange(
        Das::ExportInterface::IDasJson*,
        Das::ExportInterface::IDasJson** pp_out_result_json) override;
    DasResult Compile(
        Das::ExportInterface::IDasJson*  p_request_json,
        Das::ExportInterface::IDasJson** pp_out_result_json) override;

private:
    std::atomic<uint32_t>   ref_count_{0};
    FactoryTaskSharedState* state_ = nullptr;
};

class FakeAuthoringSessionFactory final
    : public Das::PluginInterface::IDasTaskAuthoringSessionFactory
{
public:
    FakeAuthoringSessionFactory(
        FactoryTaskSharedState* state,
        DasGuid                 factory_guid,
        bool                    can_create_session)
        : state_(state), factory_guid_(factory_guid),
          can_create_session_(can_create_session)
    {
    }

    uint32_t AddRef() override { return ++ref_count_; }

    uint32_t Release() override
    {
        auto count = --ref_count_;
        if (count == 0)
        {
            delete this;
        }
        return count;
    }

    DasResult QueryInterface(const DasGuid& iid, void** pp) override;
    DasResult GetGuid(DasGuid* p_out_guid) override;
    DasResult GetRuntimeClassName(IDasReadOnlyString** pp) override;

    DasResult CreateSession(
        const DasGuid&,
        Das::ExportInterface::IDasJson*                  p_context_json,
        Das::PluginInterface::IDasTaskAuthoringSession** pp_out_session)
        override;

private:
    std::atomic<uint32_t>   ref_count_{0};
    FactoryTaskSharedState* state_ = nullptr;
    DasGuid                 factory_guid_{};
    bool                    can_create_session_ = true;
};

class FakeTaskComponent final : public Das::PluginInterface::IDasTaskComponent
{
public:
    uint32_t AddRef() override { return ++ref_count_; }

    uint32_t Release() override
    {
        auto count = --ref_count_;
        if (count == 0)
        {
            delete this;
        }
        return count;
    }

    DasResult QueryInterface(const DasGuid& iid, void** pp) override;
    DasResult GetGuid(DasGuid* p_out_guid) override;
    DasResult GetRuntimeClassName(IDasReadOnlyString** pp) override;

    DasResult ApplySettingsChange(
        Das::ExportInterface::IDasJson*,
        Das::ExportInterface::IDasJson** pp_out_result_json) override;
    DasResult Do(
        Das::PluginInterface::IDasStopToken*,
        Das::ExportInterface::IDasReadOnlyPortMap*,
        Das::ExportInterface::IDasPortMap** pp_out_port_map) override;

private:
    std::atomic<uint32_t> ref_count_{0};
};

class FakeTaskComponentFactory final
    : public Das::PluginInterface::IDasTaskComponentFactory
{
public:
    uint32_t AddRef() override { return ++ref_count_; }

    uint32_t Release() override
    {
        auto count = --ref_count_;
        if (count == 0)
        {
            delete this;
        }
        return count;
    }

    DasResult QueryInterface(const DasGuid& iid, void** pp) override;
    DasResult GetGuid(DasGuid* p_out_guid) override;
    DasResult GetRuntimeClassName(IDasReadOnlyString** pp) override;

    DasResult CreateComponent(
        const DasGuid&                            component_guid,
        Das::PluginInterface::IDasTaskComponent** pp_out_component) override;

    DasResult SetTaskComponentHost(
        Das::PluginInterface::IDasTaskComponentHost* /*p_host*/) override
    {
        return DAS_S_OK;
    }

private:
    std::atomic<uint32_t> ref_count_{0};
};

class FactoryTaskPluginPackage final
    : public Das::PluginInterface::IDasPluginPackage
{
public:
    // 无参构造：由 dll 入口创建，state_ 取自 dll 全局 g_test_shared_state
    // （由 exe 通过 DasTestPlugin_SetSharedState 注入）。
    FactoryTaskPluginPackage();

    uint32_t AddRef() override { return ++ref_count_; }

    uint32_t Release() override
    {
        auto count = --ref_count_;
        if (count == 0)
        {
            delete this;
        }
        return count;
    }

    DasResult QueryInterface(const DasGuid& iid, void** pp_out) override;

    DasResult EnumFeature(
        uint64_t                                index,
        Das::PluginInterface::DasPluginFeature* p_out_feature) override;

    DasResult CreateFeatureInterface(
        uint64_t   index,
        IDasBase** pp_out_interface) override;

private:
    std::atomic<uint32_t>   ref_count_{0};
    FactoryTaskSharedState* state_ = nullptr;
};

DAS_NS_END

#endif // DAS_PLUGINS_SCHEDULERTESTPLUGIN_SCHEDULERTESTPLUGINIMPL_H

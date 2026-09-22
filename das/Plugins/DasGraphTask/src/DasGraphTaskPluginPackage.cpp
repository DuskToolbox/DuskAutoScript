#include <das/Plugins/DasGraphTask/DasGraphTaskPluginPackage.h>

#include "DasGraphTaskAuthoringSessionFactory.h"
#include "GraphTaskBoundaryComponents.h"
#include "GraphTaskFocusLogComponent.h"

#include <das/Core/GraphRuntime/GraphBoundaryComponents.h>
#include <das/DasApi.h>
#include <das/DasGuidHolder.h>
#include <das/IDasBase.h>
#include <das/Plugins/DasGraphTask/DasGraphTaskAdapter.h>
#include <das/Plugins/DasGraphTask/DasGraphTaskImpl.h>
#include <das/Utils/CommonUtils.hpp>

#include <array>
#include <new>
#include <string>

// Factory GUID: {E8D1A2B3-4C5F-6D7E-8A9B-0C1D2E3F4A5B}
// Must match DasGraphTask.json taskComponents.factories for manifest lookup.
DAS_DEFINE_CLASS_GUID_HOLDER_IN_NAMESPACE(
    Das::Plugins::DasGraphTask,
    DasGraphTaskComponentFactory,
    0xE8D1A2B3,
    0x4C5F,
    0x6D7E,
    0x8A,
    0x9B,
    0x0C,
    0x1D,
    0x2E,
    0x3F,
    0x4A,
    0x5B)

// Package GUID: {A1B2C3D4-E5F6-4A7B-8C9D-0E1F2A3B4C5D}
DAS_DEFINE_CLASS_GUID_HOLDER_IN_NAMESPACE(
    Das::Plugins::DasGraphTask,
    DasGraphTaskPluginPackage,
    0xA1B2C3D4,
    0xE5F6,
    0x4A7B,
    0x8C,
    0x9D,
    0x0E,
    0x1F,
    0x2A,
    0x3B,
    0x4C,
    0x5D)

namespace Das::Plugins::DasGraphTask
{

    // === DasGraphTaskComponentFactory ===

    DasGraphTaskComponentFactory::DasGraphTaskComponentFactory(
        Das::PluginInterface::IDasTaskComponentCreator* p_host)
        : host_(DasPtr<Das::PluginInterface::IDasTaskComponentCreator>(p_host))
    {
    }

    DasResult DasGraphTaskComponentFactory::CreateComponent(
        const DasGuid&                            component_guid,
        Das::PluginInterface::IDasTaskComponent** pp_out_component)
    {
        if (pp_out_component == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_component = nullptr;

        try
        {
            // boundary GUID 单一事实源在 Core 的 GraphBoundaryComponents.h
            // 常量。
            DasGuid boundary_guid{};
            if (DAS::IsOk(DasMakeDasGuid(
                    std::string(
                        Das::Core::GraphRuntime::GRAPH_BEGIN_COMPONENT_GUID)
                        .c_str(),
                    &boundary_guid))
                && component_guid == boundary_guid)
            {
                auto* component = new GraphTaskBoundaryComponent(
                    GRAPH_BEGIN_KIND,
                    host_.Get());
                component->AddRef();
                *pp_out_component =
                    static_cast<Das::PluginInterface::IDasTaskComponent*>(
                        component);
                return DAS_S_OK;
            }
            if (DAS::IsOk(DasMakeDasGuid(
                    std::string(
                        Das::Core::GraphRuntime::GRAPH_END_COMPONENT_GUID)
                        .c_str(),
                    &boundary_guid))
                && component_guid == boundary_guid)
            {
                auto* component =
                    new GraphTaskBoundaryComponent(GRAPH_END_KIND, host_.Get());
                component->AddRef();
                *pp_out_component =
                    static_cast<Das::PluginInterface::IDasTaskComponent*>(
                        component);
                return DAS_S_OK;
            }

            // focus-log：GUID 单一事实源在组件头常量；生产不注入
            // recorder（Do 内经 C ABI 具名服务 resolve）。
            DasGuid focus_log_guid{};
            if (DAS::IsOk(DasMakeDasGuid(
                    std::string(FOCUS_LOG_COMPONENT_GUID_TEXT).c_str(),
                    &focus_log_guid))
                && component_guid == focus_log_guid)
            {
                auto* component = new GraphTaskFocusLogComponent();
                component->AddRef();
                *pp_out_component =
                    static_cast<Das::PluginInterface::IDasTaskComponent*>(
                        component);
                return DAS_S_OK;
            }

            const DasGuid graph_task_guid = DasIidOf<DasGraphTaskImpl>();
            if (!(component_guid == graph_task_guid))
            {
                return DAS_E_NOT_FOUND;
            }

            auto* component = new DasGraphTaskImpl(host_.Get());
            component->AddRef();
            *pp_out_component =
                static_cast<Das::PluginInterface::IDasTaskComponent*>(
                    component);
            return DAS_S_OK;
        }
        catch (const std::bad_alloc&)
        {
            return DAS_E_OUT_OF_MEMORY;
        }
    }

    // === DasGraphTaskPluginPackage ===

    DasResult DasGraphTaskPluginPackage::EnumFeature(
        size_t                                  index,
        Das::PluginInterface::DasPluginFeature* p_out_feature)
    {
        if (p_out_feature == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }

        // Mirrors DasMaaPi: TASK (IDasTask entry) + AUTHORING_FACTORY +
        // COMPONENT_FACTORY (graph-node path), plus two appended boundary-host
        // TASK entries (begin/end capability carriers). Index order must stay
        // aligned with CreateFeatureInterface below; 0/1/2 keep their
        // historical indices (tests pin them).
        static constexpr std::array<Das::PluginInterface::DasPluginFeature, 5>
            features{
                Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK,
                Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY,
                Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY,
                Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK,
                Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK};
        if (index >= features.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }

        *p_out_feature = features[index];
        return DAS_S_OK;
    }

    DasResult DasGraphTaskPluginPackage::CreateFeatureInterface(
        size_t     index,
        IDasBase** pp_out_interface)
    {
        if (pp_out_interface == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_interface = nullptr;

        try
        {
            if (index == 0)
            {
                // TASK -> IDasTask: the Scheduler top-level executor and the
                // entry by which das.graph appears in availableTaskTypes.
                // host 经构造注入（包默认存储）。
                auto* task = new DasGraphTaskAdapter{GetTaskComponentCreator()};
                task->AddRef();
                *pp_out_interface =
                    static_cast<Das::PluginInterface::IDasTask*>(task);
                return DAS_S_OK;
            }
            if (index == 1)
            {
                // TASK_AUTHORING_FACTORY: wires up the pre-existing
                // DasGraphTaskAuthoringSessionFactory (GUID EFC3D51E), which
                // was implemented but never registered (dead code) before this.
                // creator 经构造注入（包默认存储 host_——D-07 会话编译路径
                // 的注入通道来源）。
                auto* factory = new DasGraphTaskAuthoringSessionFactory{
                    GetTaskComponentCreator()};
                factory->AddRef();
                *pp_out_interface = static_cast<
                    Das::PluginInterface::IDasTaskAuthoringSessionFactory*>(
                    factory);
                return DAS_S_OK;
            }
            if (index == 2)
            {
                // TASK_COMPONENT_FACTORY: existing graph-node component path;
                // host 经构造注入（包默认存储）。
                auto* factory =
                    new DasGraphTaskComponentFactory{GetTaskComponentCreator()};
                factory->AddRef();
                *pp_out_interface = static_cast<
                    Das::PluginInterface::IDasTaskComponentFactory*>(factory);
                return DAS_S_OK;
            }
            if (index == 3 || index == 4)
            {
                // TASK -> IDasTask（begin/end capability 托管载体）：供
                // SchedulerService 既有能力注册面按 begin/end 组件 guid 建
                // record——QI 链据此把 CreateSession(组件 guid) 路由到本插
                // 件会话工厂（其 begin/end 分支包装 Core 边界会话）。结构
                // 组件不可调度（Do/排程一律拒绝）。
                DasGuid                boundary_guid{};
                const std::string_view boundary_kind =
                    index == 3
                        ? Das::Core::GraphRuntime::GRAPH_BEGIN_COMPONENT_GUID
                        : Das::Core::GraphRuntime::GRAPH_END_COMPONENT_GUID;
                if (DAS::IsFailed(DasMakeDasGuid(
                        std::string(boundary_kind).c_str(),
                        &boundary_guid)))
                {
                    return DAS_E_FAIL;
                }
                auto* task = new GraphTaskBoundaryHostTask{boundary_guid};
                task->AddRef();
                *pp_out_interface =
                    static_cast<Das::PluginInterface::IDasTask*>(task);
                return DAS_S_OK;
            }
        }
        catch (const std::bad_alloc&)
        {
            return DAS_E_OUT_OF_MEMORY;
        }

        return DAS_E_OUT_OF_RANGE;
    }

} // namespace Das::Plugins::DasGraphTask

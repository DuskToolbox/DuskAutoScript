#define DAS_BUILD_SHARED

#include "DasGraphTaskAuthoringSessionFactory.h"

#include "DasGraphTaskAuthoringSession.h"

#include <das/Core/GraphRuntime/GraphBoundaryComponents.h>
#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/IDasBase.h>

#include <new>
#include <string>
#include <string_view>
#include <utility>

DAS_NS_BEGIN
namespace Plugins::DasGraphTask
{
    namespace
    {
        /// Read the context document JSON (a formSequence doc, a graph doc, or
        /// empty) for the session to seed its Core-owned store from.
        std::string InitialSequenceFromContext(
            ExportInterface::IDasJson* context)
        {
            if (context == nullptr)
            {
                return {};
            }
            DasPtr<IDasReadOnlyString> text;
            if (DAS::IsFailed(context->ToString(0, text.Put()))
                || text.Get() == nullptr)
            {
                return {};
            }
            const char* utf8 = nullptr;
            if (DAS::IsFailed(text->GetUtf8(&utf8)) || utf8 == nullptr)
            {
                return {};
            }
            return std::string(utf8);
        }

        // begin/end 边界组件 guid 的 DasGuid 形态——从 Core
        // GraphBoundaryComponents.h 的 string_view 单一事实源静态解析一次
        // （结构体 == 结构体比对；不再把调用方手里的 guid 序列化回串）。
        DasGuid GuidFromText(std::string_view guid_text)
        {
            DasGuid           parsed{};
            const std::string owned{guid_text};
            DasMakeDasGuid(owned.c_str(), &parsed);
            return parsed;
        }

        const DasGuid& BeginComponentGuidStorage()
        {
            static const DasGuid guid = GuidFromText(
                Das::Core::GraphRuntime::GRAPH_BEGIN_COMPONENT_GUID);
            return guid;
        }

        const DasGuid& EndComponentGuidStorage()
        {
            static const DasGuid guid =
                GuidFromText(Das::Core::GraphRuntime::GRAPH_END_COMPONENT_GUID);
            return guid;
        }
    } // namespace

    DasResult DasGraphTaskAuthoringSessionFactory::CreateSession(
        const DasGuid&                              task_guid,
        ExportInterface::IDasJson*                  p_context_json,
        PluginInterface::IDasTaskAuthoringSession** pp_out_session)
    {
        if (pp_out_session == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_session = nullptr;

        // begin/end 边界组件 guid：托管 Core 边界 authoring 会话（钦定出
        // 口）。guid 直传指针、context 原文经只读串透传——会话由 Core
        // 侧 new+AddRef 自持；非 DAS_S_OK 返回码原样透传（fail-closed）。
        if (task_guid == BeginComponentGuidStorage()
            || task_guid == EndComponentGuidStorage())
        {
            const std::string context =
                InitialSequenceFromContext(p_context_json);
            DasPtr<IDasReadOnlyString> context_text;
            if (!context.empty())
            {
                CreateIDasReadOnlyStringFromUtf8(
                    context.c_str(),
                    context_text.Put());
            }
            return CreateGraphBoundaryAuthoringSession(
                &task_guid,
                context_text.Get(),
                pp_out_session);
        }

        try
        {
            auto initial = InitialSequenceFromContext(p_context_json);
            // creator 借用指针随会话下传（begin/end 边界会话分支不传——
            // 边界会话无绑条目节点）。
            auto* session =
                new DasGraphTaskAuthoringSession(std::move(initial), creator_);
            session->AddRef();
            *pp_out_session = session;
            return DAS_S_OK;
        }
        catch (const std::bad_alloc&)
        {
            return DAS_E_OUT_OF_MEMORY;
        }
    }
} // namespace Plugins::DasGraphTask
DAS_NS_END

#include <das/Core/TaskScheduler/SchedulerServiceImpl.h>
#include <das/DasExport.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasGuidVector.h>
#include <filesystem>
#include <new>
#include <string_view>
#include <vector>

namespace Das::Core::TaskScheduler
{
    namespace
    {
        DasResult ParseObjectJsonString(
            IDasReadOnlyString* p_json,
            yyjson::value&      out_json)
        {
            DAS_UTILS_CHECK_POINTER(p_json)

            const char* u8_str = nullptr;
            auto        result = p_json->GetUtf8(&u8_str);
            if (DAS::IsFailed(result))
            {
                return result;
            }

            auto parsed = Das::Utils::ParseYyjsonFromString(
                u8_str ? std::string_view(u8_str) : std::string_view{});
            if (!parsed || !parsed->is_object())
            {
                return DAS_E_INVALID_JSON;
            }

            out_json = std::move(*parsed);
            return DAS_S_OK;
        }

        DasResult WriteJsonString(
            const yyjson::value& json,
            IDasReadOnlyString** pp_out_json)
        {
            DAS_UTILS_CHECK_POINTER(pp_out_json)

            auto serialized = Das::Utils::SerializeYyjsonValue(json, false);
            if (!serialized)
            {
                return DAS_E_INVALID_JSON;
            }

            DasOutPtr<IDasReadOnlyString> result(pp_out_json);
            auto cr = CreateIDasReadOnlyStringFromUtf8(
                serialized->c_str(),
                result.Put());
            if (DAS::IsOk(cr))
            {
                result.Keep();
            }
            return cr;
        }
    }

    SchedulerServiceImpl::SchedulerServiceImpl(SchedulerService& svc)
        : svc_(svc)
    {
    }

    uint32_t DAS_STD_CALL SchedulerServiceImpl::AddRef()
    {
        return ++ref_count_;
    }

    uint32_t DAS_STD_CALL SchedulerServiceImpl::Release()
    {
        auto count = --ref_count_;
        if (count == 0)
        {
            delete this;
        }
        return count;
    }

    DasResult DAS_STD_CALL
    SchedulerServiceImpl::QueryInterface(const DasGuid& iid, void** pp_out)
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

        if (iid == DasIidOf<IDasSchedulerService>())
        {
            *pp_out = static_cast<IDasSchedulerService*>(this);
            AddRef();
            return DAS_S_OK;
        }

        *pp_out = nullptr;
        return DAS_E_NO_INTERFACE;
    }

    DasResult SchedulerServiceImpl::Initialize(
        IDasReadOnlyString*                           p_plugin_dir,
        Das::ExportInterface::IDasReadOnlyGuidVector* p_disabled_guids)
    {
        DAS_UTILS_CHECK_POINTER(p_plugin_dir)

        // 反序列化 plugin_dir: IDasReadOnlyString -> std::filesystem::path
        const char* u8_path = nullptr;
        auto        result = p_plugin_dir->GetUtf8(&u8_path);
        if (DAS::IsFailed(result))
        {
            return result;
        }
        std::filesystem::path plugin_dir =
            std::filesystem::path(reinterpret_cast<const char8_t*>(u8_path));

        // 反序列化 disabled_guids: IDasReadOnlyGuidVector ->
        // std::vector<DasGuid>
        std::vector<DasGuid> disabled_guids;
        if (p_disabled_guids)
        {
            uint64_t size = 0;
            p_disabled_guids->Size(&size);
            disabled_guids.reserve(static_cast<size_t>(size));
            for (uint64_t i = 0; i < size; ++i)
            {
                DasGuid guid;
                auto    at_result = p_disabled_guids->At(i, &guid);
                if (DAS::IsFailed(at_result))
                {
                    return at_result;
                }
                disabled_guids.push_back(guid);
            }
        }

        return svc_.Initialize(plugin_dir, disabled_guids);
    }

    DasResult SchedulerServiceImpl::Start() { return svc_.Enable(); }

    DasResult SchedulerServiceImpl::Stop() { return svc_.Disable(); }

    DasResult SchedulerServiceImpl::GetState(SchedulerState* p_out_state) const
    {
        if (p_out_state == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *p_out_state = svc_.Status();
        return DAS_S_OK;
    }

    DasResult SchedulerServiceImpl::Get(IDasReadOnlyString** pp_out_json)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_json)

        DasOutPtr<IDasReadOnlyString> result(pp_out_json);

        auto json = svc_.Get();
        auto serialized = Das::Utils::SerializeYyjsonValue(json, false);
        if (!serialized)
        {
            return DAS_E_INVALID_JSON;
        }

        auto cr =
            CreateIDasReadOnlyStringFromUtf8(serialized->c_str(), result.Put());
        if (DAS::IsOk(cr))
        {
            result.Keep();
        }
        return cr;
    }

    DasResult
    SchedulerServiceImpl::GetTaskRepository(IDasReadOnlyString** pp_out_json)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_json)

        auto result = svc_.GetTaskRepository();
        return WriteJsonString(result, pp_out_json);
    }

    DasResult SchedulerServiceImpl::CreateRepositoryEntry(
        IDasReadOnlyString*  p_request_json,
        IDasReadOnlyString** pp_out_json)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_json)

        yyjson::value request;
        auto parse_result = ParseObjectJsonString(p_request_json, request);
        if (DAS::IsFailed(parse_result))
        {
            return parse_result;
        }

        auto result = svc_.CreateRepositoryEntry(request);
        return WriteJsonString(result, pp_out_json);
    }

    DasResult SchedulerServiceImpl::AddTask(
        const DasGuid& task_guid,
        int64_t*       p_out_task_id)
    {
        DAS_UTILS_CHECK_POINTER(p_out_task_id)

        return svc_.AddTask(task_guid, p_out_task_id);
    }

    DasResult SchedulerServiceImpl::DeleteTask(int64_t task_id)
    {
        return svc_.DeleteTask(task_id);
    }

    DasResult SchedulerServiceImpl::UpdateTaskProperties(
        int64_t             task_id,
        IDasReadOnlyString* p_properties_json)
    {
        DAS_UTILS_CHECK_POINTER(p_properties_json)

        const char* u8_str = nullptr;
        auto        result = p_properties_json->GetUtf8(&u8_str);
        if (DAS::IsFailed(result))
        {
            return result;
        }

        auto props = Das::Utils::ParseYyjsonFromString(
            u8_str ? std::string_view(u8_str) : std::string_view{});
        if (!props)
        {
            return DAS_E_INVALID_JSON;
        }
        return svc_.UpdateTaskProperties(task_id, *props);
    }

    DasResult SchedulerServiceImpl::UpdateTaskInternalProperties(
        int64_t             task_id,
        IDasReadOnlyString* p_properties_json)
    {
        DAS_UTILS_CHECK_POINTER(p_properties_json)

        const char* u8_str = nullptr;
        auto        result = p_properties_json->GetUtf8(&u8_str);
        if (DAS::IsFailed(result))
        {
            return result;
        }

        auto props = Das::Utils::ParseYyjsonFromString(
            u8_str ? std::string_view(u8_str) : std::string_view{});
        if (!props)
        {
            return DAS_E_INVALID_JSON;
        }
        return svc_.UpdateTaskInternalProperties(task_id, *props);
    }

    DasResult SchedulerServiceImpl::GetTaskAuthoringDocument(
        int64_t              task_id,
        IDasReadOnlyString*  p_request_json,
        IDasReadOnlyString** pp_out_json)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_json)

        yyjson::value request;
        auto parse_result = ParseObjectJsonString(p_request_json, request);
        if (DAS::IsFailed(parse_result))
        {
            return parse_result;
        }

        auto result = svc_.GetTaskAuthoringDocument(task_id, request);
        return WriteJsonString(result, pp_out_json);
    }

    DasResult SchedulerServiceImpl::ApplyTaskAuthoringChange(
        int64_t              task_id,
        IDasReadOnlyString*  p_change_json,
        IDasReadOnlyString** pp_out_json)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_json)

        yyjson::value change;
        auto parse_result = ParseObjectJsonString(p_change_json, change);
        if (DAS::IsFailed(parse_result))
        {
            return parse_result;
        }

        auto result = svc_.ApplyTaskAuthoringChange(task_id, change);
        return WriteJsonString(result, pp_out_json);
    }

    DasResult SchedulerServiceImpl::CompileTaskAuthoring(
        int64_t              task_id,
        IDasReadOnlyString*  p_request_json,
        IDasReadOnlyString** pp_out_json)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_json)

        yyjson::value request;
        auto parse_result = ParseObjectJsonString(p_request_json, request);
        if (DAS::IsFailed(parse_result))
        {
            return parse_result;
        }

        auto result = svc_.CompileTaskAuthoring(task_id, request);
        return WriteJsonString(result, pp_out_json);
    }

    DasResult SchedulerServiceImpl::SetStateNotifyCallback(
        SchedulerNotifyFunc func,
        void*               user_data)
    {
        svc_.SetStateNotifyCallback(func, user_data);
        return DAS_S_OK;
    }

} // namespace Das::Core::TaskScheduler

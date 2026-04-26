#include <das/Core/TaskScheduler/SchedulerServiceImpl.h>
#include <das/DasExport.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/abi/IDasGuidVector.h>
#include <filesystem>
#include <new>
#include <nlohmann/json.hpp>
#include <vector>

namespace Das::Core::TaskScheduler
{

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
        auto json_str = json.dump();

        auto cr =
            CreateIDasReadOnlyStringFromUtf8(json_str.c_str(), result.Put());
        if (DAS::IsOk(cr))
        {
            result.Keep();
        }
        return cr;
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

        try
        {
            auto props = nlohmann::json::parse(u8_str);
            return svc_.UpdateTaskProperties(task_id, props);
        }
        catch (const nlohmann::json::exception&)
        {
            return DAS_E_INVALID_JSON;
        }
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

        try
        {
            auto props = nlohmann::json::parse(u8_str);
            return svc_.UpdateTaskInternalProperties(task_id, props);
        }
        catch (const nlohmann::json::exception&)
        {
            return DAS_E_INVALID_JSON;
        }
    }

} // namespace Das::Core::TaskScheduler

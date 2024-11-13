#include <AutoStarRail/Core/ForeignInterfaceHost/CppSwigInterop.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/TaskManager.h>
#include <AutoStarRail/Core/Logger/Logger.h>
#include <AutoStarRail/Core/Utils/InternalUtils.h>

#include <utility>

ASR_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

ASR_NS_ANONYMOUS_DETAILS_BEGIN

template <class Map>
AsrResult AddTask(
    Map&                          map,
    std::shared_ptr<PluginDesc>   sp_desc,
    AsrGuid                       key,
    AsrPtr<TaskManager::TaskInfo> value)
{
    if (const auto it = map.find(key); it != map.end())
    {
        return ASR_E_DUPLICATE_ELEMENT;
    }

    AsrPtr<IAsrWeakReference> p_weak_task_info{};
    value->GetWeakReference(p_weak_task_info.Put());

    const boost::signals2::scoped_connection connection =
        sp_desc->on_settings_changed.connect(
            [p_same_weak_task_info = std::move(p_weak_task_info)](
                std::shared_ptr<PluginDesc::SettingsJson> sp_settings_json)
            {
                AsrPtr<IAsrBase> p_base{};
                const auto       resolve_result =
                    p_same_weak_task_info->Resolve(p_base.Put());
                if (IsFailed(resolve_result))
                {
                    return;
                }

                AsrPtr<TaskManager::TaskInfo> p_task_info{};
                const auto qi_result = p_base.As(p_task_info);
                if (IsFailed(qi_result))
                {
                    ASR_CORE_LOG_ERROR(
                        "Failed to get TaskManager::TaskInfo. Error code = {}. Pointer = {}",
                        qi_result,
                        Utils::VoidP(p_base.Get()));
                    return;
                }

                AsrPtr<IAsrReadOnlyString> p_settings_json{};
                sp_settings_json->GetValue(p_settings_json.Put());
                p_task_info->SetSettingsJson(p_settings_json.Get());
            });

    map[key] = std::move(value);
    return ASR_S_OK;
}

template <const char* ErrorMessage, class T>
AsrResult GetTaskProperty(T getter)
{
    const auto error_code = getter();
    if (IsFailed(error_code))
    {
        ASR_CORE_LOG_ERROR(ErrorMessage, error_code);
        return ASR_S_FALSE;
    }
    return ASR_S_OK;
}

ASR_NS_ANONYMOUS_DETAILS_END

Details::TaskInfoImpl::TaskInfoImpl(IAsrTask* p_task)
{
    AsrResult result;

    result = p_task->GetGuid(&iid_);
    if (IsFailed(result))
    {
        ASR_CORE_LOG_ERROR("Failed to get guid. Error code = ", result);
        result = ASR_S_FALSE;
    }

    constexpr static char GET_RUNTIME_CLASS_NAME_FAILED_MESSAGE[] =
        "Failed to get task runtime class name. Error code = {}.";
    result = Details::GetTaskProperty<GET_RUNTIME_CLASS_NAME_FAILED_MESSAGE>(
        [this, p_task]
        { return p_task->GetRuntimeClassName(type_name_.Put()); });
    SetStateByCheckResult(result);

    constexpr static char GET_NAME_FAILED_MESSAGE[] =
        "Failed to get task name. Error code = {}.";
    result = Details::GetTaskProperty<GET_NAME_FAILED_MESSAGE>(
        [this, p_task] { return p_task->GetName(name_.Put()); });
    SetStateByCheckResult(result);

    constexpr static char GET_DESCRIPTION_FAILED_MESSAGE[] =
        "Failed to get task description. Error code = {}.";
    result = Details::GetTaskProperty<GET_DESCRIPTION_FAILED_MESSAGE>(
        [this, p_task] { return p_task->GetDescription(description_.Put()); });
    SetStateByCheckResult(result);

    constexpr static char GET_LABEL_FAILED_MESSAGE[] =
        "Failed to get task label. Error code = {}.";
    result = Details::GetTaskProperty<GET_LABEL_FAILED_MESSAGE>(
        [this, p_task] { return p_task->GetLabel(label_.Put()); });
    SetStateByCheckResult(result);
}

IAsrReadOnlyString* Details::TaskInfoImpl::GetName() const noexcept
{
    return name_.To<IAsrReadOnlyString*>();
}

IAsrReadOnlyString* Details::TaskInfoImpl::GetDescription() const noexcept
{
    return description_.To<IAsrReadOnlyString*>();
}

IAsrReadOnlyString* Details::TaskInfoImpl::GetLabel() const noexcept
{
    return label_.To<IAsrReadOnlyString*>();
}

IAsrReadOnlyString* Details::TaskInfoImpl::GetTypeName() const noexcept
{
    return type_name_.To<IAsrReadOnlyString*>();
}

IAsrReadOnlyString* Details::TaskInfoImpl::GetSettingsJson() const noexcept
{
    return settings_json_.Get();
}

void Details::TaskInfoImpl::SetSettingsJson(IAsrReadOnlyString* p_settings)
{
    settings_json_ = p_settings;
}

void Details::TaskInfoImpl::GetTask(IAsrTask** pp_out_task) const
{
    *pp_out_task = p_task_.Get();
    p_task_->AddRef();
}

IAsrTask* Details::TaskInfoImpl::GetTask() const noexcept
{
    return p_task_.Get();
}

AsrResult Details::TaskInfoImpl::GetProperty(
    const char*  property_name,
    const char** pp_out_value) const
{
    ASR_UTILS_CHECK_POINTER(property_name);
    ASR_UTILS_CHECK_POINTER(pp_out_value);

    std::string buffer;
    std::string input{property_name};
    if (property_name
        == ASR_TASK_INFO_PROPERTIES[ASR_TASK_INFO_PROPERTIES_NAME_INDEX])
    {
        *pp_out_value = name_.To<const char*>();
    }
    else if (
        property_name
        == ASR_TASK_INFO_PROPERTIES[ASR_TASK_INFO_PROPERTIES_DESCRIPTION_INDEX])
    {
        *pp_out_value = description_.To<const char*>();
    }
    else if (
        property_name
        == ASR_TASK_INFO_PROPERTIES[ASR_TASK_INFO_PROPERTIES_LABEL_INDEX])
    {
        *pp_out_value = label_.To<const char*>();
    }
    else if (
        property_name
        == ASR_TASK_INFO_PROPERTIES[ASR_TASK_INFO_PROPERTIES_TYPE_NAME_INDEX])
    {
        *pp_out_value = type_name_.To<const char*>();
    }
    return ASR_S_OK;
}

AsrResult Details::TaskInfoImpl::GetInitializeState() const noexcept
{
    return state_;
}

AsrResult Details::TaskInfoImpl::GetIid(AsrGuid* p_out_iid) const noexcept
{
    ASR_UTILS_CHECK_POINTER(p_out_iid);
    *p_out_iid = iid_;
    return ASR_S_OK;
}
void Details::TaskInfoImpl::SetConnection(
    boost::signals2::connection&& connection)
{
    on_settings_changed_handler_ = std::move(connection);
}

void Details::TaskInfoImpl::SetStateByCheckResult(AsrResult result)
{
    if (IsFailed(result))
    {
        state_ = ASR_S_FALSE;
    }
}

TaskManager::TaskInfo::TaskInfo(IAsrTask* p_task)
    : sp_impl_{std::make_shared<Details::TaskInfoImpl>(p_task)}
{
}

TaskManager::TaskInfo::TaskInfo(SPImpl sp_impl) : sp_impl_{std::move(sp_impl)}
{
}

IAsrReadOnlyString* TaskManager::TaskInfo::GetName() const noexcept
{
    return sp_impl_->GetName();
}

IAsrReadOnlyString* TaskManager::TaskInfo::GetDescription() const noexcept
{
    return sp_impl_->GetDescription();
}

IAsrReadOnlyString* TaskManager::TaskInfo::GetLabel() const noexcept
{
    return sp_impl_->GetLabel();
}

IAsrReadOnlyString* TaskManager::TaskInfo::GetTypeName() const noexcept
{
    return sp_impl_->GetTypeName();
}

IAsrReadOnlyString* TaskManager::TaskInfo::GetSettingsJson() const noexcept
{
    return sp_impl_->GetSettingsJson();
}

void TaskManager::TaskInfo::SetSettingsJson(IAsrReadOnlyString* p_settings)
{
    sp_impl_->SetSettingsJson(p_settings);
}

AsrResult TaskManager::TaskInfo::QueryInterface(
    const AsrGuid& iid,
    void**         pp_out_object)
{
    return Utils::QueryInterface<IAsrTaskInfo>(this, iid, pp_out_object);
}

AsrResult TaskManager::TaskInfo::GetWeakReference(
    IAsrWeakReference** pp_out_weak)
{
    ASR_UTILS_CHECK_POINTER(pp_out_weak);
    try
    {
        const auto p_result = MakeAsrPtr<TaskInfoWeakRefImpl>(sp_impl_);
        Utils::SetResult(p_result, pp_out_weak);
        return ASR_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return ASR_E_OUT_OF_MEMORY;
    }
}

AsrResult TaskManager::TaskInfo::GetProperty(
    const char*  property_name,
    const char** pp_out_value)
{
    return sp_impl_->GetProperty(property_name, pp_out_value);
}

AsrResult TaskManager::TaskInfo::GetInitializeState()
{
    return sp_impl_->GetInitializeState();
}

AsrResult TaskManager::TaskInfo::GetIid(AsrGuid* p_out_iid)
{
    return sp_impl_->GetIid(p_out_iid);
}

void TaskManager::TaskInfo::SetConnection(
    boost::signals2::connection&& connection)
{
    sp_impl_->SetConnection(std::move(connection));
}

void TaskManager::TaskInfo::GetTask(IAsrTask** pp_out_task)
{
    sp_impl_->GetTask(pp_out_task);
}

IAsrTask* TaskManager::TaskInfo::GetTask() const noexcept
{
    return sp_impl_->GetTask();
}

TaskManager::TaskInfoWeakRefImpl::TaskInfoWeakRefImpl(
    const std::shared_ptr<Details::TaskInfoImpl>& sp_data)
    : wp_impl_{sp_data}
{
}

AsrResult TaskManager::TaskInfoWeakRefImpl::QueryInterface(
    const AsrGuid& iid,
    void**         pp_out_object)
{
    return Utils::QueryInterface<IAsrWeakReference>(this, iid, pp_out_object);
}

AsrResult TaskManager::TaskInfoWeakRefImpl::Resolve(IAsrBase** pp_out_object)
{
    const auto sp_impl = wp_impl_.lock();
    if (!sp_impl)
    {
        return ASR_E_STRONG_REFERENCE_NOT_AVAILABLE;
    }
    ASR_UTILS_CHECK_POINTER(pp_out_object);
    const auto p_result = MakeAsrPtr<TaskInfo>(sp_impl);
    Utils::SetResult(p_result, pp_out_object);
    return ASR_S_OK;
}

AsrResult TaskManager::Register(
    std::shared_ptr<PluginDesc> sp_desc,
    IAsrTask*                   p_task,
    AsrGuid                     guid)
{
    const auto p_task_info = MakeAsrPtr<TaskInfo>(p_task);
    const auto error_code = Details::AddTask(map_, sp_desc, guid, p_task_info);
    if (IsFailed(error_code))
    {
        ASR_CORE_LOG_WARN(
            "Duplicate IAsrTask object registered."
            "Guid = {}. Error code = {}. ",
            error_code,
            guid);
    }
    return error_code;
}

AsrResult TaskManager::Register(
    std::shared_ptr<PluginDesc> sp_desc,
    IAsrSwigTask*               p_swig_task,
    AsrGuid                     guid)
{
    auto expected_p_task = MakeInterop<IAsrTask>(p_swig_task);
    if (!expected_p_task)
    {
        return expected_p_task.error();
    }
    const AsrPtr<IAsrTask> p_task = std::move(expected_p_task.value());
    auto                   p_task_info = MakeAsrPtr<TaskInfo>(p_task.Get());

    const auto error_code = Details::AddTask(map_, sp_desc, guid, p_task_info);
    if (IsFailed(error_code))
    {
        ASR_CORE_LOG_WARN(
            "Duplicate IAsrSwigTask object registered."
            "Guid = {}. Error code = {}.",
            error_code,
            guid);
    }
    return error_code;
}

AsrResult TaskManager::FindInterface(
    const AsrGuid& guid,
    IAsrTask**     pp_out_task)
{
    ASR_UTILS_CHECK_POINTER(pp_out_task)
    if (const auto it = map_.find(guid); it != map_.end())
    {
        const auto p_task = it->second->GetTask();
        Utils::SetResult(p_task, pp_out_task);
        return ASR_S_OK;
    }
    return ASR_E_NO_INTERFACE;
}

ASR_CORE_FOREIGNINTERFACEHOST_NS_END

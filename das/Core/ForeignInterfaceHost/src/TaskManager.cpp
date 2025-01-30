#include <das/Core/ForeignInterfaceHost/CppSwigInterop.h>
#include <das/Core/ForeignInterfaceHost/TaskManager.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/Utils/InternalUtils.h>

#include <utility>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DAS_NS_ANONYMOUS_DETAILS_BEGIN

template <class Map>
DasResult AddTask(
    Map&                               map,
    std::shared_ptr<PluginPackageDesc> sp_desc,
    DasGuid                            key,
    DasPtr<TaskManager::TaskInfo>      value)
{
    if (const auto it = map.find(key); it != map.end())
    {
        return DAS_E_DUPLICATE_ELEMENT;
    }

    DasPtr<IDasWeakReference> p_weak_task_info{};
    value->GetWeakReference(p_weak_task_info.Put());

    const boost::signals2::scoped_connection connection =
        sp_desc->on_settings_changed.connect(
            [p_same_weak_task_info = std::move(p_weak_task_info)](
                std::shared_ptr<PluginPackageDesc::SettingsJson>
                    sp_settings_json)
            {
                DasPtr<IDasBase> p_base{};
                const auto       resolve_result =
                    p_same_weak_task_info->Resolve(p_base.Put());
                if (IsFailed(resolve_result))
                {
                    return;
                }

                DasPtr<TaskManager::TaskInfo> p_task_info{};
                const auto qi_result = p_base.As(p_task_info);
                if (IsFailed(qi_result))
                {
                    DAS_CORE_LOG_ERROR(
                        "Failed to get TaskManager::TaskInfo. Error code = {}. Pointer = {}",
                        qi_result,
                        Utils::VoidP(p_base.Get()));
                    return;
                }

                DasPtr<IDasReadOnlyString> p_settings_json{};
                sp_settings_json->GetValue(p_settings_json.Put());
                p_task_info->SetSettingsJson(p_settings_json.Get());
            });

    map[key] = std::move(value);
    return DAS_S_OK;
}

template <const char* ErrorMessage, class T>
DasResult GetTaskProperty(T getter)
{
    const auto error_code = getter();
    if (IsFailed(error_code))
    {
        DAS_CORE_LOG_ERROR(ErrorMessage, error_code);
        return DAS_S_FALSE;
    }
    return DAS_S_OK;
}

DAS_NS_ANONYMOUS_DETAILS_END

Details::TaskInfoImpl::TaskInfoImpl(IDasTask* p_task)
{
    DasResult result;

    result = p_task->GetGuid(&iid_);
    if (IsFailed(result))
    {
        DAS_CORE_LOG_ERROR("Failed to get guid. Error code = ", result);
        result = DAS_S_FALSE;
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
        [this, p_task] { return p_task->GetGameName(game_name_.Put()); });
    SetStateByCheckResult(result);
}

IDasReadOnlyString* Details::TaskInfoImpl::GetName() const noexcept
{
    IDasReadOnlyString* result{};
    name_.GetTo(result);
    return result;
}

IDasReadOnlyString* Details::TaskInfoImpl::GetDescription() const noexcept
{
    IDasReadOnlyString* result{};
    description_.GetTo(result);
    return result;
}

IDasReadOnlyString* Details::TaskInfoImpl::GetGameName() const noexcept
{
    IDasReadOnlyString* result{};
    game_name_.GetTo(result);
    return result;
}

IDasReadOnlyString* Details::TaskInfoImpl::GetTypeName() const noexcept
{
    IDasReadOnlyString* result{};
    type_name_.GetTo(result);
    return result;
}

IDasReadOnlyString* Details::TaskInfoImpl::GetSettingsJson() const noexcept
{
    return settings_json_.Get();
}

void Details::TaskInfoImpl::SetSettingsJson(IDasReadOnlyString* p_settings)
{
    settings_json_ = p_settings;
}

void Details::TaskInfoImpl::GetTask(IDasTask** pp_out_task) const
{
    *pp_out_task = p_task_.Get();
    p_task_->AddRef();
}

IDasTask* Details::TaskInfoImpl::GetTask() const noexcept
{
    return p_task_.Get();
}

DasResult Details::TaskInfoImpl::GetProperty(
    const char*  property_name,
    const char** pp_out_value) const
{
    DAS_UTILS_CHECK_POINTER(property_name);
    DAS_UTILS_CHECK_POINTER(pp_out_value);

    std::string buffer;
    std::string input{property_name};
    if (property_name
        == DAS_TASK_INFO_PROPERTIES[DAS_TASK_INFO_PROPERTIES_NAME_INDEX])
    {
        name_.GetTo(*pp_out_value);
    }
    else if (
        property_name
        == DAS_TASK_INFO_PROPERTIES[DAS_TASK_INFO_PROPERTIES_DESCRIPTION_INDEX])
    {
        description_.GetTo(*pp_out_value);
    }
    else if (
        property_name
        == DAS_TASK_INFO_PROPERTIES[DAS_TASK_INFO_PROPERTIES_GAME_NAME_INDEX])
    {
        game_name_.GetTo(*pp_out_value);
    }
    else if (
        property_name
        == DAS_TASK_INFO_PROPERTIES[DAS_TASK_INFO_PROPERTIES_TYPE_NAME_INDEX])
    {
        type_name_.GetTo(*pp_out_value);
    }
    return DAS_S_OK;
}

DasResult Details::TaskInfoImpl::GetInitializeState() const noexcept
{
    return state_;
}

DasResult Details::TaskInfoImpl::GetIid(DasGuid* p_out_iid) const noexcept
{
    DAS_UTILS_CHECK_POINTER(p_out_iid);
    *p_out_iid = iid_;
    return DAS_S_OK;
}
void Details::TaskInfoImpl::SetConnection(
    boost::signals2::connection&& connection)
{
    on_settings_changed_handler_ = std::move(connection);
}

void Details::TaskInfoImpl::SetStateByCheckResult(DasResult result)
{
    if (IsFailed(result))
    {
        state_ = DAS_S_FALSE;
    }
}

TaskManager::TaskInfo::TaskInfo(IDasTask* p_task)
    : sp_impl_{std::make_shared<Details::TaskInfoImpl>(p_task)}
{
}

TaskManager::TaskInfo::TaskInfo(SPImpl sp_impl) : sp_impl_{std::move(sp_impl)}
{
}

IDasReadOnlyString* TaskManager::TaskInfo::GetName() const noexcept
{
    return sp_impl_->GetName();
}

IDasReadOnlyString* TaskManager::TaskInfo::GetDescription() const noexcept
{
    return sp_impl_->GetDescription();
}

IDasReadOnlyString* TaskManager::TaskInfo::GetGameName() const noexcept
{
    return sp_impl_->GetGameName();
}

IDasReadOnlyString* TaskManager::TaskInfo::GetTypeName() const noexcept
{
    return sp_impl_->GetTypeName();
}

IDasReadOnlyString* TaskManager::TaskInfo::GetSettingsJson() const noexcept
{
    return sp_impl_->GetSettingsJson();
}

void TaskManager::TaskInfo::SetSettingsJson(IDasReadOnlyString* p_settings)
{
    sp_impl_->SetSettingsJson(p_settings);
}

DasResult TaskManager::TaskInfo::QueryInterface(
    const DasGuid& iid,
    void**         pp_out_object)
{
    return Utils::QueryInterface<IDasTaskInfo>(this, iid, pp_out_object);
}

DasResult TaskManager::TaskInfo::GetWeakReference(
    IDasWeakReference** pp_out_weak)
{
    DAS_UTILS_CHECK_POINTER(pp_out_weak);
    try
    {
        const auto p_result = MakeDasPtr<TaskInfoWeakRefImpl>(sp_impl_);
        Utils::SetResult(p_result, pp_out_weak);
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult TaskManager::TaskInfo::GetProperty(
    const char*  property_name,
    const char** pp_out_value)
{
    return sp_impl_->GetProperty(property_name, pp_out_value);
}

DasResult TaskManager::TaskInfo::GetInitializeState()
{
    return sp_impl_->GetInitializeState();
}

DasResult TaskManager::TaskInfo::GetIid(DasGuid* p_out_iid)
{
    return sp_impl_->GetIid(p_out_iid);
}

void TaskManager::TaskInfo::SetConnection(
    boost::signals2::connection&& connection)
{
    sp_impl_->SetConnection(std::move(connection));
}

void TaskManager::TaskInfo::GetTask(IDasTask** pp_out_task)
{
    sp_impl_->GetTask(pp_out_task);
}

IDasTask* TaskManager::TaskInfo::GetTask() const noexcept
{
    return sp_impl_->GetTask();
}

TaskManager::TaskInfoWeakRefImpl::TaskInfoWeakRefImpl(
    const std::shared_ptr<Details::TaskInfoImpl>& sp_data)
    : wp_impl_{sp_data}
{
}

DasResult TaskManager::TaskInfoWeakRefImpl::QueryInterface(
    const DasGuid& iid,
    void**         pp_out_object)
{
    return Utils::QueryInterface<IDasWeakReference>(this, iid, pp_out_object);
}

DasResult TaskManager::TaskInfoWeakRefImpl::Resolve(IDasBase** pp_out_object)
{
    const auto sp_impl = wp_impl_.lock();
    if (!sp_impl)
    {
        return DAS_E_STRONG_REFERENCE_NOT_AVAILABLE;
    }
    DAS_UTILS_CHECK_POINTER(pp_out_object);
    const auto p_result = MakeDasPtr<TaskInfo>(sp_impl);
    Utils::SetResult(p_result, pp_out_object);
    return DAS_S_OK;
}

DasResult TaskManager::Register(
    std::shared_ptr<PluginPackageDesc> sp_desc,
    IDasTask*                          p_task,
    DasGuid                            guid)
{
    const auto p_task_info = MakeDasPtr<TaskInfo>(p_task);
    const auto error_code = Details::AddTask(map_, sp_desc, guid, p_task_info);
    if (IsFailed(error_code))
    {
        DAS_CORE_LOG_WARN(
            "Duplicate IDasTask object registered."
            "Guid = {}. Error code = {}. ",
            error_code,
            guid);
    }
    return error_code;
}

DasResult TaskManager::Register(
    std::shared_ptr<PluginPackageDesc> sp_desc,
    IDasSwigTask*                      p_swig_task,
    DasGuid                            guid)
{
    auto expected_p_task = MakeInterop<IDasTask>(p_swig_task);
    if (!expected_p_task)
    {
        return expected_p_task.error();
    }
    const DasPtr<IDasTask> p_task = std::move(expected_p_task.value());
    auto                   p_task_info = MakeDasPtr<TaskInfo>(p_task.Get());

    const auto error_code = Details::AddTask(map_, sp_desc, guid, p_task_info);
    if (IsFailed(error_code))
    {
        DAS_CORE_LOG_WARN(
            "Duplicate IDasSwigTask object registered."
            "Guid = {}. Error code = {}.",
            error_code,
            guid);
    }
    return error_code;
}

DasResult TaskManager::FindInterface(
    const DasGuid& guid,
    IDasTask**     pp_out_task)
{
    DAS_UTILS_CHECK_POINTER(pp_out_task)
    if (const auto it = map_.find(guid); it != map_.end())
    {
        const auto p_task = it->second->GetTask();
        Utils::SetResult(p_task, pp_out_task);
        return DAS_S_OK;
    }
    return DAS_E_NO_INTERFACE;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

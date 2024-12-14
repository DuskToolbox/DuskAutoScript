#ifndef DAS_CORE_FOREIGNINTERFACEHOST_TASKMANAGER_H
#define DAS_CORE_FOREIGNINTERFACEHOST_TASKMANAGER_H

#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/ExportInterface/IDasTaskScheduler.h>
#include <das/PluginInterface/IDasTask.h>
#include <das/Utils/CommonUtils.hpp>

#include <boost/signals2/connection.hpp>

#include <unordered_map>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

namespace Details
{
    class TaskInfoImpl
    {
    public:
        TaskInfoImpl(IDasTask* p_task);

        IDasReadOnlyString* GetName() const noexcept;
        IDasReadOnlyString* GetDescription() const noexcept;
        IDasReadOnlyString* GetGameName() const noexcept;
        IDasReadOnlyString* GetTypeName() const noexcept;
        IDasReadOnlyString* GetSettingsJson() const noexcept;
        void                SetSettingsJson(IDasReadOnlyString* p_settings);
        void                GetTask(IDasTask** pp_out_task) const;
        IDasTask*           GetTask() const noexcept;

        // IDasTaskInfo
        DAS_IMPL GetProperty(
            const char*  property_name,
            const char** pp_out_value) const;
        DAS_IMPL GetInitializeState() const noexcept;
        DAS_IMPL GetIid(DasGuid* p_out_iid) const noexcept;

        void SetConnection(boost::signals2::connection&& connection);

    private:
        void SetStateByCheckResult(DasResult result);

        DasResult                          state_{DAS_S_OK};
        DasPtr<IDasTask>                   p_task_;
        DasReadOnlyStringWrapper           name_;
        DasReadOnlyStringWrapper           description_;
        DasReadOnlyStringWrapper           game_name_;
        DasReadOnlyStringWrapper           type_name_;
        DasGuid                            iid_;
        DasReadOnlyStringWrapper           settings_json_;
        boost::signals2::scoped_connection on_settings_changed_handler_;
    };
} // Details

/**
 * @brief task不得被外部调用，因此FindInterface不支持SWIG是符合预期的
 */
class TaskManager
{
public:
    class TaskInfo final : public IDasTaskInfo
    {
        using SPImpl = std::shared_ptr<Details::TaskInfoImpl>;

    public:
        TaskInfo(IDasTask* p_task);
        TaskInfo(SPImpl sp_impl);
        ~TaskInfo() = default;

        IDasReadOnlyString* GetName() const noexcept;
        IDasReadOnlyString* GetDescription() const noexcept;
        IDasReadOnlyString* GetGameName() const noexcept;
        IDasReadOnlyString* GetTypeName() const noexcept;
        IDasReadOnlyString* GetSettingsJson() const noexcept;
        void                SetSettingsJson(IDasReadOnlyString* p_settings);
        void                GetTask(IDasTask** pp_out_task);
        IDasTask*           GetTask() const noexcept;

        // IDasBase
        DAS_UTILS_IDASBASE_AUTO_IMPL(TaskInfo)
        DasResult QueryInterface(const DasGuid& iid, void** pp_object) override;
        // IDasWeakRef
        DAS_IMPL GetWeakReference(IDasWeakReference** pp_out_weak) override;
        // IDasTaskInfo
        DAS_IMPL GetProperty(
            const char*  property_name,
            const char** pp_out_value) override;
        DAS_IMPL GetInitializeState() override;
        DAS_IMPL GetIid(DasGuid* p_out_iid) override;

        void SetConnection(boost::signals2::connection&& connection);

    private:
        SPImpl sp_impl_;
    };

    class TaskInfoWeakRefImpl final : public IDasWeakReference
    {
        DAS_UTILS_IDASBASE_AUTO_IMPL(TaskInfoWeakRefImpl)
        std::weak_ptr<Details::TaskInfoImpl> wp_impl_;

    public:
        TaskInfoWeakRefImpl(
            const std::shared_ptr<Details::TaskInfoImpl>& sp_data);
        ~TaskInfoWeakRefImpl() = default;
        DAS_IMPL
        QueryInterface(const DasGuid& iid, void** pp_out_object) override;
        // IDasWeakRef
        DAS_IMPL Resolve(IDasBase** pp_out_object) override;
    };

private:
    std::unordered_map<DasGuid, DasPtr<TaskInfo>> map_;

public:
    DasResult Register(
        std::shared_ptr<PluginDesc> sp_desc,
        IDasTask*                   p_task,
        DasGuid                     guid);
    DasResult Register(
        std::shared_ptr<PluginDesc> sp_desc,
        IDasSwigTask*               p_swig_task,
        DasGuid                     guid);
    DasResult FindInterface(const DasGuid& guid, IDasTask** pp_out_task);
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

// {A2152D32-A507-4AA9-8FAB-AC9244AB0784}
extern "C++"
{
    template <>
    struct DasIidHolder<DAS::Core::ForeignInterfaceHost::TaskManager::TaskInfo>
    {
        static constexpr DasGuid iid = {
            0xa2152d32,
            0xa507,
            0x4aa9,
            {0x8f, 0xab, 0xac, 0x92, 0x44, 0xab, 0x7, 0x84}};
    };

    template <>
    constexpr const DasGuid&
    DasIidOf<DAS::Core::ForeignInterfaceHost::TaskManager::TaskInfo>()
    {
        return DasIidHolder<
            DAS::Core::ForeignInterfaceHost::TaskManager::TaskInfo>::iid;
    }

    template <>
    constexpr const DasGuid&
    DasIidOf<DAS::Core::ForeignInterfaceHost::TaskManager::TaskInfo*>()
    {
        return DasIidHolder<
            DAS::Core::ForeignInterfaceHost::TaskManager::TaskInfo>::iid;
    }
};

#endif // DAS_CORE_FOREIGNINTERFACEHOST_TASKMANAGER_H

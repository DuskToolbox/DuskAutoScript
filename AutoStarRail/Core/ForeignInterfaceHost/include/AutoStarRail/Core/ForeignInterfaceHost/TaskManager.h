#ifndef ASR_CORE_FOREIGNINTERFACEHOST_TASKMANAGER_H
#define ASR_CORE_FOREIGNINTERFACEHOST_TASKMANAGER_H

#include <AutoStarRail/Core/ForeignInterfaceHost/AsrGuid.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/AsrStringImpl.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/Config.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <AutoStarRail/ExportInterface/IAsrScheduler.h>
#include <AutoStarRail/PluginInterface/IAsrTask.h>
#include <AutoStarRail/Utils/CommonUtils.hpp>

#include <boost/signals2/connection.hpp>

#include <unordered_map>

ASR_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

namespace Details
{
    class TaskInfoImpl
    {
    public:
        TaskInfoImpl(IAsrTask* p_task);

        IAsrReadOnlyString* GetName() const noexcept;
        IAsrReadOnlyString* GetDescription() const noexcept;
        IAsrReadOnlyString* GetLabel() const noexcept;
        IAsrReadOnlyString* GetTypeName() const noexcept;
        IAsrReadOnlyString* GetSettingsJson() const noexcept;
        void                SetSettingsJson(IAsrReadOnlyString* p_settings);
        void                GetTask(IAsrTask** pp_out_task) const;
        IAsrTask*           GetTask() const noexcept;

        // IAsrTaskInfo
        ASR_IMPL GetProperty(
            const char*  property_name,
            const char** pp_out_value) const;
        ASR_IMPL GetInitializeState() const noexcept;
        ASR_IMPL GetIid(AsrGuid* p_out_iid) const noexcept;

        void SetConnection(boost::signals2::connection&& connection);

    private:
        void SetStateByCheckResult(AsrResult result);

        AsrResult                          state_{ASR_S_OK};
        AsrPtr<IAsrTask>                   p_task_;
        AsrReadOnlyStringWrapper           name_;
        AsrReadOnlyStringWrapper           description_;
        AsrReadOnlyStringWrapper           label_;
        AsrReadOnlyStringWrapper           type_name_;
        AsrGuid                            iid_;
        AsrReadOnlyStringWrapper           settings_json_;
        boost::signals2::scoped_connection on_settings_changed_handler_;
    };
} // Details

/**
 * @brief task不得被外部调用，因此FindInterface不支持SWIG是符合预期的
 */
class TaskManager
{
public:
    class TaskInfo final : public IAsrTaskInfo
    {
        using SPImpl = std::shared_ptr<Details::TaskInfoImpl>;

    public:
        TaskInfo(IAsrTask* p_task);
        TaskInfo(SPImpl sp_impl);
        ~TaskInfo() = default;

        IAsrReadOnlyString* GetName() const noexcept;
        IAsrReadOnlyString* GetDescription() const noexcept;
        IAsrReadOnlyString* GetLabel() const noexcept;
        IAsrReadOnlyString* GetTypeName() const noexcept;
        IAsrReadOnlyString* GetSettingsJson() const noexcept;
        void                SetSettingsJson(IAsrReadOnlyString* p_settings);
        void                GetTask(IAsrTask** pp_out_task);
        IAsrTask*           GetTask() const noexcept;

        // IAsrBase
        ASR_UTILS_IASRBASE_AUTO_IMPL(TaskInfo)
        AsrResult QueryInterface(const AsrGuid& iid, void** pp_object) override;
        // IAsrWeakRef
        ASR_IMPL GetWeakReference(IAsrWeakReference** pp_out_weak) override;
        // IAsrTaskInfo
        ASR_IMPL GetProperty(
            const char*  property_name,
            const char** pp_out_value) override;
        ASR_IMPL GetInitializeState() override;
        ASR_IMPL GetIid(AsrGuid* p_out_iid) override;

        void SetConnection(boost::signals2::connection&& connection);

    private:
        SPImpl sp_impl_;
    };

    class TaskInfoWeakRefImpl final : public IAsrWeakReference
    {
        ASR_UTILS_IASRBASE_AUTO_IMPL(TaskInfoWeakRefImpl)
        std::weak_ptr<Details::TaskInfoImpl> wp_impl_;

    public:
        TaskInfoWeakRefImpl(
            const std::shared_ptr<Details::TaskInfoImpl>& sp_data);
        ~TaskInfoWeakRefImpl() = default;
        ASR_IMPL
        QueryInterface(const AsrGuid& iid, void** pp_out_object) override;
        // IAsrWeakRef
        ASR_IMPL Resolve(IAsrBase** pp_out_object) override;
    };

private:
    std::unordered_map<AsrGuid, AsrPtr<TaskInfo>> map_;

public:
    AsrResult Register(
        std::shared_ptr<PluginDesc> sp_desc,
        IAsrTask*                   p_task,
        AsrGuid                     guid);
    AsrResult Register(
        std::shared_ptr<PluginDesc> sp_desc,
        IAsrSwigTask*               p_swig_task,
        AsrGuid                     guid);
    AsrResult FindInterface(const AsrGuid& guid, IAsrTask** pp_out_task);
};

ASR_CORE_FOREIGNINTERFACEHOST_NS_END

// {A2152D32-A507-4AA9-8FAB-AC9244AB0784}
extern "C++"
{
    template <>
    struct AsrIidHolder<ASR::Core::ForeignInterfaceHost::TaskManager::TaskInfo>
    {
        static constexpr AsrGuid iid = {
            0xa2152d32,
            0xa507,
            0x4aa9,
            {0x8f, 0xab, 0xac, 0x92, 0x44, 0xab, 0x7, 0x84}};
    };

    template <>
    constexpr const AsrGuid&
    AsrIidOf<ASR::Core::ForeignInterfaceHost::TaskManager::TaskInfo>()
    {
        return AsrIidHolder<
            ASR::Core::ForeignInterfaceHost::TaskManager::TaskInfo>::iid;
    }

    template <>
    constexpr const AsrGuid&
    AsrIidOf<ASR::Core::ForeignInterfaceHost::TaskManager::TaskInfo*>()
    {
        return AsrIidHolder<
            ASR::Core::ForeignInterfaceHost::TaskManager::TaskInfo>::iid;
    }
};

#endif // ASR_CORE_FOREIGNINTERFACEHOST_TASKMANAGER_H

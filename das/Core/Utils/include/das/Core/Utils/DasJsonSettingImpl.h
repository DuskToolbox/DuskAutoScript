#ifndef DAS_CORE_UTILS_DASJSONSETTINGIMPL_H
#define DAS_CORE_UTILS_DASJSONSETTINGIMPL_H

#include <das/Core/Utils/Config.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasJsonSetting.Implements.hpp>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <shared_mutex>

DAS_CORE_UTILS_NS_BEGIN

class DasJsonSettingImpl final
    : public Das::ExportInterface::DasJsonSettingImplBase<DasJsonSettingImpl>
{
public:
    DasJsonSettingImpl() = default;
    explicit DasJsonSettingImpl(const std::filesystem::path& file_path);
    ~DasJsonSettingImpl() override;

    DAS_IMPL ToString(IDasReadOnlyString** pp_out_string) override;
    DAS_IMPL FromString(IDasReadOnlyString* p_in_settings) override;
    DAS_IMPL SaveToWorkingDirectory(
        IDasReadOnlyString* p_relative_path) override;
    DAS_IMPL Save() override;
    DAS_IMPL SetOnDeletedHandler(
        Das::ExportInterface::IDasJsonSettingOnDeletedHandler* p_handler)
        override;
    DAS_IMPL ExecuteAtomically(
        Das::ExportInterface::IDasJsonSettingOperator* p_operator) override;

private:
    DasResult SaveLocked();

    nlohmann::json            json_;
    mutable std::shared_mutex mutex_;
    std::filesystem::path     file_path_;
    DasPtr<Das::ExportInterface::IDasJsonSettingOnDeletedHandler>
        on_deleted_handler_;
};

DAS_CORE_UTILS_NS_END

#endif // DAS_CORE_UTILS_DASJSONSETTINGIMPL_H

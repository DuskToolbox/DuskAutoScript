#pragma once

#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/TaskScheduler/TaskRepositoryDtos.h>
#include <das/DasTypes.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace Das::Core::TaskScheduler
{
    class TaskRepositoryStore
    {
    public:
        TaskRepositoryStore(
            Das::Core::SettingsManager::SettingsManager& settings,
            std::string                                  profile_id);

        std::vector<Repository::Dto::RepositoryEntryDto> ListEntries() const;

        DasResult CreateEntry(
            const Repository::Dto::CreateRepositoryEntryRequestDto& request,
            std::string_view descriptor_name,
            const std::vector<
                Das::Core::ForeignInterfaceHost::PluginSettingDesc>&
                descriptors,
            Repository::Dto::RepositoryEntryDto& out_entry);

        DasResult DeleteEntry(int64_t entry_id);

        DasResult RenameEntry(
            int64_t entry_id,
            const Repository::Dto::RenameRepositoryEntryRequestDto& request,
            Repository::Dto::RepositoryEntryDto& out_entry);

        DasResult UpdateAuthoring(
            int64_t              entry_id,
            const yyjson::value& accepted_properties,
            const yyjson::value& authoring,
            yyjson::value&       out_entry_json);

    private:
        Das::Core::SettingsManager::SettingsManager& settings_;
        std::string                                  profile_id_;
    };
} // namespace Das::Core::TaskScheduler

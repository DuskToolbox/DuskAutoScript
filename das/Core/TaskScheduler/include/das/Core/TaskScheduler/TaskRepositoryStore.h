#pragma once

#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/Core/TaskScheduler/TaskRepositoryDtos.h>

#include <string>
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

    private:
        Das::Core::SettingsManager::SettingsManager& settings_;
        std::string                                  profile_id_;
    };
} // namespace Das::Core::TaskScheduler

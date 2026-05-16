#include <das/Core/TaskScheduler/TaskRepositoryStore.h>

#include <algorithm>

namespace Das::Core::TaskScheduler
{
    TaskRepositoryStore::TaskRepositoryStore(
        Das::Core::SettingsManager::SettingsManager& settings,
        std::string                                  profile_id)
        : settings_(settings), profile_id_(std::move(profile_id))
    {
    }

    std::vector<Repository::Dto::RepositoryEntryDto>
    TaskRepositoryStore::ListEntries() const
    {
        std::vector<Repository::Dto::RepositoryEntryDto> entries;
        auto ids = settings_.ListTaskRepositoryEntryIds(profile_id_);
        entries.reserve(ids.size());

        for (const auto entry_id : ids)
        {
            auto entry_json =
                settings_.GetTaskRepositoryEntryJson(profile_id_, entry_id);
            if (!entry_json.is_object())
            {
                continue;
            }

            try
            {
                entries.push_back(
                    yyjson::cast<Repository::Dto::RepositoryEntryDto>(
                        entry_json));
            }
            catch (const yyjson::bad_cast&)
            {
                continue;
            }
        }

        std::sort(
            entries.begin(),
            entries.end(),
            [](const auto& lhs, const auto& rhs)
            { return lhs.entry_id < rhs.entry_id; });
        return entries;
    }
} // namespace Das::Core::TaskScheduler

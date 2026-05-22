#include <das/Core/TaskScheduler/TaskRepositoryStore.h>
#include <das/Utils/DasJsonCore.h>

#include <algorithm>
#include <type_traits>
#include <utility>

namespace Das::Core::TaskScheduler
{
    namespace
    {
        template <typename Json>
        static yyjson::value CloneJsonValue(const Json& value)
        {
            return Das::Utils::CloneYyjsonValue(value);
        }

        static bool MatchesDescriptorType(
            const yyjson::value&          value,
            Das::ExportInterface::DasType descriptor_type)
        {
            switch (descriptor_type)
            {
            case Das::ExportInterface::DAS_TYPE_BOOL:
                return value.is_bool();
            case Das::ExportInterface::DAS_TYPE_INT:
                return value.is_int();
            case Das::ExportInterface::DAS_TYPE_FLOAT:
                return value.is_num();
            case Das::ExportInterface::DAS_TYPE_STRING:
                return value.is_string();
            default:
                return false;
            }
        }

        static const Das::Core::ForeignInterfaceHost::PluginSettingDesc*
        FindDescriptor(
            const std::vector<
                Das::Core::ForeignInterfaceHost::PluginSettingDesc>&
                             descriptors,
            std::string_view name)
        {
            auto it = std::find_if(
                descriptors.begin(),
                descriptors.end(),
                [name](const auto& descriptor)
                { return descriptor.name == name; });
            if (it == descriptors.end())
            {
                return nullptr;
            }
            return &*it;
        }

        static void ApplyDescriptorDefaults(
            yyjson::value& accepted_properties,
            const std::vector<
                Das::Core::ForeignInterfaceHost::PluginSettingDesc>&
                descriptors)
        {
            auto props_obj = accepted_properties.as_object();
            for (const auto& desc : descriptors)
            {
                if (std::holds_alternative<std::monostate>(desc.default_value))
                {
                    continue;
                }

                std::visit(
                    [&props_obj, &desc](const auto& value)
                    {
                        if constexpr (!std::is_same_v<
                                          std::decay_t<decltype(value)>,
                                          std::monostate>)
                        {
                            auto property = props_obj->operator[](
                                std::string_view(desc.name));
                            if constexpr (std::is_same_v<
                                              std::decay_t<decltype(value)>,
                                              std::string>)
                            {
                                property = std::string(value);
                            }
                            else
                            {
                                property = value;
                            }
                        }
                    },
                    desc.default_value);
            }
        }

        static DasResult MergeInitialProperties(
            yyjson::value&       accepted_properties,
            const yyjson::value& initial_properties,
            const std::vector<
                Das::Core::ForeignInterfaceHost::PluginSettingDesc>&
                descriptors)
        {
            if (initial_properties.is_null())
            {
                return DAS_S_OK;
            }

            auto initial_obj = initial_properties.as_object();
            if (!initial_obj)
            {
                return DAS_E_INVALID_JSON;
            }

            auto accepted_obj = accepted_properties.as_object();
            for (auto it = initial_obj->begin(); it != initial_obj->end(); ++it)
            {
                const std::string property_name{std::string_view(it->first)};
                auto              property_value = CloneJsonValue(it->second);
                auto* descriptor = FindDescriptor(descriptors, property_name);
                if (!descriptor)
                {
                    return DAS_E_NOT_FOUND;
                }
                if (!MatchesDescriptorType(property_value, descriptor->type))
                {
                    return DAS_E_TYPE_ERROR;
                }

                accepted_obj->operator[](std::string_view(property_name)) =
                    CloneJsonValue(property_value);
            }

            return DAS_S_OK;
        }
    } // namespace

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

    DasResult TaskRepositoryStore::CreateEntry(
        const Repository::Dto::CreateRepositoryEntryRequestDto& request,
        std::string_view                                        descriptor_name,
        const std::vector<Das::Core::ForeignInterfaceHost::PluginSettingDesc>&
                                             descriptors,
        Repository::Dto::RepositoryEntryDto& out_entry)
    {
        Repository::Dto::RepositoryEntryDto entry;
        entry.entry_id =
            settings_.AllocateNextTaskRepositoryEntryId(profile_id_);
        entry.display_name =
            request.display_name.value_or(std::string(descriptor_name));
        entry.plugin_guid = request.plugin_guid;
        entry.task_type_guid = request.task_type_guid;
        entry.authoring.revision = 0;
        entry.accepted_properties = Das::Utils::MakeYyjsonObject();
        entry.availability.state = "available";

        ApplyDescriptorDefaults(entry.accepted_properties, descriptors);
        auto merge_result = MergeInitialProperties(
            entry.accepted_properties,
            request.initial_properties,
            descriptors);
        if (DAS::IsFailed(merge_result))
        {
            return merge_result;
        }

        auto entry_json = CloneJsonValue(yyjson::object(entry));
        auto persist_result = settings_.UpdateTaskRepositoryEntryJson(
            profile_id_,
            entry.entry_id,
            entry_json);
        if (DAS::IsFailed(persist_result))
        {
            return persist_result;
        }

        out_entry = std::move(entry);
        return DAS_S_OK;
    }

    DasResult TaskRepositoryStore::DeleteEntry(int64_t entry_id)
    {
        auto entry_json =
            settings_.GetTaskRepositoryEntryJson(profile_id_, entry_id);
        if (!entry_json.is_object())
        {
            return DAS_E_NOT_FOUND;
        }

        auto delete_result =
            settings_.DeleteTaskRepositoryEntryJson(profile_id_, entry_id);
        if (delete_result == DAS_S_FALSE)
        {
            return DAS_E_NOT_FOUND;
        }
        return delete_result;
    }

    DasResult TaskRepositoryStore::RenameEntry(
        int64_t                                                 entry_id,
        const Repository::Dto::RenameRepositoryEntryRequestDto& request,
        Repository::Dto::RepositoryEntryDto&                    out_entry)
    {
        auto entry_json =
            settings_.GetTaskRepositoryEntryJson(profile_id_, entry_id);
        if (!entry_json.is_object())
        {
            return DAS_E_NOT_FOUND;
        }

        Repository::Dto::RepositoryEntryDto entry;
        try
        {
            entry =
                yyjson::cast<Repository::Dto::RepositoryEntryDto>(entry_json);
        }
        catch (const yyjson::bad_cast&)
        {
            return DAS_E_INVALID_JSON;
        }

        entry.display_name = request.display_name;
        auto persist_result = settings_.UpdateTaskRepositoryEntryJson(
            profile_id_,
            entry_id,
            CloneJsonValue(yyjson::object(entry)));
        if (DAS::IsFailed(persist_result))
        {
            return persist_result;
        }

        out_entry = std::move(entry);
        return DAS_S_OK;
    }

    DasResult TaskRepositoryStore::UpdateAuthoring(
        int64_t              entry_id,
        const yyjson::value& accepted_properties,
        const yyjson::value& authoring,
        yyjson::value&       out_entry_json)
    {
        auto entry_json =
            settings_.GetTaskRepositoryEntryJson(profile_id_, entry_id);
        if (!entry_json.is_object())
        {
            return DAS_E_NOT_FOUND;
        }

        auto entry_obj = entry_json.as_object();
        (*entry_obj)[std::string_view("acceptedProperties")] =
            CloneJsonValue(accepted_properties);
        (*entry_obj)[std::string_view("authoring")] = CloneJsonValue(authoring);

        auto persist_result = settings_.UpdateTaskRepositoryEntryJson(
            profile_id_,
            entry_id,
            entry_json);
        if (DAS::IsFailed(persist_result))
        {
            return persist_result;
        }

        out_entry_json = CloneJsonValue(entry_json);
        return DAS_S_OK;
    }
} // namespace Das::Core::TaskScheduler

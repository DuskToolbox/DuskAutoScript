#ifndef DAS_HTTP_DTO_PROFILE_HPP
#define DAS_HTTP_DTO_PROFILE_HPP

#include "Global.hpp"

/**
 *  定义配置文件相关数据类型
 *  Define profile related data types
 */

namespace Das::Http::Dto
{

    // 配置文件描述符
    // Profile descriptor
    struct ProfileDesc
    {
        std::string name;
        std::string profile_id;

        yyjson::writer::detail::value ToJson() const
        {
            yyjson::writer::detail::value j(yyjson::construct_object_type_t{});
            j["name"] = std::string{name};
            j["profileId"] = std::string{profile_id};
            return j;
        }

        static ProfileDesc FromJson(const yyjson::writer::detail::value& j)
        {
            ProfileDesc desc;
            auto        name_val = j["name"];
            auto        name_opt = name_val.as_string();
            desc.name = name_opt ? std::string(name_opt.value()) : "";
            auto pid_val = j["profileId"];
            auto pid_opt = pid_val.as_string();
            desc.profile_id = pid_opt ? std::string(pid_opt.value()) : "";
            return desc;
        }
    };

    struct ProfileDescList
    {
        std::vector<ProfileDesc> profile_list;

        yyjson::writer::detail::value ToJson() const
        {
            auto j = Das::Utils::MakeYyjsonObject();
            auto arr = Das::Utils::MakeYyjsonArray();
            auto arr_ref = arr.as_array();
            if (arr_ref)
            {
                for (const auto& desc : profile_list)
                {
                    arr_ref->emplace_back(desc.ToJson());
                }
            }
            j["profileList"] = std::move(arr);
            return j;
        }

        static ProfileDescList FromJson(const yyjson::writer::detail::value& j)
        {
            ProfileDescList list;
            auto            pl_val = j["profileList"];
            auto            pl_opt = pl_val.as_array();
            if (pl_opt)
            {
                for (const auto& item : pl_opt.value())
                {
                    list.profile_list.push_back(ProfileDesc::FromJson(item));
                }
            }
            return list;
        }
    };

    using ProfileDescListResponse = ApiResponse<ProfileDescList>;

    // 忽略的GUID列表，初始化插件管理器时使用
    struct ProfileInitializeParms
    {
        std::vector<std::string> ignored_guid_list;
        std::string              profile_id;

        yyjson::writer::detail::value ToJson() const
        {
            auto j = Das::Utils::MakeYyjsonObject();
            auto arr = Das::Utils::MakeYyjsonArray();
            auto arr_ref = arr.as_array();
            if (arr_ref)
            {
                for (const auto& guid : ignored_guid_list)
                {
                    arr_ref->emplace_back(std::string{guid});
                }
            }
            j["ignoredGuidList"] = std::move(arr);
            j["profileId"] = std::string{profile_id};
            return j;
        }

        static ProfileInitializeParms FromJson(
            const yyjson::writer::detail::value& j)
        {
            ProfileInitializeParms parms;
            auto                   igl_val = j["ignoredGuidList"];
            auto                   igl_opt = igl_val.as_array();
            if (igl_opt)
            {
                for (const auto& item : igl_opt.value())
                {
                    auto str_opt = item.as_string();
                    if (str_opt)
                    {
                        parms.ignored_guid_list.push_back(
                            std::string(str_opt.value()));
                    }
                }
            }
            auto pid_val = j["profileId"];
            auto pid_opt = pid_val.as_string();
            parms.profile_id = pid_opt ? std::string(pid_opt.value()) : "";
            return parms;
        }
    };

    // 配置文件描状态
    // Profile status
    struct ProfileStatus
    {
        std::string profile_id;
        bool        run;
        bool        enable;

        yyjson::writer::detail::value ToJson() const
        {
            yyjson::writer::detail::value j(yyjson::construct_object_type_t{});
            j["profileId"] = std::string{profile_id};
            j["run"] = run;
            j["enable"] = enable;
            return j;
        }

        static ProfileStatus FromJson(const yyjson::writer::detail::value& j)
        {
            ProfileStatus status;
            auto          pid_val = j["profileId"];
            auto          pid_opt = pid_val.as_string();
            status.profile_id = pid_opt ? std::string(pid_opt.value()) : "";
            auto run_val = j["run"];
            auto run_opt = run_val.as_bool();
            status.run = run_opt ? run_opt.value() : false;
            auto enable_val = j["enable"];
            auto enable_opt = enable_val.as_bool();
            status.enable = enable_opt ? enable_opt.value() : false;
            return status;
        }
    };

    using ProfileStatusList = ApiResponse<std::vector<ProfileStatus>>;

    // 配置文件运行状态
    // Profile is runing
    struct ProfileRunning
    {
        std::string profile_id;
        bool        run;

        yyjson::writer::detail::value ToJson() const
        {
            yyjson::writer::detail::value j(yyjson::construct_object_type_t{});
            j["profileId"] = std::string{profile_id};
            j["run"] = run;
            return j;
        }

        static ProfileRunning FromJson(const yyjson::writer::detail::value& j)
        {
            ProfileRunning running;
            auto           pid_val = j["profileId"];
            auto           pid_opt = pid_val.as_string();
            running.profile_id = pid_opt ? std::string(pid_opt.value()) : "";
            auto run_val = j["run"];
            auto run_opt = run_val.as_bool();
            running.run = run_opt ? run_opt.value() : false;
            return running;
        }
    };

    struct ProfileId
    {
        std::string profile_id;

        yyjson::writer::detail::value ToJson() const
        {
            yyjson::writer::detail::value j(yyjson::construct_object_type_t{});
            j["profileId"] = std::string{profile_id};
            return j;
        }

        static ProfileId FromJson(const yyjson::writer::detail::value& j)
        {
            ProfileId id;
            auto      pid_val = j["profileId"];
            auto      pid_opt = pid_val.as_string();
            id.profile_id = pid_opt ? std::string(pid_opt.value()) : "";
            return id;
        }
    };

    struct ProfileEnabled
    {
        std::string profile_id;
        int32_t     enabled;

        yyjson::writer::detail::value ToJson() const
        {
            yyjson::writer::detail::value j(yyjson::construct_object_type_t{});
            j["profileId"] = std::string{profile_id};
            j["enabled"] = static_cast<int64_t>(enabled);
            return j;
        }

        static ProfileEnabled FromJson(const yyjson::writer::detail::value& j)
        {
            ProfileEnabled enabled;
            auto           pid_val = j["profileId"];
            auto           pid_opt = pid_val.as_string();
            enabled.profile_id = pid_opt ? std::string(pid_opt.value()) : "";
            auto en_val = j["enabled"];
            auto en_opt = en_val.as_sint();
            enabled.enabled = en_opt ? static_cast<int32_t>(en_opt.value()) : 0;
            return enabled;
        }
    };

    struct ProfileInfo
    {
        std::string profile_id;
        int32_t     enabled;

        yyjson::writer::detail::value ToJson() const
        {
            yyjson::writer::detail::value j(yyjson::construct_object_type_t{});
            j["profileId"] = std::string{profile_id};
            j["enabled"] = static_cast<int64_t>(enabled);
            return j;
        }

        static ProfileInfo FromJson(const yyjson::writer::detail::value& j)
        {
            ProfileInfo info;
            auto        pid_val = j["profileId"];
            auto        pid_opt = pid_val.as_string();
            info.profile_id = pid_opt ? std::string(pid_opt.value()) : "";
            auto en_val = j["enabled"];
            auto en_opt = en_val.as_sint();
            info.enabled = en_opt ? static_cast<int32_t>(en_opt.value()) : 0;
            return info;
        }
    };

} // namespace Das::Http::Dto

#endif // DAS_HTTP_DTO_PROFILE_HPP

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
    };

    struct ProfileDescList
    {
        std::vector<ProfileDesc> profile_list;

        yyjson::value ToJson() const
        {
            auto j = Das::Utils::MakeYyjsonObject();
            auto arr = Das::Utils::MakeYyjsonArray();
            auto arr_obj_opt = arr.as_array();
            if (arr_obj_opt)
            {
                auto& arr_obj = arr_obj_opt.value();
                for (const auto& desc : profile_list)
                {
                    arr_obj.emplace_back(desc);
                }
            }
            auto obj_opt = j.as_object();
            if (obj_opt)
            {
                obj_opt.value()["profileList"] = std::move(arr);
            }
            return j;
        }

        static ProfileDescList FromJson(const yyjson::value& j)
        {
            ProfileDescList list;
            auto            obj_opt = j.as_object();
            if (obj_opt)
            {
                const auto& obj = obj_opt.value();
                auto        pl_val = obj["profileList"];
                auto        pl_opt = pl_val.as_array();
                if (pl_opt)
                {
                    for (const auto& item : pl_opt.value())
                    {
                        list.profile_list.push_back(
                            yyjson::cast<ProfileDesc>(item));
                    }
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

        yyjson::value ToJson() const
        {
            auto j = Das::Utils::MakeYyjsonObject();
            auto arr = Das::Utils::MakeYyjsonArray();
            auto arr_obj_opt = arr.as_array();
            if (arr_obj_opt)
            {
                auto& arr_obj = arr_obj_opt.value();
                for (const auto& guid : ignored_guid_list)
                {
                    arr_obj.emplace_back(std::string{guid});
                }
            }
            auto obj_opt = j.as_object();
            if (obj_opt)
            {
                auto& obj = obj_opt.value();
                obj["ignoredGuidList"] = std::move(arr);
                obj["profileId"] = std::string{profile_id};
            }
            return j;
        }

        static ProfileInitializeParms FromJson(const yyjson::value& j)
        {
            ProfileInitializeParms parms;
            auto                   obj_opt = j.as_object();
            if (obj_opt)
            {
                const auto& obj = obj_opt.value();
                auto        igl_val = obj["ignoredGuidList"];
                auto        igl_opt = igl_val.as_array();
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
                auto pid_val = obj["profileId"];
                auto pid_opt = pid_val.as_string();
                parms.profile_id = pid_opt ? std::string(pid_opt.value()) : "";
            }
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
    };

    using ProfileStatusList = ApiResponse<std::vector<ProfileStatus>>;

    // 配置文件运行状态
    // Profile is runing
    struct ProfileRunning
    {
        std::string profile_id;
        bool        run;
    };

    struct ProfileId
    {
        std::string profile_id;
    };

    struct ProfileEnabled
    {
        std::string profile_id;
        int32_t     enabled;
    };

    struct ProfileInfo
    {
        std::string profile_id;
        int32_t     enabled;
    };

} // namespace Das::Http::Dto

#endif // DAS_HTTP_DTO_PROFILE_HPP

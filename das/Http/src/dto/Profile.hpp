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
    
    nlohmann::json ToJson() const
    {
        nlohmann::json j;
        j["name"] = name;
        j["profileId"] = profile_id;
        return j;
    }
    
    static ProfileDesc FromJson(const nlohmann::json& j)
    {
        ProfileDesc desc;
        desc.name = j.value("name", "");
        desc.profile_id = j.value("profileId", "");
        return desc;
    }
};

struct ProfileDescList
{
    std::vector<ProfileDesc> profile_list;
    
    nlohmann::json ToJson() const
    {
        nlohmann::json j;
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& desc : profile_list)
        {
            arr.push_back(desc.ToJson());
        }
        j["profileList"] = arr;
        return j;
    }
    
    static ProfileDescList FromJson(const nlohmann::json& j)
    {
        ProfileDescList list;
        if (j.contains("profileList") && j["profileList"].is_array())
        {
            for (const auto& item : j["profileList"])
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
    std::string profile_id;
    
    nlohmann::json ToJson() const
    {
        nlohmann::json j;
        j["ignoredGuidList"] = ignored_guid_list;
        j["profileId"] = profile_id;
        return j;
    }
    
    static ProfileInitializeParms FromJson(const nlohmann::json& j)
    {
        ProfileInitializeParms parms;
        if (j.contains("ignoredGuidList") && j["ignoredGuidList"].is_array())
        {
            parms.ignored_guid_list = j["ignoredGuidList"].get<std::vector<std::string>>();
        }
        parms.profile_id = j.value("profileId", "");
        return parms;
    }
};

// 配置文件描状态
// Profile status
struct ProfileStatus
{
    std::string profile_id;
    bool run;
    bool enable;
    
    nlohmann::json ToJson() const
    {
        nlohmann::json j;
        j["profileId"] = profile_id;
        j["run"] = run;
        j["enable"] = enable;
        return j;
    }
    
    static ProfileStatus FromJson(const nlohmann::json& j)
    {
        ProfileStatus status;
        status.profile_id = j.value("profileId", "");
        status.run = j.value("run", false);
        status.enable = j.value("enable", false);
        return status;
    }
};

using ProfileStatusList = ApiResponse<std::vector<ProfileStatus>>;

// 配置文件运行状态
// Profile is runing
struct ProfileRunning
{
    std::string profile_id;
    bool run;
    
    nlohmann::json ToJson() const
    {
        nlohmann::json j;
        j["profileId"] = profile_id;
        j["run"] = run;
        return j;
    }
    
    static ProfileRunning FromJson(const nlohmann::json& j)
    {
        ProfileRunning running;
        running.profile_id = j.value("profileId", "");
        running.run = j.value("run", false);
        return running;
    }
};

struct ProfileId
{
    std::string profile_id;
    
    nlohmann::json ToJson() const
    {
        nlohmann::json j;
        j["profileId"] = profile_id;
        return j;
    }
    
    static ProfileId FromJson(const nlohmann::json& j)
    {
        ProfileId id;
        id.profile_id = j.value("profileId", "");
        return id;
    }
};

struct ProfileEnabled
{
    std::string profile_id;
    int32_t enabled;
    
    nlohmann::json ToJson() const
    {
        nlohmann::json j;
        j["profileId"] = profile_id;
        j["enabled"] = enabled;
        return j;
    }
    
    static ProfileEnabled FromJson(const nlohmann::json& j)
    {
        ProfileEnabled enabled;
        enabled.profile_id = j.value("profileId", "");
        enabled.enabled = j.value("enabled", 0);
        return enabled;
    }
};

struct ProfileInfo
{
    std::string profile_id;
    int32_t enabled;
    
    nlohmann::json ToJson() const
    {
        nlohmann::json j;
        j["profileId"] = profile_id;
        j["enabled"] = enabled;
        return j;
    }
    
    static ProfileInfo FromJson(const nlohmann::json& j)
    {
        ProfileInfo info;
        info.profile_id = j.value("profileId", "");
        info.enabled = j.value("enabled", 0);
        return info;
    }
};

} // namespace Das::Http::Dto

#endif // DAS_HTTP_DTO_PROFILE_HPP

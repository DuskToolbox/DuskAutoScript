#ifndef DAS_HTTP_DTO_SETTINGS_HPP
#define DAS_HTTP_DTO_SETTINGS_HPP

#include "Global.hpp"

/**
 *  定义设置相关数据类型
 *  Define settings related data type
 */

namespace Das::Http::Dto
{

// 应用描述符
// App descriptor
struct AppDesc
{
    std::string name;
    std::string package_name;
    
    nlohmann::json ToJson() const
    {
        nlohmann::json j;
        j["name"] = name;
        j["packageName"] = package_name;
        return j;
    }
    
    static AppDesc FromJson(const nlohmann::json& j)
    {
        AppDesc desc;
        desc.name = j.value("name", "");
        desc.package_name = j.value("packageName", "");
        return desc;
    }
};

using AppDescList = ApiResponse<std::vector<AppDesc>>;

// 插件描述符
// Plugin descriptor
struct PluginPackageDesc
{
    std::string name;
    std::string plugin_id;
    
    nlohmann::json ToJson() const
    {
        nlohmann::json j;
        j["name"] = name;
        j["pluginId"] = plugin_id;
        return j;
    }
    
    static PluginPackageDesc FromJson(const nlohmann::json& j)
    {
        PluginPackageDesc desc;
        desc.name = j.value("name", "");
        desc.plugin_id = j.value("pluginId", "");
        return desc;
    }
};

using PluginPackageDescList = ApiResponse<std::vector<PluginPackageDesc>>;

// 任务描述符
// Task descriptor
struct TaskDesc
{
    std::string name;
    std::string plugin_id;
    std::string game_name;
    // 此字段由前端内部管理
    // std::string sub_group;
    
    nlohmann::json ToJson() const
    {
        nlohmann::json j;
        j["name"] = name;
        j["pluginId"] = plugin_id;
        j["gameName"] = game_name;
        return j;
    }
    
    static TaskDesc FromJson(const nlohmann::json& j)
    {
        TaskDesc desc;
        desc.name = j.value("name", "");
        desc.plugin_id = j.value("pluginId", "");
        desc.game_name = j.value("gameName", "");
        return desc;
    }
};

using TaskDescList = ApiResponse<std::vector<TaskDesc>>;

} // namespace Das::Http::Dto

#endif // DAS_HTTP_DTO_SETTINGS_HPP

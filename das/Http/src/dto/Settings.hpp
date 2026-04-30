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
    };

    using AppDescList = ApiResponse<std::vector<AppDesc>>;

    // 插件描述符
    // Plugin descriptor
    struct PluginPackageDesc
    {
        std::string name;
        std::string plugin_id;
    };

    using PluginPackageDescList = ApiResponse<std::vector<PluginPackageDesc>>;

    // 任务描述符
    // Task descriptor
    struct TaskDesc
    {
        std::string name;
        std::string plugin_id;
        std::string game_name;
    };

    using TaskDescList = ApiResponse<std::vector<TaskDesc>>;

} // namespace Das::Http::Dto

#endif // DAS_HTTP_DTO_SETTINGS_HPP

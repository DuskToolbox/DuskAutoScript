#ifndef DAS_HTTP_DTO_SETTINGS_HPP
#define DAS_HTTP_DTO_SETTINGS_HPP

#include "Global.hpp"

#include OATPP_CODEGEN_BEGIN(DTO)

/**
 *  定义设置相关数据类型
 *  Define settings related data type
 */

// 应用描述符-
// App descriptor
class AppDesc : public oatpp::DTO
{

    DTO_INIT(AppDesc, DTO)

    DTO_FIELD(String, name, "name");
    DTO_FIELD(String, package_name, "packageName");
};

using AppDescList = ApiResponse<oatpp::List<oatpp::Object<AppDesc>>>;

// 插件描述符
// Plugin descriptor
class PluginPackageDesc : public oatpp::DTO
{

    DTO_INIT(PluginPackageDesc, DTO)

    DTO_FIELD(String, name, "name");
    DTO_FIELD(String, plugin_id, "pluginId");
};

using PluginPackageDescList =
    ApiResponse<oatpp::List<oatpp::Object<PluginPackageDesc>>>;

// 任务描述符
// Task descriptor
class TaskDesc : public oatpp::DTO
{

    DTO_INIT(TaskDesc, DTO)

    DTO_FIELD(String, name, "name");
    DTO_FIELD(String, plugin_id, "pluginId");
    DTO_FIELD(String, game_name, "gameName");
    // 此字段由前端内部管理
    // DTO_FIELD(String, sub_group, "sub_group");
};

using TaskDescList = ApiResponse<oatpp::List<oatpp::Object<TaskDesc>>>;

#include OATPP_CODEGEN_END(DTO)

#endif // DAS_HTTP_DTO_SETTINGS_HPP

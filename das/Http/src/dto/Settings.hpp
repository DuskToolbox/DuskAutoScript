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

        yyjson::writer::detail::value ToJson() const
        {
            auto j = Das::Utils::MakeYyjsonObject();
            auto obj_opt = j.as_object();
            if (obj_opt)
            {
                auto& obj = obj_opt.value();
                obj["name"] = std::string{name};
                obj["packageName"] = std::string{package_name};
            }
            return j;
        }

        static AppDesc FromJson(const yyjson::writer::detail::value& j)
        {
            AppDesc desc;
            auto    obj_opt = j.as_object();
            if (obj_opt)
            {
                const auto& obj = obj_opt.value();
                auto        name_val = obj["name"];
                auto        name_opt = name_val.as_string();
                desc.name = name_opt ? std::string(name_opt.value()) : "";
                auto pn_val = obj["packageName"];
                auto pn_opt = pn_val.as_string();
                desc.package_name = pn_opt ? std::string(pn_opt.value()) : "";
            }
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

        yyjson::writer::detail::value ToJson() const
        {
            auto j = Das::Utils::MakeYyjsonObject();
            auto obj_opt = j.as_object();
            if (obj_opt)
            {
                auto& obj = obj_opt.value();
                obj["name"] = std::string{name};
                obj["pluginId"] = std::string{plugin_id};
            }
            return j;
        }

        static PluginPackageDesc FromJson(
            const yyjson::writer::detail::value& j)
        {
            PluginPackageDesc desc;
            auto              obj_opt = j.as_object();
            if (obj_opt)
            {
                const auto& obj = obj_opt.value();
                auto        name_val = obj["name"];
                auto        name_opt = name_val.as_string();
                desc.name = name_opt ? std::string(name_opt.value()) : "";
                auto pid_val = obj["pluginId"];
                auto pid_opt = pid_val.as_string();
                desc.plugin_id = pid_opt ? std::string(pid_opt.value()) : "";
            }
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

        yyjson::writer::detail::value ToJson() const
        {
            auto j = Das::Utils::MakeYyjsonObject();
            auto obj_opt = j.as_object();
            if (obj_opt)
            {
                auto& obj = obj_opt.value();
                obj["name"] = std::string{name};
                obj["pluginId"] = std::string{plugin_id};
                obj["gameName"] = std::string{game_name};
            }
            return j;
        }

        static TaskDesc FromJson(const yyjson::writer::detail::value& j)
        {
            TaskDesc desc;
            auto     obj_opt = j.as_object();
            if (obj_opt)
            {
                const auto& obj = obj_opt.value();
                auto        name_val = obj["name"];
                auto        name_opt = name_val.as_string();
                desc.name = name_opt ? std::string(name_opt.value()) : "";
                auto pid_val = obj["pluginId"];
                auto pid_opt = pid_val.as_string();
                desc.plugin_id = pid_opt ? std::string(pid_opt.value()) : "";
                auto gn_val = obj["gameName"];
                auto gn_opt = gn_val.as_string();
                desc.game_name = gn_opt ? std::string(gn_opt.value()) : "";
            }
            return desc;
        }
    };

    using TaskDescList = ApiResponse<std::vector<TaskDesc>>;

} // namespace Das::Http::Dto

#endif // DAS_HTTP_DTO_SETTINGS_HPP

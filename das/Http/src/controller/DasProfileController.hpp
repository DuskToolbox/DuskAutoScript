#ifndef DAS_HTTP_CONTROLLER_DASPROFILECONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_DASPROFILECONTROLLER_HPP

#include "Config.h"
#include "beast/JsonUtils.hpp"
#include "beast/Request.hpp"
#include "component/Helper.hpp"
#include "das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h"
#include "das/_autogen/idl/abi/DasLogger.h"
#include "das/_autogen/idl/abi/DasSettings.h"
#include "das/_autogen/idl/abi/IDasTaskScheduler.h"
#include "dto/Profile.hpp"
#include "dto/Settings.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Das::Http
{

    /**
     *  @brief 定义配置文件管理相关API
     *  Define profile related APIs
     */
    class DasProfileManagerController
    {
        DAS::DasPtr<ExportInterface::IDasTaskScheduler> p_task_scheduler_{};
        DAS::DasPtr<ExportInterface::IDasJsonSetting>   p_settings_for_ui_{};

    public:
        DasProfileManagerController()
        {
            // TODO: GetIDasSettingsForUi is not available, will be rewritten
            // GetIDasSettingsForUi(p_settings_for_ui_.Put());
            // GetIDasTaskScheduler(p_task_scheduler_.Put());
        }

        // 获取配置文件列表
        // Get profile list
        Beast::HttpResponse GetProfileList(const Beast::HttpRequest& request)
        {
            return Beast::HttpResponse::CreateErrorResponse(
                DAS_E_NO_IMPLEMENTATION,
                "Profile list API is not implemented");
        }

        Beast::HttpResponse GetProfile(const Beast::HttpRequest& request)
        {
            return Beast::HttpResponse::CreateErrorResponse(
                DAS_E_NO_IMPLEMENTATION,
                "Profile get API is not implemented");
        }

        // 获取配置文件状态
        // Get profile status
        Beast::HttpResponse GetProfileStatus(const Beast::HttpRequest& request)
        {
            return Beast::HttpResponse::CreateErrorResponse(
                DAS_E_NO_IMPLEMENTATION,
                "Profile status API is not implemented");
        }

        Beast::HttpResponse CreateProfile(const Beast::HttpRequest& request)
        {
            return Beast::HttpResponse::CreateErrorResponse(
                DAS_E_NO_IMPLEMENTATION,
                "Profile create API is not implemented");
        }

        Beast::HttpResponse DeleteProfile(const Beast::HttpRequest& request)
        {
            return Beast::HttpResponse::CreateErrorResponse(
                DAS_E_NO_IMPLEMENTATION,
                "Profile delete API is not implemented");
        }

        // 启用/禁用配置文件
        // Enable/disable profile
        Beast::HttpResponse SetEnable(const Beast::HttpRequest& request)
        {
            return Beast::HttpResponse::CreateErrorResponse(
                DAS_E_NO_IMPLEMENTATION,
                "Profile set enable API is not implemented");
        }

        Beast::HttpResponse StartProfile(const Beast::HttpRequest& request)
        {
            return Beast::HttpResponse::CreateErrorResponse(
                DAS_E_NO_IMPLEMENTATION,
                "Profile start API is not implemented");
        }

        Beast::HttpResponse StopProfile(const Beast::HttpRequest& request)
        {
            return Beast::HttpResponse::CreateErrorResponse(
                DAS_E_NO_IMPLEMENTATION,
                "Profile stop API is not implemented");
        }

    private:
        // 获取任务列表
        // Get task list
        Beast::HttpResponse GetTaskList(const Beast::HttpRequest& request)
        {
            return Beast::HttpResponse::CreateErrorResponse(
                DAS_E_NO_IMPLEMENTATION,
                "Task list API is not implemented");
        }
    };

} // namespace Das::Http

#endif // DAS_HTTP_CONTROLLER_DASPROFILECONTROLLER_HPP

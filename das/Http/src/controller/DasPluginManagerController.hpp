#ifndef DAS_HTTP_CONTROLLER_DASPLUGINMANAGERCONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_DASPLUGINMANAGERCONTROLLER_HPP

#include "../component/DasInitializePluginManagerCallback.h"
#include "Config.h"
#include "beast/JsonUtils.hpp"
#include "beast/Request.hpp"
#include "component/Helper.hpp"
#include "das/IDasBase.h"
#include "das/Utils/fmt.h"
#include "das/_autogen/idl/abi/DasLogger.h"
#include "das/_autogen/idl/abi/IDasPluginManager.h"
#include "das/_autogen/idl/abi/IDasTaskScheduler.h"
#include "dto/Profile.hpp"
#include "dto/Settings.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Das::Http
{

    class DasPluginManagerController
    {
    };

} // namespace Das::Http

#endif // DAS_HTTP_CONTROLLER_DASPLUGINMANAGERCONTROLLER_HPP

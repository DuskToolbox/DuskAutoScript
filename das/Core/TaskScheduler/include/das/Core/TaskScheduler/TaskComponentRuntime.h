#pragma once

#include <cassert>
#include <cpp_yyjson.hpp>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>

namespace Das::Core::TaskScheduler
{

    yyjson::value MakeTaskComponentResult(
        std::string_view status,
        yyjson::value    outputs,
        yyjson::value    signals);

    DasPtr<Das::PluginInterface::IDasTaskComponentFactory>
    CreateOfficialFlowControlTaskComponentFactory();

} // namespace Das::Core::TaskScheduler

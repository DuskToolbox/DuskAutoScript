#pragma once

#include <cassert>

#include <cpp_yyjson.hpp>

#include <string_view>

namespace Das::Core::TaskScheduler
{

    yyjson::value MakeTaskComponentResult(
        std::string_view status,
        yyjson::value    outputs,
        yyjson::value    signals);

} // namespace Das::Core::TaskScheduler

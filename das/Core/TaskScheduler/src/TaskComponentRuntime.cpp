#include <das/Core/TaskScheduler/TaskComponentRuntime.h>

#include <das/Utils/DasJsonCore.h>

namespace Das::Core::TaskScheduler
{

    yyjson::value MakeTaskComponentResult(
        std::string_view status,
        yyjson::value    outputs,
        yyjson::value    signals)
    {
        auto result = Das::Utils::MakeYyjsonObject();
        auto obj = *result.as_object();
        obj[std::string_view("status")] = status;
        obj[std::string_view("outputs")] = std::move(outputs);
        obj[std::string_view("diagnostics")] = Das::Utils::MakeYyjsonArray();
        obj[std::string_view("signals")] = std::move(signals);
        return result;
    }

} // namespace Das::Core::TaskScheduler

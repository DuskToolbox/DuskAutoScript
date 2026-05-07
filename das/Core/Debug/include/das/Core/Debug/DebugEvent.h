#ifndef DAS_CORE_DEBUG_DEBUGEVENT_H
#define DAS_CORE_DEBUG_DEBUGEVENT_H

#include <das/Core/Debug/Config.h>

#include <string>

DAS_CORE_DEBUG_NS_BEGIN

struct DebugEvent
{
    std::string type;
    std::string timestamp;
    std::string params_json{"{}"};
    std::string result_json{"{}"};
    std::string image_filename;
    double      elapsed_ms{0.0};
};

DebugEvent MakeDebugEvent(
    std::string type,
    std::string params_json = "{}",
    std::string result_json = "{}");

DAS_CORE_DEBUG_NS_END

#endif // DAS_CORE_DEBUG_DEBUGEVENT_H

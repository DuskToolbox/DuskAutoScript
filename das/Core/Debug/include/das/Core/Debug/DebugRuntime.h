#ifndef DAS_CORE_DEBUG_DEBUGRUNTIME_H
#define DAS_CORE_DEBUG_DEBUGRUNTIME_H

#include <das/Core/Debug/Config.h>
#include <das/DasTypes.hpp>

#include <filesystem>

DAS_CORE_DEBUG_NS_BEGIN

struct DebugRuntimeOptions
{
    std::filesystem::path debug_dir;
};

struct DebugEvent;
class IDebugSink;
class IDebugDrain;

class DebugRuntime
{
public:
    static DasResult Initialize(const DebugRuntimeOptions& options);
    static bool IsEnabled();
    static const std::filesystem::path& DebugDir();
    static DasResult SubmitEvent(const DebugEvent& event);
    static DasResult Flush();
    static void Shutdown();
    static void ResetForTest();
};

DAS_CORE_DEBUG_NS_END

#endif // DAS_CORE_DEBUG_DEBUGRUNTIME_H

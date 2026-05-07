#ifndef DAS_CORE_DEBUG_DEBUGRUNTIME_H
#define DAS_CORE_DEBUG_DEBUGRUNTIME_H

#include <das/Core/Debug/Config.h>
#include <das/DasTypes.hpp>

#include <filesystem>
#include <memory>

DAS_CORE_DEBUG_NS_BEGIN

struct DebugRuntimeOptions
{
    std::filesystem::path debug_dir;
};

struct DebugEvent;
struct DebugImageSnapshot;
class IDebugSink;
class IDebugDrain;

class DebugRuntime
{
public:
    static DasResult Initialize(const DebugRuntimeOptions& options);
    static bool IsEnabled();
    static const std::filesystem::path& DebugDir();
    static DasResult SubmitEvent(const DebugEvent& event);
    static void RegisterSink(std::shared_ptr<IDebugSink> sink);
    static void RegisterDrain(std::shared_ptr<IDebugDrain> drain);
    static void SetLatestImage(std::shared_ptr<DebugImageSnapshot> image);
    static std::shared_ptr<DebugImageSnapshot> GetLatestImage();
    static void ClearLatestImage();
    static DasResult Flush();
    static void Shutdown();
    static void ResetForTest();
};

DAS_CORE_DEBUG_NS_END

#endif // DAS_CORE_DEBUG_DEBUGRUNTIME_H

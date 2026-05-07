#ifndef DAS_CORE_DEBUG_DEBUGSINK_H
#define DAS_CORE_DEBUG_DEBUGSINK_H

#include <das/Core/Debug/Config.h>
#include <das/DasTypes.hpp>

DAS_CORE_DEBUG_NS_BEGIN

struct DebugEvent;

class IDebugSink
{
public:
    virtual ~IDebugSink() = default;
    virtual DasResult Submit(const DebugEvent& event) = 0;
    virtual DasResult Flush() = 0;
    virtual void Shutdown() {}
};

class IDebugDrain
{
public:
    virtual ~IDebugDrain() = default;
    virtual DasResult Flush() = 0;
    virtual void Shutdown() {}
};

DAS_CORE_DEBUG_NS_END

#endif // DAS_CORE_DEBUG_DEBUGSINK_H

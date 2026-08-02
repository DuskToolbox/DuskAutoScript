#pragma once

#include <das/_autogen/idl/abi/DasResult.h>

namespace Das::Plugins::DasMaaPi
{
    // D-09: MaaPi execution engine error codes (-10000 series).
    // These are defined as constexpr DasResult values outside the
    // auto-generated enum range [-1073750000, -1073800004].

    /// PI file missing at the given path.
    inline constexpr DasResult DAS_E_MAAPI_PI_MISSING =
        static_cast<DasResult>(-10001);

    /// PI parse or catalog load failed (malformed JSON, missing imports, etc.).
    inline constexpr DasResult DAS_E_MAAPI_PI_PARSE_FAILED =
        static_cast<DasResult>(-10002);

    /// Specified task does not exist in the PI catalog.
    inline constexpr DasResult DAS_E_MAAPI_TASK_MISSING =
        static_cast<DasResult>(-10003);

    /// Option parsing failed during compile.
    inline constexpr DasResult DAS_E_MAAPI_OPTION_PARSE_FAILED =
        static_cast<DasResult>(-10004);

    /// MaaFramework execution failed (task timeout, recognition failure, etc.).
    inline constexpr DasResult DAS_E_MAAPI_EXECUTION_FAILED =
        static_cast<DasResult>(-10005);

    /// PI v2.7.0 pretask failed to launch or exited with a nonzero status.
    /// Runtime emits `pretask-failed` (spawn failure) or `pretask-nonzero-exit`
    /// diagnostics and aborts before CreateController (D-86-02).
    inline constexpr DasResult DAS_E_MAAPI_PRETASK_FAILED =
        static_cast<DasResult>(-10006);

    /// Update version check failed (interface.json unreadable / missing
    /// version / all check sources failed or returned no usable download info).
    inline constexpr DasResult DAS_E_MAAPI_UPDATE_CHECK_FAILED =
        static_cast<DasResult>(-10007);

    /// Update apply failed (download / extract / replace error). The old
    /// project directory has been rolled back before this is returned.
    inline constexpr DasResult DAS_E_MAAPI_UPDATE_APPLY_FAILED =
        static_cast<DasResult>(-10008);

    /// Proxy scheme unsupported (socks5://). PiHttpClient rejects the URL up
    /// front; this code converges the semantics for consumers that need a
    /// distinct "proxy unsupported" result (D-89-08).
    inline constexpr DasResult DAS_E_MAAPI_PROXY_UNSUPPORTED =
        static_cast<DasResult>(-10009);

} // namespace Das::Plugins::DasMaaPi

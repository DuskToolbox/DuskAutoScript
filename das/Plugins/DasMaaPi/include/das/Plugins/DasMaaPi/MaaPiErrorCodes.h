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

} // namespace Das::Plugins::DasMaaPi

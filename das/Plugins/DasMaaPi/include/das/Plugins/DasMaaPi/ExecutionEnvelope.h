#pragma once

#include <cassert>

#include <cpp_yyjson.hpp>
#include <das/Plugins/DasMaaPi/AgentRuntimeDto.h>
#include <das/Plugins/DasMaaPi/MaaApiBoundary.h>
#include <das/Plugins/DasMaaPi/MaapiDto.h>
#include <das/Plugins/DasMaaPi/PiDto.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Das::Plugins::DasMaaPi
{
    struct PiEnvSnapshotDto
    {
        std::string interface_version = "2";
        std::string client_name = "DAS";
        std::string client_version;
        std::string client_language = "cpp";
        std::string client_maafw_version;
        std::string project_version;
        std::string controller_json;
        std::string resource_json;
    };

    struct MaaTaskExecutionDto
    {
        std::string   task_name;
        std::string   entry;
        yyjson::value pipeline_override;
    };

    // PI v2.7.0 pretask (D-86-02). External processes executed in order BEFORE
    // CreateController, with CWD = interface_directory. PiCompiler pre-filters
    // by v2.8.1 controller/resource and serializes any selected option values
    // into a trailing single-line compact JSON appended to `args`. MaaRuntime
    // spawns each entry via IPretaskRunner and aborts on launch failure or
    // nonzero exit (DAS_E_MAAPI_PRETASK_FAILED).
    struct MaaPretaskDto
    {
        std::string              name; // optional, for diagnostics
        std::string              exec;
        std::vector<std::string> args; // includes trailing option JSON if any
    };

    struct MaaExecutionPlanDto
    {
        std::string    interface_directory;
        std::string    controller_name;
        ControllerSpec controller;
        // Optional typed init params parallel to `controller`. CompileMaapi
        // fills both: `controller` stays wire-shaped (yyjson round-trip),
        // `controller_init_params` carries win32/gamepad typed bits + display
        // options consumed by CreateControllerFromParams. Adb/dbg controllers
        // leave this nullopt and MaaRuntime::Run uses the legacy
        // single-arg CreateController path.
        std::optional<ControllerInitParams>     controller_init_params;
        std::string                             resource_name;
        std::vector<std::string>                resource_paths;
        std::optional<std::string>              resource_hash;
        bool                                    fail_fast = true;
        bool                                    requires_agent_runtime = false;
        std::vector<AgentRuntime::AgentSpecDto> agent;
        // PI v2.7.0 pretasks. Empty when the project declares no pretask or
        // the v2.8.1 controller/resource filter excluded all entries.
        std::vector<MaaPretaskDto>       pretasks;
        PiEnvSnapshotDto                 pi_env;
        std::vector<MaaTaskExecutionDto> tasks;
    };

    struct ExecutionEnvelopeDto
    {
        int32_t             version = 1;
        std::string         plugin_guid = std::string(kPluginGuidText);
        std::string         task_type_guid = std::string(kTaskGuidText);
        MaaExecutionPlanDto maapi;
    };

    struct CompileSummaryDto
    {
        bool                       can_execute = false;
        std::optional<std::string> controller_name;
        std::optional<std::string> resource_name;
        std::vector<std::string>   task_names;
        bool                       requires_agent_runtime = false;
    };

    struct CompileResultDto
    {
        bool                                ok = true;
        bool                                can_execute = false;
        CompileSummaryDto                   summary;
        std::vector<PiDiagnosticDto>        diagnostics;
        std::optional<ExecutionEnvelopeDto> execution_input;
    };
} // namespace Das::Plugins::DasMaaPi

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::ControllerSpec>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::Win32InitParams>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::GamepadInitParams>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::AdbInitParams>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::DbgInitParams>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::ControllerInitParams>
{
    using type = yyjson::snake_to_camel_transform;
};

// cpp_yyjson's default_caster dispatch checks `std::constructible_from<T,
// const char*>` BEFORE the `is_optional_v<T>` branch. Under C++20 aggregate
// parenthesized initialization, `ControllerInitParams` (an aggregate with a
// leading `std::string` field) IS constructible_from<const char*>, so
// `std::optional<ControllerInitParams>` is also constructible_from<const
// char*> via the optional converting constructor. That makes the dispatch
// pick the `const char*` branch for non-null JSON objects and throw
// "optional<ControllerInitParams> is not constructible from JSON string"
// even when the JSON value is a valid object.
//
// The same trap applies to Win32InitParams / GamepadInitParams (leading
// std::string field aggregates wrapped in optional inside
// ControllerInitParams). These explicit caster specializations short-circuit
// the from_json dispatch and route directly through aggregate reflection
// (default_caster<T>::from_json), so the JSON round-trip works for both null
// (→nullopt) and object (→populated) payloads. The to_json overloads mirror
// the default caster<optional<T>> signature so aggregate serialization via
// PFR field iteration still works (the field is written through a
// writer::value_ref, which dispatches back to the aggregate to_json path).
// This is the path that production IPC takes when the real plugin emits a
// typed controller envelope and the consumer calls ParseExecutionEnvelope.
//
// Specialization order matters: Win32/Gamepad optional casters must be
// declared BEFORE the ControllerInitParams optional caster, because the
// latter references ControllerInitParams (whose aggregate reflection touches
// its optional<Win32InitParams> / optional<GamepadInitParams> fields) and
// would otherwise implicitly instantiate the default optional caster before
// the explicit specialization is visible ("explicit specialization after
// instantiation" error).
//
// optional<bool> also needs an explicit specialization: C++ permits a
// deprecated pointer→bool conversion, so `constructible_from<bool, const
// char*>` is true and the same trap fires for optional<bool> when the JSON
// value is not a string (e.g. JSON false for display_raw).
template <>
struct yyjson::caster<std::optional<bool>>
{
    using value_type = bool;

    template <typename Json>
    static std::optional<value_type> from_json(const Json& json)
    {
        if (json.is_null())
        {
            return std::nullopt;
        }
        if (const auto v = json.as_bool())
        {
            return *v;
        }
        throw yyjson::bad_cast(
            "std::optional<bool> is not constructible from JSON bool");
    }

    template <typename... Ts>
    static auto to_json(
        yyjson::writer::value_ref&       v,
        const std::optional<value_type>& t,
        Ts...)
    {
        if (t.has_value())
        {
            v = *t;
        }
    }

    template <typename... Ts>
    static auto
    to_json(yyjson::writer::value_ref& v, std::optional<value_type>&& t, Ts...)
    {
        if (t.has_value())
        {
            v = std::move(*t);
        }
    }
};

template <>
struct yyjson::caster<std::optional<Das::Plugins::DasMaaPi::Win32InitParams>>
{
    using value_type = Das::Plugins::DasMaaPi::Win32InitParams;

    template <typename Json>
    static std::optional<value_type> from_json(const Json& json)
    {
        if (json.is_null())
        {
            return std::nullopt;
        }
        return std::optional<value_type>(yyjson::cast<value_type>(json));
    }

    template <typename... Ts>
        requires requires(yyjson::writer::value_ref& v, value_type t) {
            v = t;
            v = std::pair(t, yyjson::copy_string);
        }
    static auto to_json(
        yyjson::writer::value_ref&       v,
        const std::optional<value_type>& t,
        Ts...)
    {
        if (t.has_value())
        {
            v = *t;
        }
    }

    template <typename... Ts>
        requires requires(yyjson::writer::value_ref& v, value_type t) {
            v = t;
            v = std::pair(t, yyjson::copy_string);
        }
    static auto
    to_json(yyjson::writer::value_ref& v, std::optional<value_type>&& t, Ts...)
    {
        if (t.has_value())
        {
            v = std::move(*t);
        }
    }
};

template <>
struct yyjson::caster<std::optional<Das::Plugins::DasMaaPi::GamepadInitParams>>
{
    using value_type = Das::Plugins::DasMaaPi::GamepadInitParams;

    template <typename Json>
    static std::optional<value_type> from_json(const Json& json)
    {
        if (json.is_null())
        {
            return std::nullopt;
        }
        return std::optional<value_type>(yyjson::cast<value_type>(json));
    }

    template <typename... Ts>
        requires requires(yyjson::writer::value_ref& v, value_type t) {
            v = t;
            v = std::pair(t, yyjson::copy_string);
        }
    static auto to_json(
        yyjson::writer::value_ref&       v,
        const std::optional<value_type>& t,
        Ts...)
    {
        if (t.has_value())
        {
            v = *t;
        }
    }

    template <typename... Ts>
        requires requires(yyjson::writer::value_ref& v, value_type t) {
            v = t;
            v = std::pair(t, yyjson::copy_string);
        }
    static auto
    to_json(yyjson::writer::value_ref& v, std::optional<value_type>&& t, Ts...)
    {
        if (t.has_value())
        {
            v = std::move(*t);
        }
    }
};

template <>
struct yyjson::caster<std::optional<Das::Plugins::DasMaaPi::AdbInitParams>>
{
    using value_type = Das::Plugins::DasMaaPi::AdbInitParams;

    template <typename Json>
    static std::optional<value_type> from_json(const Json& json)
    {
        if (json.is_null())
        {
            return std::nullopt;
        }
        return std::optional<value_type>(yyjson::cast<value_type>(json));
    }

    template <typename... Ts>
        requires requires(yyjson::writer::value_ref& v, value_type t) {
            v = t;
            v = std::pair(t, yyjson::copy_string);
        }
    static auto to_json(
        yyjson::writer::value_ref&       v,
        const std::optional<value_type>& t,
        Ts...)
    {
        if (t.has_value())
        {
            v = *t;
        }
    }

    template <typename... Ts>
        requires requires(yyjson::writer::value_ref& v, value_type t) {
            v = t;
            v = std::pair(t, yyjson::copy_string);
        }
    static auto
    to_json(yyjson::writer::value_ref& v, std::optional<value_type>&& t, Ts...)
    {
        if (t.has_value())
        {
            v = std::move(*t);
        }
    }
};

template <>
struct yyjson::caster<std::optional<Das::Plugins::DasMaaPi::DbgInitParams>>
{
    using value_type = Das::Plugins::DasMaaPi::DbgInitParams;

    template <typename Json>
    static std::optional<value_type> from_json(const Json& json)
    {
        if (json.is_null())
        {
            return std::nullopt;
        }
        return std::optional<value_type>(yyjson::cast<value_type>(json));
    }

    template <typename... Ts>
        requires requires(yyjson::writer::value_ref& v, value_type t) {
            v = t;
            v = std::pair(t, yyjson::copy_string);
        }
    static auto to_json(
        yyjson::writer::value_ref&       v,
        const std::optional<value_type>& t,
        Ts...)
    {
        if (t.has_value())
        {
            v = *t;
        }
    }

    template <typename... Ts>
        requires requires(yyjson::writer::value_ref& v, value_type t) {
            v = t;
            v = std::pair(t, yyjson::copy_string);
        }
    static auto
    to_json(yyjson::writer::value_ref& v, std::optional<value_type>&& t, Ts...)
    {
        if (t.has_value())
        {
            v = std::move(*t);
        }
    }
};

template <>
struct yyjson::caster<
    std::optional<Das::Plugins::DasMaaPi::ControllerInitParams>>
{
    using value_type = Das::Plugins::DasMaaPi::ControllerInitParams;

    template <typename Json>
    static std::optional<value_type> from_json(const Json& json)
    {
        if (json.is_null())
        {
            return std::nullopt;
        }
        return std::optional<value_type>(yyjson::cast<value_type>(json));
    }

    template <typename... Ts>
        requires requires(yyjson::writer::value_ref& v, value_type t) {
            v = t;
            v = std::pair(t, yyjson::copy_string);
        }
    static auto to_json(
        yyjson::writer::value_ref&       v,
        const std::optional<value_type>& t,
        Ts...)
    {
        if (t.has_value())
        {
            v = *t;
        }
    }

    template <typename... Ts>
        requires requires(yyjson::writer::value_ref& v, value_type t) {
            v = t;
            v = std::pair(t, yyjson::copy_string);
        }
    static auto
    to_json(yyjson::writer::value_ref& v, std::optional<value_type>&& t, Ts...)
    {
        if (t.has_value())
        {
            v = std::move(*t);
        }
    }
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::PiEnvSnapshotDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::MaaTaskExecutionDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::MaaPretaskDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::MaaExecutionPlanDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::ExecutionEnvelopeDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::CompileSummaryDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::CompileResultDto>
{
    using type = yyjson::snake_to_camel_transform;
};

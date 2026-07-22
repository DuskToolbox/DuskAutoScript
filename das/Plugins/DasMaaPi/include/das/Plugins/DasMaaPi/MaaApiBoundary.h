#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Das::Plugins::DasMaaPi
{
    using MaaResourceHandle = std::uintptr_t;
    using MaaControllerHandle = std::uintptr_t;
    using MaaTaskerHandle = std::uintptr_t;
    using MaaAgentClientHandle = std::uintptr_t;
    using MaaAsyncId = std::int64_t;

    inline constexpr MaaResourceHandle    kInvalidMaaResourceHandle = 0;
    inline constexpr MaaControllerHandle  kInvalidMaaControllerHandle = 0;
    inline constexpr MaaTaskerHandle      kInvalidMaaTaskerHandle = 0;
    inline constexpr MaaAgentClientHandle kInvalidMaaAgentClientHandle = 0;
    inline constexpr MaaAsyncId           kInvalidMaaAsyncId = 0;

    enum class MaaTaskStatus : std::int32_t
    {
        Invalid = 0,
        Pending = 1000,
        Running = 2000,
        Succeeded = 3000,
        Failed = 4000,
    };

    struct ControllerSpec
    {
        std::string name;
        std::string type;
        std::string read_path;
        std::string address;
        std::string adb_path = "adb";
        std::string config_json = "{}";
        std::string agent_path;
    };

    // Typed init parameters for every supported controller type. The single
    // CreateControllerFromParams dispatch fans out by `controller_type` to
    // the matching optional block: adb/dbg are cross-platform core symbols
    // (hard-linked), win32/gamepad are platform-specific (runtime resolved
    // via MaaDynamicSymbols). Tier2 types (MacOS/PlayCover/WlRoots/...) have
    // no optional block here and fall through to
    // controller-type-unsupported-on-platform.
    //
    // Display options (display_short_side / display_long_side / display_raw)
    // are read from the controller object's same-level keys (not the `config`
    // sub-key) and applied post-create via MaaControllerSetOption.
    struct Win32InitParams
    {
        std::string   class_regex;
        std::string   window_regex;
        std::uint64_t screencap_method = 0; // MaaWin32ScreencapMethod bitmask
        std::uint64_t mouse_method = 0;     // MaaWin32InputMethod single-select
        std::uint64_t keyboard_method = 0;  // MaaWin32InputMethod single-select
    };

    struct GamepadInitParams
    {
        std::string   class_regex;
        std::string   window_regex;
        std::uint64_t gamepad_type = 0;     // MaaGamepadType single-select
        std::uint64_t screencap_method = 0; // MaaWin32ScreencapMethod bitmask
    };

    struct AdbInitParams
    {
        std::string   adb_path = "adb";
        std::string   address;
        std::uint64_t screencap_method = 0; // MaaAdbScreencapMethod bitmask
        std::uint64_t input_method = 0;     // MaaAdbInputMethod bitmask
        std::string   config_json = "{}";
        std::string   agent_path;
    };

    struct DbgInitParams
    {
        std::string read_path;
    };

    struct ControllerInitParams
    {
        std::string                      controller_type;
        std::optional<Win32InitParams>   win32;
        std::optional<GamepadInitParams> gamepad;
        std::optional<AdbInitParams>     adb;
        std::optional<DbgInitParams>     dbg;
        std::optional<std::int32_t>      display_short_side;
        std::optional<std::int32_t>      display_long_side;
        std::optional<bool>              display_raw;
    };

    // Result of CreateControllerFromParams. The boundary surfaces structured
    // failure context (error_code/message) so MaaRuntime can attach typed
    // diagnostics (e.g. controller-window-not-found) without reverse-
    // engineering a bare kInvalidMaaControllerHandle.
    struct CreateControllerResult
    {
        MaaControllerHandle handle = kInvalidMaaControllerHandle;
        std::string         error_code; // empty on success
        std::string         error_message;
    };

    struct MaaApiResult
    {
        bool         ok = true;
        std::int64_t provider_code = 0;
        std::string  message;
        MaaAsyncId   id = kInvalidMaaAsyncId;

        static MaaApiResult Ok(MaaAsyncId id = kInvalidMaaAsyncId)
        {
            MaaApiResult result;
            result.id = id;
            return result;
        }

        static MaaApiResult Failure(
            std::int64_t provider_code,
            std::string  message)
        {
            MaaApiResult result;
            result.ok = false;
            result.provider_code = provider_code;
            result.message = std::move(message);
            return result;
        }
    };

    class IMaaApiBoundary
    {
    public:
        virtual ~IMaaApiBoundary() = default;

        virtual MaaResourceHandle CreateResource() = 0;
        virtual void DestroyResource(MaaResourceHandle resource) noexcept = 0;
        virtual MaaApiResult LoadResource(
            MaaResourceHandle resource,
            std::string_view  path) = 0;
        virtual std::optional<std::string> GetResourceHash(
            MaaResourceHandle resource) = 0;

        // Unified typed dispatch: fans out by params.controller_type to the
        // matching typed block (adb/dbg are hard-linked cross-platform core;
        // win32/gamepad are runtime-resolved via MaaDynamicSymbols). Tier2
        // controller types (macos/playcover/wlroots) and any genuinely
        // unknown type surface as kInvalidMaaControllerHandle with
        // controller-type-unsupported-on-platform — MaaFW does not export
        // those symbols on Windows builds.
        virtual CreateControllerResult CreateControllerFromParams(
            const ControllerSpec&       spec,
            const ControllerInitParams& params) = 0;
        virtual void DestroyController(
            MaaControllerHandle controller) noexcept = 0;

        virtual MaaTaskerHandle CreateTasker() = 0;
        virtual void         DestroyTasker(MaaTaskerHandle tasker) noexcept = 0;
        virtual MaaApiResult BindResource(
            MaaTaskerHandle   tasker,
            MaaResourceHandle resource) = 0;
        virtual MaaApiResult BindController(
            MaaTaskerHandle     tasker,
            MaaControllerHandle controller) = 0;

        virtual MaaApiResult PostTask(
            MaaTaskerHandle  tasker,
            std::string_view entry,
            std::string_view pipeline_override) = 0;
        virtual MaaTaskStatus WaitTask(
            MaaTaskerHandle tasker,
            MaaAsyncId      task_id) = 0;
        virtual MaaApiResult PostStop(MaaTaskerHandle tasker) = 0;

        virtual MaaAgentClientHandle CreateAgentClientV2(
            std::optional<std::string_view> identifier) = 0;
        virtual MaaAgentClientHandle CreateAgentClientTcp(
            std::uint16_t port) = 0;
        virtual void DestroyAgentClient(
            MaaAgentClientHandle client) noexcept = 0;
        virtual std::optional<std::string> GetAgentClientIdentifier(
            MaaAgentClientHandle client) = 0;
        virtual MaaApiResult BindAgentClientResource(
            MaaAgentClientHandle client,
            MaaResourceHandle    resource) = 0;
        virtual MaaApiResult RegisterAgentClientResourceSink(
            MaaAgentClientHandle client,
            MaaResourceHandle    resource) = 0;
        virtual MaaApiResult RegisterAgentClientControllerSink(
            MaaAgentClientHandle client,
            MaaControllerHandle  controller) = 0;
        virtual MaaApiResult RegisterAgentClientTaskerSink(
            MaaAgentClientHandle client,
            MaaTaskerHandle      tasker) = 0;
        virtual MaaApiResult SetAgentClientTimeout(
            MaaAgentClientHandle client,
            std::int64_t         milliseconds) = 0;
        virtual MaaApiResult ConnectAgentClient(
            MaaAgentClientHandle client) = 0;
        virtual bool DisconnectAgentClient(
            MaaAgentClientHandle client) noexcept = 0;
        virtual bool IsAgentClientConnected(MaaAgentClientHandle client) = 0;
        virtual bool IsAgentClientAlive(MaaAgentClientHandle client) = 0;
    };

    IMaaApiBoundary& DefaultMaaApiBoundary();
} // namespace Das::Plugins::DasMaaPi

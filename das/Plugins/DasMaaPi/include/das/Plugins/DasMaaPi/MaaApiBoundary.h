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

    // Parallel typed init struct produced by NormalizeControllerInitParams.
    // ControllerSpec stays the wire type (yyjson field_name_rule =
    // snake_to_camel; MaapiRuntimeTest.cpp:107 hard-codes its JSON shape), so
    // typed init parameters live in this separate struct. CompileMaapi fills
    // both, and CreateController consumes both: existing call sites that pass
    // only ControllerSpec keep working (adb/dbg path), new typed branches
    // (win32/gamepad) read the parallel struct.
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

    struct ControllerInitParams
    {
        std::string                      controller_type;
        std::optional<Win32InitParams>   win32;
        std::optional<GamepadInitParams> gamepad;
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

        virtual MaaControllerHandle CreateController(
            const ControllerSpec& spec) = 0;
        // Typed-init overload: dispatches to MaaWin32ControllerCreate /
        // MaaGamepadControllerCreate when the corresponding optional block is
        // present. Tier2 controller types (macos/playcover/wlroots) return
        // kInvalidMaaControllerHandle with
        // controller-type-unsupported-on-platform on Windows builds — MaaFW
        // does not export those symbols here. Adb/dbg callers continue using
        // the legacy single-arg overload.
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

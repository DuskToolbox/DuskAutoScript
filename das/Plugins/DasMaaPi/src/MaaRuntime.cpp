#include <das/Plugins/DasMaaPi/ExecutionEnvelope.h>
#include <das/Plugins/DasMaaPi/MaaPiErrorCodes.h>
#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/fmt.h>

#include "AgentProcessRunner.h"
#include "AgentRuntimeService.h"
#include "MaaCallbackLogger.h"
#include "MaaDynamicSymbols.h"
#include "MaaHandle.h"
#include "PathResolver.h"
#include "PiHttpClient.h"
#include "PluginUtils.h"
#include "PretaskRunner.h"
#include "Win32WindowFinder.h"

#ifndef DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY
#include <MaaAgentClient/MaaAgentClientAPI.h>
#include <MaaFramework/MaaAPI.h>
#endif

// boost::dll::this_line_location() 不依赖 MaaFW，测试构建（守卫生效）也要
// 可用，否则 AgentOutputLogger 在测试 target 编译时解析不到 plugin_dir。
#include <boost/dll/runtime_symbol_info.hpp>
#include <das/Utils/BoostPathBridge.h>
#include <das/Utils/FileUtils.hpp>
#include <das/Utils/PathText.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// PI V2 协议 `agent: object | object[]` 的多态反序列化：单对象包成 1 元素
// vector，数组直接成 vector。替换原手写 ReadAgentSpecs 的三形态识别；每个
// 元素的字段解析委托 default_caster<AgentSpecDto>（field_name_rule =
// snake_to_camel，读 camelCase），envelope 路径的 agent 始终由 PiCompiler
// 产出为 camelCase。
template <>
struct yyjson::caster<
    std::vector<Das::Plugins::DasMaaPi::AgentRuntime::AgentSpecDto>>
{
    using AgentSpecDto = Das::Plugins::DasMaaPi::AgentRuntime::AgentSpecDto;

    template <yyjson::json_value Json>
    static std::vector<AgentSpecDto> from_json(const Json& json)
    {
        // 元素字段解析委托 default_caster<AgentSpecDto>（field_name_rule =
        // snake_to_camel）。B′ 之后 envelope 由 PiCompiler 用 default_caster
        // to_json 产出，optional identifier 以 null 存在，聚合反射 is_null
        // 跳过即可处理；caster 只保留 object|array 多态分流。
        std::vector<AgentSpecDto> result;
        if (auto single = json.as_object())
        {
            result.emplace_back(yyjson::cast<AgentSpecDto>(json));
            return result;
        }
        if (auto array = json.as_array())
        {
            for (auto it = array->begin(); it != array->end(); ++it)
            {
                result.emplace_back(yyjson::cast<AgentSpecDto>(*it));
            }
            return result;
        }
        throw yyjson::bad_cast("agent must be an object or array");
    }
};

// MaaExecutionPlanDto：整体委托 default_caster 聚合反射（field_name_rule =
// snake_to_camel）。controller 作为普通非 optional 字段直接读——PiCompiler 是
// envelope 唯一生产者且总产出 controller 对象（PiCompiler.cpp:520），运行时无
// 「无 controller → piEnv 兜底」路径，故移除两源 fallback（原 fallback
// 是死代码， 仅合成测试覆盖）。缺 key 抛 std::out_of_range、类型错误抛
// yyjson::bad_cast， 由 ParseExecutionEnvelope 统一映射为
// DAS_E_INVALID_ARGUMENT。
template <>
struct yyjson::caster<Das::Plugins::DasMaaPi::MaaExecutionPlanDto>
{
    template <yyjson::json_value Json>
    static Das::Plugins::DasMaaPi::MaaExecutionPlanDto from_json(
        const Json& json)
    {
        auto obj = json.as_object();
        if (!obj)
        {
            throw yyjson::bad_cast("maapi must be an object");
        }
        return yyjson::detail::default_caster<
            Das::Plugins::DasMaaPi::MaaExecutionPlanDto>::from_json(*obj);
    }
};

namespace Das::Plugins::DasMaaPi
{
    namespace
    {
        // WaitConnection poll-loop bounds (mirror MXU's
        // callbackCache.ts:104-133 deadline-polling shape). The previous
        // implementation called MaaControllerWait synchronously, which has
        // no timeout form parameter — an unresponsive adb server / frozen
        // emulator / stuck driver blocked the runtime indefinitely. The
        // poll loop bounds the wait with CONNECT_WAIT_DEADLINE_MS while
        // checking MaaControllerStatus every CONNECT_POLL_INTERVAL_MS.
        // Run-level StopRequested (checked before PostConnection) provides
        // cooperative cancel at the entry point; the deadline is the bound
        // for in-progress connect ops that ignore the stop request.
        inline constexpr int CONNECT_WAIT_DEADLINE_MS = 30000;
        inline constexpr int CONNECT_POLL_INTERVAL_MS = 50;

#ifndef DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY
        class ScopedMaaApiBoundary final : public IMaaApiBoundary
        {
        public:
            MaaResourceHandle CreateResource() override
            {
                return reinterpret_cast<MaaResourceHandle>(MaaResourceCreate());
            }

            void DestroyResource(MaaResourceHandle resource) noexcept override
            {
                MaaResourceDestroy(reinterpret_cast<MaaResource*>(resource));
            }

            MaaApiResult LoadResource(
                MaaResourceHandle resource,
                std::string_view  path) override
            {
                auto* maa_resource = reinterpret_cast<MaaResource*>(resource);
                auto  id = MaaResourcePostBundle(
                    maa_resource,
                    std::string(path).c_str());
                if (id == MaaInvalidId)
                {
                    return MaaApiResult::Failure(
                        id,
                        "MaaResourcePostBundle failed");
                }
                const auto status = MaaResourceWait(maa_resource, id);
                if (status != MaaStatus_Succeeded)
                {
                    return MaaApiResult::Failure(
                        status,
                        "Maa resource load failed");
                }
                return MaaApiResult::Ok(id);
            }

            std::optional<std::string> GetResourceHash(
                MaaResourceHandle resource) override
            {
                const std::unique_ptr<
                    MaaStringBuffer,
                    decltype(&MaaStringBufferDestroy)>
                    buffer(MaaStringBufferCreate(), MaaStringBufferDestroy);
                if (!buffer)
                {
                    return std::nullopt;
                }
                if (!MaaResourceGetHash(
                        reinterpret_cast<MaaResource*>(resource),
                        buffer.get()))
                {
                    return std::nullopt;
                }
                const char* value = MaaStringBufferGet(buffer.get());
                return value ? std::optional<std::string>(value) : std::nullopt;
            }

            std::vector<AdbDeviceInfo> FindAdbDevices() override
            {
                return FindAllAdbDevices(DefaultAdbDeviceEnumerator());
            }

            CreateControllerResult CreateControllerFromParams(
                const ControllerSpec&       spec,
                const ControllerInitParams& params) override
            {
                CreateControllerResult result;
                const auto             type = Lower(params.controller_type);
                if (type == "adb" && params.adb)
                {
                    return CreateAdbController(params);
                }
                if ((type == "dbg" || type == "debug") && params.dbg)
                {
                    return CreateDbgController(params);
                }
                if (type == "win32" && params.win32)
                {
                    return CreateWin32Controller(params);
                }
                if (type == "gamepad" && params.gamepad)
                {
                    return CreateGamepadController(params);
                }
                // Tier2 controller types (MacOS/PlayCover/WlRoots/...) and
                // any genuinely unknown type fall through to
                // controller-type-unsupported-on-platform here. The same
                // diagnostic is reached NATURALLY on platforms whose MaaFW
                // build does not export the Win32/Gamepad create symbols —
                // MaaDynamicSymbols leaves the pointer null and
                // CreateWin32Controller / CreateGamepadController short-
                // circuit with the same error_code, so no #ifdef is needed
                // to keep the cross-platform binary link-safe.
                result.error_code = "controller-type-unsupported-on-platform";
                result.error_message =
                    "Controller type '" + params.controller_type
                    + "' is not available on this build of DasMaaPi (spec name='"
                    + spec.name + "')";
                return result;
            }

        private:
            static void ApplyDisplayOptions(
                MaaController*              controller,
                const ControllerInitParams& params)
            {
                if (params.display_short_side)
                {
                    const auto value = *params.display_short_side;
                    MaaControllerSetOption(
                        controller,
                        MaaCtrlOption_ScreenshotTargetShortSide,
                        const_cast<void*>(static_cast<const void*>(&value)),
                        sizeof(value));
                }
                if (params.display_long_side)
                {
                    const auto value = *params.display_long_side;
                    MaaControllerSetOption(
                        controller,
                        MaaCtrlOption_ScreenshotTargetLongSide,
                        const_cast<void*>(static_cast<const void*>(&value)),
                        sizeof(value));
                }
                if (params.display_raw)
                {
                    const auto value =
                        *params.display_raw ? MaaBool{1} : MaaBool{0};
                    MaaControllerSetOption(
                        controller,
                        MaaCtrlOption_ScreenshotUseRawSize,
                        const_cast<void*>(static_cast<const void*>(&value)),
                        sizeof(value));
                }
            }

            static CreateControllerResult CreateAdbController(
                const ControllerInitParams& params)
            {
                CreateControllerResult result;
                const auto&            a = *params.adb;
                // MaaAdbControllerCreate / MaaDbgControllerCreate stay
                // HARD-LINKED (cross-platform core). Only Win32/Gamepad go
                // through MaaDynamicSymbols runtime resolution. Empty address
                // is rejected as a configuration error — Adb requires a
                // device address.
                if (a.address.empty())
                {
                    result.error_code = "controller-create-failed";
                    result.error_message =
                        "Adb controller requires a non-empty address";
                    return result;
                }
                const auto adb_path =
                    a.adb_path.empty() ? std::string("adb") : a.adb_path;
                const auto config =
                    a.config_json.empty() ? std::string("{}") : a.config_json;
                // When screencap/input are both zero (PI did not specify),
                // fall back to MaaFW defaults — preserves the prior
                // MaaAdbScreencapMethod_Default / MaaAdbInputMethod_Default
                // behavior of the legacy overload.
                const auto screencap =
                    a.screencap_method != 0
                        ? static_cast<MaaAdbScreencapMethod>(a.screencap_method)
                        : MaaAdbScreencapMethod_Default;
                const auto input =
                    a.input_method != 0
                        ? static_cast<MaaAdbInputMethod>(a.input_method)
                        : MaaAdbInputMethod_Default;
                auto* controller =
                    reinterpret_cast<MaaController*>(MaaAdbControllerCreate(
                        adb_path.c_str(),
                        a.address.c_str(),
                        screencap,
                        input,
                        config.c_str(),
                        a.agent_path.c_str()));
                if (!controller)
                {
                    result.error_code = "controller-create-failed";
                    result.error_message =
                        "MaaAdbControllerCreate returned null for address='"
                        + a.address + "'";
                    return result;
                }
                ApplyDisplayOptions(controller, params);
                result.handle =
                    reinterpret_cast<MaaControllerHandle>(controller);
                return result;
            }

            static CreateControllerResult CreateDbgController(
                const ControllerInitParams& params)
            {
                CreateControllerResult result;
                const auto&            d = *params.dbg;
                // MaaDbgControllerCreate is hard-linked (cross-platform
                // core). The single read_path arg mirrors the legacy
                // overload; no display options to apply for the dbg
                // controller.
                auto* controller = reinterpret_cast<MaaController*>(
                    MaaDbgControllerCreate(d.read_path.c_str()));
                if (!controller)
                {
                    result.error_code = "controller-create-failed";
                    result.error_message =
                        "MaaDbgControllerCreate returned null for read_path='"
                        + d.read_path + "'";
                    return result;
                }
                ApplyDisplayOptions(controller, params);
                result.handle =
                    reinterpret_cast<MaaControllerHandle>(controller);
                return result;
            }

            static CreateControllerResult CreateWin32Controller(
                const ControllerInitParams& params)
            {
                CreateControllerResult result;
                const auto&            w = *params.win32;
                // Win32 controller create is resolved at runtime via
                // MaaDynamicSymbols (one cross-platform binary). When the
                // host MaaFW build does not export MaaWin32ControllerCreate
                // (non-Windows), the pointer is null and the typed path
                // surfaces controller-type-unsupported-on-platform — no
                // #ifdef DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY sprawl, the
                // fallback falls out of dynamic availability naturally.
                const auto& symbols = LoadMaaDynamicSymbols();
                if (!symbols.win32_create)
                {
                    result.error_code =
                        "controller-type-unsupported-on-platform";
                    result.error_message =
                        "MaaWin32ControllerCreate is not available on this "
                        "build of DasMaaPi (class_regex='"
                        + w.class_regex + "' window_regex='" + w.window_regex
                        + "')";
                    return result;
                }
                auto hwnd = FindWindowByRegex(
                    w.class_regex,
                    w.window_regex,
                    DefaultWindowEnumerator());
                if (hwnd.handle == nullptr)
                {
                    result.error_code = hwnd.error_code;
                    result.error_message = hwnd.error_message;
                    return result;
                }
                auto* controller =
                    reinterpret_cast<MaaController*>(symbols.win32_create(
                        hwnd.handle,
                        w.screencap_method,
                        w.mouse_method,
                        w.keyboard_method));
                if (!controller)
                {
                    result.error_code = "controller-create-failed";
                    result.error_message =
                        "MaaWin32ControllerCreate returned null for class_regex='"
                        + w.class_regex + "' window_regex='" + w.window_regex
                        + "'";
                    return result;
                }
                ApplyDisplayOptions(controller, params);
                result.handle =
                    reinterpret_cast<MaaControllerHandle>(controller);
                return result;
            }

            static CreateControllerResult CreateGamepadController(
                const ControllerInitParams& params)
            {
                CreateControllerResult result;
                const auto&            g = *params.gamepad;
                // Same dynamic-resolution pattern as CreateWin32Controller:
                // MaaGamepadControllerCreate is only exported on Windows
                // MaaFW; on other platforms the loader leaves the pointer
                // null and we return controller-type-unsupported-on-platform.
                const auto& symbols = LoadMaaDynamicSymbols();
                if (!symbols.gamepad_create)
                {
                    result.error_code =
                        "controller-type-unsupported-on-platform";
                    result.error_message =
                        "MaaGamepadControllerCreate is not available on this "
                        "build of DasMaaPi (gamepad_type="
                        + std::to_string(g.gamepad_type) + ")";
                    return result;
                }
                // Gamepad hWnd is optional (screencap disabled when null).
                // Only invoke Win32WindowFinder when the PI supplied a regex.
                void* hwnd = nullptr;
                if (!g.class_regex.empty() || !g.window_regex.empty())
                {
                    auto found = FindWindowByRegex(
                        g.class_regex,
                        g.window_regex,
                        DefaultWindowEnumerator());
                    if (found.handle == nullptr)
                    {
                        result.error_code = found.error_code;
                        result.error_message = found.error_message;
                        return result;
                    }
                    hwnd = found.handle;
                }
                auto* controller =
                    reinterpret_cast<MaaController*>(symbols.gamepad_create(
                        hwnd,
                        g.gamepad_type,
                        g.screencap_method));
                if (!controller)
                {
                    result.error_code = "controller-create-failed";
                    result.error_message =
                        "MaaGamepadControllerCreate returned null for gamepad_type="
                        + std::to_string(g.gamepad_type);
                    return result;
                }
                ApplyDisplayOptions(controller, params);
                result.handle =
                    reinterpret_cast<MaaControllerHandle>(controller);
                return result;
            }

            void DestroyController(
                MaaControllerHandle controller) noexcept override
            {
                MaaControllerDestroy(
                    reinterpret_cast<MaaController*>(controller));
            }

            MaaTaskerHandle CreateTasker() override
            {
                return reinterpret_cast<MaaTaskerHandle>(MaaTaskerCreate());
            }

            void DestroyTasker(MaaTaskerHandle tasker) noexcept override
            {
                MaaTaskerDestroy(reinterpret_cast<MaaTasker*>(tasker));
            }

            MaaApiResult BindResource(
                MaaTaskerHandle   tasker,
                MaaResourceHandle resource) override
            {
                if (!MaaTaskerBindResource(
                        reinterpret_cast<MaaTasker*>(tasker),
                        reinterpret_cast<MaaResource*>(resource)))
                {
                    return MaaApiResult::Failure(
                        0,
                        "MaaTaskerBindResource failed");
                }
                return MaaApiResult::Ok();
            }

            MaaApiResult BindController(
                MaaTaskerHandle     tasker,
                MaaControllerHandle controller) override
            {
                if (!MaaTaskerBindController(
                        reinterpret_cast<MaaTasker*>(tasker),
                        reinterpret_cast<MaaController*>(controller)))
                {
                    return MaaApiResult::Failure(
                        0,
                        "MaaTaskerBindController failed");
                }
                return MaaApiResult::Ok();
            }

            // Cross-platform core MaaFW symbols (exported by every MaaFW
            // build) — hard-linked direct calls per the binding principle,
            // same category as MaaControllerSetOption above. They do not go
            // through MaaDynamicSymbols; the linker resolves them against the
            // SHARED IMPORTED DasBundledMaaFramework dependency at build time.
            MaaApiResult PostConnection(MaaControllerHandle controller) override
            {
                const auto id =
                    MaaControllerPostConnection(AsMaaController(controller));
                if (id == MaaInvalidId)
                {
                    return MaaApiResult::Failure(
                        id,
                        "MaaControllerPostConnection failed");
                }
                return MaaApiResult::Ok(id);
            }

            MaaTaskStatus WaitConnection(
                MaaControllerHandle controller,
                MaaAsyncId          connection_id) override
            {
                // Poll MaaControllerStatus instead of blocking on
                // MaaControllerWait. MaaControllerWait has no timeout form
                // parameter — without this loop an unresponsive adb
                // server / frozen emulator / stuck device driver would
                // block the runtime indefinitely (the DAS stop_token
                // cannot interrupt a syscall stuck inside MaaFW). The poll
                // loop bounds the wait with CONNECT_WAIT_DEADLINE_MS while
                // checking MaaControllerStatus every
                // CONNECT_POLL_INTERVAL_MS, returning control between
                // checks so a Run-level stop request observed before
                // PostConnection can still short-circuit. On deadline
                // expiry the loop returns Invalid — Run treats that as a
                // connect failure via the existing
                // status != Succeeded || !IsConnected check (decimal
                // DasResult diagnostic, never hex). The interface contract
                // is unchanged: callers still see MaaTaskStatus.
                const auto ctrl = AsMaaController(controller);
                const auto id = static_cast<MaaCtrlId>(connection_id);
                const auto deadline =
                    std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(CONNECT_WAIT_DEADLINE_MS);
                for (;;)
                {
                    const auto status = MaaControllerStatus(ctrl, id);
                    // Pending/Running are non-terminal — keep polling.
                    // Invalid/Succeeded/Failed are terminal — return them
                    // to the caller for connection-state judgment.
                    if (status != MaaStatus_Pending
                        && status != MaaStatus_Running)
                    {
                        return static_cast<MaaTaskStatus>(status);
                    }
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        // Deadline expired without a terminal status —
                        // surface as Invalid (Run's existing
                        // status != Succeeded check handles it).
                        return MaaTaskStatus::Invalid;
                    }
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(CONNECT_POLL_INTERVAL_MS));
                }
            }

            bool IsConnected(MaaControllerHandle controller) override
            {
                return MaaControllerConnected(AsMaaController(controller)) != 0;
            }

            MaaApiResult PostTask(
                MaaTaskerHandle  tasker,
                std::string_view entry,
                std::string_view pipeline_override) override
            {
                const auto id = MaaTaskerPostTask(
                    reinterpret_cast<MaaTasker*>(tasker),
                    std::string(entry).c_str(),
                    std::string(pipeline_override).c_str());
                if (id == MaaInvalidId)
                {
                    return MaaApiResult::Failure(
                        id,
                        "MaaTaskerPostTask failed");
                }
                return MaaApiResult::Ok(id);
            }

            MaaTaskStatus WaitTask(MaaTaskerHandle tasker, MaaAsyncId task_id)
                override
            {
                return static_cast<MaaTaskStatus>(MaaTaskerWait(
                    reinterpret_cast<MaaTasker*>(tasker),
                    static_cast<MaaTaskId>(task_id)));
            }

            MaaApiResult PostStop(MaaTaskerHandle tasker) override
            {
                const auto id =
                    MaaTaskerPostStop(reinterpret_cast<MaaTasker*>(tasker));
                if (id == MaaInvalidId)
                {
                    return MaaApiResult::Failure(
                        id,
                        "MaaTaskerPostStop failed");
                }
                return MaaApiResult::Ok(id);
            }

            MaaApiResult SetCallback(
                MaaTaskerHandle  tasker,
                MaaMsgCallbackFn callback,
                void*            trans_arg) override
            {
                // Sink family is cross-platform core MaaFW — every MaaFW
                // build exports MaaTaskerAddSink, so it is a link-time
                // dependency resolved against the SHARED IMPORTED
                // DasBundledMaaFramework. No dynamic symbol resolution, no
                // null check: if the binary links, the symbol exists.
                // Failure here means MaaFW rejected the sink (invalid tasker
                // handle, etc.) — surface as Failure, never crash.
                const auto sink_id = MaaTaskerAddSink(
                    AsMaaTasker(tasker),
                    reinterpret_cast<MaaEventCallback>(callback),
                    trans_arg);
                if (sink_id == MaaInvalidId)
                {
                    return MaaApiResult::Failure(
                        sink_id,
                        "MaaTaskerAddSink failed");
                }
                return MaaApiResult::Ok(sink_id);
            }

            MaaApiResult RegisterResourceSink(
                MaaResourceHandle resource,
                MaaMsgCallbackFn  callback,
                void*             trans_arg) override
            {
                // Same cross-platform core sink family as SetCallback above.
                // Resource sink funnels resource-loading events into the
                // shared MaaEventCallback. Hard-link direct call — no
                // dynamic resolution, no null check.
                const auto sink_id = MaaResourceAddSink(
                    AsMaaResource(resource),
                    reinterpret_cast<MaaEventCallback>(callback),
                    trans_arg);
                if (sink_id == MaaInvalidId)
                {
                    return MaaApiResult::Failure(
                        sink_id,
                        "MaaResourceAddSink failed");
                }
                return MaaApiResult::Ok(sink_id);
            }

            MaaApiResult RegisterControllerSink(
                MaaControllerHandle controller,
                MaaMsgCallbackFn    callback,
                void*               trans_arg) override
            {
                // Same cross-platform core sink family as SetCallback above.
                // Controller sink funnels controller-connection / state
                // events into the shared MaaEventCallback.
                const auto sink_id = MaaControllerAddSink(
                    AsMaaController(controller),
                    reinterpret_cast<MaaEventCallback>(callback),
                    trans_arg);
                if (sink_id == MaaInvalidId)
                {
                    return MaaApiResult::Failure(
                        sink_id,
                        "MaaControllerAddSink failed");
                }
                return MaaApiResult::Ok(sink_id);
            }

            MaaApiResult RegisterTaskerContextSink(
                MaaTaskerHandle  tasker,
                MaaMsgCallbackFn callback,
                void*            trans_arg) override
            {
                // Context sink — the second of MaaFW's two tasker sink
                // channels. Node.* messages (Recognition / Action /
                // NextList / WaitFreezes) carry the `focus` payload focus
                // templates match against, and MaaFW delivers them ONLY
                // through MaaTaskerAddContextSink (MaaTasker.h:32); the
                // main sink (MaaTaskerAddSink, wired in SetCallback) never
                // sees them. Same cross-platform core sink family — every
                // MaaFW build exports it, so the hard-link direct call has
                // no null check. Failure means MaaFW rejected the sink and
                // is surfaced as MaaApiResult::Failure.
                const auto sink_id = MaaTaskerAddContextSink(
                    AsMaaTasker(tasker),
                    reinterpret_cast<MaaEventCallback>(callback),
                    trans_arg);
                if (sink_id == MaaInvalidId)
                {
                    return MaaApiResult::Failure(
                        sink_id,
                        "MaaTaskerAddContextSink failed");
                }
                return MaaApiResult::Ok(sink_id);
            }

            Das::DasPtr<Das::ExportInterface::IDasImage> GetCachedImage(
                MaaControllerHandle controller) override
            {
                // Non-blocking read of MaaFW's internal cached frame
                // (MaaControllerCachedImage, cross-platform core — hard-linked
                // direct call per the binding principle). Returns false when
                // no frame is available (unconnected / no screencap yet);
                // the caller (FocusImageProvider closure) then proceeds
                // without an image.
                std::
                    unique_ptr<MaaImageBuffer, decltype(&MaaImageBufferDestroy)>
                        buffer(MaaImageBufferCreate(), &MaaImageBufferDestroy);
                if (!buffer)
                {
                    return {};
                }
                if (!MaaControllerCachedImage(
                        AsMaaController(controller),
                        buffer.get()))
                {
                    return {};
                }
                const auto* encoded = MaaImageBufferGetEncoded(buffer.get());
                const auto  size = MaaImageBufferGetEncodedSize(buffer.get());
                if (encoded == nullptr || size == 0)
                {
                    return {};
                }
                // MaaFW encodes screenshots as JPG by default (configurable
                // via MaaCtrlOption, but the encoded buffer always matches
                // whatever format MaaFW chose internally). Both JPG and PNG
                // are accepted by CreateIDasImageFromEncodedData.
                DasImageDesc desc{
                    .p_data = reinterpret_cast<char*>(
                        const_cast<std::uint8_t*>(encoded)),
                    .data_size = size,
                    .data_format = Das::ExportInterface::DAS_IMAGE_FORMAT_JPG};
                Das::DasPtr<Das::ExportInterface::IDasImage> image;
                if (DAS::IsFailed(
                        CreateIDasImageFromEncodedData(&desc, image.Put())))
                {
                    return {};
                }
                return image;
            }

            MaaAgentClientHandle CreateAgentClientV2(
                std::optional<std::string_view> identifier) override
            {
                std::unique_ptr<
                    MaaStringBuffer,
                    decltype(&MaaStringBufferDestroy)>
                                       buffer(nullptr, MaaStringBufferDestroy);
                const MaaStringBuffer* id_buffer = nullptr;
                if (identifier)
                {
                    buffer.reset(MaaStringBufferCreate());
                    if (!buffer
                        || !MaaStringBufferSetEx(
                            buffer.get(),
                            identifier->data(),
                            identifier->size()))
                    {
                        return INVALID_MAA_AGENT_CLIENT_HANDLE;
                    }
                    id_buffer = buffer.get();
                }
                return reinterpret_cast<MaaAgentClientHandle>(
                    MaaAgentClientCreateV2(id_buffer));
            }

            MaaAgentClientHandle CreateAgentClientTcp(
                std::uint16_t port) override
            {
                return reinterpret_cast<MaaAgentClientHandle>(
                    MaaAgentClientCreateTcp(port));
            }

            void DestroyAgentClient(
                MaaAgentClientHandle client) noexcept override
            {
                MaaAgentClientDestroy(
                    reinterpret_cast<MaaAgentClient*>(client));
            }

            std::optional<std::string> GetAgentClientIdentifier(
                MaaAgentClientHandle client) override
            {
                const std::unique_ptr<
                    MaaStringBuffer,
                    decltype(&MaaStringBufferDestroy)>
                    buffer(MaaStringBufferCreate(), MaaStringBufferDestroy);
                if (!buffer)
                {
                    return std::nullopt;
                }
                if (!MaaAgentClientIdentifier(
                        reinterpret_cast<MaaAgentClient*>(client),
                        buffer.get()))
                {
                    return std::nullopt;
                }
                const char* value = MaaStringBufferGet(buffer.get());
                return value ? std::optional<std::string>(value) : std::nullopt;
            }

            MaaApiResult BindAgentClientResource(
                MaaAgentClientHandle client,
                MaaResourceHandle    resource) override
            {
                if (!MaaAgentClientBindResource(
                        reinterpret_cast<MaaAgentClient*>(client),
                        reinterpret_cast<MaaResource*>(resource)))
                {
                    return MaaApiResult::Failure(
                        0,
                        "MaaAgentClientBindResource failed");
                }
                return MaaApiResult::Ok();
            }

            MaaApiResult RegisterAgentClientResourceSink(
                MaaAgentClientHandle client,
                MaaResourceHandle    resource) override
            {
                if (!MaaAgentClientRegisterResourceSink(
                        reinterpret_cast<MaaAgentClient*>(client),
                        reinterpret_cast<MaaResource*>(resource)))
                {
                    return MaaApiResult::Failure(
                        0,
                        "MaaAgentClientRegisterResourceSink failed");
                }
                return MaaApiResult::Ok();
            }

            MaaApiResult RegisterAgentClientControllerSink(
                MaaAgentClientHandle client,
                MaaControllerHandle  controller) override
            {
                if (!MaaAgentClientRegisterControllerSink(
                        reinterpret_cast<MaaAgentClient*>(client),
                        reinterpret_cast<MaaController*>(controller)))
                {
                    return MaaApiResult::Failure(
                        0,
                        "MaaAgentClientRegisterControllerSink failed");
                }
                return MaaApiResult::Ok();
            }

            MaaApiResult RegisterAgentClientTaskerSink(
                MaaAgentClientHandle client,
                MaaTaskerHandle      tasker) override
            {
                if (!MaaAgentClientRegisterTaskerSink(
                        reinterpret_cast<MaaAgentClient*>(client),
                        reinterpret_cast<MaaTasker*>(tasker)))
                {
                    return MaaApiResult::Failure(
                        0,
                        "MaaAgentClientRegisterTaskerSink failed");
                }
                return MaaApiResult::Ok();
            }

            MaaApiResult SetAgentClientTimeout(
                MaaAgentClientHandle client,
                std::int64_t         milliseconds) override
            {
                if (!MaaAgentClientSetTimeout(
                        reinterpret_cast<MaaAgentClient*>(client),
                        milliseconds))
                {
                    return MaaApiResult::Failure(
                        0,
                        "MaaAgentClientSetTimeout failed");
                }
                return MaaApiResult::Ok();
            }

            MaaApiResult ConnectAgentClient(
                MaaAgentClientHandle client) override
            {
                if (!MaaAgentClientConnect(
                        reinterpret_cast<MaaAgentClient*>(client)))
                {
                    return MaaApiResult::Failure(
                        0,
                        "MaaAgentClientConnect failed");
                }
                return MaaApiResult::Ok();
            }

            bool DisconnectAgentClient(
                MaaAgentClientHandle client) noexcept override
            {
                return MaaAgentClientDisconnect(
                    reinterpret_cast<MaaAgentClient*>(client));
            }

            bool IsAgentClientConnected(MaaAgentClientHandle client) override
            {
                return MaaAgentClientConnected(
                    reinterpret_cast<MaaAgentClient*>(client));
            }

            bool IsAgentClientAlive(MaaAgentClientHandle client) override
            {
                return MaaAgentClientAlive(
                    reinterpret_cast<MaaAgentClient*>(client));
            }

        private:
            static std::string Lower(std::string value)
            {
                std::transform(
                    value.begin(),
                    value.end(),
                    value.begin(),
                    [](unsigned char ch)
                    { return static_cast<char>(std::tolower(ch)); });
                return value;
            }

            // MaaControllerHandle（uintptr_t）与 MaaController*（MaaFW 不透明
            // 指针）之间的转换是 IMaaApiBoundary 抽象的固有代价：handle 以
            // uintptr_t 跨 ABI 边界（可 mock、可序列化、FakeMaaApiBoundary 无
            // 需 MaaFW 头），调 MaaFW C API 时必须转回指针。收敛到单一命名
            // helper，比在每个方法里散布 reinterpret_cast 更清晰。
            static MaaController* AsMaaController(MaaControllerHandle handle)
            {
                // NOLINTNEXTLINE(performance-no-int-to-ptr)
                return reinterpret_cast<MaaController*>(handle);
            }

            // MaaTaskerHandle（uintptr_t）→ MaaTasker*：同 AsMaaController 的
            // 边界恢复语义，用于 sink 家族与 task 操作的硬链直调。
            static MaaTasker* AsMaaTasker(MaaTaskerHandle handle)
            {
                // NOLINTNEXTLINE(performance-no-int-to-ptr)
                return reinterpret_cast<MaaTasker*>(handle);
            }

            // MaaResourceHandle（uintptr_t）→ MaaResource*：同 AsMaaController
            // 的边界恢复语义，用于 resource sink 的硬链直调。
            static MaaResource* AsMaaResource(MaaResourceHandle handle)
            {
                // NOLINTNEXTLINE(performance-no-int-to-ptr)
                return reinterpret_cast<MaaResource*>(handle);
            }
        };
#else
        class ScopedMaaApiBoundary final : public IMaaApiBoundary
        {
        public:
            MaaResourceHandle CreateResource() override
            {
                return INVALID_MAA_RESOURCE_HANDLE;
            }

            void DestroyResource(MaaResourceHandle) noexcept override {}

            MaaApiResult LoadResource(MaaResourceHandle, std::string_view)
                override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            std::optional<std::string> GetResourceHash(
                MaaResourceHandle) override
            {
                return std::nullopt;
            }

            std::vector<AdbDeviceInfo> FindAdbDevices() override
            {
                // Real Toolkit unavailable in the disabled-boundary test
                // target — an empty scan keeps the engine's device
                // resolution on its explicit adb-device-not-found failure
                // path instead of crashing.
                return {};
            }

            CreateControllerResult CreateControllerFromParams(
                const ControllerSpec&,
                const ControllerInitParams&) override
            {
                CreateControllerResult result;
                result.error_code = "controller-type-unsupported-on-platform";
                result.error_message = "Real Maa boundary disabled in tests";
                return result;
            }

            void DestroyController(MaaControllerHandle) noexcept override {}

            MaaTaskerHandle CreateTasker() override
            {
                return INVALID_MAA_TASKER_HANDLE;
            }

            void DestroyTasker(MaaTaskerHandle) noexcept override {}

            MaaApiResult BindResource(MaaTaskerHandle, MaaResourceHandle)
                override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            MaaApiResult BindController(MaaTaskerHandle, MaaControllerHandle)
                override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            MaaApiResult PostConnection(MaaControllerHandle) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            MaaTaskStatus WaitConnection(MaaControllerHandle, MaaAsyncId)
                override
            {
                return MaaTaskStatus::Invalid;
            }

            bool IsConnected(MaaControllerHandle) override { return false; }

            MaaApiResult PostTask(
                MaaTaskerHandle,
                std::string_view,
                std::string_view) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            MaaTaskStatus WaitTask(MaaTaskerHandle, MaaAsyncId) override
            {
                return MaaTaskStatus::Invalid;
            }

            MaaApiResult PostStop(MaaTaskerHandle) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            MaaApiResult SetCallback(MaaTaskerHandle, MaaMsgCallbackFn, void*)
                override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            MaaApiResult RegisterResourceSink(
                MaaResourceHandle,
                MaaMsgCallbackFn,
                void*) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            MaaApiResult RegisterControllerSink(
                MaaControllerHandle,
                MaaMsgCallbackFn,
                void*) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            MaaApiResult RegisterTaskerContextSink(
                MaaTaskerHandle,
                MaaMsgCallbackFn,
                void*) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            Das::DasPtr<Das::ExportInterface::IDasImage> GetCachedImage(
                MaaControllerHandle) override
            {
                return {};
            }

            MaaAgentClientHandle CreateAgentClientV2(
                std::optional<std::string_view>) override
            {
                return INVALID_MAA_AGENT_CLIENT_HANDLE;
            }

            MaaAgentClientHandle CreateAgentClientTcp(std::uint16_t) override
            {
                return INVALID_MAA_AGENT_CLIENT_HANDLE;
            }

            void DestroyAgentClient(MaaAgentClientHandle) noexcept override {}

            std::optional<std::string> GetAgentClientIdentifier(
                MaaAgentClientHandle) override
            {
                return std::nullopt;
            }

            MaaApiResult BindAgentClientResource(
                MaaAgentClientHandle,
                MaaResourceHandle) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            MaaApiResult RegisterAgentClientResourceSink(
                MaaAgentClientHandle,
                MaaResourceHandle) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            MaaApiResult RegisterAgentClientControllerSink(
                MaaAgentClientHandle,
                MaaControllerHandle) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            MaaApiResult RegisterAgentClientTaskerSink(
                MaaAgentClientHandle,
                MaaTaskerHandle) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            MaaApiResult SetAgentClientTimeout(
                MaaAgentClientHandle,
                std::int64_t) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            MaaApiResult ConnectAgentClient(MaaAgentClientHandle) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            bool DisconnectAgentClient(MaaAgentClientHandle) noexcept override
            {
                return false;
            }

            bool IsAgentClientConnected(MaaAgentClientHandle) override
            {
                return false;
            }

            bool IsAgentClientAlive(MaaAgentClientHandle) override
            {
                return false;
            }
        };
#endif

        IMaaApiBoundary*                   g_boundary_for_test = nullptr;
        AgentRuntime::IAgentProcessRunner* g_agent_runner_for_test = nullptr;

        void AddDiagnostic(
            MaaRuntimeResult&           result,
            std::string                 code,
            std::string                 message,
            std::optional<std::int64_t> provider_code = std::nullopt,
            std::string                 severity = "error")
        {
            result.diagnostics.emplace_back(
                MaaRuntimeDiagnostic{
                    .severity = std::move(severity),
                    .code = std::move(code),
                    .message = std::move(message),
                    .provider_code = provider_code});
        }

        void AddAgentDiagnostics(
            MaaRuntimeResult&                          result,
            const AgentRuntime::AgentRuntimeResultDto& agent_result)
        {
            for (const auto& diagnostic : agent_result.diagnostics)
            {
                AddDiagnostic(
                    result,
                    diagnostic.code,
                    diagnostic.message,
                    std::nullopt,
                    diagnostic.severity);
            }
        }

        AgentRuntime::PiEnvDto ToAgentPiEnv(const PiEnvSnapshotDto& env)
        {
            AgentRuntime::PiEnvDto result;
            result.interface_version = env.interface_version;
            result.client_name = env.client_name;
            result.client_version = env.client_version;
            result.client_language = env.client_language;
            result.client_maafw_version = env.client_maafw_version;
            result.project_version = env.project_version;
            result.controller_json = env.controller_json;
            result.resource_json = env.resource_json;
            return result;
        }

        AgentRuntime::AgentRuntimeRequestDto BuildAgentStartRequest(
            const ExecutionEnvelopeDto& envelope)
        {
            AgentRuntime::AgentRuntimeRequestDto request;
            request.operation = "start";
            request.interface_directory = envelope.maapi.interface_directory;
            request.agent = envelope.maapi.agent;
            request.pi_env = ToAgentPiEnv(envelope.maapi.pi_env);
            return request;
        }

        class ScopedAgentRuntimeSession final
        {
        public:
            void Arm(
                AgentRuntime::AgentRuntimeService&   service,
                std::string                          session_id,
                AgentRuntime::AgentRuntimeOptionsDto options)
            {
                service_ = &service;
                session_id_ = std::move(session_id);
                options_ = options;
            }

            AgentRuntime::AgentRuntimeResultDto StopNow()
            {
                if (!service_ || !session_id_)
                {
                    return {};
                }

                auto result = service_->Stop(*session_id_, options_);
                service_ = nullptr;
                session_id_.reset();
                return result;
            }

            ~ScopedAgentRuntimeSession() { (void)StopNow(); }

        private:
            AgentRuntime::AgentRuntimeService*   service_ = nullptr;
            std::optional<std::string>           session_id_;
            AgentRuntime::AgentRuntimeOptionsDto options_;
        };

        bool StopRequested(PluginInterface::IDasStopToken* stop_token)
        {
            bool requested = false;
            return stop_token
                   && DAS::IsOk(stop_token->StopRequested(&requested))
                   && requested;
        }

        // agent stdout/stderr 本地落盘的旋转日志上限：5MB 单文件 × 3 文件 =
        // 15MB 总上限，对齐 spdlog rotating_file_sink
        // 既有范式（das/Gateway/Logger.cpp :16-19）。agent 输出是诊断用途，15MB
        // 足以覆盖一次完整运行排查； spdlog 自动轮转清理，无需 sweep。
        constexpr std::size_t MAX_AGENT_LOG_BYTES = 5 * 1024 * 1024;
        constexpr std::size_t MAX_AGENT_LOG_FILES = 3;

        // 进程级单例 spdlog rotating logger，把 agent stdout/stderr 落盘到
        // {plugin_dir}/logs/DasMaaPi-agent.log（与 MaaFW maafw.log
        // 同目录，文件名 不冲突）。函数级 static（C++11
        // 起首次初始化线程安全）。 plugin_dir 经
        // boost::dll::this_line_location().parent_path() 发现（既有 范式
        // MaaRuntime.cpp:1583-1585、MaaDynamicSymbols.cpp:85-86）。 整函数
        // try/catch 降级返空——本地落盘绝不阻塞 agent：plugin_dir 只读 / 磁盘满
        // / this_line_location 异平台抛时静默退化为不落盘。 spdlog::logger
        // 默认带 mutex + sink_mt 多线程安全：io_thread 闭包 （RecordAgentOutput
        // 调用方 :1931）与 Run 线程失败路径（:2054）并发写 无虞。**显式
        // spdlog::register_logger 不可省**：rotating_logger_mt 默认 走
        // synchronous_factory::create → initialize_logger（仅 set level /
        // pattern），不进 registry，spdlog::get 命中不了；Gateway/Logger.cpp:27
        // 既有范式同此事。

        std::shared_ptr<spdlog::logger> AgentOutputLogger()
        {
            try
            {
                const auto registered = spdlog::get("DasMaaPiAgent");
                if (registered)
                {
                    return registered;
                }
                const auto plugin_module = boost::dll::this_line_location();
                const auto plugin_dir = plugin_module.parent_path();
                const auto log_dir = plugin_dir / DAS_PATH_LITERAL("logs");
                std::error_code error_code;
                if (!Das::Utils::CreateDirectoryRecursive(
                        Das::Utils::ToStdPath(log_dir),
                        error_code))
                {
                    return nullptr;
                }
                // spdlog 的 filename_t 是 ACP 窄串（_fsopen 打开），此处
                // 保留 boost path 的 ACP 出向与其消费面一致；换 UTF-8
                // 出向会在中文 plugin 目录下产生乱码文件名。
                auto sink =
                    std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                        (log_dir / DAS_PATH_LITERAL("DasMaaPi-agent.log"))
                            .string(),
                        MAX_AGENT_LOG_BYTES,
                        MAX_AGENT_LOG_FILES);
                auto logger =
                    std::make_shared<spdlog::logger>("DasMaaPiAgent", sink);
                spdlog::register_logger(logger);
                return logger;
            }
            catch (const std::exception&)
            {
                return nullptr;
            }
        }

        // 把 agent 一行 stdout/stderr/失败消息包成 IDasLogEntry 投到 recorder。
        // severity 映射：stdout→INFO，stderr→ERROR，failure→ERROR。
        // stream_label：可读流标识（"stdout"/"stderr"/"failure"），写入
        // stream field 供 UI 可选过滤。source/agentId/stream 的 value 都是
        // JSON-encoded quoted literal（recorder 经 SetField JSON 解析后存盘），
        // 经 DasU8StringOnStack 借用 std::string 持久内存——SetField 同步解析，
        // 调用返回后字符串不再被引用。
        // yyjson 字符串安全：不直接拼裸字符串进 user.jsonl；SetField value 经
        // yyjson 解析、文本经 AddText 自动 JSON 转义。agent 来源标识经
        // [AGENT <agentId>] 文本前缀表达（不占 type 槽）。
        DasResult RecordAgentOutput(
            Das::ExportInterface::IDasProcessLogRecorder* recorder,
            std::string_view                              agent_id,
            std::string_view                              line,
            Das::ExportInterface::DasLogType              severity,
            std::string_view                              stream_label)
        {
            // 本地落盘先于 recorder 检查：recorder 降级空 / IPC 断 / 主进程没起
            // 时 user.jsonl 投不到，本地 spdlog rotating
            // 文件照写（事后排查靠它）。 AgentOutputLogger()
            // 失败返空时静默跳过，不抛不阻断既有路径。
            if (const auto logger = AgentOutputLogger())
            {
                if (severity == Das::ExportInterface::DAS_LOG_TYPE_ERROR)
                {
                    logger->error("[AGENT {}] {}", agent_id, line);
                }
                else
                {
                    logger->info("[AGENT {}] {}", agent_id, line);
                }
            }

            if (recorder == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            Das::DasPtr<Das::ExportInterface::IDasLogEntry> entry;
            auto hr = CreateIDasLogEntry(entry.Put());
            if (DAS::IsFailed(hr) || !entry)
            {
                return hr;
            }
            hr = entry->SetType(severity);
            if (DAS::IsFailed(hr))
            {
                return hr;
            }
            const std::string prefixed =
                DAS_FMT_NS::format("[AGENT {}] {}", agent_id, line);
            DasU8StringOnStack text_string{prefixed.c_str()};
            hr = entry->AddText(&text_string);
            if (DAS::IsFailed(hr))
            {
                return hr;
            }
            // value 是 JSON 字符串字面量：值需带引号。本地 std::string 持有引号
            // 包裹后的值，DasU8StringOnStack 借用其 c_str()。SetField 调用返回
            // 前 recorder 已同步解析/拷贝，本地 string 可安全析构。
            const std::string agent_id_value =
                DAS_FMT_NS::format("\"{}\"", agent_id);
            const std::string stream_value =
                DAS_FMT_NS::format("\"{}\"", stream_label);
            DasU8StringOnStack source_key{"source"};
            DasU8StringOnStack source_value{"\"agent\""};
            hr = entry->SetField(&source_key, &source_value);
            if (DAS::IsFailed(hr))
            {
                return hr;
            }
            DasU8StringOnStack agent_id_key{"agentId"};
            DasU8StringOnStack agent_id_json{agent_id_value.c_str()};
            hr = entry->SetField(&agent_id_key, &agent_id_json);
            if (DAS::IsFailed(hr))
            {
                return hr;
            }
            DasU8StringOnStack stream_key{"stream"};
            DasU8StringOnStack stream_json{stream_value.c_str()};
            hr = entry->SetField(&stream_key, &stream_json);
            if (DAS::IsFailed(hr))
            {
                return hr;
            }
            // recorder 失败降级：按 severity 映射打 [focus-degraded] 前缀
            // fallback（stderr → ERROR，stdout → INFO），落 logs/DasCore.log
            // 不再静默黑洞。line 是 RecordAgentOutput 既有局部量，与
            // on_output 闭包的 severity 映射一致。
            return RecordEntryOrLog(
                recorder,
                entry.Get(),
                severity == Das::ExportInterface::DAS_LOG_TYPE_ERROR
                    ? Das::ExportInterface::DAS_LOG_TYPE_ERROR
                    : Das::ExportInterface::DAS_LOG_TYPE_INFO,
                line);
        }

        std::string SerializeJson(const yyjson::value& value)
        {
            auto serialized = Das::Utils::SerializeYyjsonValue(value);
            return serialized.value_or("{}");
        }

        // PI v2.7.0 pretask stage (D-86-02). Runs each pretask via the
        // configured runner BEFORE CreateController. Returns false and emits a
        // structured diagnostic when a pretask fails to spawn or exits with a
        // nonzero code; the caller surfaces DAS_E_MAAPI_PRETASK_FAILED and
        // aborts the run.
        bool RunPretasks(
            MaaRuntimeResult&               result,
            const ExecutionEnvelopeDto&     envelope,
            PluginInterface::IDasStopToken* stop_token)
        {
            // pretask 阶段取证日志：进入运行时的 pretask 数量与各条目，
            // 空列表（编译链丢失）与执行失败（后续诊断）由此可分。
            for (const auto& pretask : envelope.maapi.pretasks)
            {
                DAS_LOG_INFO(
                    DAS_FMT_NS::format(
                        "[DasMaaPi] pretask stage: name={} exec={}",
                        pretask.name,
                        pretask.exec)
                        .c_str());
            }
            if (envelope.maapi.pretasks.empty())
            {
                DAS_LOG_INFO(
                    "[DasMaaPi] pretask stage: envelope has no pretasks");
                return true;
            }
            BoostPretaskRunner default_runner;
            auto* const        test_runner = PretaskRunnerForRuntime();
            auto& runner = test_runner
                               ? *test_runner
                               : static_cast<IPretaskRunner&>(default_runner);
            for (const auto& pretask : envelope.maapi.pretasks)
            {
                if (StopRequested(stop_token))
                {
                    result.das_result = DAS_E_FAIL;
                    result.stopped = true;
                    AddDiagnostic(
                        result,
                        "pretask-stop-requested",
                        "DAS stop token requested during pretask execution");
                    return false;
                }
                PretaskLaunchRequest request;
                // 相对路径 exec 必须基于 interface_directory 预解析成绝对
                // 路径后再 spawn：Windows CreateProcess 用父进程 CWD 解析
                // 相对可执行路径，process_start_dir 只决定子进程启动后的
                // CWD——不预解析会 spawn 失败（与 agent 共用 ResolveChildExec，
                // 对齐 MXU maa_agent.rs:339 resolve_child_exec_path）。
                request.exec = Das::Utils::PathToUtf8(ResolveChildExec(
                    pretask.exec,
                    envelope.maapi.interface_directory));
                request.args = pretask.args;
                request.working_directory = Das::Utils::PathFromUtf8(
                    envelope.maapi.interface_directory);
                auto run = runner.Run(request);
                if (!run.launched)
                {
                    result.das_result = DAS_E_MAAPI_PRETASK_FAILED;
                    AddDiagnostic(
                        result,
                        "pretask-failed",
                        "PI pretask '" + pretask.name
                            + "' failed to launch: " + run.failure_message);
                    return false;
                }
                if (!run.exit_code || *run.exit_code != 0)
                {
                    const auto code = run.exit_code.value_or(-1);
                    result.das_result = DAS_E_MAAPI_PRETASK_FAILED;
                    AddDiagnostic(
                        result,
                        "pretask-nonzero-exit",
                        "PI pretask '" + pretask.name
                            + "' exited with nonzero status",
                        static_cast<std::int64_t>(code));
                    return false;
                }
            }
            return true;
        }

        // 手写 Field/FromObject helper（OptionalStringField /
        // RequiredStringField / StringArrayField / BoolField /
        // OptionalInt32Field / CopyJsonValue）已由 default_caster + caster
        // 特化全面取代并移除；SerializeJson 保留 （MaaFW 边界 opaque JSON
        // 透传，MaaRuntime::Run 在用）。
    } // namespace

    IMaaApiBoundary& DefaultMaaApiBoundary()
    {
        static ScopedMaaApiBoundary boundary;
        return boundary;
    }

    void SetMaaApiBoundaryForTest(IMaaApiBoundary* boundary)
    {
        g_boundary_for_test = boundary;
    }

    void SetAgentProcessRunnerForTest(AgentRuntime::IAgentProcessRunner* runner)
    {
        g_agent_runner_for_test = runner;
    }

    AgentRuntime::IAgentProcessRunner* AgentProcessRunnerForRuntime()
    {
        return g_agent_runner_for_test;
    }

    IMaaApiBoundary& MaaApiBoundaryForRuntime()
    {
        return g_boundary_for_test ? *g_boundary_for_test
                                   : DefaultMaaApiBoundary();
    }

    ParsedExecutionEnvelope ParseExecutionEnvelope(const yyjson::value& value)
    {
        ParsedExecutionEnvelope parsed;
        try
        {
            // 整体委托 default_caster<ExecutionEnvelopeDto>：外层 version/
            // pluginGuid/taskTypeGuid 自动反射，maapi 走 caster<
            // MaaExecutionPlanDto>（controller 两源 fallback）。default_caster
            // 聚合反射缺 key 抛 std::out_of_range，类型/结构错误抛
            // yyjson::bad_cast（含 maapi caster 抛出的），统一映射为参数错误。
            parsed.envelope = yyjson::cast<ExecutionEnvelopeDto>(value);
        }
        catch (const std::exception& e)
        {
            parsed.result = DAS_E_INVALID_ARGUMENT;
            parsed.message = e.what();
            return parsed;
        }
        parsed.result = ValidateExecutionEnvelope(parsed.envelope);
        return parsed;
    }

    DasResult ValidateExecutionEnvelope(const ExecutionEnvelopeDto& envelope)
    {
        if (envelope.plugin_guid != PLUGIN_GUID_TEXT
            || envelope.task_type_guid != TASK_GUID_TEXT)
        {
            return DAS_E_INVALID_ARGUMENT;
        }
        if (envelope.version != 1)
        {
            return DAS_E_INVALID_ARGUMENT;
        }
        if (envelope.maapi.requires_agent_runtime
            && envelope.maapi.agent.empty())
        {
            return DAS_E_INVALID_ARGUMENT;
        }
        if (envelope.maapi.tasks.empty())
        {
            return DAS_E_INVALID_ARGUMENT;
        }
        return DAS_S_OK;
    }

    MaaRuntimeResult MaaRuntime::Run(
        const ExecutionEnvelopeDto&     envelope,
        IMaaApiBoundary&                boundary,
        PluginInterface::IDasStopToken* stop_token)
    {
        MaaRuntimeResult result;
        const auto       validation = ValidateExecutionEnvelope(envelope);
        if (DAS::IsFailed(validation))
        {
            result.das_result = validation;
            AddDiagnostic(
                result,
                "invalid-envelope",
                "Execution envelope is not supported by the MAAPI runtime");
            return result;
        }

        // PI v2 的 controller.adb 对象不含 address（设备由客户端运行时发现，
        // 协议 L168-171）。物化时未选设备的 envelope 携带空 address，真实边界
        // 只会在 MaaAdbControllerCreate 处报 "requires a non-empty address"——
        // 用户无从知道去哪修。执行输入校验段显性拒绝：诊断走
        // adb-device-missing（i18n 提示重新扫描并选择设备）。
        // （Lower helper 位于 prod-only 宏分支内，这里自带 ASCII 小写化。）
        std::string controller_type = envelope.maapi.controller.type;
        std::transform(
            controller_type.begin(),
            controller_type.end(),
            controller_type.begin(),
            [](unsigned char character)
            { return static_cast<char>(std::tolower(character)); });
        if (controller_type == "adb"
            && envelope.maapi.controller.address.empty()
            && (!envelope.maapi.controller_init_params
                || !envelope.maapi.controller_init_params->adb
                || envelope.maapi.controller_init_params->adb->address.empty()))
        {
            result.das_result = DAS_E_MAAPI_ADB_DEVICE_MISSING;
            AddDiagnostic(
                result,
                "adb-device-missing",
                "Adb controller requires a selected device; re-materialize "
                "the task after scanning and selecting an ADB device");
            return result;
        }

        // Recorder + callback context MUST be declared before every sink
        // owner (resource / controller / tasker). C++ destroys locals in
        // reverse declaration order, so tasker, controller and resource all
        // destruct BEFORE log_context — any final sink callback that
        // Maa*Destroy internally flushes through MaaCallbackLoggerEntry still
        // sees a valid trans_arg. All three sinks share the same
        // log_context.get() trans_arg (SetCallback on the tasker,
        // RegisterResourceSink on the resource, RegisterControllerSink on the
        // controller), so the lifetime invariant must cover all three, not
        // just the tasker. Recorder creation failure degrades focus logging
        // (log_context stays null, sinks see a null trans_arg) but does not
        // block the task.
        // resolve-or-degrade（IDasProgressService 范式）：本插件 loadMode=ipc
        // 跑在 Host 进程，Host 无 session/notifier，本地 new 出的 recorder
        // Record 写盘返 OBJECT_NOT_INIT（focus 日志在 IPC
        // 下从没工作过的根因）。 先经 C ABI DasQueryMainProcessInterfaceByName
        // 跨进程 resolve 主进程 具名服务 "das.focus-log-recorder"（App.cpp
        // 注册的语义角色名）， proxy.Record 经 IPC marshal 到主进程写
        // user.jsonl + notifier->Signal(UserJsonl)。resolve 失败 → 降级本地
        // CreateIDasProcessLogRecorder 工厂（in-process /
        // 测试路径保持行为一致）。 走 C ABI 而非内部 IPC C++ 接口：插件 DLL
        // 通过稳定 ABI 契约访问 Core， 不依赖 das/Core/IPC 内部 thread_local 与
        // IResolveContext vtable。
        Das::DasPtr<Das::ExportInterface::IDasProcessLogRecorder> log_recorder;
        DasResult        recorder_rc = DAS_E_FAIL;
        DasPtr<IDasBase> resolved_base;
        const auto       resolve_rc = DasQueryMainProcessInterfaceByName(
            "das.focus-log-recorder",
            resolved_base.Put());
        if (DAS::IsOk(resolve_rc) && resolved_base)
        {
            (void)resolved_base.As(log_recorder);
            recorder_rc = log_recorder ? DAS_S_OK : DAS_E_FAIL;
        }
        if (!log_recorder)
        {
            // 降级：in-process / 测试路径（无 IPC context）或 resolve 失败
            // （主进程未注册服务）。本地 recorder 在 Host 进程无 session 时
            // Record 返 OBJECT_NOT_INIT，与 retire 前的 LogRecorderRuntime
            // 未初始化行为一致（不阻塞 task）。打 DAS_LOG_ERROR（含 resolve_rc
            // 十进制）让生产中降级可 grep 发现，focus/agent 日志经
            // RecordEntryOrLog fallback 落 logs/DasCore.log（不再静默黑洞）。
            const std::string degrade_message = DAS_FMT_NS::format(
                "Focus recorder IPC resolve failed (resolve_rc={}); "
                "falling back to spdlog only - focus/agent logs go to "
                "logs/DasCore.log, NOT user.jsonl",
                static_cast<std::int64_t>(resolve_rc));
            DAS_LOG_ERROR(degrade_message.c_str());
            recorder_rc = CreateIDasProcessLogRecorder(
                nullptr,
                nullptr,
                log_recorder.Put());
        }
        // BeastHttpClient owner 声明在 log_context **之前**：C++ 逆序析构保证
        // owner 在 log_context **之后**销毁——任一 Maa*Destroy 内部冲刷 final
        // 回调时 log_context.http_client 裸指针持续有效（Pitfall 1 生命周期
        // 不变量）。5 秒短超时 + verify_tls=true 默认 TLS 校验（SSRF 缓解：
        // scheme 限制 + 短超时 + TLS 校验 + 失败降级字面文本 + 1 MiB 响应
        // 体截断）。
        std::unique_ptr<PiHttpClient>             http_client_owner;
        std::unique_ptr<MaaCallbackLoggerContext> log_context;
        if (DAS::IsFailed(recorder_rc))
        {
            AddDiagnostic(
                result,
                "create-recorder-failed",
                "CreateIDasProcessLogRecorder failed",
                static_cast<std::int64_t>(recorder_rc),
                "warning");
        }
        else
        {
            http_client_owner = std::make_unique<BeastHttpClient>(
                true,
                std::chrono::seconds(5));
            log_context = std::make_unique<MaaCallbackLoggerContext>();
            log_context->recorder = std::move(log_recorder);
            // focus content 解析所需的 log_context 四字段：
            // language / interface_directory / translations 来自 envelope，
            // http_client 指向 BeastHttpClient owner（非 owning 裸指针）。
            log_context->http_client = http_client_owner.get();
            log_context->language =
                std::string{envelope.maapi.pi_env.client_language};
            log_context->interface_directory =
                Das::Utils::PathFromUtf8(envelope.maapi.interface_directory);
            if (!envelope.maapi.focus_translations_path.empty())
            {
                // 一次性读取 translations 文件（PiCompiler 经
                // catalog.language_paths[pi_language] 注入路径）。读/解析失败
                // 时 translations 留空，focus content `$key` 解析降级为去 `$`
                // 前缀的 key（ResolveFocusContent 既有 fallback），不阻塞
                // task。
                const auto translations_path = Das::Utils::PathFromUtf8(
                    envelope.maapi.focus_translations_path);
                std::ifstream translations_input{translations_path};
                if (translations_input.is_open())
                {
                    std::string translations_text(
                        (std::istreambuf_iterator<char>(translations_input)),
                        std::istreambuf_iterator<char>());
                    auto parsed_translations =
                        Das::Utils::ParseYyjsonFromString(translations_text);
                    auto translations_obj =
                        parsed_translations ? parsed_translations->as_object()
                                            : std::nullopt;
                    if (translations_obj)
                    {
                        for (auto it = translations_obj->begin();
                             it != translations_obj->end();
                             ++it)
                        {
                            if (auto value = it->second.as_string())
                            {
                                log_context
                                    ->translations[std::string{it->first}] =
                                    std::string{value->data(), value->size()};
                            }
                        }
                    }
                    else
                    {
                        AddDiagnostic(
                            result,
                            "focus-translations-parse-failed",
                            "Failed to parse focus translations JSON at: "
                                + Das::Utils::PathToUtf8(translations_path)
                                + "; focus content `$key` resolution "
                                  "degrades to key text",
                            std::nullopt,
                            "warning");
                    }
                }
                else
                {
                    AddDiagnostic(
                        result,
                        "focus-translations-missing",
                        "Focus translations file not found at: "
                            + Das::Utils::PathToUtf8(translations_path)
                            + "; focus content `$key` resolution degrades "
                              "to key text",
                        std::nullopt,
                        "warning");
                }
            }
        }

#ifndef DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY
        // Configure MaaFW's global LogDir ONCE per process, before any
        // MaaFW resource/controller/tasker is created. MaaFW writes its
        // own maafw.log (full framework trace: message stream + internal
        // recognition/action details) plus a .bak rotation into this
        // directory — DasMaaPi does NOT write the file. The plugin's
        // process working directory may differ from its install location
        // (DasHostX loads plugins from arbitrary paths), so derive
        // plugin_dir from this_line_location().parent_path() — bare-name
        // load("MaaFramework") or default cwd assumptions fail when the
        // plugin is loaded from a non-system directory. user.jsonl (the
        // user-visible focus subset written by MaaCallbackLogger) is
        // unchanged: maafw.log is the full framework trace, user.jsonl is
        // the focus subset; the two complement each other. LogDir is a
        // process-global option, so a once-flag prevents re-setting it on
        // Run reentry.
        static std::once_flag log_dir_once;
        std::call_once(
            log_dir_once,
            []()
            {
                try
                {
                    const auto plugin_module = boost::dll::this_line_location();
                    const auto plugin_dir = plugin_module.parent_path();
                    // MaaFW 的 LogDir 通道按 UTF-8 解释窄串（C API 约定），
                    // 出向取 UTF-8 文本而非 ACP。
                    const auto log_dir = Das::Utils::PathToUtf8(
                        Das::Utils::ToStdPath(plugin_dir)
                        / DAS_PATH_LITERAL("logs"));
                    // MaaGlobalSetOption is the cross-platform core MaaFW
                    // symbol (MaaGlobal.h:25, hard-linked direct call per
                    // the binding principle). value is the C-string path,
                    // val_size is the string length (no null terminator).
                    // Failure is best-effort: MaaFW falls back to its
                    // default <cwd>/debug/maafw.log when the option is not
                    // set, so a failure here does not block the run.
                    // (MaaGlobalSetOption is the non-deprecated name for
                    // the older MaaSetGlobalOption — same behavior.)
                    (void)MaaGlobalSetOption(
                        MaaGlobalOption_LogDir,
                        const_cast<void*>(
                            static_cast<const void*>(log_dir.c_str())),
                        static_cast<MaaOptionValueSize>(log_dir.size()));
                }
                catch (const std::exception&)
                {
                    // this_line_location() can throw on exotic platforms
                    // or when the module handle is not resolvable; MaaFW
                    // falls back to its default LogDir. LogDir setup is
                    // diagnostic-only — never blocks the task.
                }
            });
#endif

        const ScopedResource resource(boundary, boundary.CreateResource());
        if (resource.get() == INVALID_MAA_RESOURCE_HANDLE)
        {
            result.das_result = DAS_E_FAIL;
            AddDiagnostic(
                result,
                "create-resource-failed",
                "Maa resource creation failed");
            return result;
        }

        // Resource sink MUST be registered BEFORE LoadResource so the
        // Resource.Loading.Starting/Succeeded/Failed lifecycle events reach
        // the callback (MaaFW emits them during LoadResource's
        // post_bundle/wait). Registering after — as the previous wiring did
        // — silently loses every Resource.Loading.* message because the sink
        // is attached only after the op that emits them has finished.
        // log_context is already constructed above (declared before every
        // sink owner), so trans_arg stays valid through ScopedResource
        // destruction. Failure degrades to a warning, never short-circuits.
        if (log_context)
        {
            auto reg_res = boundary.RegisterResourceSink(
                resource.get(),
                &MaaCallbackLoggerEntry,
                log_context.get());
            if (!reg_res.ok)
            {
                AddDiagnostic(
                    result,
                    "register-resource-sink-failed",
                    reg_res.message,
                    reg_res.provider_code,
                    "warning");
            }
        }

        for (const auto& path : envelope.maapi.resource_paths)
        {
            auto load = boundary.LoadResource(resource.get(), path);
            if (!load.ok)
            {
                result.das_result = DAS_E_FAIL;
                AddDiagnostic(
                    result,
                    "load-resource-failed",
                    load.message,
                    load.provider_code);
                return result;
            }
        }

        if (envelope.maapi.resource_hash)
        {
            auto actual_hash = boundary.GetResourceHash(resource.get());
            if (actual_hash && *actual_hash != *envelope.maapi.resource_hash)
            {
                AddDiagnostic(
                    result,
                    "resource-hash-mismatch",
                    "Maa resource hash does not match the PI envelope",
                    std::nullopt,
                    "warning");
            }
        }
        // PI v2.2.0 controller.attach_resource_path：resource.path 加载完成
        // 且 resource.hash 校验（v2.6）之后，额外加载所选控制器声明的附加
        // 资源（协议 L236-238 校验时机：path 后、attach 前）。后加载的资源
        // 覆盖先加载的同名任务（协议 L1076 资源覆盖）。失败按 resource.path
        // 同语义中止——attach 是项目声明的必需资源，缺失应阻断并给出诊断，
        // 而非静默丢失（W1 修复前的痛点正是无诊断的隐性资源丢失）。
        for (const auto& path : envelope.maapi.attach_resource_paths)
        {
            auto attach_load = boundary.LoadResource(resource.get(), path);
            if (!attach_load.ok)
            {
                result.das_result = DAS_E_FAIL;
                AddDiagnostic(
                    result,
                    "load-attach-resource-failed",
                    attach_load.message,
                    attach_load.provider_code);
                return result;
            }
        }

        // PI v2.7.0 pretask stage (D-86-02): run BEFORE CreateController so
        // the pretask can bootstrap the environment (launch emulator, log in,
        // etc.) before the controller connects. Abort on launch failure or
        // nonzero exit per protocol L259.
        if (!RunPretasks(result, envelope, stop_token))
        {
            return result;
        }

        ControllerSpec controller_spec = envelope.maapi.controller;
        if (controller_spec.name.empty())
        {
            controller_spec.name = envelope.maapi.controller_name;
        }
        MaaControllerHandle controller_handle = INVALID_MAA_CONTROLLER_HANDLE;
        if (!envelope.maapi.controller_init_params)
        {
            // Every supported controller type (Adb/Dbg/Win32/Gamepad/Tier2)
            // is emitted by PiCompiler with typed init params. Reaching here
            // means the envelope was hand-crafted without them — surface a
            // deterministic error instead of silently calling a deleted
            // legacy overload.
            result.das_result = DAS_E_FAIL;
            AddDiagnostic(
                result,
                "controller-create-failed",
                "Controller init params missing for controller '"
                    + controller_spec.name + "'");
            return result;
        }
        // Unified typed dispatch (Adb/Dbg/Win32/Gamepad/Tier2): surface
        // structured failure codes (controller-window-not-found /
        // controller-window-ambiguous / controller-type-unsupported-on-
        // platform) instead of the generic controller-create-failed.
        auto typed = boundary.CreateControllerFromParams(
            controller_spec,
            *envelope.maapi.controller_init_params);
        controller_handle = typed.handle;
        if (controller_handle == INVALID_MAA_CONTROLLER_HANDLE)
        {
            result.das_result = DAS_E_FAIL;
            AddDiagnostic(
                result,
                typed.error_code.empty() ? "controller-create-failed"
                                         : typed.error_code,
                typed.error_message.empty()
                    ? "Maa typed controller creation failed"
                    : typed.error_message);
            return result;
        }
        const ScopedController controller(boundary, controller_handle);

        // Controller sink MUST be registered BEFORE PostConnection so the
        // Controller.Action.Starting/Succeeded/Failed events emitted during
        // the connect op reach the callback. Registering after — as the
        // previous wiring did — silently loses every Controller.Action.*
        // message. log_context is already constructed above, trans_arg
        // stays valid through ScopedController destruction. Failure degrades
        // to a warning, never short-circuits.
        if (log_context)
        {
            auto reg_ctrl = boundary.RegisterControllerSink(
                controller.get(),
                &MaaCallbackLoggerEntry,
                log_context.get());
            if (!reg_ctrl.ok)
            {
                AddDiagnostic(
                    result,
                    "register-controller-sink-failed",
                    reg_ctrl.message,
                    reg_ctrl.provider_code,
                    "warning");
            }
        }

        const ScopedTasker tasker(boundary, boundary.CreateTasker());
        if (tasker.get() == INVALID_MAA_TASKER_HANDLE)
        {
            result.das_result = DAS_E_FAIL;
            AddDiagnostic(
                result,
                "create-tasker-failed",
                "Maa tasker creation failed");
            return result;
        }

        auto bind_resource =
            boundary.BindResource(tasker.get(), resource.get());
        if (!bind_resource.ok)
        {
            result.das_result = DAS_E_FAIL;
            AddDiagnostic(
                result,
                "bind-resource-failed",
                bind_resource.message,
                bind_resource.provider_code);
            return result;
        }

        auto bind_controller =
            boundary.BindController(tasker.get(), controller.get());
        if (!bind_controller.ok)
        {
            result.das_result = DAS_E_FAIL;
            AddDiagnostic(
                result,
                "bind-controller-failed",
                bind_controller.message,
                bind_controller.provider_code);
            return result;
        }

        // 显式连接控制器（必败根因修复）：BindController 仅把 controller 绑到
        // tasker，不等同于 controller 已连接目标设备/窗口。MaaFW 协议要求所有
        // controller 类型（含 Dbg）显式 PostConnection + Wait + Connected
        // 校验； 未连接的 controller 进 PostTask 必在 HitResource/HitController
        // 阶段 抛出下游歧义错误。连接失败短路返回，不进 SetCallback/PostTask。
        // WaitConnection 阻塞至 MaaFW 报告连接 op 完成（无超时形参），故在
        // PostConnection 之前先查 stop_token 给已停止请求一个早退机会。
        if (StopRequested(stop_token))
        {
            result.das_result = DAS_E_FAIL;
            result.stopped = true;
            AddDiagnostic(
                result,
                "stop-requested-before-connect",
                "DAS stop token requested before controller connection");
            return result;
        }

        auto post_connection = boundary.PostConnection(controller.get());
        if (!post_connection.ok)
        {
            result.das_result = DAS_E_FAIL;
            AddDiagnostic(
                result,
                "controller-connect-failed",
                post_connection.message,
                post_connection.provider_code);
            return result;
        }

        const auto connection_status =
            boundary.WaitConnection(controller.get(), post_connection.id);
        if (connection_status != MaaTaskStatus::Succeeded
            || !boundary.IsConnected(controller.get()))
        {
            result.das_result = DAS_E_FAIL;
            AddDiagnostic(
                result,
                "controller-not-connected",
                "Controller failed to connect (status="
                    + std::to_string(
                        static_cast<std::int32_t>(connection_status))
                    + ")",
                static_cast<std::int64_t>(connection_status));
            return result;
        }

        // Inject the production FocusImageProvider closure before sink
        // registration: focus templates with {image} resolve to real
        // screenshots via boundary.GetCachedImage (non-blocking read of the
        // MaaFW internal cache). The closure captures the controller handle
        // BY VALUE (uintptr_t stable — avoids destructor-order UAF if the
        // controller unique_ptr resets before a sink callback drain) and
        // references boundary (Run scope lifetime, outlives every callback).
        // Assignment is a single std::function store — after every sink is
        // registered the context fields are read-only to callback threads.
        if (log_context)
        {
            const auto image_controller_handle = controller.get();
            log_context->image_provider = [image_controller_handle, &boundary]()
            { return boundary.GetCachedImage(image_controller_handle); };
        }

        if (log_context)
        {
            // 注册 MaaMsgAsyncCallback → MaaCallbackLogger 转发（D-88-07）。
            // 符号解析失败（T-88-13）或回调注册失败仅告警：focus 日志降级，
            // 任务本身不受影响。
            auto set_callback = boundary.SetCallback(
                tasker.get(),
                &MaaCallbackLoggerEntry,
                log_context.get());
            if (!set_callback.ok)
            {
                AddDiagnostic(
                    result,
                    "set-callback-failed",
                    set_callback.message,
                    set_callback.provider_code,
                    "warning");
            }
            // Context sink — the second MaaFW tasker sink channel. Node.*
            // messages (focus-bearing) are delivered ONLY through this
            // channel; without it every focus template silently misses at
            // the production boundary. Shares the same callback +
            // trans_arg as the main sink so Node.* focus flows into the
            // existing MaaCallbackLogger pipeline (focus template parse +
            // {image} provider). Failure degrades to a warning, mirroring
            // SetCallback — does not short-circuit PostTask.
            auto reg_ctx = boundary.RegisterTaskerContextSink(
                tasker.get(),
                &MaaCallbackLoggerEntry,
                log_context.get());
            if (!reg_ctx.ok)
            {
                AddDiagnostic(
                    result,
                    "register-tasker-context-sink-failed",
                    reg_ctx.message,
                    reg_ctx.provider_code,
                    "warning");
            }
            // Resource / controller sinks are registered earlier — at the
            // respective owner's creation site (before LoadResource /
            // PostConnection) — so their lifecycle events reach the
            // callback. See the registration blocks next to ScopedResource
            // and ScopedController above.
        }

        AgentRuntime::BoostAgentProcessRunner default_agent_runner;
        auto&                                 agent_runner =
            g_agent_runner_for_test
                ? *g_agent_runner_for_test
                : static_cast<AgentRuntime::IAgentProcessRunner&>(
                      default_agent_runner);
        AgentRuntime::AgentRuntimeService agent_service(boundary, agent_runner);
        ScopedAgentRuntimeSession         agent_session;
        if (envelope.maapi.requires_agent_runtime)
        {
            auto agent_request = BuildAgentStartRequest(envelope);
            // on_output 闭包：每行 stdout/stderr 经 RecordAgentOutput 投到
            // resolved recorder（IPC proxy 主进程 user.jsonl，或降级本地
            // recorder；service 透传，不引用 recorder）。severity：stdout→
            // INFO，stderr→WARNING。失败→ERROR 由 agent stop 后单独路径发
            // （exit_code != 0 时）。recorder 经 DasPtr& 捕获，闭包拷贝
            // （std::function copy）增 recorder refcount，与 BoostAgentProcess
            // 同寿。
            AgentRuntime::AgentOutputCallback agent_output_closure;
            if (log_context && log_context->recorder)
            {
                agent_output_closure = [&recorder = log_context->recorder](
                                           std::string_view agent_id,
                                           std::string_view line,
                                           bool             is_stderr)
                {
                    const auto severity =
                        is_stderr ? Das::ExportInterface::DAS_LOG_TYPE_ERROR
                                  : Das::ExportInterface::DAS_LOG_TYPE_INFO;
                    const auto stream_label = is_stderr
                                                  ? std::string_view{"stderr"}
                                                  : std::string_view{"stdout"};
                    (void)RecordAgentOutput(
                        recorder.Get(),
                        agent_id,
                        line,
                        severity,
                        stream_label);
                };
            }
            auto agent_start = agent_service.Start(
                agent_request,
                AgentRuntime::AgentRuntimeMaaContext{
                    .resource = resource.get(),
                    .controller = controller.get(),
                    .tasker = tasker.get()},
                agent_output_closure);
            if (agent_start.status != "succeeded" || !agent_start.session_id)
            {
                result.das_result = DAS_E_FAIL;
                AddAgentDiagnostics(result, agent_start);
                if (result.diagnostics.empty())
                {
                    AddDiagnostic(
                        result,
                        "agent-runtime-start-failed",
                        "PI agent runtime startup failed");
                }
                return result;
            }
            agent_session.Arm(
                agent_service,
                *agent_start.session_id,
                agent_request.options);
        }

        for (const auto& task : envelope.maapi.tasks)
        {
            if (StopRequested(stop_token))
            {
                boundary.PostStop(tasker.get());
                result.stopped = true;
                result.das_result = DAS_E_FAIL;
                AddDiagnostic(
                    result,
                    "stop-requested",
                    "DAS stop token requested Maa tasker stop");
                return result;
            }

            auto post = boundary.PostTask(
                tasker.get(),
                task.entry,
                SerializeJson(task.pipeline_override));
            if (!post.ok)
            {
                result.das_result = DAS_E_FAIL;
                AddDiagnostic(
                    result,
                    "post-task-failed",
                    post.message,
                    post.provider_code);
                return result;
            }

            const auto status = boundary.WaitTask(tasker.get(), post.id);
            if (status == MaaTaskStatus::Succeeded)
            {
                result.completed_tasks.emplace_back(task.task_name);
                continue;
            }

            result.das_result = DAS_E_FAIL;
            AddDiagnostic(
                result,
                "task-failed",
                "Maa task failed: " + task.task_name,
                static_cast<std::int64_t>(status));
            if (envelope.maapi.fail_fast)
            {
                return result;
            }
        }

        if (envelope.maapi.requires_agent_runtime)
        {
            auto agent_stop = agent_session.StopNow();
            if (agent_stop.status == "failed")
            {
                result.das_result = DAS_E_FAIL;
                AddAgentDiagnostics(result, agent_stop);
                if (result.diagnostics.empty())
                {
                    AddDiagnostic(
                        result,
                        "agent-runtime-stop-failed",
                        "PI agent runtime cleanup failed");
                }
            }
            else if (agent_stop.signals.timed_out)
            {
                AddDiagnostic(
                    result,
                    "agent-runtime-stop-timeout",
                    "PI agent runtime cleanup timed out",
                    std::nullopt,
                    "warning");
            }
            // 失败 agent → 单条 ERROR 投递（severity 映射第三档）。exit_code
            // 非零即视作失败；该路径与逐行 INFO/WARNING 闭包互补，覆盖 agent
            // 整体失败语义（MXU maa_agent.rs 的 stderr→warn! 仅覆盖流，整体
            // 失败需要单独 ERROR 信号）。
            if (log_context && log_context->recorder)
            {
                for (const auto& agent_state : agent_stop.agent)
                {
                    const bool failed = agent_state.exit_code.has_value()
                                        && *agent_state.exit_code != 0;
                    if (!failed)
                    {
                        continue;
                    }
                    std::string failure_message =
                        "agent exited with code "
                        + std::to_string(*agent_state.exit_code);
                    (void)RecordAgentOutput(
                        log_context->recorder.Get(),
                        agent_state.agent_id,
                        failure_message,
                        Das::ExportInterface::DAS_LOG_TYPE_ERROR,
                        "failure");
                }
            }
        }

        return result;
    }

    std::vector<RunLogLine> BuildRunLogLines(const MaaRuntimeResult& result)
    {
        std::vector<RunLogLine> lines;
        const bool              failed = DAS::IsFailed(result.das_result);

        // 标题 i18n（maapi.runFailed，{0} = 十进制错误码）；i18n 缺失 /
        // key miss 回退英文硬编码。仅失败时投递——标题是失败事件的锚点。
        if (failed)
        {
            const auto title_tmpl = TranslatePluginKey("maapi.runFailed");
            const std::string result_text =
                DAS_FMT_NS::format("{}", result.das_result);
            std::string title;
            if (!title_tmpl.empty())
            {
                // make_format_args 持引用而非拥有：参数须是具名局部变量，
                // 生命周期覆盖 vformat 调用（禁临时量）。
                title = DAS_FMT_NS::vformat(
                    std::string_view(title_tmpl),
                    DAS_FMT_NS::make_format_args(result_text));
            }
            else
            {
                title = DAS_FMT_NS::format(
                    "[DasMaaPi] MaaRuntime run failed, result={}",
                    result.das_result);
            }
            lines.push_back(
                {Das::ExportInterface::DAS_LOG_TYPE_ERROR, std::move(title)});
        }

        for (const auto& diagnostic : result.diagnostics)
        {
            // 成功结果仅投递 warning 级诊断（sink 注册降级等在成功运行
            // 也会出现）；error 级只伴随失败。
            if (!failed && diagnostic.severity != "warning")
            {
                continue;
            }
            // 诊断行 i18n：maapi.diagnostic.<code> key，miss 回退原始英文
            // message；code 原样保留（稳定标识，日志可 grep）。
            // provider_code 数值语言中立，始终附加。
            // 模板可含 {0} 占位：填原始英文 message（携带 pretask 名、
            // OS spawn 错误、设备 address 等动态细节）——纯文案模板不含
            // {0} 时行为不变（整行替换），避免翻译遮蔽排障所需信息。
            const auto line_tmpl = TranslatePluginKey(
                ("maapi.diagnostic." + diagnostic.code).c_str());
            const std::string& original_message = diagnostic.message;
            std::string        line;
            if (!line_tmpl.empty())
            {
                if (line_tmpl.find("{0}") != std::string::npos
                    && !original_message.empty())
                {
                    // make_format_args 持引用而非拥有：参数须是具名局部变量，
                    // 生命周期覆盖 vformat 调用（禁临时量）。
                    line = DAS_FMT_NS::vformat(
                        std::string_view(line_tmpl),
                        DAS_FMT_NS::make_format_args(original_message));
                }
                else
                {
                    line = line_tmpl;
                }
            }
            else
            {
                line = original_message;
            }
            if (diagnostic.provider_code)
            {
                line += DAS_FMT_NS::format(
                    " (provider_code={})",
                    *diagnostic.provider_code);
            }
            lines.push_back(
                {diagnostic.severity == "warning"
                     ? Das::ExportInterface::DAS_LOG_TYPE_WARNING
                     : Das::ExportInterface::DAS_LOG_TYPE_ERROR,
                 DAS_FMT_NS::format("[{}] {}", diagnostic.code, line)});
        }
        return lines;
    }

    void LogRunResult(const MaaRuntimeResult& result)
    {
        const auto lines = BuildRunLogLines(result);
        if (lines.empty())
        {
            return;
        }

        // 写 focus 事件日志（user.jsonl → UI 事件日志面板）：resolve 主进程
        // recorder（与 Run 内同款 C ABI 路径）。逐行建独立条目，severity
        // 保真（UI 按条目级别渲染 / 过滤）。Record 失败由 RecordEntryOrLog
        // 按行降级 [focus-degraded] 落 logs/DasCore.log，不静默。
        Das::DasPtr<Das::ExportInterface::IDasProcessLogRecorder> recorder;
        DasPtr<IDasBase>                                          resolved_base;
        const auto resolve_rc = DasQueryMainProcessInterfaceByName(
            "das.focus-log-recorder",
            resolved_base.Put());
        if (DAS::IsOk(resolve_rc) && resolved_base)
        {
            (void)resolved_base.As(recorder);
        }
        if (!recorder)
        {
            for (const auto& line : lines)
            {
                const std::string degraded =
                    DAS_FMT_NS::format("[focus-degraded] {}", line.text);
                switch (line.type)
                {
                case Das::ExportInterface::DAS_LOG_TYPE_ERROR:
                    DAS_LOG_ERROR(degraded.c_str());
                    break;
                case Das::ExportInterface::DAS_LOG_TYPE_WARNING:
                    DAS_LOG_WARNING(degraded.c_str());
                    break;
                default:
                    DAS_LOG_INFO(degraded.c_str());
                    break;
                }
            }
            DAS_LOG_ERROR(
                DAS_FMT_NS::format(
                    "[DasMaaPi] LogRunResult: focus recorder resolve failed, "
                    "result lines only in debug log (resolve_rc={})",
                    resolve_rc)
                    .c_str());
            return;
        }

        for (const auto& line : lines)
        {
            Das::DasPtr<Das::ExportInterface::IDasLogEntry> entry;
            if (DAS::IsFailed(CreateIDasLogEntry(entry.Put())) || !entry)
            {
                continue;
            }
            if (DAS::IsFailed(entry->SetType(line.type)))
            {
                continue;
            }
            DasU8StringOnStack text_string{line.text.c_str()};
            if (DAS::IsFailed(entry->AddText(&text_string)))
            {
                continue;
            }
            (void)RecordEntryOrLog(
                recorder.Get(),
                entry.Get(),
                line.type,
                line.text);
        }
    }
} // namespace Das::Plugins::DasMaaPi

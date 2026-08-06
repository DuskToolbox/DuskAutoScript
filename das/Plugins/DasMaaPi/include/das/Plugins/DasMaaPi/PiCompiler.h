#pragma once

#include <das/DasPtr.hpp>
#include <das/Plugins/DasMaaPi/AcceptedSettings.h>
#include <das/Plugins/DasMaaPi/ExecutionEnvelope.h>
#include <das/Plugins/DasMaaPi/PiCatalog.h>
#include <das/_autogen/idl/abi/DasJson.h>

namespace Das::Plugins::DasMaaPi
{
    // Forward decl — full definition in src/AdbDeviceFinder.h. Used as const
    // pointer in CompileMaapi / NormalizeControllerInitParams signatures so
    // the header compiles without pulling MaaToolkit includes here.
    struct AdbDeviceInfo;

    // device_cache 可选传入：编译期按 settings.pi.controller_adb_device 在
    // cache 中查六字段（Toolkit 推荐值），找到则覆盖 raw_json 解析结果。
    // 默认 nullptr = cache 不可用，完全用 raw_json（用户未扫描设备的
    // fallback）。
    CompileResultDto CompileMaapi(
        const AcceptedSettingsDto&        settings,
        const PiCatalog&                  catalog,
        std::string_view                  purpose,
        const std::vector<AdbDeviceInfo>* device_cache = nullptr);

    // Build typed controller init params from a PI controller's raw_json.
    // Reads win32/gamepad same-level keys (NOT the `config` sub-key) plus
    // display_short_side/long_side/raw. Returns a struct with empty typed
    // blocks for controller types that don't carry them (Adb/Dbg/MacOS/...).
    // ControllerSpec wire format is unaffected — callers still obtain the
    // wire-shaped spec via the existing CompileMaapi path.
    //
    // selected_adb_device 可选：CompileMaapi 已从 settings + device_cache 解析
    // 好的选中设备指针。非空时覆盖 adb 块的六字段（Toolkit 推荐值优先于
    // raw_json，符合"用户选了设备就用 Toolkit 探测的最新值"语义）。
    ControllerInitParams NormalizeControllerInitParams(
        const PiController&  controller,
        const AdbDeviceInfo* selected_adb_device = nullptr);

    // 把 CompileResultDto 序列化并直接构造为 IDasJson 句柄。原接口返回
    // yyjson::value，但 owning yyjson::value 经多次跨函数 move 后再 write
    // 时会出现 invalid UTF-8 错误（cpp_yyjson owning value move 链路问题，
    // 参见 cpp_yyjson-value-moved-from-null-deref.md）。在函数内立即 write
    // + parse 成 IDasJson 可避开 owning value 跨函数 move。
    Das::DasPtr<Das::ExportInterface::IDasJson> SerializeCompileResult(
        const CompileResultDto& result,
        std::string_view        purpose);
} // namespace Das::Plugins::DasMaaPi

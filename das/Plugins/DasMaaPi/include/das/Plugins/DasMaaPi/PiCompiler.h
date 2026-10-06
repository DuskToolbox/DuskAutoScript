#pragma once

#include <das/DasPtr.hpp>
#include <das/Plugins/DasMaaPi/AcceptedSettings.h>
#include <das/Plugins/DasMaaPi/AdbDeviceFinder.h>
#include <das/Plugins/DasMaaPi/ExecutionEnvelope.h>
#include <das/Plugins/DasMaaPi/PiCatalog.h>
#include <das/_autogen/idl/abi/DasJson.h>

namespace Das::Plugins::DasMaaPi
{
    // adb_enumerator 可选注入：默认空 = 生产 DefaultAdbDeviceEnumerator
    // （与 FindAllAdbDevices 既有「空=生产默认」的调用方解析约定一致），
    // 测试注入固定设备表驱动 0/1/N 覆盖。设备六字段装配只发生在
    // purpose=execution 编译（现扫、按地址唯一命中）；preview 零设备动作，
    // 编译路径不消费任何会话设备缓存。
    CompileResultDto CompileMaapi(
        const AcceptedSettingsDto& settings,
        const PiCatalog&           catalog,
        std::string_view           purpose,
        const AdbDeviceEnumerator& adb_enumerator = {});

    // Build typed controller init params from a PI controller's raw_json.
    // Reads win32/gamepad same-level keys (NOT the `config` sub-key) plus
    // display_short_side/long_side/raw. Returns a struct with empty typed
    // blocks for controller types that don't carry them (Adb/Dbg/MacOS/...).
    // ControllerSpec wire format is unaffected — callers still obtain the
    // wire-shaped spec via the existing CompileMaapi path.
    //
    // selected_adb_device 可选：CompileMaapi 已从活扫描解析好的选中设备
    // 指针。非空时覆盖 adb 块的六字段（Toolkit 探测的推荐值优先于
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

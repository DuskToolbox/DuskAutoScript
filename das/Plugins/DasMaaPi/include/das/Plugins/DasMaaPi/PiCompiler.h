#pragma once

#include <das/DasPtr.hpp>
#include <das/Plugins/DasMaaPi/AcceptedSettings.h>
#include <das/Plugins/DasMaaPi/ExecutionEnvelope.h>
#include <das/Plugins/DasMaaPi/PiCatalog.h>
#include <das/_autogen/idl/abi/DasJson.h>

namespace Das::Plugins::DasMaaPi
{
    CompileResultDto CompileMaapi(
        const AcceptedSettingsDto& settings,
        const PiCatalog&           catalog,
        std::string_view           purpose);

    // 把 CompileResultDto 序列化并直接构造为 IDasJson 句柄。原接口返回
    // yyjson::value，但 owning yyjson::value 经多次跨函数 move 后再 write
    // 时会出现 invalid UTF-8 错误（cpp_yyjson owning value move 链路问题，
    // 参见 cpp_yyjson-value-moved-from-null-deref.md）。在函数内立即 write
    // + parse 成 IDasJson 可避开 owning value 跨函数 move。
    Das::DasPtr<Das::ExportInterface::IDasJson> SerializeCompileResult(
        const CompileResultDto& result,
        std::string_view        purpose);
} // namespace Das::Plugins::DasMaaPi

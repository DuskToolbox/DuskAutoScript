#ifndef DAS_CORE_GRAPHRUNTIME_COMPILEDPLANFINGERPRINT_H
#define DAS_CORE_GRAPHRUNTIME_COMPILEDPLANFINGERPRINT_H

#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>

#include <cpp_yyjson.hpp>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Utils/DasJsonCore.h>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

// FNV-1a 64-bit —— 32 位同族先例为 IPC MethodMetadata.h 的
// ComputeInterfaceId（参数升至 64 位 prime/offset basis）。constexpr 计算，
// 无静态初始化顺序依赖。
constexpr std::uint64_t ComputeFnv1a64(std::string_view bytes) noexcept
{
    constexpr std::uint64_t FNV_PRIME = 0x100000001b3ULL;
    constexpr std::uint64_t FNV_OFFSET_BASIS = 0xcbf29ce484222325ULL;

    std::uint64_t hash_value = FNV_OFFSET_BASIS;
    for (char byte : bytes)
    {
        hash_value ^= static_cast<unsigned char>(byte);
        hash_value *= FNV_PRIME;
    }
    return hash_value;
}

// 指纹的十六进制形态：16 位小写、固定位宽（同一哈希恒同串，直接可比）。
inline std::string FormatFnv1a64Hex(std::uint64_t hash_value)
{
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";

    std::string result(16, '0');
    for (int position = 15; position >= 0; --position)
    {
        result[static_cast<std::size_t>(position)] =
            HEX_DIGITS[hash_value & 0xF];
        hash_value >>= 4;
    }
    return result;
}

// 对 plan 子树经标准序列化器产出的字符串计算 FNV-1a 64 指纹。编译侧
// （外壳产出）与执行侧（外壳校验）必须同经此函数，保证比对字节流构造性
// 一致。序列化失败返回空串（调用方按校验失败处理）。
inline std::string ComputeCompiledPlanFingerprint(const yyjson::value& plan)
{
    const auto serialized = Das::Utils::SerializeYyjsonValue(plan);
    if (!serialized.has_value())
    {
        return {};
    }
    return FormatFnv1a64Hex(ComputeFnv1a64(*serialized));
}

// 编译 wire 产物外壳：{"fingerprint": "<hex>", "plan": <CompiledGraphPlanDto
// 全量>}。执行侧解析外壳、取 plan 子树重算指纹比对——不等即产物被改或
// 损坏，拒绝执行。
inline yyjson::value BuildCompiledPlanShell(const yyjson::value& plan)
{
    yyjson::value shell(Das::Utils::MakeYyjsonObject());
    auto          shell_obj = shell.as_object();
    (*shell_obj)[std::string_view("fingerprint")] = std::make_pair(
        std::string_view(ComputeCompiledPlanFingerprint(plan)),
        yyjson::copy_string);
    (*shell_obj)[std::string_view("plan")] = Das::Utils::CloneYyjsonValue(plan);
    return shell;
}

DAS_CORE_GRAPHRUNTIME_NS_END

#endif // DAS_CORE_GRAPHRUNTIME_COMPILEDPLANFINGERPRINT_H

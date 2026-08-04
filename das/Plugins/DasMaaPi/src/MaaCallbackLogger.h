#pragma once

#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/Plugins/DasMaaPi/MaaApiBoundary.h>
#include <das/Utils/DasJsonCore.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Das::Plugins::DasMaaPi
{
    // Focus 模板解析结果（D-88-08）：content + display 渠道列表。字符串简写
    // 等价 channels=["log"]；对象完整写法读 content + display（string 或
    // string 数组）。Core 侧只把 channels 作为 fields.displayChannels 语义
    // 字段透传，不感知 Maa 协议。
    struct FocusTemplateResult
    {
        std::string content;
        std::vector<std::string>
            channels; // log/toast/notification/dialog/modal
    };

    // 截图获取的可注入 seam（RESEARCH A2 低置信）：默认返回空 DasPtr →
    // imageRefs 留空（Pitfall 4，截图取不到不阻塞 Record）；测试可注入假图。
    using FocusImageProvider =
        std::function<Das::DasPtr<Das::ExportInterface::IDasImage>()>;

    // 回调上下文：持有 recorder（复用）与截图 seam。生命周期由
    // MaaRuntime::Run 的 unique_ptr 管理——在 Run 中先于 resource /
    // controller / tasker 三个 sink owner 声明，逆序析构保证三者析构期间
    // trans_arg 持续有效（任一 Maa*Destroy 内部冲刷 final 回调时仍可安全
    // 解引用）。
    struct MaaCallbackLoggerContext
    {
        Das::DasPtr<Das::ExportInterface::IDasProcessLogRecorder> recorder;
        FocusImageProvider image_provider;
    };

    // 解析 details_json 的 focus[message]：字符串简写 / 对象完整写法 / display
    // 渠道数组。message 不在 focus 内或结构非法返回 nullopt。
    std::optional<FocusTemplateResult> ParseFocusTemplate(
        const yyjson::value& details,
        std::string_view     message);

    // /\{(\w+)\}/g 占位符替换（照搬 MXU useMaaCallbackLogger.ts）：{image}
    // 保留由截图通路单独处理，其余从 details 取 String(value)，字段缺失用
    // 占位符原样保留（T-88-11，转义由 yyjson 序列化兜底）。
    std::string ReplaceFocusPlaceholders(
        std::string_view     text,
        const yyjson::value& details);

    // display 渠道数组 → JSON 数组字符串（如 ["log","toast"]），供
    // SetField("displayChannels", ...)（D-88-08）。
    std::string FormatDisplayChannelsJson(
        const std::vector<std::string>& channels);

    // MaaEventCallback 转发入口（trans_arg = MaaCallbackLoggerContext*）。
    // 以 DAS_MAA_CALL（__stdcall on Windows）匹配 MaaFW 回调约定。
    void DAS_MAA_CALL MaaCallbackLoggerEntry(
        void*       handle,
        const char* message,
        const char* details_json,
        void*       trans_arg);

    // 处理一条回调消息（可单测）：解析 focus → 占位符替换 → 截图 seam →
    // 组装 FOCUS 条目（SetType FOCUS + AddText + AddImage + SetField
    // displayChannels）→ Record。
    void LogFocusMessage(
        MaaCallbackLoggerContext& context,
        const char*               message,
        const char*               details_json);
} // namespace Das::Plugins::DasMaaPi

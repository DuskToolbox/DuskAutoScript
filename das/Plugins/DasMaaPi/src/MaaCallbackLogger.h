#pragma once

#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/Plugins/DasMaaPi/MaaApiBoundary.h>
#include <das/Utils/DasJsonCore.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Das::Plugins::DasMaaPi
{
    // Focus 模板解析结果：content + display 渠道列表。字符串简写
    // 等价 channels=["log"]；对象完整写法读 content + display（string 或
    // string 数组）。Core 侧只把 channels 作为 fields.displayChannels 语义
    // 字段透传，不感知 Maa 协议。
    struct FocusTemplateResult
    {
        std::string content;
        std::vector<std::string>
            channels; // log/toast/notification/dialog/modal
    };

    // 截图获取的可注入 seam：默认返回空 DasPtr → imageRefs 留空
    // （截图取不到不阻塞 Record）；测试可注入假图。
    using FocusImageProvider =
        std::function<Das::DasPtr<Das::ExportInterface::IDasImage>()>;

    // focus content 模板解析所需的 i18n translations / 文件路径基 /
    // HTTP 加载器 / 当前 UI 语言。PiCompiler 经 envelope 透传 translations
    // 文件路径，MaaRuntime 启动时一次性读取填 translations map，回调线程
    // 经 ResolveFocusContent 查表（微秒级 in-memory lookup）。http_client
    // 为非 owning 指针，由 MaaRuntime::Run 栈上 owner 保活。
    class PiHttpClient;

    struct MaaCallbackLoggerContext
    {
        Das::DasPtr<Das::ExportInterface::IDasProcessLogRecorder> recorder;
        FocusImageProvider image_provider;
        // i18n key → 翻译文本（key/value 均 std::string）。来自
        // envelope.maapi.focus_translations_path 指向的 JSON 文件，MaaRuntime
        // 启动时一次性读取填充。
        std::unordered_map<std::string, std::string> translations;
        // focus content 文件路径解析基（来自
        // envelope.maapi.interface_directory）。 命名对齐
        // MaaExecutionPlanDto::interface_directory，禁缩写。
        std::filesystem::path interface_directory;
        // HTTP 加载器（BeastHttpClient 注入，非 owning）。MaaRuntime::Run 栈上
        // unique_ptr<PiHttpClient> owner 必须在 log_context 之前声明，逆序
        // 析构保证 owner 在 log_context 之后销毁——final 回调冲刷期间裸指针
        // 持续有效（生命周期不变量）。
        PiHttpClient* http_client = nullptr;
        // 当前 UI 语言代码（如 "zh_cn"），来自
        // envelope.maapi.pi_env.client_language（PiCompiler 经 SelectPiLanguage
        // 选择结果注入）。
        std::string language;
    };

    // 解析 details_json 的 focus[message]：字符串简写 / 对象完整写法 / display
    // 渠道数组。message 不在 focus 内或结构非法返回 nullopt。
    std::optional<FocusTemplateResult> ParseFocusTemplate(
        const yyjson::value& details,
        std::string_view     message);

    // /\{(\w+)\}/g 占位符替换：{image} 保留由截图通路单独处理，其余从
    // details 取 String(value)，字段缺失用占位符原样保留（转义由 yyjson
    // 序列化兜底）。
    std::string ReplaceFocusPlaceholders(
        std::string_view     text,
        const yyjson::value& details);

    // focus content 模板解析（i18n `$key` → URL → file → literal 四级
    // 优先级，镜像协议规范「文件路径 / URL / 直接文本 / `$key` i18n」四类源）。
    // 当前实现 i18n `$key` 与 literal 两分支；URL/file 分支后续补全（函数
    // 签名固定不改）。任何解析失败均降级为非空字符串（去 `$` 前缀的 key 或
    // 原文），绝不返回空、不抛异常、不阻塞 MaaFW 回调线程。
    std::string ResolveFocusContent(
        std::string_view                content,
        const MaaCallbackLoggerContext& context);

    // display 渠道数组 → JSON 数组字符串（如 ["log","toast"]），供
    // SetField("displayChannels", ...)。
    std::string FormatDisplayChannelsJson(
        const std::vector<std::string>& channels);

    // MaaEventCallback 转发入口（trans_arg = MaaCallbackLoggerContext*）。
    // 以 DAS_MAA_CALL（__stdcall on Windows）匹配 MaaFW 回调约定。
    void DAS_MAA_CALL MaaCallbackLoggerEntry(
        void*       handle,
        const char* message,
        const char* details_json,
        void*       trans_arg);

    // 处理一条回调消息（可单测）：解析 focus → ResolveFocusContent 解析
    // `$key`/URL/file/字面 → 占位符替换 → 截图 seam → 组装 FOCUS 条目
    // （SetType FOCUS + AddText + AddImage + SetField displayChannels）→
    // Record。
    void LogFocusMessage(
        MaaCallbackLoggerContext& context,
        const char*               message,
        const char*               details_json);
} // namespace Das::Plugins::DasMaaPi

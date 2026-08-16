#include <das/Plugins/DasMaaPi/AuthoringProjector.h>

#include "DasProgressReporter.h"
#include "I18nResolver.h"
#include "MaaDynamicSymbols.h"
#include "MaapiAuthoringSession.h"
#include "PiHttpClient.h"
#include "PiUpdateService.h"
#include "UpdateJobRegistry.h"
#include <das/Plugins/DasMaaPi/AdbDeviceFinder.h>

#ifndef DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY
#include <MaaFramework/MaaAPI.h>
#endif

#include <das/Core/IPC/CurrentIpcContextScope.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/Plugins/DasMaaPi/MaaPiErrorCodes.h>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/StringUtils.h>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/abi/IDasI18n.h>

#include <boost/dll/runtime_symbol_info.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace Das::Plugins::DasMaaPi
{
    namespace
    {
        yyjson::value Object() { return Das::Utils::MakeYyjsonObject(); }
        yyjson::value Array() { return Das::Utils::MakeYyjsonArray(); }
        yyjson::value JsonString(std::string_view value)
        {
            return yyjson::value(std::string(value));
        }

        // ResolveI18n has been extracted to src/I18nResolver.h/.cpp so that
        // PiCompiler can share the same `$key → catalog.translations[lang]`
        // resolution. The strict variant below (ResolveI18nOrDiag) remains
        // projection-local because its diagnostic-emitting contract is
        // specific to field-label projection.

        // host i18n（DasMaaPi 自有 UI 文案翻译表）由 GetAuthoringHostI18n()
        // 函数内 static cache 持有；无文件级全局状态。

        // boost::filesystem::path（this_line_location 返回）→ UTF-8
        // std::string。 boost path 没有 u8string()；.string() 在 Windows 是 ACP
        // narrow（中文 路径乱码）。经 std::filesystem::path 桥接：Windows 走
        // wstring（UTF-16）， Unix 走 string（native narrow），再 u8string() 出
        // UTF-8 字节。
        std::string BoostPathToUtf8(const boost::filesystem::path& boost_path)
        {
#ifdef _WIN32
            const std::filesystem::path std_path{boost_path.wstring()};
#else
            const std::filesystem::path std_path{boost_path.string()};
#endif
            return std::string{DAS::Utils::U8AsString(std_path.u8string())};
        }

        // 取 host i18n 翻译表（DasMaaPi 自有 UI 文案）。函数内 static cache：
        // 首次调用经 lambda 从 <plugin_dir>/i18n 装载（this_line_location 推导
        // 模块路径），magic statics 保证初始化线程安全；mutex 保护访问。
        // 返回 DasPtr（AddRef）——调用方持有独立引用计数，与 cache 解耦。
        // 目录缺失/装载失败 cache 为空，Translate 降级返回 key 本身。
        DasPtr<Das::ExportInterface::IDasI18n> GetAuthoringHostI18n()
        {
            static const DasPtr<Das::ExportInterface::IDasI18n> cache =
                []() -> DasPtr<Das::ExportInterface::IDasI18n>
            {
                try
                {
                    const auto plugin_module = boost::dll::this_line_location();
                    const auto boost_i18n_dir =
                        plugin_module.parent_path() / "i18n";
                    const auto dir_utf8 = BoostPathToUtf8(boost_i18n_dir);
                    DasPtr<IDasReadOnlyString> dir_str;
                    if (DAS::IsFailed(
                            ::CreateIDasReadOnlyStringFromUtf8(
                                dir_utf8.c_str(),
                                dir_str.Put())))
                    {
                        return {};
                    }
                    DasPtr<Das::ExportInterface::IDasI18n> i18n;
                    if (DAS::IsFailed(
                            ::CreateIDasI18nFromDirectory(
                                dir_str.Get(),
                                i18n.Put()))
                        || i18n == nullptr)
                    {
                        return {};
                    }
                    return i18n;
                }
                catch (...)
                {
                    return {};
                }
            }();
            static std::mutex                 cache_mutex;
            const std::lock_guard<std::mutex> lock(cache_mutex);
            return cache;
        }

        // 经 host i18n 翻译 key；i18n 为空 / key miss 返回空串（调用方回退
        // 硬编码）。镜像 DasProgressReporter 的 TranslateKey。
        std::string TranslateHostKey(
            Das::ExportInterface::IDasI18n* i18n,
            std::string_view                key)
        {
            if (i18n == nullptr)
            {
                return {};
            }
            const std::string          key_str(key);
            DasPtr<IDasReadOnlyString> key_param;
            if (DAS::IsFailed(
                    ::CreateIDasReadOnlyStringFromUtf8(
                        key_str.c_str(),
                        key_param.Put())))
            {
                return {};
            }
            DasPtr<IDasReadOnlyString> text;
            if (DAS::IsFailed(i18n->Translate(key_param.Get(), text.Put())))
            {
                return {};
            }
            const char* raw = nullptr;
            if (text == nullptr || DAS::IsFailed(text->GetUtf8(&raw))
                || raw == nullptr)
            {
                return {};
            }
            return std::string(raw);
        }

        // 翻译 DAS host i18n 字符串（DasMaaPi 自有 UI 文案）。无额外实参 → 纯
        // 查表；带实参 → 把模板里的 {0},{1}… 按位置用 std::format 填充。
        // 缺 key 返回 key 本身（翻译缺失显性可见，不硬编码兜底）。对仗 MXU
        // 的 t(key) / t(key, params)。与 ResolveI18nOrDiag（MaaPI 作者内容
        // $key 解析）严格分离，二者各司其职。
        template <typename... Args>
        std::string Translate(std::string_view key, const Args&... args)
        {
            const DasPtr<Das::ExportInterface::IDasI18n> i18n =
                GetAuthoringHostI18n();
            std::string text = TranslateHostKey(i18n.Get(), key);
            if (text.empty())
            {
                text = std::string(key); // 缺译 → 返回 key 本身，让缺失暴露
            }
            if constexpr (sizeof...(args) == 0)
            {
                return text;
            }
            return DAS_FMT_NS::vformat(
                std::string_view(text),
                DAS_FMT_NS::make_format_args(args...));
        }

        // D-84-23: ResolveI18n 的严格化变体。当 $key 在当前 language 找不到
        // 翻译时，返回调用方提供的 fallback（通常是 PI 协议层面的用户可见
        // 标识，如 candidate.value 或 option.dto.name，而不是 $key / 机器
        // name / 'Language/Preset/Resource' 等协议英文），并向 projection
        // diagnostics 输出一条 warning。fallback 必须非空，保证 84-01 严格
        // FieldDescriptor/CandidateDescriptor label 校验通过。
        // 本函数只做 MaaPI 作者内容的 $key → PI catalog 解析，不查 DAS host
        // i18n：DAS 自有 UI 文案（chrome label）走 Translate，二者各司其职。
        // 唯一经 host i18n 的是 emit 的 diagnostic 文案（见下 Translate
        // 调用）。
        std::string_view ResolveI18nOrDiag(
            std::string_view              text,
            const PiCatalog&              catalog,
            std::string_view              lang,
            std::string_view              fallback,
            std::vector<PiDiagnosticDto>* projection_diagnostics,
            std::string_view              context_path)
        {
            if (text.empty() || !text.starts_with('$'))
            {
                return text.empty() ? fallback : text;
            }

            const std::string_view key = text.substr(1);
            auto lang_it = catalog.translations.find(std::string(lang));
            if (lang_it != catalog.translations.end())
            {
                auto key_it = lang_it->second.find(std::string(key));
                if (key_it != lang_it->second.end())
                {
                    return std::string_view(key_it->second);
                }
            }

            // 缺译：emit diagnostic + 返回 fallback。fallback 是 caller 提供
            // 的 PI 用户可见标识（value / dto.name），不是 $key
            // 也不是协议英文。
            if (projection_diagnostics != nullptr)
            {
                PiDiagnosticDto diag;
                diag.severity = "warning";
                diag.message =
                    Translate("diagnostic.missingTranslation", key, lang);
                if (!context_path.empty())
                {
                    diag.path = std::string(context_path);
                }
                projection_diagnostics->push_back(std::move(diag));
            }
            return fallback;
        }

        template <typename TObject>
        std::optional<std::string> OptionalString(
            const TObject&   obj,
            std::string_view key)
        {
            if (!obj.contains(key) || !obj[key].is_string())
            {
                return std::nullopt;
            }
            return std::string(obj[key].as_string().value_or(""));
        }

        // D-84-47: 把 PI descriptor.icon 解析成 84-01 严格 IconString 形态。
        // 接受 http(s):// 与绝对本地路径（Windows 盘符 / POSIX / UNC）原样
        // 保留；相对路径基于 catalog.interface_directory 解析为绝对路径；
        // data: URI 与空字符串原样返回（caller 决定是否 emit；84-01 验证器
        // 会拒绝 data:，故 caller 应仅在合法形态下 emit icon）。
        std::string ResolveIcon(std::string_view icon, const PiCatalog* catalog)
        {
            if (icon.empty())
            {
                return {};
            }
            if (icon.starts_with("http://") || icon.starts_with("https://"))
            {
                return std::string(icon);
            }
            if (icon.starts_with("data:"))
            {
                return {};
            }
            const bool is_absolute =
                (icon.size() >= 3
                 && std::isalpha(static_cast<unsigned char>(icon[0])) != 0
                 && icon[1] == ':' && (icon[2] == '/' || icon[2] == '\\'))
                || icon.starts_with('/') || icon.starts_with("\\\\");
            if (is_absolute)
            {
                return std::string(icon);
            }
            if (catalog && !catalog->interface_directory.empty())
            {
                const std::filesystem::path base(catalog->interface_directory);
                const std::filesystem::path leaf(icon);
                auto                        combined = base / leaf;
                return combined.generic_string();
            }
            return std::string(icon);
        }

        template <typename TObject>
        bool
        OptionalBool(const TObject& obj, std::string_view key, bool fallback)
        {
            if (!obj.contains(key) || !obj[key].is_bool())
            {
                return fallback;
            }
            return obj[key].as_bool().value_or(fallback);
        }

        // schedule_interval_minutes / schedule_daily_catch_up 等可选字段的
        // optional<T> 形态：engaged 状态需要保留（缺失→nullopt 而非默认值），
        // 与既有三参 OptionalBool（fallback→bool）语义不同。
        template <typename TObject>
        std::optional<bool> OptionalBool(
            const TObject&   obj,
            std::string_view key)
        {
            if (!obj.contains(key) || !obj[key].is_bool())
            {
                return std::nullopt;
            }
            return obj[key].as_bool().value_or(false);
        }

        template <typename TObject>
        std::optional<int> OptionalInt(const TObject& obj, std::string_view key)
        {
            if (!obj.contains(key) || !obj[key].is_sint())
            {
                return std::nullopt;
            }
            return obj[key].as_sint().value_or(0);
        }

        template <typename TObject>
        std::vector<std::string> StringArray(
            const TObject&   obj,
            std::string_view key);

        std::vector<MaapiPiOptionSettingsDto> ParseOptionSettingsArray(
            const auto&      obj,
            std::string_view key)
        {
            std::vector<MaapiPiOptionSettingsDto> result;
            if (!obj.contains(key))
            {
                return result;
            }
            auto arr = obj[key].as_array();
            if (!arr)
            {
                return result;
            }
            for (auto it = arr->begin(); it != arr->end(); ++it)
            {
                auto option_obj = it->as_object();
                if (!option_obj)
                {
                    continue;
                }
                MaapiPiOptionSettingsDto option;
                option.option_name =
                    OptionalString(*option_obj, "optionName").value_or("");
                option.kind = OptionalString(*option_obj, "kind").value_or("");
                option.selected_cases =
                    StringArray(*option_obj, "selectedCases");
                if (option_obj->contains(std::string_view("boolValue"))
                    && (*option_obj)[std::string_view("boolValue")].is_bool())
                {
                    option.bool_value =
                        (*option_obj)[std::string_view("boolValue")]
                            .as_bool()
                            .value_or(false);
                }
                if (option_obj->contains(std::string_view("inputValues")))
                {
                    if (auto inputs =
                            (*option_obj)[std::string_view("inputValues")]
                                .as_object())
                    {
                        for (auto input_it = inputs->begin();
                             input_it != inputs->end();
                             ++input_it)
                        {
                            if (input_it->second.is_string())
                            {
                                option.input_values.emplace(
                                    input_it->first,
                                    input_it->second.as_string().value_or(""));
                            }
                        }
                    }
                }
                if (!option.option_name.empty())
                {
                    result.emplace_back(std::move(option));
                }
            }
            return result;
        }

        std::vector<MaapiPiTaskSettingsDto> ParseTaskSettingsArray(
            const auto&      obj,
            std::string_view key)
        {
            std::vector<MaapiPiTaskSettingsDto> result;
            if (!obj.contains(key))
            {
                return result;
            }
            auto arr = obj[key].as_array();
            if (!arr)
            {
                return result;
            }
            for (auto it = arr->begin(); it != arr->end(); ++it)
            {
                auto task_obj = it->as_object();
                if (!task_obj)
                {
                    continue;
                }
                MaapiPiTaskSettingsDto task;
                task.item_id = OptionalString(*task_obj, "itemId").value_or("");
                task.task_name =
                    OptionalString(*task_obj, "taskName").value_or("");
                task.enabled = OptionalBool(*task_obj, "enabled", true);
                task.options = ParseOptionSettingsArray(*task_obj, "options");
                if (!task.task_name.empty())
                {
                    result.emplace_back(std::move(task));
                }
            }
            return result;
        }

        template <typename TObject>
        std::vector<std::string> StringArray(
            const TObject&   obj,
            std::string_view key)
        {
            std::vector<std::string> result;
            if (!obj.contains(key))
            {
                return result;
            }
            auto arr = obj[key].as_array();
            if (!arr)
            {
                return result;
            }
            for (auto it = arr->begin(); it != arr->end(); ++it)
            {
                if (it->is_string())
                {
                    result.emplace_back(it->as_string().value_or(""));
                }
            }
            return result;
        }

        // D-84-40 前向声明：本匿名命名空间内使用的序列化辅助。
        yyjson::value OptionSettingsArray(
            const std::vector<MaapiPiOptionSettingsDto>& options);
        // D-84.2-04: customPresets parse 辅助前向声明（定义在本命名空间后段）。
        template <typename TObject>
        std::vector<MaapiPresetSnapshotDto> ParseCustomPresetsArray(
            const TObject&   obj,
            std::string_view key);
        // D-86-03/06: pretasks parse 辅助前向声明（定义在本命名空间后段）。
        template <typename TObject>
        std::vector<MaapiPiPretaskSettingsDto> ParsePretaskSettingsArray(
            const TObject&   obj,
            std::string_view key);

        template <typename TObject>
        AcceptedSettingsDto ParseAcceptedObject(const TObject& obj)
        {
            AcceptedSettingsDto settings;
            if (obj.contains(std::string_view("adapter")))
            {
                if (auto adapter = obj[std::string_view("adapter")].as_object())
                {
                    settings.adapter.interface_path =
                        OptionalString(*adapter, "interfacePath");
                    settings.adapter.project_root =
                        OptionalString(*adapter, "projectRoot");
                    settings.adapter.proxy_url =
                        OptionalString(*adapter, "proxyUrl");
                    settings.adapter.mirrorchyan_cdk =
                        OptionalString(*adapter, "mirrorchyanCdk");
                    settings.adapter.update_channel =
                        OptionalString(*adapter, "updateChannel");
                    settings.adapter.source_fingerprint =
                        OptionalString(*adapter, "sourceFingerprint");
                    if (adapter->contains(std::string_view("executionPolicy")))
                    {
                        if (auto policy =
                                (*adapter)[std::string_view("executionPolicy")]
                                    .as_object())
                        {
                            settings.adapter.execution_policy.fail_fast =
                                OptionalBool(*policy, "failFast", true);
                        }
                    }
                }
            }

            if (obj.contains(std::string_view("pi")))
            {
                if (auto pi = obj[std::string_view("pi")].as_object())
                {
                    settings.pi.controller_name =
                        OptionalString(*pi, "controllerName");
                    settings.pi.resource_name =
                        OptionalString(*pi, "resourceName");
                    settings.pi.preset_name = OptionalString(*pi, "presetName");
                    settings.pi.language = OptionalString(*pi, "language");
                    settings.pi.controller_adb_device =
                        OptionalString(*pi, "controllerAdbDevice");
                    settings.pi.orphan_paths = StringArray(*pi, "orphanPaths");
                    // schedule 配置：与 SerializeAcceptedSettings 对称——4 字段
                    // 缺失时保持 nullopt（schedule off 契约）。
                    settings.pi.schedule_kind =
                        OptionalString(*pi, "scheduleKind");
                    settings.pi.schedule_interval_minutes =
                        OptionalInt(*pi, "scheduleIntervalMinutes");
                    settings.pi.schedule_daily_time =
                        OptionalString(*pi, "scheduleDailyTime");
                    settings.pi.schedule_daily_catch_up =
                        OptionalBool(*pi, "scheduleDailyCatchUp");
                    settings.pi.global_options =
                        ParseOptionSettingsArray(*pi, "globalOptions");
                    settings.pi.resource_options =
                        ParseOptionSettingsArray(*pi, "resourceOptions");
                    settings.pi.controller_options =
                        ParseOptionSettingsArray(*pi, "controllerOptions");
                    settings.pi.tasks = ParseTaskSettingsArray(*pi, "tasks");
                    // D-84.2-04: customPresets 是 Provider 私有 preset 快照
                    // 存储。缺失时返回空 vector（flat pi 形态）。
                    settings.pi.custom_presets =
                        ParseCustomPresetsArray(*pi, "customPresets");
                    // D-86-03/06: pretasks 是 Provider 私有的 pretask 选中值
                    // 存储。缺失时返回空 vector。
                    settings.pi.pretasks =
                        ParsePretaskSettingsArray(*pi, "pretasks");
                }
            }

            // ui（通用 kv：视图偏好 + 插件自定义）。缺失时留空 map；不参与
            // PI 业务语义/编译，Get 时前端读 values 自行合成 UI 状态。
            if (obj.contains(std::string_view("ui")))
            {
                if (auto ui = obj[std::string_view("ui")].as_object())
                {
                    if (ui->contains(std::string_view("values")))
                    {
                        if (auto vals =
                                (*ui)[std::string_view("values")].as_object())
                        {
                            for (const auto& [k, v] : *vals)
                            {
                                settings.ui.values[std::string(k)] =
                                    Das::Utils::CloneYyjsonValue(v);
                            }
                        }
                    }
                }
            }
            return settings;
        }

        // Tracer 4 会重新启用对 envelope diagnostics 的 orphan_paths / 缺译
        // 警告投影；在此之前 ProjectAuthoringDocument 不再向 document 输出
        // 任何 diagnostics（D-84-11：diagnostics 与 document 同级，位于
        // envelope）。保留 helper 以便后续 tracer 直接复用。
        [[maybe_unused]]
        void AddDiagnosticJson(yyjson::value& arr, const PiDiagnosticDto& item)
        {
            yyjson::value diag(Object());
            auto          obj = diag.as_object();
            (*obj)[std::string_view("severity")] = JsonString(item.severity);
            (*obj)[std::string_view("code")] = JsonString(item.code);
            (*obj)[std::string_view("message")] = JsonString(item.message);
            if (item.path)
            {
                (*obj)[std::string_view("path")] = JsonString(*item.path);
            }
            if (item.source)
            {
                (*obj)[std::string_view("source")] = JsonString(*item.source);
            }
            arr.as_array()->emplace_back(std::move(diag));
        }

        yyjson::value CandidateArray(
            const auto&                   items,
            const PiCatalog*              catalog,
            std::string_view              language,
            std::vector<PiDiagnosticDto>* projection_diagnostics)
        {
            yyjson::value arr(Array());
            for (const auto& item : items)
            {
                yyjson::value candidate(Object());
                auto          obj = candidate.as_object();
                // D-84-21: CandidateDescriptor 严格 {value, label, icon?}。
                // value 是 setValue 写回时使用的非 null 标量；label 是
                // Provider 已本地化的显示文本。candidate.value 取
                // controller/resource/preset 的 dto.name (PI 唯一标识)，
                // candidate.label 取 i18n 解析后的本地化名称。
                (*obj)[std::string_view("value")] = JsonString(item.dto.name);
                std::string_view label = item.dto.name;
                if (item.dto.label)
                {
                    // D-84-23: 缺译时回退到 dto.name（PI 用户可见标识），
                    // 不泄漏 $key / 协议英文；同时向 projection_diagnostics
                    // 报告 warning 便于开发者补译。
                    label = ResolveI18nOrDiag(
                        *item.dto.label,
                        *catalog,
                        language,
                        item.dto.name,
                        projection_diagnostics,
                        item.dto.name);
                }
                (*obj)[std::string_view("label")] = JsonString(label);
                // D-84-47: 候选 icon 解析为绝对路径或 http(s)；不合法则不
                // emit。
                if (item.dto.icon)
                {
                    auto resolved = ResolveIcon(*item.dto.icon, catalog);
                    if (!resolved.empty())
                    {
                        (*obj)[std::string_view("icon")] = JsonString(resolved);
                    }
                }
                arr.as_array()->emplace_back(std::move(candidate));
            }
            return arr;
        }

        yyjson::value MakePropertyField(
            std::string_view     name,
            std::string_view     label,
            int                  value_type,
            int                  input_type,
            const yyjson::value& value,
            const yyjson::value* candidates = nullptr,
            bool                 readonly = false)
        {
            yyjson::value field(Object());
            auto          obj = field.as_object();
            (*obj)[std::string_view("name")] = JsonString(name);
            (*obj)[std::string_view("label")] = JsonString(label);
            (*obj)[std::string_view("valueType")] = value_type;
            (*obj)[std::string_view("inputType")] = input_type;
            (*obj)[std::string_view("value")] =
                Das::Utils::CloneYyjsonValue(value);
            if (candidates != nullptr)
            {
                (*obj)[std::string_view("candidates")] =
                    Das::Utils::CloneYyjsonValue(*candidates);
            }
            if (readonly)
            {
                (*obj)[std::string_view("readonly")] = true;
            }
            return field;
        }

        // PropertySchema valueType/inputType mapping from PiOption.dto.type.
        // select→(4,1); checkbox→(32,2); switch→(8,0); input→pipeline_type
        // int→(0,0)/float|double→(2,0)/else→(4,0); fallback→(4,0).
        // input 多字段 → (16,0) jsonObject：单标量装不下 N 个命名值，
        // value 形态为 {inputName: value} 对象（DAS-95 方向 A）。
        struct PropertyTypeMapping
        {
            int value_type;
            int input_type;
        };

        PropertyTypeMapping MapOptionType(const PiOptionDto& dto)
        {
            if (dto.type == "select")
            {
                return {4, 1};
            }
            if (dto.type == "checkbox")
            {
                return {32, 2};
            }
            if (dto.type == "switch")
            {
                return {8, 0};
            }
            if (dto.type == "input")
            {
                if (dto.inputs.size() > 1)
                {
                    return {16, 0};
                }
                if (!dto.inputs.empty() && dto.inputs.front().pipeline_type)
                {
                    const auto& pt = *dto.inputs.front().pipeline_type;
                    if (pt == "int")
                    {
                        return {0, 0};
                    }
                    if (pt == "float" || pt == "double")
                    {
                        return {2, 0};
                    }
                }
            }
            return {4, 0};
        }

        yyjson::value OptionCandidates(
            const PiOptionDto& dto,
            const PiCatalog*   catalog,
            std::string_view   language)
        {
            yyjson::value arr(Array());
            for (const auto& item : dto.cases)
            {
                yyjson::value candidate(Object());
                auto          obj = candidate.as_object();
                // D-84-21: CandidateDescriptor 严格 {value, label, icon?}。
                // value 是 PI case 的 dto.name (setValue 写回值)，label 经
                // i18n 解析；缺 label 时回退到 case.name 本身（case.name
                // 是 PI 协议标识，不是机器回退 — 它已经是用户可读的最终值
                // 之一，与 controller/resource 的回退同构）。
                (*obj)[std::string_view("value")] = JsonString(item.name);
                std::string_view label = item.name;
                if (item.label)
                {
                    label = ResolveI18n(*item.label, *catalog, language);
                }
                (*obj)[std::string_view("label")] = JsonString(label);
                if (item.icon)
                {
                    auto resolved = ResolveIcon(*item.icon, catalog);
                    if (!resolved.empty())
                    {
                        (*obj)[std::string_view("icon")] = JsonString(resolved);
                    }
                }
                arr.as_array()->emplace_back(std::move(candidate));
            }
            return arr;
        }

        // Build a PropertySchema field for a pi option, projecting from catalog
        // dto and seeding value from the current option settings (if any).
        //
        // DAS-95 方向 A：value 按 dto.type 翻译成 IR 标量（对齐 valueType
        // 声明）， 不再泄漏 MAAPI 内部 DTO {selectedCases, boolValue,
        // inputValues}。
        // - switch    → bool
        // - select    → string（取 selected_cases[0]，MAPI select 单选）
        // - checkbox  → jsonArray[string]
        // - input 单字段 → 按 pipeline_type 转 int/float/string 标量
        // - input 多字段 → jsonObject{inputName: value}
        yyjson::value MakeOptionPropertyField(
            std::string_view                field_name,
            const PiOptionDto&              dto,
            const MaapiPiOptionSettingsDto* current,
            const PiCatalog*                catalog,
            std::string_view                language)
        {
            auto mapping = MapOptionType(dto);

            // UAT fix: PI 接口定义的 default 值必须投影到 FieldDescriptor.value
            // 即使 acceptedProperties 未初始化该 option (current == nullptr)。
            // PI 协议层面 default_case (select/checkbox) / 首个 case (select 无
            // default_case) / switch 默认 true (MaaPI 约定，见
            // InitializeOptionDefaults) / input.default_value (input) 都是
            // 静态 schema 的一部分，不应依赖 acceptedProperties 是否已注入。
            // 用户运行时值 (current != nullptr) 优先于 PI default。
            const yyjson::value value = [&]() -> yyjson::value
            {
                if (dto.type == "switch")
                {
                    if (current && current->bool_value)
                    {
                        return yyjson::value(*current->bool_value);
                    }
                    // MaaPI 约定：switch 默认 true (InitializeOptionDefaults)。
                    return yyjson::value(true);
                }
                if (dto.type == "select" || dto.type.empty())
                {
                    if (current && !current->selected_cases.empty())
                    {
                        return JsonString(current->selected_cases.front());
                    }
                    // PI default_case 优先；缺失则回退首个 case。
                    if (!dto.default_cases.empty())
                    {
                        return JsonString(dto.default_cases.front());
                    }
                    if (!dto.cases.empty())
                    {
                        return JsonString(dto.cases.front().name);
                    }
                    return JsonString(std::string_view{});
                }
                if (dto.type == "checkbox")
                {
                    yyjson::value                   arr(Array());
                    const std::vector<std::string>& src =
                        current != nullptr ? current->selected_cases
                                           : dto.default_cases;
                    for (const auto& selected_case : src)
                    {
                        arr.as_array()->emplace_back(JsonString(selected_case));
                    }
                    return arr;
                }
                if (dto.type == "input")
                {
                    if (dto.inputs.size() > 1)
                    {
                        yyjson::value obj(Object());
                        auto          o = obj.as_object();
                        if (current)
                        {
                            for (const auto& [key, input_value] :
                                 current->input_values)
                            {
                                (*o)[std::string_view(key)] =
                                    JsonString(input_value);
                            }
                        }
                        else
                        {
                            // current 缺省时回退每个 input 的 PI
                            // default_value。
                            for (const auto& input : dto.inputs)
                            {
                                if (input.default_value)
                                {
                                    (*o)[std::string_view(input.name)] =
                                        JsonString(*input.default_value);
                                }
                            }
                        }
                        return obj;
                    }
                    const std::string input_name =
                        dto.inputs.empty() ? std::string{}
                                           : dto.inputs.front().name;
                    std::string raw;
                    if (current)
                    {
                        auto it = current->input_values.find(input_name);
                        if (it != current->input_values.end())
                        {
                            raw = it->second;
                        }
                    }
                    if (raw.empty() && !dto.inputs.empty()
                        && dto.inputs.front().default_value)
                    {
                        raw = *dto.inputs.front().default_value;
                    }
                    const std::string pt =
                        (!dto.inputs.empty()
                         && dto.inputs.front().pipeline_type)
                            ? *dto.inputs.front().pipeline_type
                            : std::string{};
                    if (pt == "int")
                    {
                        try
                        {
                            return yyjson::value(
                                static_cast<std::int64_t>(std::stoll(raw)));
                        }
                        catch (...)
                        {
                            return yyjson::value(static_cast<std::int64_t>(0));
                        }
                    }
                    if (pt == "float" || pt == "double")
                    {
                        try
                        {
                            return yyjson::value(std::stod(raw));
                        }
                        catch (...)
                        {
                            return yyjson::value(0.0);
                        }
                    }
                    return JsonString(raw);
                }
                return yyjson::value(nullptr);
            }();

            yyjson::value        candidates;
            const yyjson::value* candidates_ptr = nullptr;
            if (mapping.input_type == 1 || mapping.input_type == 2)
            {
                candidates = OptionCandidates(dto, catalog, language);
                candidates_ptr = &candidates;
            }
            const std::string_view label =
                dto.label ? *dto.label : std::string_view(dto.name);
            const std::string_view resolved_label =
                ResolveI18n(label, *catalog, language);
            yyjson::value field = MakePropertyField(
                field_name,
                resolved_label,
                mapping.value_type,
                mapping.input_type,
                value,
                candidates_ptr);
            // D-84-22/23: 单字段 input 也会投影 validation。PI input.verify →
            // validation.pattern；input.pattern_msg 经 i18n 解析为当前 language
            // 文本 → validation.message。两者都非空才输出 validation。
            if (dto.type == "input" && dto.inputs.size() == 1)
            {
                const auto& input = dto.inputs.front();
                if (input.verify && !input.verify->empty() && input.pattern_msg)
                {
                    const std::string_view resolved_msg =
                        ResolveI18n(*input.pattern_msg, *catalog, language);
                    if (!resolved_msg.empty())
                    {
                        yyjson::value validation(Object());
                        auto          v_obj = validation.as_object();
                        (*v_obj)[std::string_view("pattern")] =
                            JsonString(*input.verify);
                        (*v_obj)[std::string_view("message")] =
                            JsonString(resolved_msg);
                        (*field.as_object())[std::string_view("validation")] =
                            std::move(validation);
                    }
                }
            }
            // D-84-47: option 级别 icon 解析。
            if (dto.icon)
            {
                auto resolved = ResolveIcon(*dto.icon, catalog);
                if (!resolved.empty())
                {
                    (*field.as_object())[std::string_view("icon")] =
                        JsonString(resolved);
                }
            }
            return field;
        }

        // D-84-17: 为 multi-input option 的单个 input 构造独立
        // FieldDescriptor。 不再压成 jsonObject 文本框；每个 input 有自己的
        // name/label/valueType /value，setValue 通过
        // options.{optionName}.{inputName} 寻址。 valueType 按
        // input.pipeline_type 派生（int→0, float/double→2, else→4）；
        // inputType=0（普通文本/数字输入）。
        yyjson::value MakeInputSubField(
            std::string_view  field_name,
            const PiInputDto& input,
            std::string_view  raw_value,
            const PiCatalog*  catalog,
            std::string_view  language)
        {
            const std::string& pt =
                input.pipeline_type ? *input.pipeline_type : std::string{};
            int value_type = 4; // string default
            // std::stoll/stod 不接受 string_view；先转 std::string。
            const std::string raw_string(raw_value);
            auto              value = [&]() -> yyjson::value
            {
                if (pt == "int")
                {
                    value_type = 0;
                    try
                    {
                        return yyjson::value(
                            static_cast<std::int64_t>(std::stoll(raw_string)));
                    }
                    catch (...)
                    {
                        return yyjson::value(static_cast<std::int64_t>(0));
                    }
                }
                if (pt == "float" || pt == "double")
                {
                    value_type = 2;
                    try
                    {
                        return yyjson::value(std::stod(raw_string));
                    }
                    catch (...)
                    {
                        return yyjson::value(0.0);
                    }
                }
                return JsonString(raw_value);
            }();

            const std::string_view label =
                input.label ? *input.label : std::string_view(input.name);
            const std::string_view resolved_label =
                ResolveI18n(label, *catalog, language);
            yyjson::value field(Object());
            auto          obj = field.as_object();
            (*obj)[std::string_view("name")] = JsonString(field_name);
            (*obj)[std::string_view("label")] = JsonString(resolved_label);
            (*obj)[std::string_view("valueType")] = value_type;
            (*obj)[std::string_view("inputType")] = 0;
            (*obj)[std::string_view("value")] = std::move(value);
            // D-84-22/23: PI input.verify → validation.pattern；
            // input.pattern_msg 经 i18n 解析为当前 language 文本 →
            // validation.message。 两者都非空才输出 validation（严格
            // {pattern,message} 形态）。
            if (input.verify && !input.verify->empty() && input.pattern_msg)
            {
                const std::string_view resolved_msg =
                    ResolveI18n(*input.pattern_msg, *catalog, language);
                if (!resolved_msg.empty())
                {
                    yyjson::value validation(Object());
                    auto          v_obj = validation.as_object();
                    (*v_obj)[std::string_view("pattern")] =
                        JsonString(*input.verify);
                    (*v_obj)[std::string_view("message")] =
                        JsonString(resolved_msg);
                    (*obj)[std::string_view("validation")] =
                        std::move(validation);
                }
            }
            return field;
        }

        yyjson::value TaskSettingsArray(
            const std::vector<MaapiPiTaskSettingsDto>& tasks)
        {
            yyjson::value arr(Array());
            for (const auto& task : tasks)
            {
                yyjson::value value(Object());
                auto          obj = value.as_object();
                if (!task.item_id.empty())
                {
                    (*obj)[std::string_view("itemId")] =
                        JsonString(task.item_id);
                }
                (*obj)[std::string_view("taskName")] =
                    JsonString(task.task_name);
                (*obj)[std::string_view("enabled")] = task.enabled;
                yyjson::value options(Array());
                for (const auto& option : task.options)
                {
                    yyjson::value option_json(Object());
                    auto          option_obj = option_json.as_object();
                    (*option_obj)[std::string_view("optionName")] =
                        JsonString(option.option_name);
                    (*option_obj)[std::string_view("kind")] =
                        JsonString(option.kind);
                    yyjson::value cases(Array());
                    for (const auto& selected_case : option.selected_cases)
                    {
                        cases.as_array()->emplace_back(
                            JsonString(selected_case));
                    }
                    (*option_obj)[std::string_view("selectedCases")] =
                        std::move(cases);
                    if (option.bool_value)
                    {
                        (*option_obj)[std::string_view("boolValue")] =
                            *option.bool_value;
                    }
                    yyjson::value inputs(Object());
                    for (const auto& [key, input_value] : option.input_values)
                    {
                        (*inputs.as_object())[std::string_view(key)] =
                            JsonString(input_value);
                    }
                    (*option_obj)[std::string_view("inputValues")] =
                        std::move(inputs);
                    options.as_array()->emplace_back(std::move(option_json));
                }
                (*obj)[std::string_view("options")] = std::move(options);
                arr.as_array()->emplace_back(std::move(value));
            }
            return arr;
        }

        // D-84.2-04: 序列化 pi.customPresets[]（Provider 私有 preset 快照）。
        // 形态：id/name + 可选 controller/resource/language + options 子数组 +
        // tasks 子数组。customPresets 不进入 document UI IR，只在
        // acceptedProperties.pi 序列化时输出。
        yyjson::value CustomPresetsArray(
            const std::vector<MaapiPresetSnapshotDto>& presets)
        {
            yyjson::value arr(Array());
            for (const auto& preset : presets)
            {
                yyjson::value value(Object());
                auto          obj = value.as_object();
                (*obj)[std::string_view("id")] = JsonString(preset.id);
                (*obj)[std::string_view("name")] = JsonString(preset.name);
                if (preset.controller_name)
                {
                    (*obj)[std::string_view("controllerName")] =
                        JsonString(*preset.controller_name);
                }
                if (preset.resource_name)
                {
                    (*obj)[std::string_view("resourceName")] =
                        JsonString(*preset.resource_name);
                }
                if (preset.language)
                {
                    (*obj)[std::string_view("language")] =
                        JsonString(*preset.language);
                }
                (*obj)[std::string_view("globalOptions")] =
                    OptionSettingsArray(preset.global_options);
                (*obj)[std::string_view("resourceOptions")] =
                    OptionSettingsArray(preset.resource_options);
                (*obj)[std::string_view("controllerOptions")] =
                    OptionSettingsArray(preset.controller_options);
                (*obj)[std::string_view("tasks")] =
                    TaskSettingsArray(preset.tasks);
                arr.as_array()->emplace_back(std::move(value));
            }
            return arr;
        }

        // D-84.2-04: 解析 pi.customPresets[] 数组（Provider 私有 preset
        // 快照）。缺失时返回空 vector。跳过空 id 条目。
        std::vector<MaapiPresetSnapshotDto> ParseCustomPresetsArray(
            const auto&      obj,
            std::string_view key)
        {
            std::vector<MaapiPresetSnapshotDto> result;
            if (!obj.contains(key))
            {
                return result;
            }
            auto arr = obj[key].as_array();
            if (!arr)
            {
                return result;
            }
            for (auto it = arr->begin(); it != arr->end(); ++it)
            {
                auto preset_obj = it->as_object();
                if (!preset_obj)
                {
                    continue;
                }
                MaapiPresetSnapshotDto preset;
                preset.id = OptionalString(*preset_obj, "id").value_or("");
                preset.name = OptionalString(*preset_obj, "name").value_or("");
                preset.controller_name =
                    OptionalString(*preset_obj, "controllerName");
                preset.resource_name =
                    OptionalString(*preset_obj, "resourceName");
                preset.language = OptionalString(*preset_obj, "language");
                preset.global_options =
                    ParseOptionSettingsArray(*preset_obj, "globalOptions");
                preset.resource_options =
                    ParseOptionSettingsArray(*preset_obj, "resourceOptions");
                preset.controller_options =
                    ParseOptionSettingsArray(*preset_obj, "controllerOptions");
                preset.tasks = ParseTaskSettingsArray(*preset_obj, "tasks");
                if (!preset.id.empty())
                {
                    result.emplace_back(std::move(preset));
                }
            }
            return result;
        }

        yyjson::value OptionSettingsArray(
            const std::vector<MaapiPiOptionSettingsDto>& options)
        {
            yyjson::value arr(Array());
            for (const auto& option : options)
            {
                yyjson::value value(Object());
                auto          obj = value.as_object();
                (*obj)[std::string_view("optionName")] =
                    JsonString(option.option_name);
                (*obj)[std::string_view("kind")] = JsonString(option.kind);
                yyjson::value cases(Array());
                for (const auto& selected_case : option.selected_cases)
                {
                    cases.as_array()->emplace_back(JsonString(selected_case));
                }
                (*obj)[std::string_view("selectedCases")] = std::move(cases);
                if (option.bool_value)
                {
                    (*obj)[std::string_view("boolValue")] = *option.bool_value;
                }
                yyjson::value inputs(Object());
                for (const auto& [key, input_value] : option.input_values)
                {
                    (*inputs.as_object())[std::string_view(key)] =
                        JsonString(input_value);
                }
                (*obj)[std::string_view("inputValues")] = std::move(inputs);
                arr.as_array()->emplace_back(std::move(value));
            }
            return arr;
        }

        // D-86-03/06: 序列化 pi.pretasks[]（Provider 私有 pretask 选中值
        // 存储）。形态：pretaskId + options 子数组。不进入 document UI IR，
        // 只在 acceptedProperties.pi 序列化时输出。
        yyjson::value PretaskSettingsArray(
            const std::vector<MaapiPiPretaskSettingsDto>& pretasks)
        {
            yyjson::value arr(Array());
            for (const auto& pretask : pretasks)
            {
                yyjson::value value(Object());
                auto          obj = value.as_object();
                (*obj)[std::string_view("pretaskId")] =
                    JsonString(pretask.pretask_id);
                (*obj)[std::string_view("options")] =
                    OptionSettingsArray(pretask.options);
                arr.as_array()->emplace_back(std::move(value));
            }
            return arr;
        }

        // D-86-03/06: 解析 pi.pretasks[] 数组（Provider 私有 pretask 选中值
        // 存储）。缺失时返回空 vector。跳过空 pretaskId 条目。
        std::vector<MaapiPiPretaskSettingsDto> ParsePretaskSettingsArray(
            const auto&      obj,
            std::string_view key)
        {
            std::vector<MaapiPiPretaskSettingsDto> result;
            if (!obj.contains(key))
            {
                return result;
            }
            auto arr = obj[key].as_array();
            if (!arr)
            {
                return result;
            }
            for (auto it = arr->begin(); it != arr->end(); ++it)
            {
                auto pretask_obj = it->as_object();
                if (!pretask_obj)
                {
                    continue;
                }
                MaapiPiPretaskSettingsDto pretask;
                pretask.pretask_id =
                    OptionalString(*pretask_obj, "pretaskId").value_or("");
                pretask.options =
                    ParseOptionSettingsArray(*pretask_obj, "options");
                if (!pretask.pretask_id.empty())
                {
                    result.emplace_back(std::move(pretask));
                }
            }
            return result;
        }

        // D-86-03/06: 按 pretask_id 在 acceptedProperties.pi.pretasks 中查找
        // 选中值存储。找不到时返回 nullptr（caller 视为 fresh session 走
        // PI default 回退）。
        const MaapiPiPretaskSettingsDto* FindPretaskSettings(
            const AcceptedSettingsDto& settings,
            std::string_view           pretask_id)
        {
            auto it = std::find_if(
                settings.pi.pretasks.begin(),
                settings.pi.pretasks.end(),
                [&](const MaapiPiPretaskSettingsDto& p)
                { return p.pretask_id == pretask_id; });
            return it == settings.pi.pretasks.end() ? nullptr : &*it;
        }

        MaapiPiPretaskSettingsDto* FindOrCreatePretaskSettings(
            AcceptedSettingsDto& settings,
            std::string_view     pretask_id)
        {
            auto it = std::find_if(
                settings.pi.pretasks.begin(),
                settings.pi.pretasks.end(),
                [&](const MaapiPiPretaskSettingsDto& p)
                { return p.pretask_id == pretask_id; });
            if (it != settings.pi.pretasks.end())
            {
                return &*it;
            }
            MaapiPiPretaskSettingsDto fresh;
            fresh.pretask_id = std::string(pretask_id);
            settings.pi.pretasks.emplace_back(std::move(fresh));
            return &settings.pi.pretasks.back();
        }

        // 递归收集一个 option 键可达的全部 option 键（含每个 case 上声明的
        // 嵌套 option），用于 pretask.<optionKey> 字段的归属判定——嵌套
        // option 不在任何 pretask.dto.option 顶层列表里，必须沿 option 树
        // 找。路由（setValue）与分桶（fieldLayout）共用。
        void CollectReachableOptionKeys(
            const PiCatalog&       catalog,
            std::string_view       option_name,
            std::set<std::string>& visited)
        {
            if (!visited.insert(std::string(option_name)).second)
            {
                return;
            }
            const auto* option = FindOption(catalog, option_name);
            if (option == nullptr)
            {
                return;
            }
            for (const auto& case_def : option->dto.cases)
            {
                for (const auto& nested : case_def.option)
                {
                    CollectReachableOptionKeys(catalog, nested, visited);
                }
            }
        }

        // 首个（catalog->pretasks 序）可达该 option 键（含嵌套）的 pretask——
        // 与投影侧 emitted_pretask_keys「首个 pretask 的引用胜出」语义一致。
        // 返回 nullptr 表示没有任何 pretask 声明该 option。
        const PiPretask* FindOwnerPretask(
            const PiCatalog& catalog,
            std::string_view option_name)
        {
            for (const auto& pretask : catalog.pretasks)
            {
                std::set<std::string> reachable;
                for (const auto& declared : pretask.dto.option)
                {
                    CollectReachableOptionKeys(catalog, declared, reachable);
                }
                if (reachable.count(std::string(option_name)) != 0)
                {
                    return &pretask;
                }
            }
            return nullptr;
        }

        // stable pretask_id（dto.name 非空→name，否则 exec），投影 / setValue
        // 路由 / PiCompiler 物化三处共用同一派生规则。
        std::string StablePretaskId(const PiPretask& pretask)
        {
            return pretask.dto.name.empty() ? pretask.dto.exec
                                            : pretask.dto.name;
        }

        bool HasTask(const PiCatalog& catalog, std::string_view name)
        {
            return std::any_of(
                catalog.tasks.begin(),
                catalog.tasks.end(),
                [&](const PiTask& task) { return task.dto.name == name; });
        }

        bool ContainsName(
            const std::vector<std::string>& values,
            std::string_view                name)
        {
            return std::find(values.begin(), values.end(), name)
                   != values.end();
        }

        const PiController* FindController(
            const PiCatalog& catalog,
            std::string_view name)
        {
            auto it = std::find_if(
                catalog.controllers.begin(),
                catalog.controllers.end(),
                [&](const PiController& item)
                { return item.dto.name == name; });
            return it == catalog.controllers.end() ? nullptr : &*it;
        }

        const PiResource* FindResource(
            const PiCatalog& catalog,
            std::string_view name)
        {
            auto it = std::find_if(
                catalog.resources.begin(),
                catalog.resources.end(),
                [&](const PiResource& item) { return item.dto.name == name; });
            return it == catalog.resources.end() ? nullptr : &*it;
        }

        const PiTask* FindTask(const PiCatalog& catalog, std::string_view name)
        {
            auto it = std::find_if(
                catalog.tasks.begin(),
                catalog.tasks.end(),
                [&](const PiTask& item) { return item.dto.name == name; });
            return it == catalog.tasks.end() ? nullptr : &*it;
        }

        MaapiPiTaskSettingsDto* FindSelectedTask(
            AcceptedSettingsDto& settings,
            std::string_view     task_name)
        {
            auto it = std::find_if(
                settings.pi.tasks.begin(),
                settings.pi.tasks.end(),
                [&](const MaapiPiTaskSettingsDto& item)
                { return item.task_name == task_name; });
            return it == settings.pi.tasks.end() ? nullptr : &*it;
        }

        std::vector<MaapiPiOptionSettingsDto>* FindTaskOptionContainer(
            AcceptedSettingsDto& settings,
            const PiCatalog&     catalog,
            std::string_view     option_name)
        {
            for (auto& task : settings.pi.tasks)
            {
                const auto* catalog_task = FindTask(catalog, task.task_name);
                if (catalog_task
                    && ContainsName(catalog_task->dto.option, option_name))
                {
                    return &task.options;
                }
            }

            auto catalog_task = std::find_if(
                catalog.tasks.begin(),
                catalog.tasks.end(),
                [&](const PiTask& item)
                { return ContainsName(item.dto.option, option_name); });
            if (catalog_task == catalog.tasks.end())
            {
                return nullptr;
            }

            MaapiPiTaskSettingsDto selected;
            selected.task_name = catalog_task->dto.name;
            // itemId 偶现 bug Layer 2：apply 路径新建 task 实例时必须分配
            // Provider 颁发的稳定 item_id；否则 doc.item.id 投影为空，前端
            // 报 "FormSequenceItem.id must be non-empty"。
            selected.item_id = AllocateItemId(settings);
            settings.pi.tasks.emplace_back(std::move(selected));
            return &settings.pi.tasks.back().options;
        }

        const MaapiPiOptionSettingsDto* FindOptionSettings(
            const std::vector<MaapiPiOptionSettingsDto>& options,
            std::string_view                             option_name)
        {
            auto it = std::find_if(
                options.begin(),
                options.end(),
                [&](const MaapiPiOptionSettingsDto& item)
                { return item.option_name == option_name; });
            return it == options.end() ? nullptr : &*it;
        }

        // pretask option 投影的激活 case 计算，与投影侧 switch 值语义一致
        // （无存储回退 true，MaaPI 约定；MakeOptionPropertyField 同款）。
        // select / checkbox：用户 selected_cases，无存储回退 default_cases。
        std::vector<std::string> ActivatedOptionCases(
            const PiOptionDto&              dto,
            const MaapiPiOptionSettingsDto* current)
        {
            if (dto.type == "switch")
            {
                if (current && !current->bool_value
                    && !current->selected_cases.empty())
                {
                    return {current->selected_cases.front()};
                }
                const bool desired = current && current->bool_value
                                         ? *current->bool_value
                                         : true;
                for (const auto& case_def : dto.cases)
                {
                    const bool truthy =
                        case_def.name == "Yes" || case_def.name == "yes"
                        || case_def.name == "Y" || case_def.name == "y";
                    const bool falsy =
                        case_def.name == "No" || case_def.name == "no"
                        || case_def.name == "N" || case_def.name == "n";
                    if ((desired && truthy) || (!desired && falsy))
                    {
                        return {case_def.name};
                    }
                }
                return {};
            }
            return current ? current->selected_cases : dto.default_cases;
        }

        // pretask option 递归投影：emit 一个 pretask.<optionKey> 字段后，沿
        // 激活 case 的嵌套 option（协议 case.option）继续 emit——服务端重
        // 投影范式：用户切换外层 case → document 重投影，嵌套字段随之出现/
        // 消失，前端零条件可见性。emitted_keys 跨 pretask 共享（首个引用
        // 胜出去重）。
        void EmitPretaskOptionField(
            const MaapiPiPretaskSettingsDto* pretask_settings,
            const PiCatalog*                 catalog,
            std::string_view                 current_language,
            std::string_view                 option_name,
            std::set<std::string>&           emitted_keys,
            yyjson::value&                   fields)
        {
            if (!emitted_keys.insert(std::string(option_name)).second)
            {
                return;
            }
            const auto* option = FindOption(*catalog, option_name);
            if (option == nullptr)
            {
                return;
            }
            const auto* current =
                pretask_settings
                    ? FindOptionSettings(pretask_settings->options, option_name)
                    : nullptr;
            fields.as_array()->emplace_back(MakeOptionPropertyField(
                std::string{"pretask."} + std::string(option_name),
                option->dto,
                current,
                catalog,
                current_language));
            const auto activated = ActivatedOptionCases(option->dto, current);
            for (const auto& case_def : option->dto.cases)
            {
                if (std::find(activated.begin(), activated.end(), case_def.name)
                    == activated.end())
                {
                    continue;
                }
                for (const auto& nested : case_def.option)
                {
                    EmitPretaskOptionField(
                        pretask_settings,
                        catalog,
                        current_language,
                        nested,
                        emitted_keys,
                        fields);
                }
            }
        }

        void RemoveOption(
            std::vector<MaapiPiOptionSettingsDto>& options,
            std::string_view                       option_name)
        {
            std::erase_if(
                options,
                [&](const MaapiPiOptionSettingsDto& item)
                { return item.option_name == option_name; });
        }

        void RemoveOptionEverywhere(
            AcceptedSettingsDto& settings,
            std::string_view     option_name)
        {
            RemoveOption(settings.pi.global_options, option_name);
            RemoveOption(settings.pi.resource_options, option_name);
            RemoveOption(settings.pi.controller_options, option_name);
            for (auto& task : settings.pi.tasks)
            {
                RemoveOption(task.options, option_name);
            }
        }

        void RemoveOrphanPath(
            AcceptedSettingsDto& settings,
            std::string_view     path)
        {
            std::erase_if(
                settings.pi.orphan_paths,
                [&](const std::string& item) { return item == path; });
        }

        void RemoveOrphanPrefix(
            AcceptedSettingsDto& settings,
            std::string_view     prefix)
        {
            std::erase_if(
                settings.pi.orphan_paths,
                [&](const std::string& item)
                { return item.starts_with(prefix); });
        }

        void UpsertOption(
            std::vector<MaapiPiOptionSettingsDto>& options,
            MaapiPiOptionSettingsDto               option)
        {
            auto it = std::find_if(
                options.begin(),
                options.end(),
                [&](const MaapiPiOptionSettingsDto& item)
                { return item.option_name == option.option_name; });
            if (it == options.end())
            {
                options.emplace_back(std::move(option));
                return;
            }
            *it = std::move(option);
        }

        // 初始化单个 option 的默认值（包括嵌套 option）
        void InitializeOptionDefaults(
            std::vector<MaapiPiOptionSettingsDto>& result_options,
            const std::vector<std::string>&        option_names,
            const PiCatalog&                       catalog)
        {
            for (const auto& option_name : option_names)
            {
                const auto* option = FindOption(catalog, option_name);
                if (!option)
                {
                    continue;
                }
                MaapiPiOptionSettingsDto opt_setting;
                opt_setting.option_name = option->dto.name;
                opt_setting.kind = option->dto.type;

                if (option->dto.type == "select")
                {
                    if (!option->dto.default_cases.empty())
                    {
                        opt_setting.selected_cases = option->dto.default_cases;
                    }
                    else if (!option->dto.cases.empty())
                    {
                        opt_setting.selected_cases.emplace_back(
                            option->dto.cases.front().name);
                    }

                    for (const auto& selected_case : opt_setting.selected_cases)
                    {
                        auto case_it = std::find_if(
                            option->dto.cases.begin(),
                            option->dto.cases.end(),
                            [&](const PiCaseDto& c)
                            { return c.name == selected_case; });
                        if (case_it != option->dto.cases.end())
                        {
                            InitializeOptionDefaults(
                                result_options,
                                case_it->option,
                                catalog);
                        }
                    }
                }
                else if (option->dto.type == "checkbox")
                {
                    opt_setting.selected_cases = option->dto.default_cases;
                }
                else if (option->dto.type == "switch")
                {
                    opt_setting.bool_value = true;
                }
                else if (option->dto.type == "input")
                {
                    for (const auto& input : option->dto.inputs)
                    {
                        if (input.default_value)
                        {
                            opt_setting.input_values.emplace(
                                input.name,
                                *input.default_value);
                        }
                    }
                }
                result_options.emplace_back(std::move(opt_setting));
            }
        }

        // 按 option type 转换 preset 值
        std::optional<MaapiPiOptionSettingsDto> ConvertPresetOptionValue(
            const std::string&                     option_name,
            const yyjson::value&                   preset_value,
            const PiCatalog&                       catalog,
            std::vector<MaapiPiOptionSettingsDto>& result_options)
        {
            const auto* option = FindOption(catalog, option_name);
            if (!option)
            {
                return std::nullopt;
            }

            MaapiPiOptionSettingsDto result;
            result.option_name = option->dto.name;
            result.kind = option->dto.type;

            if (option->dto.type == "switch")
            {
                if (preset_value.is_string())
                {
                    const auto str_val =
                        std::string(preset_value.as_string().value_or(""));
                    result.bool_value =
                        (str_val == "Yes" || str_val == "yes" || str_val == "Y"
                         || str_val == "y");
                }
                else if (preset_value.is_bool())
                {
                    result.bool_value = preset_value.as_bool().value_or(false);
                }
                return result;
            }

            if (option->dto.type == "checkbox")
            {
                if (preset_value.is_array())
                {
                    auto arr = preset_value.as_array();
                    for (auto it = arr->begin(); it != arr->end(); ++it)
                    {
                        if (it->is_string())
                        {
                            const auto case_name =
                                std::string(it->as_string().value_or(""));
                            if (std::any_of(
                                    option->dto.cases.begin(),
                                    option->dto.cases.end(),
                                    [&](const PiCaseDto& c)
                                    { return c.name == case_name; }))
                            {
                                result.selected_cases.emplace_back(case_name);
                            }
                        }
                    }
                }
                return result;
            }

            if (option->dto.type == "input")
            {
                if (auto obj = preset_value.as_object())
                {
                    for (auto input_it = obj->begin(); input_it != obj->end();
                         ++input_it)
                    {
                        if (input_it->second.is_string())
                        {
                            result.input_values.emplace(
                                input_it->first,
                                input_it->second.as_string().value_or(""));
                        }
                    }
                }
                return result;
            }

            if (option->dto.type == "select" || option->dto.type.empty())
            {
                if (preset_value.is_string())
                {
                    const auto case_name =
                        std::string(preset_value.as_string().value_or(""));
                    result.selected_cases.emplace_back(case_name);

                    auto case_it = std::find_if(
                        option->dto.cases.begin(),
                        option->dto.cases.end(),
                        [&](const PiCaseDto& c)
                        { return c.name == case_name; });
                    if (case_it != option->dto.cases.end())
                    {
                        InitializeOptionDefaults(
                            result_options,
                            case_it->option,
                            catalog);
                    }
                }
                return result;
            }

            return result;
        }

        std::optional<MaapiPiOptionSettingsDto> ParseOptionValue(
            const PiOption&      option,
            const yyjson::value& value)
        {
            if (value.is_null())
            {
                return std::nullopt;
            }

            MaapiPiOptionSettingsDto result;
            result.option_name = option.dto.name;
            result.kind = option.dto.type;

            // IR 标量形态（DAS-95 方向 A emit 产出）：
            if (value.is_bool())
            {
                result.bool_value = value.as_bool().value_or(false);
                return result;
            }
            if (value.is_sint() || value.is_uint())
            {
                if (option.dto.type == "input" && !option.dto.inputs.empty())
                {
                    const auto n = value.is_sint()
                                       ? value.as_sint().value_or(0)
                                       : static_cast<std::int64_t>(
                                             value.as_uint().value_or(0));
                    result.input_values.emplace(
                        option.dto.inputs.front().name,
                        std::to_string(n));
                }
                return result;
            }
            if (value.is_real())
            {
                if (option.dto.type == "input" && !option.dto.inputs.empty())
                {
                    result.input_values.emplace(
                        option.dto.inputs.front().name,
                        std::to_string(value.as_real().value_or(0.0)));
                }
                return result;
            }
            if (value.is_string())
            {
                const auto text = std::string(value.as_string().value_or(""));
                if (option.dto.type == "input" && !option.dto.inputs.empty())
                {
                    result.input_values.emplace(
                        option.dto.inputs.front().name,
                        text);
                }
                else
                {
                    result.selected_cases.emplace_back(text);
                }
                return result;
            }
            if (auto arr = value.as_array())
            {
                for (auto it = arr->begin(); it != arr->end(); ++it)
                {
                    if (it->is_string())
                    {
                        result.selected_cases.emplace_back(
                            it->as_string().value_or(""));
                    }
                }
                return result;
            }
            if (auto obj = value.as_object())
            {
                // IR 标量方向 A：对象形态仅 input 多字段合法（{inputName:
                // value}）。 其它 option 类型收到对象 = 非法 IR 输入，返回空
                // result。
                if (option.dto.type == "input" && option.dto.inputs.size() > 1)
                {
                    for (auto it = obj->begin(); it != obj->end(); ++it)
                    {
                        if (it->second.is_string())
                        {
                            result.input_values.emplace(
                                it->first,
                                it->second.as_string().value_or(""));
                        }
                    }
                }
                return result;
            }

            return result;
        }

        void ApplyOptionValueChange(
            AcceptedSettingsDto& settings,
            const PiCatalog&     catalog,
            std::string_view     option_name,
            const yyjson::value& value)
        {
            const auto* option = FindOption(catalog, option_name);
            if (!option)
            {
                if (value.is_null())
                {
                    RemoveOptionEverywhere(settings, option_name);
                    RemoveOrphanPath(
                        settings,
                        std::string{"options:"} + std::string{option_name});
                    RemoveOrphanPath(
                        settings,
                        std::string{"options."} + std::string{option_name});
                    return;
                }
                settings.pi.orphan_paths.emplace_back(
                    std::string{"options:"} + std::string{option_name});
                return;
            }

            RemoveOrphanPath(
                settings,
                std::string{"options:"} + std::string{option_name});
            RemoveOrphanPath(
                settings,
                std::string{"options."} + std::string{option_name});
            if (value.is_null())
            {
                RemoveOptionEverywhere(settings, option_name);
                return;
            }

            auto parsed = ParseOptionValue(*option, value);
            if (!parsed)
            {
                RemoveOptionEverywhere(settings, option_name);
                return;
            }

            RemoveOptionEverywhere(settings, option_name);
            if (std::any_of(
                    catalog.global_options.begin(),
                    catalog.global_options.end(),
                    [&](const PiOption& item)
                    { return item.dto.name == option_name; }))
            {
                UpsertOption(settings.pi.global_options, std::move(*parsed));
                return;
            }

            const auto controller_name = settings.pi.controller_name.value_or(
                catalog.controllers.empty()
                    ? std::string{}
                    : catalog.controllers.front().dto.name);
            if (const auto* controller =
                    FindController(catalog, controller_name);
                controller && ContainsName(controller->dto.option, option_name))
            {
                UpsertOption(
                    settings.pi.controller_options,
                    std::move(*parsed));
                return;
            }

            const auto resource_name = settings.pi.resource_name.value_or(
                catalog.resources.empty() ? std::string{}
                                          : catalog.resources.front().dto.name);
            if (const auto* resource = FindResource(catalog, resource_name);
                resource && ContainsName(resource->dto.option, option_name))
            {
                UpsertOption(settings.pi.resource_options, std::move(*parsed));
                return;
            }

            if (auto* task_options =
                    FindTaskOptionContainer(settings, catalog, option_name))
            {
                UpsertOption(*task_options, std::move(*parsed));
            }
        }

        // D-84-40: itemId 分配辅助 (内部实现)。公共 API 在
        // Das::Plugins::DasMaaPi 命名空间下声明；这里在匿名 namespace 提供
        // CollectUsed* 辅助，避免暴露 std::set 头依赖。
        // 收集 settings 内所有已使用 itemId（flat pi.tasks）。
        std::set<std::string> CollectUsedItemIds(
            const AcceptedSettingsDto& settings)
        {
            std::set<std::string> ids;
            auto                  collect_from =
                [&ids](const std::vector<MaapiPiTaskSettingsDto>& tasks)
            {
                for (const auto& task : tasks)
                {
                    if (!task.item_id.empty())
                    {
                        ids.emplace(task.item_id);
                    }
                }
            };
            collect_from(settings.pi.tasks);
            return ids;
        }

        // D-84.2-04: 收集 settings.pi.custom_presets 内所有已使用 presetId，
        // 供 AllocatePresetId 避免与既有 snapshot id 冲突。customPresets 为空
        // 时返回空集。
        std::set<std::string> CollectUsedPresetIds(
            const AcceptedSettingsDto& settings)
        {
            std::set<std::string> ids;
            for (const auto& preset : settings.pi.custom_presets)
            {
                if (!preset.id.empty())
                {
                    ids.emplace(preset.id);
                }
            }
            return ids;
        }

        // 全进程 atomic 计数器，保证同一进程内分配的 id 单调递增且唯一。
        // 跨进程持久化层另有 taskId*.json 的 revision 保证；这里只需
        // 同一会话内 id 不冲突即可。坚持不从 taskName/actionId/index 派生，
        // 满足 D-84-40 的稳定身份要求。
        std::string AllocateIdWithPrefix(
            std::string_view      prefix,
            std::set<std::string> existing)
        {
            static std::atomic<std::uint64_t> counter{1};
            for (int retry = 0; retry < 10000; ++retry)
            {
                const auto  next = counter.fetch_add(1);
                std::string candidate =
                    std::string(prefix) + std::to_string(next);
                if (existing.count(candidate) == 0)
                {
                    return candidate;
                }
            }
            // 退化路径：拼接 extra entropy 保证唯一。
            const auto next = counter.fetch_add(1);
            return std::string(prefix) + std::to_string(next) + "_x";
        }
    } // namespace

    std::string AllocateItemId(const AcceptedSettingsDto& settings)
    {
        return AllocateIdWithPrefix("item_", CollectUsedItemIds(settings));
    }

    // D-84.2-04: 为新 preset snapshot 分配 Provider 颁发的稳定 presetId。
    // 基于 atomic counter 生成 "preset_<n>" 形式，避免与既有 snapshot id 冲突。
    std::string AllocatePresetId(const AcceptedSettingsDto& settings)
    {
        return AllocateIdWithPrefix("preset_", CollectUsedPresetIds(settings));
    }

    // D-84.2-04: newPreset invokeCommand 实现。把当前 flat pi 字段镜像为一个
    // snapshot，追加到 pi.custom_presets，并把 preset_name 指向新 snapshot。这
    // 样 apply 返回的 document 里 presetName select 的当前值会切到新 preset，
    // 经既有的 apply-returns-document 反馈通道产生立即可见的变化；否则新 preset
    // 仅作为下拉候选项静默出现，UI 看不到任何变化。snapshot.id 由
    // AllocatePresetId 颁发；name 自动生成；controller/resource/language/
    // options/tasks 从 flat pi 字段拷贝。
    void ApplyNewPreset(AcceptedSettingsDto& settings)
    {
        const std::string      new_preset_id = AllocatePresetId(settings);
        MaapiPresetSnapshotDto snapshot;
        snapshot.id = new_preset_id;
        snapshot.name =
            "Preset " + std::to_string(settings.pi.custom_presets.size() + 1);
        snapshot.controller_name = settings.pi.controller_name;
        snapshot.resource_name = settings.pi.resource_name;
        snapshot.language = settings.pi.language;
        snapshot.global_options = settings.pi.global_options;
        snapshot.resource_options = settings.pi.resource_options;
        snapshot.controller_options = settings.pi.controller_options;
        snapshot.tasks = settings.pi.tasks;
        settings.pi.custom_presets.emplace_back(std::move(snapshot));
        // 自动选中新 preset：返回 document 的 presetName 当前值切到新 preset，
        // 使新建结果立即可见。
        settings.pi.preset_name = new_preset_id;
    }

    AcceptedSettingsDto ParseAcceptedSettings(const yyjson::value& value)
    {
        auto obj = value.as_object();
        if (!obj)
        {
            return {};
        }
        if (obj->contains(std::string_view("properties")))
        {
            if (auto props = (*obj)[std::string_view("properties")].as_object())
            {
                return ParseAcceptedObject(*props);
            }
        }
        return ParseAcceptedObject(*obj);
    }

    yyjson::value SerializeAcceptedSettings(const AcceptedSettingsDto& settings)
    {
        yyjson::value result(Object());
        auto          obj = result.as_object();
        (*obj)[std::string_view("version")] = settings.version;

        yyjson::value adapter(Object());
        auto          adapter_obj = adapter.as_object();
        if (settings.adapter.interface_path)
        {
            (*adapter_obj)[std::string_view("interfacePath")] =
                JsonString(*settings.adapter.interface_path);
        }
        if (settings.adapter.proxy_url)
        {
            (*adapter_obj)[std::string_view("proxyUrl")] =
                JsonString(*settings.adapter.proxy_url);
        }
        if (settings.adapter.mirrorchyan_cdk)
        {
            (*adapter_obj)[std::string_view("mirrorchyanCdk")] =
                JsonString(*settings.adapter.mirrorchyan_cdk);
        }
        if (settings.adapter.update_channel)
        {
            (*adapter_obj)[std::string_view("updateChannel")] =
                JsonString(*settings.adapter.update_channel);
        }
        if (settings.adapter.project_root)
        {
            (*adapter_obj)[std::string_view("projectRoot")] =
                JsonString(*settings.adapter.project_root);
        }
        if (settings.adapter.source_fingerprint)
        {
            (*adapter_obj)[std::string_view("sourceFingerprint")] =
                JsonString(*settings.adapter.source_fingerprint);
        }
        yyjson::value policy(Object());
        (*policy.as_object())[std::string_view("failFast")] =
            settings.adapter.execution_policy.fail_fast;
        (*adapter_obj)[std::string_view("executionPolicy")] = std::move(policy);
        (*obj)[std::string_view("adapter")] = std::move(adapter);

        yyjson::value pi(Object());
        auto          pi_obj = pi.as_object();
        if (settings.pi.controller_name)
        {
            (*pi_obj)[std::string_view("controllerName")] =
                JsonString(*settings.pi.controller_name);
        }
        if (settings.pi.resource_name)
        {
            (*pi_obj)[std::string_view("resourceName")] =
                JsonString(*settings.pi.resource_name);
        }
        if (settings.pi.preset_name)
        {
            (*pi_obj)[std::string_view("presetName")] =
                JsonString(*settings.pi.preset_name);
        }
        if (settings.pi.language)
        {
            (*pi_obj)[std::string_view("language")] =
                JsonString(*settings.pi.language);
        }
        if (settings.pi.controller_adb_device)
        {
            (*pi_obj)[std::string_view("controllerAdbDevice")] =
                JsonString(*settings.pi.controller_adb_device);
        }
        (*pi_obj)[std::string_view("globalOptions")] =
            OptionSettingsArray(settings.pi.global_options);
        (*pi_obj)[std::string_view("resourceOptions")] =
            OptionSettingsArray(settings.pi.resource_options);
        (*pi_obj)[std::string_view("controllerOptions")] =
            OptionSettingsArray(settings.pi.controller_options);
        (*pi_obj)[std::string_view("tasks")] =
            TaskSettingsArray(settings.pi.tasks);
        // D-86-03/06: pretasks 是 Provider 私有 pretask 选中值存储。
        // 仅在非空时输出，避免 legacy fixture 出现空数组。
        if (!settings.pi.pretasks.empty())
        {
            (*pi_obj)[std::string_view("pretasks")] =
                PretaskSettingsArray(settings.pi.pretasks);
        }
        // D-84.2-04: customPresets 是 Provider 私有 preset 快照存储。
        // 仅在非空时输出，避免 legacy fixture 出现空数组。
        if (!settings.pi.custom_presets.empty())
        {
            (*pi_obj)[std::string_view("customPresets")] =
                CustomPresetsArray(settings.pi.custom_presets);
        }
        yyjson::value orphans(Array());
        for (const auto& path : settings.pi.orphan_paths)
        {
            orphans.as_array()->emplace_back(JsonString(path));
        }
        (*pi_obj)[std::string_view("orphanPaths")] = std::move(orphans);
        // schedule 配置：flat optional 字段，engaged 才写（与
        // controllerName/resourceName 范式一致）。off 时 4 字段全部不写，
        // 反序列化读不回 → 全部 nullopt（schedule off 契约）。
        if (settings.pi.schedule_kind)
        {
            (*pi_obj)[std::string_view("scheduleKind")] =
                JsonString(*settings.pi.schedule_kind);
        }
        if (settings.pi.schedule_interval_minutes)
        {
            (*pi_obj)[std::string_view("scheduleIntervalMinutes")] =
                *settings.pi.schedule_interval_minutes;
        }
        if (settings.pi.schedule_daily_time)
        {
            (*pi_obj)[std::string_view("scheduleDailyTime")] =
                JsonString(*settings.pi.schedule_daily_time);
        }
        if (settings.pi.schedule_daily_catch_up)
        {
            (*pi_obj)[std::string_view("scheduleDailyCatchUp")] =
                *settings.pi.schedule_daily_catch_up;
        }
        (*obj)[std::string_view("pi")] = std::move(pi);

        // ui（通用 kv）：非空时输出 ui.values，与反序列化（ParseAcceptedObject
        // 查 ui.values）对称。此前扁平铺到 ui 会导致 session 从
        // acceptedProperties 重建时 读不回 → BuildDocument 的 defaultExpanded
        // 合并失效。
        if (!settings.ui.values.empty())
        {
            yyjson::value ui(Das::Utils::MakeYyjsonObject());
            auto          ui_obj = ui.as_object();
            yyjson::value vals(Das::Utils::MakeYyjsonObject());
            auto          vals_obj = vals.as_object();
            for (const auto& [k, v] : settings.ui.values)
            {
                (*vals_obj)[std::string_view(k)] =
                    Das::Utils::CloneYyjsonValue(v);
            }
            (*ui_obj)[std::string_view("values")] = std::move(vals);
            (*obj)[std::string_view("ui")] = std::move(ui);
        }
        return result;
    }

    // 构建根级 fieldLayout[]。叶子 fieldName 集合 == fields[].name 集合
    // （全覆盖、唯一），满足 schema validator ValidateFieldLayout 契约。
    // 分区策略（PI v2.8.0/v2.7.0 语义）：pretask 是项目级预任务，每个
    // pretask 独立一 section（label = pretask 作者 label 经 i18n 解析，
    // 让用户知道这组选项属于哪个预任务，协议 L308-315 呈现义务）；
    // setting 是协议自带的设置页分区声明，每元素独立一 section。
    // setting / pretask section.label 是作者内容（dto.label），经
    // ResolveI18nOrDiag 解析，缺译时 fallback 到
    // dto.name；project/update/welcome section label 是 DAS
    // chrome，在各 emit lambda 内经 Translate 解析（不在此函数）。 游离
    // global_option（未被任何 setting 引用）不进 fields[]，故不进 layout —— PI
    // v2.8.0 setting 才是"哪些 option 进设置页"的声明，未声明的不渲染。
    yyjson::value BuildRootFieldLayout(
        const yyjson::value&                        fields,
        const PiCatalog*                            catalog,
        std::string_view                            current_language,
        std::vector<PiDiagnosticDto>*               projection_diagnostics,
        const std::map<std::string, yyjson::value>& ui_values,
        bool                                        update_section_top = false)
    {
        yyjson::value layout(Array());
        auto          layout_arr = layout.as_array();
        if (!layout_arr)
        {
            return layout;
        }
        auto fields_arr = fields.as_array();
        if (!fields_arr)
        {
            return layout;
        }

        // 按 name 前缀/字面量分类（保留 fields[] 声明顺序，layout 与 fields
        // 顺序一致）。fields[] 不含 options.<name>（游离 global_option
        // 不投影），故已知字段分组：update 更新字段（CDK / 代理 / 检查更新
        // 通道 + 版本只读字段，独立 section）、project 项目级字段、
        // settings.<opt> 分区字段、pretask.<optKey> 按「声明该 option 的
        // pretask」分组（FindOwnerPretask 首个可达胜出，含嵌套 option）。
        // 未识别前缀的 root field 兜底进
        // project_entries（项目设置 section），保证 fieldLayout 全覆盖，防
        // 未来新增 root field 前缀漏列触发 ValidateFieldLayout 的
        // layoutMissingField reject。新增 root field 前缀时应在此显式登记，
        // 以便走更贴切的分类桶。
        std::vector<std::string> update_entries;
        std::vector<std::string> project_entries;
        std::vector<std::string> welcome_entries;
        std::vector<std::string> schedule_entries;
        // pretask 分组桶：pretask 首现顺序保持（与投影 emit 顺序一致）。
        std::vector<std::pair<const PiPretask*, std::vector<std::string>>>
                                           pretask_buckets;
        std::map<std::string, std::string> settings_by_opt;
        constexpr std::string_view         settings_prefix = "settings.";
        constexpr std::string_view         schedule_prefix = "schedule.";
        constexpr std::string_view         pretask_prefix = "pretask.";
        for (auto it = fields_arr->begin(); it != fields_arr->end(); ++it)
        {
            auto field_obj = it->as_object();
            if (!field_obj)
            {
                continue;
            }
            auto name_opt = (*field_obj)[std::string_view("name")].as_string();
            if (!name_opt)
            {
                continue;
            }
            const std::string_view name = *name_opt;
            if (name.empty())
            {
                continue;
            }
            if (name == "adapter.mirrorchyanCdk" || name == "adapter.proxyUrl"
                || name == "adapter.updateChannel" || name == "resourceVersion"
                || name == "maafwVersion" || name == "updateVersion"
                || name == "releaseNote" || name == "updateProgress"
                || name == "downloadRate" || name == "downloadElapsed")
            {
                // 更新 section：CDK / 代理 / 检查更新通道 + 版本只读字段 +
                // 更新 panel 五字段（与 checkUpdate/applyUpdate 按钮同属"更新"
                // 语义分区；代理设置并入更新 section，与 CDK 相邻）。
                update_entries.emplace_back(name);
            }
            else if (name == "welcome")
            {
                // welcome section：PI 顶层 welcome markdown 公告。独立成桶防
                // 落入 project_entries 兜底（兜底桶会把 welcome 混入项目设置
                // section，前端数据驱动渲染会显示在错误面板）。
                welcome_entries.emplace_back(name);
            }
            else if (name.starts_with(pretask_prefix) && catalog != nullptr)
            {
                // pretask.<optionKey> 按声明该 option 的 pretask 分组
                // （含嵌套 option 树，与投影/setValue 路由同一归属规则）。
                // catalog 缺席时兜底进 project_entries（保持全覆盖契约）。
                const auto  option_name = name.substr(pretask_prefix.size());
                const auto* owner = FindOwnerPretask(*catalog, option_name);
                if (owner == nullptr)
                {
                    project_entries.emplace_back(name);
                    continue;
                }
                auto bucket_it = std::find_if(
                    pretask_buckets.begin(),
                    pretask_buckets.end(),
                    [&](const std::pair<
                        const PiPretask*,
                        std::vector<std::string>>& bucket)
                    { return bucket.first == owner; });
                if (bucket_it == pretask_buckets.end())
                {
                    pretask_buckets.emplace_back(
                        owner,
                        std::vector<std::string>{std::string(name)});
                }
                else
                {
                    bucket_it->second.emplace_back(name);
                }
            }
            else if (
                name == "controllerName" || name == "resourceName"
                || name == "presetName" || name == "language"
                || name == "adbDevice" || name.starts_with("adapter."))
            {
                project_entries.emplace_back(name);
            }
            else if (name.starts_with(settings_prefix))
            {
                settings_by_opt.emplace(
                    std::string(name.substr(settings_prefix.size())),
                    std::string(name));
            }
            else if (name.starts_with(schedule_prefix))
            {
                // schedule section：任务级调度配置（schedule.kind /
                // intervalMinutes / dailyTime / dailyCatchUp）。叶子随
                // ProjectAuthoringDocument 按 kind 投影的 fields[] 自然匹配
                // （服务端按 kind 重投影，前端零条件可见性）。
                schedule_entries.emplace_back(name);
            }
            else
            {
                // 兜底桶：未识别前缀的 root field 归项目设置 section，避免
                // layout 漏列触发 ValidateFieldLayout reject。
                project_entries.emplace_back(name);
            }
        }

        auto make_leaf = [](std::string_view field_name) -> yyjson::value
        {
            yyjson::value leaf(Object());
            (*leaf.as_object())[std::string_view("fieldName")] =
                JsonString(field_name);
            return leaf;
        };

        // section defaultExpanded 合并（B 方案）：查
        // ui.values["sectionExpand.<id>"] 是否有持久化偏好。键存在且为 bool →
        // 覆盖原始默认；缺失或非 bool → 用原始默认（cpp_yyjson safe cast，失败
        // fallback 不抛）。前端直接读 defaultExpanded，不解析 raw kv；Get 响应
        // 不加 acceptedProperties（B 方案核心约束）。
        auto resolve_default_expanded = [&ui_values](
                                            std::string_view section_id,
                                            bool original_default) -> bool
        {
            const std::string key = "sectionExpand." + std::string(section_id);
            auto              it = ui_values.find(key);
            if (it == ui_values.end())
            {
                return original_default;
            }
            if (!it->second.is_bool())
            {
                return original_default;
            }
            return it->second.as_bool().value_or(original_default);
        };

        // 项目设置 section。defaultExpanded=true，默认展开（与任务设置 section
        // 一致；"项目设置默认折叠"的旧 UI 行为已废弃）。
        auto emit_project_section = [&]()
        {
            yyjson::value section(Object());
            auto          s_obj = section.as_object();
            (*s_obj)[std::string_view("id")] = JsonString("project");
            const std::string project_label = Translate("section.project");
            (*s_obj)[std::string_view("label")] = JsonString(project_label);
            (*s_obj)[std::string_view("defaultExpanded")] =
                resolve_default_expanded("project", true);
            yyjson::value entries(Array());
            // 当 fields[] 同时含 controllerName + adbDevice（adb controller
            // 才会投影 adbDevice 字段），两字段横排成一行：用户在选 controller
            // 之后立刻看到设备 dropdown，扫描/选择/控制器类型形成一个连贯的
            // 操作组。discoverAdbDevices commandId 叶子由 Plan C Task 2 在
            // commands[] emit 之后追加进此 row。
            const bool has_controller_name = std::find(
                                                 project_entries.begin(),
                                                 project_entries.end(),
                                                 "controllerName")
                                             != project_entries.end();
            const bool has_adb_device = std::find(
                                            project_entries.begin(),
                                            project_entries.end(),
                                            "adbDevice")
                                        != project_entries.end();
            const bool emit_adb_row = has_controller_name && has_adb_device;
            for (const auto& name : project_entries)
            {
                if (emit_adb_row
                    && (name == "controllerName" || name == "adbDevice"))
                {
                    // 已在 row 内 emit，跳过单独 leaf。
                    if (name != "controllerName")
                    {
                        continue;
                    }
                    yyjson::value row(Object());
                    yyjson::value row_arr(Array());
                    row_arr.as_array()->emplace_back(
                        make_leaf("controllerName"));
                    row_arr.as_array()->emplace_back(make_leaf("adbDevice"));
                    // discoverAdbDevices command 叶子：仅 adb controller 投影
                    // 该 command（ProjectAuthoringDocument 经
                    // IsCurrentControllerAdb 守卫 emit），此处引用为 row
                    // 横排第三格，构成 [controllerName, adbDevice,
                    // discoverAdbDevices] 操作组。 validator
                    // 引用完整性（commandId in commands[]）保证前端 按 id
                    // lookup 不会落空。
                    yyjson::value cmd_leaf(Object());
                    (*cmd_leaf.as_object())[std::string_view("commandId")] =
                        JsonString("discoverAdbDevices");
                    row_arr.as_array()->emplace_back(std::move(cmd_leaf));
                    (*row.as_object())[std::string_view("row")] =
                        std::move(row_arr);
                    entries.as_array()->emplace_back(std::move(row));
                    continue;
                }
                entries.as_array()->emplace_back(make_leaf(name));
            }
            (*s_obj)[std::string_view("entries")] = std::move(entries);
            layout_arr->emplace_back(std::move(section));
        };

        // 更新 section：CDK / 代理 / 检查更新通道（可编辑）+ 版本只读字段 +
        // 更新 panel 五字段 + checkUpdate/applyUpdate 命令按钮组（row 横排）。
        // label 走 host i18n（Translate，DAS chrome），随 GlobalLocale 切换。
        // defaultExpanded=true，与项目设置 section 一致。checkUpdate/
        // applyUpdate 命令从根级 commands[] 渲染入口归位到本 section 的 row
        // 横排成按钮组（与字段共 section，避免"按钮都在顶上"）。
        auto emit_update_section = [&]()
        {
            yyjson::value section(Object());
            auto          s_obj = section.as_object();
            (*s_obj)[std::string_view("id")] = JsonString("update");
            const std::string update_label = Translate("section.update");
            (*s_obj)[std::string_view("label")] = JsonString(update_label);
            (*s_obj)[std::string_view("defaultExpanded")] =
                resolve_default_expanded("update", true);
            yyjson::value entries(Array());
            // 命令按钮组置顶作为操作入口：row 横排
            // [{commandId:checkUpdate}, {commandId:applyUpdate}]。commands 定义
            // 仍在根级 commands[] 投影 emit（disabled 状态投影机制保持），
            // 此处只是渲染入口从 doc 根级移到 update section 内。
            {
                yyjson::value row(Object());
                yyjson::value row_arr(Array());
                auto          make_command_leaf = [](std::string_view cid)
                {
                    yyjson::value leaf(Object());
                    auto          leaf_obj = leaf.as_object();
                    (*leaf_obj)[std::string_view("commandId")] =
                        JsonString(cid);
                    return leaf;
                };
                row_arr.as_array()->emplace_back(
                    make_command_leaf("checkUpdate"));
                row_arr.as_array()->emplace_back(
                    make_command_leaf("applyUpdate"));
                (*row.as_object())[std::string_view("row")] =
                    std::move(row_arr);
                entries.as_array()->emplace_back(std::move(row));
            }
            // 版本/日志紧贴按钮 row：检查到更新后用户一眼看到新版本与日志，
            // 再往下才是 CDK/代理/通道等配置与其余字段。纯 layout 顺序改动，
            // 不动 fields[] 声明与字段投影本体（ValidateFieldLayout 只查全
            // 覆盖不查顺序）。
            auto emit_update_leaf_if_present =
                [&entries, &update_entries, &make_leaf](
                    std::string_view field_name)
            {
                if (std::any_of(
                        update_entries.begin(),
                        update_entries.end(),
                        [&field_name](const std::string& entry_name)
                        { return entry_name == field_name; }))
                {
                    entries.as_array()->emplace_back(make_leaf(field_name));
                }
            };
            emit_update_leaf_if_present("updateVersion");
            emit_update_leaf_if_present("releaseNote");
            for (const auto& name : update_entries)
            {
                if (name == "updateVersion" || name == "releaseNote")
                {
                    continue;
                }
                entries.as_array()->emplace_back(make_leaf(name));
            }
            (*s_obj)[std::string_view("entries")] = std::move(entries);
            layout_arr->emplace_back(std::move(section));
        };

        // welcome section：PI 顶层 welcome markdown 公告面板。label 走 host
        // i18n（Translate，DAS chrome），随 GlobalLocale
        // 切换。defaultExpanded=true （首位展示语义，welcome
        // 内容应默认可见，不应折叠）。
        auto emit_welcome_section = [&]()
        {
            yyjson::value section(Object());
            auto          s_obj = section.as_object();
            (*s_obj)[std::string_view("id")] = JsonString("welcome");
            const std::string welcome_label = Translate("section.welcome");
            (*s_obj)[std::string_view("label")] = JsonString(welcome_label);
            (*s_obj)[std::string_view("defaultExpanded")] =
                resolve_default_expanded("welcome", true);
            yyjson::value entries(Array());
            for (const auto& name : welcome_entries)
            {
                entries.as_array()->emplace_back(make_leaf(name));
            }
            (*s_obj)[std::string_view("entries")] = std::move(entries);
            layout_arr->emplace_back(std::move(section));
        };

        // schedule section：任务级调度配置。label 走 host i18n（Translate，DAS
        // chrome），与 section.project/update/welcome 同查表机制（不经 PI 作者
        // 内容 ResolveI18nOrDiag）。叶子复用 make_leaf emit {fieldName} 形状，
        // 随 ProjectAuthoringDocument 按 kind 投影的 schedule.* fields[]
        // 自然匹配（服务端重投影）。
        auto emit_schedule_section = [&]()
        {
            yyjson::value section(Object());
            auto          s_obj = section.as_object();
            (*s_obj)[std::string_view("id")] = JsonString("schedule");
            const std::string schedule_label = Translate("section.schedule");
            (*s_obj)[std::string_view("label")] = JsonString(schedule_label);
            (*s_obj)[std::string_view("defaultExpanded")] =
                resolve_default_expanded("schedule", true);
            yyjson::value entries(Array());
            for (const auto& name : schedule_entries)
            {
                entries.as_array()->emplace_back(make_leaf(name));
            }
            (*s_obj)[std::string_view("entries")] = std::move(entries);
            layout_arr->emplace_back(std::move(section));
        };

        // section emit 顺序：[update(若 top)] → [welcome] → [schedule] →
        // [project] → [update(非 top)] → [settings]。schedule 置于 project 之前
        // （定时是任务级配置，置顶于项目设置之上符合 UI 直觉）。welcome 在
        // update-top 之后，满足面板顺序「检测到更新时更新插到 welcome 之前；
        // 否则 welcome 在首位」（PI V2 welcome 字段置顶语义）。前端按
        // fieldLayout 首现顺序数据驱动渲染，后端 emit 顺序即显示顺序。
        const bool update_first = update_section_top && !update_entries.empty();
        if (update_first)
        {
            emit_update_section();
        }
        if (!welcome_entries.empty())
        {
            emit_welcome_section();
        }
        if (!schedule_entries.empty())
        {
            emit_schedule_section();
        }
        if (!project_entries.empty())
        {
            emit_project_section();
        }
        if (!update_first && !update_entries.empty())
        {
            emit_update_section();
        }

        // 每个 pretask 独立一 section（分区归属 = FindOwnerPretask，与投影
        // /setValue 路由一致）。label 是 pretask 作者内容（dto.label 经
        // ResolveI18nOrDiag，缺译 fallback dto.name / exec——协议 L308-309
        // 的呈现回退顺序）。defaultExpanded=true（选项组通常少，默认展开）。
        for (const auto& [owner_pretask, entry_names] : pretask_buckets)
        {
            yyjson::value     section(Object());
            auto              s_obj = section.as_object();
            const std::string section_id =
                "pretask_" + StablePretaskId(*owner_pretask);
            (*s_obj)[std::string_view("id")] = JsonString(section_id);
            const std::string fallback_name = owner_pretask->dto.name.empty()
                                                  ? owner_pretask->dto.exec
                                                  : owner_pretask->dto.name;
            const std::string_view label_text =
                owner_pretask->dto.label
                    ? std::string_view(*owner_pretask->dto.label)
                    : std::string_view(fallback_name);
            const std::string_view resolved_label = ResolveI18nOrDiag(
                label_text,
                *catalog,
                current_language,
                fallback_name,
                projection_diagnostics,
                "fieldLayout.section." + section_id + ".label");
            (*s_obj)[std::string_view("label")] = JsonString(resolved_label);
            (*s_obj)[std::string_view("defaultExpanded")] =
                resolve_default_expanded(section_id, true);
            yyjson::value entries(Array());
            for (const auto& field_name : entry_names)
            {
                entries.as_array()->emplace_back(make_leaf(field_name));
            }
            (*s_obj)[std::string_view("entries")] = std::move(entries);
            layout_arr->emplace_back(std::move(section));
        }

        // 每个 catalog->settings 元素独立一 section。按 setting.dto.option
        // 顺序，仅当对应 settings.<opt> 字段存在且未被先前 setting 占用时计入
        // （首个引用胜出，镜像 ProjectAuthoringDocument 的 setting 投影去重）。
        // 空分区（无字段）省略，不发空 entries 的 section。
        if (catalog != nullptr)
        {
            std::set<std::string> claimed_keys;
            for (std::size_t i = 0; i < catalog->settings.size(); ++i)
            {
                const auto&   setting = catalog->settings[i];
                yyjson::value entries(Array());
                for (const auto& opt_name : setting.dto.option)
                {
                    auto mit = settings_by_opt.find(opt_name);
                    if (mit == settings_by_opt.end())
                    {
                        continue;
                    }
                    if (!claimed_keys.insert(opt_name).second)
                    {
                        continue;
                    }
                    entries.as_array()->emplace_back(make_leaf(mit->second));
                }
                auto entries_arr = entries.as_array();
                if (!entries_arr || entries_arr->empty())
                {
                    continue;
                }
                yyjson::value     section(Object());
                auto              s_obj = section.as_object();
                const std::string section_id =
                    "setting_"
                    + (setting.dto.name.empty() ? std::to_string(i)
                                                : setting.dto.name);
                (*s_obj)[std::string_view("id")] = JsonString(section_id);
                const std::string_view label_text =
                    setting.dto.label ? std::string_view(*setting.dto.label)
                                      : std::string_view(setting.dto.name);
                const std::string_view resolved_label = ResolveI18nOrDiag(
                    label_text,
                    *catalog,
                    current_language,
                    setting.dto.name,
                    projection_diagnostics,
                    "fieldLayout.section." + section_id + ".label");
                (*s_obj)[std::string_view("label")] =
                    JsonString(resolved_label);
                (*s_obj)[std::string_view("defaultExpanded")] =
                    resolve_default_expanded(
                        section_id,
                        setting.dto.default_expand);
                (*s_obj)[std::string_view("entries")] = std::move(entries);
                layout_arr->emplace_back(std::move(section));
            }
        }

        return layout;
    }

    // 判断当前选中 controller 是否为 Adb 类型。controller_name 为空时回退到
    // catalog 首个 controller（与 CompileMaapi 一致）。catalog 缺席或类型未知
    // 返回 false（不投影 adbDevice 字段，UI 不显示 dropdown）。
    bool IsCurrentControllerAdb(
        const AcceptedSettingsDto& settings,
        const PiCatalog&           catalog)
    {
        const auto* controller = [&]() -> const PiController*
        {
            const auto controller_name =
                settings.pi.controller_name.value_or(std::string{});
            if (!controller_name.empty())
            {
                return FindController(catalog, controller_name);
            }
            return catalog.controllers.empty() ? nullptr
                                               : &catalog.controllers.front();
        }();
        if (controller == nullptr)
        {
            return false;
        }
        const auto& type = controller->dto.type;
        if (type.size() != 3)
        {
            return false;
        }
        auto eq = [](char actual, char expected)
        {
            return std::tolower(static_cast<unsigned char>(actual)) == expected;
        };
        return eq(type[0], 'a') && eq(type[1], 'd') && eq(type[2], 'b');
    }

    // interface_path 的父目录 = 项目目录（interface.json 所在目录，更新目标
    // = 项目目录整体替换）。投影与 dispatch（checkUpdate/applyUpdate 前置
    // 校验、registry IsRunning 键）共用。
    std::optional<std::filesystem::path> ResolvePiDir(
        const AcceptedSettingsDto& settings)
    {
        if (!settings.adapter.interface_path
            || settings.adapter.interface_path->empty())
        {
            return std::nullopt;
        }
        return std::filesystem::path(*settings.adapter.interface_path)
            .parent_path();
    }

    yyjson::value ProjectAuthoringDocument(
        const AcceptedSettingsDto&          settings,
        const PiCatalog*                    catalog,
        const std::vector<PiDiagnosticDto>& diagnostics,
        int64_t                             revision,
        std::vector<PiDiagnosticDto>*       projection_diagnostics,
        const std::optional<PiUpdateInfo>*  pending_update,
        const std::vector<AdbDeviceInfo>*   device_cache)
    {
        yyjson::value document(Object());
        auto          obj = document.as_object();
        // D-84-02/13: document 携带 viewKind 而不是 kind；contractVersion /
        // viewKind / revision 是必需字段；不再泄漏 sourceFingerprint / catalog
        // / kind / diagnostics 等 MaaPI 私有或 legacy 字段。
        (*obj)[std::string_view("contractVersion")] = 1;
        (*obj)[std::string_view("viewKind")] = "formSequence";
        (*obj)[std::string_view("revision")] = revision;

        // 当前 PI language：优先 settings.pi.language；缺省时按
        // zh_cn → 首个可用 language 的顺序兜底。所有 i18n 解析共用此值。
        // SelectPiLanguage 与 PiCompiler 共用同一回退策略，保证同一 catalog
        // 在 envelope 与 document 两侧解析为同一语言。catalog 为 nullptr 时
        // 返回空串（与旧实现的未初始化默认一致）。
        const std::string current_language =
            SelectPiLanguage(settings.pi.language, catalog);

        // D-84-13: 根字段直接挂在 document 上。adapter / executionPolicy 是
        // MaaPI 适配层字段，controllerName / resourceName / presetName /
        // language 是 PI 配置字段，全部本地化 label。
        yyjson::value fields(Array());
        fields.as_array()->emplace_back(MakePropertyField(
            "adapter.interfacePath",
            Translate("field.interfacePath"),
            4,
            0,
            settings.adapter.interface_path
                ? yyjson::value(*settings.adapter.interface_path)
                : yyjson::value(std::string{})));
        // 更新 section 字段（CDK / 代理 / 检查通道）：CDK 前置（MXU 语义，
        // 代理设置在 CDK 之后，同属更新 section）。CDK 仅在通道为
        // mirrorchyan（或未设置）时投影；选 github 时字段消失（fields[] 与
        // fieldLayout 同步不发射 CDK，DasNativeUI 按 document 渲染，零改动）。
        const bool hide_mirrorchyan_cdk =
            settings.adapter.update_channel.has_value()
            && !settings.adapter.update_channel->empty()
            && *settings.adapter.update_channel == "github";
        if (!hide_mirrorchyan_cdk)
        {
            fields.as_array()->emplace_back(MakePropertyField(
                "adapter.mirrorchyanCdk",
                Translate("field.cdk"),
                4,
                0,
                settings.adapter.mirrorchyan_cdk
                    ? yyjson::value(*settings.adapter.mirrorchyan_cdk)
                    : yyjson::value(std::string{})));
        }
        fields.as_array()->emplace_back(MakePropertyField(
            "adapter.proxyUrl",
            Translate("field.proxyAddress"),
            4,
            0,
            settings.adapter.proxy_url
                ? yyjson::value(*settings.adapter.proxy_url)
                : yyjson::value(std::string{})));
        // 检查更新通道（MirrorChyan / GitHub 二选一）。通用 select 字段
        // （valueType=4 string / inputType=1 select，复用 presetName 的
        // candidates 机制），值域 mirrorchyan / github，UI 用通用 select
        // 渲染，业务中立。空或未设置时投影默认 mirrorchyan（现有
        // CheckUpdate 逻辑优先 MirrorChyan）。
        {
            yyjson::value channel_candidates(Array());
            {
                yyjson::value mirror(Object());
                auto          mirror_obj = mirror.as_object();
                (*mirror_obj)[std::string_view("value")] =
                    JsonString("mirrorchyan");
                (*mirror_obj)[std::string_view("label")] =
                    JsonString(Translate("candidate.mirror.mirrorchyan"));
                channel_candidates.as_array()->emplace_back(std::move(mirror));
                yyjson::value github(Object());
                auto          github_obj = github.as_object();
                (*github_obj)[std::string_view("value")] = JsonString("github");
                (*github_obj)[std::string_view("label")] = JsonString("GitHub");
                channel_candidates.as_array()->emplace_back(std::move(github));
            }
            const std::string channel_value =
                settings.adapter.update_channel.value_or("mirrorchyan");
            fields.as_array()->emplace_back(MakePropertyField(
                "adapter.updateChannel",
                Translate("field.updateChannel"),
                4,
                1,
                yyjson::value(channel_value),
                &channel_candidates));
        }
        fields.as_array()->emplace_back(MakePropertyField(
            "adapter.executionPolicy.failFast",
            Translate("field.failFast"),
            8,
            0,
            yyjson::value(settings.adapter.execution_policy.fail_fast)));

        if (catalog)
        {
            // controller/resource/preset/language 是 MaaPI adapter
            // 固定字段（select PI controller/resource/preset/language
            // 实例），label 走 PI i18n 解析； 缺译时回退到 MaaPI
            // 内置中文默认（不依赖 PI i18n，因为这些字段的 UI label 在 MaaPI
            // 层面是固定的），并向 projection_diagnostics 报告 warning
            // 便于开发者补译。
            auto controller_candidates = CandidateArray(
                catalog->controllers,
                catalog,
                current_language,
                projection_diagnostics);
            fields.as_array()->emplace_back(MakePropertyField(
                "controllerName",
                std::string(ResolveI18nOrDiag(
                    "$field.controller",
                    *catalog,
                    current_language,
                    "控制器",
                    projection_diagnostics,
                    "controllerName")),
                4,
                1,
                settings.pi.controller_name
                    ? yyjson::value(*settings.pi.controller_name)
                    : yyjson::value(std::string{}),
                &controller_candidates));
            // adbDevice 字段：仅当前选中 controller 是 Adb 类型时投影
            // （win32/dbg/gamepad controller 不显示 ADB 设备 dropdown）。value
            // 是持久化的 controllerAdbDevice（用户选中的设备 address）；
            // candidates 来自 session device_cache_（discoverAdbDevices 命令填
            // 充，扫描结果不落盘）。cache 为空时 candidates
            // 空数组（前端 dropdown 渲染空候补，字段仍 emit 触发 discover
            // 按钮调用）。
            if (IsCurrentControllerAdb(settings, *catalog))
            {
                yyjson::value device_candidates(Array());
                if (device_cache != nullptr)
                {
                    for (const auto& dev : *device_cache)
                    {
                        yyjson::value candidate(Object());
                        auto          c_obj = candidate.as_object();
                        (*c_obj)[std::string_view("value")] =
                            JsonString(dev.address);
                        const std::string& label =
                            dev.name.empty() ? dev.address : dev.name;
                        (*c_obj)[std::string_view("label")] = JsonString(label);
                        device_candidates.as_array()->emplace_back(
                            std::move(candidate));
                    }
                }
                fields.as_array()->emplace_back(MakePropertyField(
                    "adbDevice",
                    Translate("field.adbDevice"),
                    4,
                    1,
                    yyjson::value(
                        settings.pi.controller_adb_device.value_or("")),
                    &device_candidates));
            }
            auto resource_candidates = CandidateArray(
                catalog->resources,
                catalog,
                current_language,
                projection_diagnostics);
            fields.as_array()->emplace_back(MakePropertyField(
                "resourceName",
                std::string(ResolveI18nOrDiag(
                    "$field.resource",
                    *catalog,
                    current_language,
                    "资源",
                    projection_diagnostics,
                    "resourceName")),
                4,
                1,
                settings.pi.resource_name
                    ? yyjson::value(*settings.pi.resource_name)
                    : yyjson::value(std::string{}),
                &resource_candidates));
            auto preset_candidates = CandidateArray(
                catalog->presets,
                catalog,
                current_language,
                projection_diagnostics);
            // D-84.2-04: 把 Provider 私有 customPresets 合并到 presetName
            // select candidates 作为 {value,label}，与 PI 官方 preset 候选
            // 同形态。customPresets 内容不进入 document 其他 IR。
            for (const auto& custom : settings.pi.custom_presets)
            {
                yyjson::value candidate(Object());
                auto          c_obj = candidate.as_object();
                (*c_obj)[std::string_view("value")] = JsonString(custom.id);
                (*c_obj)[std::string_view("label")] = JsonString(custom.name);
                preset_candidates.as_array()->emplace_back(
                    std::move(candidate));
            }
            fields.as_array()->emplace_back(MakePropertyField(
                "presetName",
                std::string(ResolveI18nOrDiag(
                    "$field.preset",
                    *catalog,
                    current_language,
                    "预设",
                    projection_diagnostics,
                    "presetName")),
                4,
                1,
                settings.pi.preset_name
                    ? yyjson::value(*settings.pi.preset_name)
                    : yyjson::value(std::string{}),
                &preset_candidates));

            yyjson::value language_candidates(Array());
            for (const auto& [lang_code, _] : catalog->translations)
            {
                yyjson::value candidate(Object());
                auto          lang_obj = candidate.as_object();
                (*lang_obj)[std::string_view("value")] = JsonString(lang_code);
                (*lang_obj)[std::string_view("label")] = JsonString(lang_code);
                language_candidates.as_array()->emplace_back(
                    std::move(candidate));
            }
            fields.as_array()->emplace_back(MakePropertyField(
                "language",
                std::string(ResolveI18nOrDiag(
                    "$field.language",
                    *catalog,
                    current_language,
                    "语言",
                    projection_diagnostics,
                    "language")),
                4,
                1,
                yyjson::value(current_language),
                &language_candidates));

            // 游离 global_option（未被任何 setting 引用）不投影成根字段
            // options.<name>。PI v2.8.0：setting 才是"哪些 option 进设置页"的
            // 声明，未声明的 global_option 不渲染为可编辑字段，其默认值改由
            // pipeline_override 合并处理。被 setting 引用的 global_option
            // 仍通过下面的 settings.<opt> 投影路径独占根字段。

            // D-86-04/06: setting 分区的 option 投影为 root FieldDescriptor
            // (settings.<optionKey>)。值取自 global_options 存储（setting 与
            // global_option 共享 global 级合并，D-86-04）。多个 setting 引用
            // 同一 option 键时去重（首个 setting 的引用胜出，与 import
            // global_option 先者为准一致）。
            std::set<std::string> emitted_setting_keys;
            for (const auto& setting : catalog->settings)
            {
                for (const auto& opt_name : setting.dto.option)
                {
                    if (!emitted_setting_keys.insert(opt_name).second)
                    {
                        continue;
                    }
                    const auto* option_dto = FindOption(*catalog, opt_name);
                    if (option_dto == nullptr)
                    {
                        continue;
                    }
                    const auto* current = FindOptionSettings(
                        settings.pi.global_options,
                        opt_name);
                    fields.as_array()->emplace_back(MakeOptionPropertyField(
                        std::string{"settings."} + opt_name,
                        option_dto->dto,
                        current,
                        catalog,
                        current_language));
                }
            }

            // D-86-03/06: pretask 的 option 投影为 root FieldDescriptor
            // (pretask.<optionKey>)。值取自
            // acceptedProperties.pi.pretasks[<id>].options（镜像
            // custom_presets 的私有存储模式）。stable pretask_id 遵循
            // D-86-03（dto.name 非空→name，否则 exec）。多个 pretask 引用
            // 同一 option 键时去重（首个 pretask 的引用胜出）。嵌套 option
            // （case.option）经 EmitPretaskOptionField 按激活 case 递归投影。
            std::set<std::string> emitted_pretask_keys;
            for (const auto& pretask : catalog->pretasks)
            {
                const auto* pretask_settings =
                    FindPretaskSettings(settings, StablePretaskId(pretask));
                for (const auto& opt_name : pretask.dto.option)
                {
                    EmitPretaskOptionField(
                        pretask_settings,
                        catalog,
                        current_language,
                        opt_name,
                        emitted_pretask_keys,
                        fields);
                }
            }
        }

        // D-89-07 修订：版本显示唯一机制 = authoring doc 只读字段（不建专用查询
        // 通道 / 专用 UI 区）。resourceVersion = interface.json 顶层 version
        // （PiCatalog.version 已解析，D-89-07；catalog 为空 → 空串）；
        // maafwVersion = MaaFW MaaVersion() 动态符号（89-02 已解析，
        // C 边界返回 const char* 立即拷贝防 dangling，T-89-03-05；符号未映射 →
        // 空串）。字段名是通用只读字段，UI 不识别 Provider 业务前缀、不硬编码
        // 标题（authoring-doc-layering）；DasNativeUI 渲染器已支持 readonly →
        // TextBlock，零改动。只读字段不经 setValue 写回（UI 不可编辑）。
        const std::string resource_version =
            catalog != nullptr ? catalog->version : std::string{};
        // 字段 label 走 host i18n（DAS chrome），随 GlobalLocale 切换。
        const std::string resource_label = Translate("field.resourceVersion");
        fields.as_array()->emplace_back(MakePropertyField(
            "resourceVersion",
            resource_label,
            4,
            0,
            yyjson::value(resource_version),
            nullptr,
            /*readonly=*/true));
        // maafwVersion = MaaFW MaaVersion() 硬链调用（与 PiCompiler 同源，
        // 跨平台天然正确）。测试构建（DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY）
        // 不链接 MaaFramework，MaaVersion() 不可用，退化为空串。
#ifndef DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY
        const std::string maafw_version = std::string{MaaVersion()};
#else
        const std::string maafw_version{};
#endif
        const std::string maafw_label = Translate("field.maafwVersion");
        fields.as_array()->emplace_back(MakePropertyField(
            "maafwVersion",
            maafw_label,
            4,
            0,
            yyjson::value(maafw_version),
            nullptr,
            /*readonly=*/true));

        // 更新 section 五字段（pending engaged 且有更新时投影，不常驻）。
        // key 传递方案 B：不投影 progress_key/updateKey——SSE 订阅 key 经
        // checkUpdate 响应 commandResult 返回，不污染 document。字段集合在
        // 初始 document 固定（增量只改 value，不增删字段）。
        if (pending_update != nullptr && pending_update->has_value()
            && (*pending_update)->has_update)
        {
            const auto& update_info = **pending_update;

            const std::string current_version =
                catalog != nullptr ? catalog->version : std::string{};
            const std::string target_version = update_info.version_name;
            std::string       version_text;
            if (current_version.empty())
            {
                version_text = target_version;
            }
            else if (target_version.empty())
            {
                version_text = current_version;
            }
            else
            {
                version_text = current_version + " → " + target_version;
            }
            const std::string release_text =
                update_info.release_note.empty()
                    ? Translate("field.releaseNote.empty")
                    : update_info.release_note;

            fields.as_array()->emplace_back(MakePropertyField(
                "updateVersion",
                Translate("field.updateVersion"),
                4,
                0,
                yyjson::value(version_text),
                nullptr,
                /*readonly=*/true));
            // releaseNote：inputType=7=markdown，契约强制
            // readonly（只读展示）。
            fields.as_array()->emplace_back(MakePropertyField(
                "releaseNote",
                Translate("field.releaseNote"),
                4,
                7,
                yyjson::value(release_text),
                nullptr,
                /*readonly=*/true));
            fields.as_array()->emplace_back(MakePropertyField(
                "updateProgress",
                Translate("field.updateProgress"),
                4,
                0,
                yyjson::value(Translate("field.updateProgress.initial")),
                nullptr,
                /*readonly=*/true));
            fields.as_array()->emplace_back(MakePropertyField(
                "downloadRate",
                Translate("field.downloadRate"),
                4,
                0,
                yyjson::value(std::string{}),
                nullptr,
                /*readonly=*/true));
            fields.as_array()->emplace_back(MakePropertyField(
                "downloadElapsed",
                Translate("field.downloadElapsed"),
                4,
                0,
                yyjson::value(std::string{}),
                nullptr,
                /*readonly=*/true));
        }

        // welcome：ProjectInterface V2 顶层静态 catalog 字段。投影为只读
        // markdown field（与 releaseNote 同构三元组 valueType=4 / inputType=7 /
        // readonly=true），复用既有 markdown 渲染链路。welcome 是初始 document
        // 内容（非 apply change），Core schema/validator 零改动。
        // 内容以 '$' 开头时走 ResolveI18nOrDiag 解析 $key；非 '$' 时原样返回。
        // $key 缺译时 ResolveI18nOrDiag 返回 caller fallback（= 原 welcome
        // 文本） 并 emit warning diagnostic——此时 resolved 仍是 $key
        // 字面量，跳过投影 避免用户看到 $key。
        if (catalog != nullptr && catalog->welcome.has_value()
            && !catalog->welcome->empty())
        {
            const std::string_view welcome_source = *catalog->welcome;
            const std::string      welcome_label = Translate("section.welcome");
            const std::string_view welcome_value = ResolveI18nOrDiag(
                welcome_source,
                *catalog,
                current_language,
                welcome_source,
                projection_diagnostics,
                "fields.welcome.value");
            // 缺译保护：welcome 内容是 $key 但 ResolveI18nOrDiag 未能解析（返回
            // 值仍以 '$' 开头，即 caller fallback = 原 $key
            // 字面量）时跳过投影。 ResolveI18nOrDiag 已 emit warning
            // diagnostic，此处只需跳过 emplace。
            if (!welcome_value.starts_with('$'))
            {
                fields.as_array()->emplace_back(MakePropertyField(
                    "welcome",
                    welcome_label,
                    4, // valueType=string
                    7, // inputType=markdown
                    yyjson::value(std::string(welcome_value)),
                    nullptr,
                    /*readonly=*/true));
            }
        }

        // schedule 字段（任务级调度，业务中立通用配置）。kind 恒发（select，
        // 让用户切换 schedule 类型）；intervalMinutes / dailyTime+dailyCatchUp
        // 按 kind 重投影——fields[] 只含当前 kind 的字段，fieldLayout schedule
        // section 经前缀分桶自然匹配（服务端重投影，前端零条件可见性）。
        // dailyCatchUp 缺失默认 true（补跑倾向），与 PiCompiler 编译默认一致。
        {
            const std::string schedule_kind =
                settings.pi.schedule_kind.value_or("");
            // kind select：candidates off/interval/daily。
            yyjson::value kind_candidates(Array());
            for (const std::string_view kv : {"off", "interval", "daily"})
            {
                yyjson::value candidate(Object());
                auto          candidate_obj = candidate.as_object();
                (*candidate_obj)[std::string_view("value")] = JsonString(kv);
                (*candidate_obj)[std::string_view("label")] = JsonString(kv);
                kind_candidates.as_array()->emplace_back(std::move(candidate));
            }
            fields.as_array()->emplace_back(MakePropertyField(
                "schedule.kind",
                Translate("field.scheduleKind"),
                4, // valueType=string
                1, // inputType=select
                yyjson::value(schedule_kind),
                &kind_candidates));
            if (schedule_kind == "interval")
            {
                const int interval_minutes =
                    settings.pi.schedule_interval_minutes.value_or(0);
                fields.as_array()->emplace_back(MakePropertyField(
                    "schedule.intervalMinutes",
                    Translate("field.scheduleInterval"),
                    0, // valueType=int
                    0, // inputType=plain
                    yyjson::value(interval_minutes)));
            }
            else if (schedule_kind == "daily")
            {
                const std::string daily_time =
                    settings.pi.schedule_daily_time.value_or("");
                fields.as_array()->emplace_back(MakePropertyField(
                    "schedule.dailyTime",
                    Translate("field.scheduleDailyTime"),
                    4, // valueType=string "HH:MM"
                    8, // inputType=time
                    yyjson::value(daily_time)));
                const bool catch_up =
                    settings.pi.schedule_daily_catch_up.value_or(true);
                fields.as_array()->emplace_back(MakePropertyField(
                    "schedule.dailyCatchUp",
                    Translate("field.scheduleCatchUp"),
                    8, // valueType=bool
                    0, // inputType=plain
                    yyjson::value(catch_up)));
            }
        }

        // 根级 fieldLayout 必须在 fields 移走前构建（按 const ref 读 fields）。
        // pending engaged → 更新 section 置顶（检测到更新时更新 panel 出现在
        // document 顶部，前端零硬编码）。
        const bool    update_section_top = pending_update != nullptr
                                           && pending_update->has_value()
                                           && (*pending_update)->has_update;
        yyjson::value field_layout = BuildRootFieldLayout(
            fields,
            catalog,
            current_language,
            projection_diagnostics,
            settings.ui.values,
            update_section_top);
        (*obj)[std::string_view("fields")] = std::move(fields);
        (*obj)[std::string_view("fieldLayout")] = std::move(field_layout);

        // D-84-13: formSequence 只承载 items[]；不再有根级 fields/sequence。
        // D-84-40: FormSequenceItem.id = Provider 颁发的 itemId，绝不回退到
        // taskName/actionId/index。D-84-07: settings 已删除，可编辑值只在
        // fields[]。
        yyjson::value items(Array());
        for (const auto& task : settings.pi.tasks)
        {
            yyjson::value item(Object());
            auto          item_obj = item.as_object();
            (*item_obj)[std::string_view("id")] = JsonString(task.item_id);
            (*item_obj)[std::string_view("componentGuid")] =
                JsonString("69F20008-0000-4000-8000-000000000001");
            (*item_obj)[std::string_view("targetKind")] =
                JsonString("componentRef");

            // item.label/description 投影 PI task 的本地化展示文案。前端
            // DasNativeUI 依赖 label 显示 task 卡标题；label 缺失会 fallback
            // 到 id（如 task_1）。优先用 PI i18n 解析 dto.label/description
            // 里的 $key；缺译或无 dto.label 时，label 回退到 task_name（PI
            // 用户可见标识，非空、唯一），description 回退到空字符串。
            // D-84-40 仅约束 id 不能用 taskName 兜底；label 用 task_name
            // 是合理的（label 本就是展示用）。
            if (catalog)
            {
                const auto* catalog_task = FindTask(*catalog, task.task_name);
                const std::string label_path =
                    "formSequence.items." + task.item_id + ".label";
                const std::string desc_path =
                    "formSequence.items." + task.item_id + ".description";
                const std::string_view label_text =
                    catalog_task && catalog_task->dto.label
                        ? std::string_view(*catalog_task->dto.label)
                        : std::string_view{};
                const std::string_view desc_text =
                    catalog_task && catalog_task->dto.description
                        ? std::string_view(*catalog_task->dto.description)
                        : std::string_view{};
                (*item_obj)[std::string_view("label")] =
                    JsonString(ResolveI18nOrDiag(
                        label_text,
                        *catalog,
                        current_language,
                        task.task_name,
                        projection_diagnostics,
                        label_path));
                (*item_obj)[std::string_view("description")] =
                    JsonString(ResolveI18nOrDiag(
                        desc_text,
                        *catalog,
                        current_language,
                        std::string_view{},
                        projection_diagnostics,
                        desc_path));
            }
            else
            {
                // catalog 缺席（无 PI 接口）：label 用 task_name 兜底，
                // description 空。label 仍非空。
                (*item_obj)[std::string_view("label")] =
                    JsonString(task.task_name);
                (*item_obj)[std::string_view("description")] =
                    JsonString(std::string_view{});
            }

            yyjson::value item_fields(Array());
            // D-84-17: 当 item 含 multi-input option 时，必须 emit fieldLayout
            // 覆盖全部字段，并在 multi-input 处用纵向 FieldSectionDescriptor
            // 把每个 input 子字段聚集。否则 84-01 验证器要求 fieldLayout 必须
            // 恰好引用 fields[] 全部字段一次。先扫描是否含 multi-input，再
            // 决定是否 emit layout。
            bool emit_layout = false;
            if (catalog)
            {
                const auto* catalog_task = FindTask(*catalog, task.task_name);
                if (catalog_task)
                {
                    for (const auto& option_name : catalog_task->dto.option)
                    {
                        const auto* option = FindOption(*catalog, option_name);
                        if (option && option->dto.type == "input"
                            && option->dto.inputs.size() > 1)
                        {
                            emit_layout = true;
                            break;
                        }
                    }
                }
            }

            yyjson::value item_layout(Array());
            if (catalog)
            {
                const auto* catalog_task = FindTask(*catalog, task.task_name);
                if (catalog_task)
                {
                    for (const auto& option_name : catalog_task->dto.option)
                    {
                        const auto* option = FindOption(*catalog, option_name);
                        if (!option)
                        {
                            continue;
                        }
                        const auto* current =
                            FindOptionSettings(task.options, option_name);
                        const std::string field_name = "options." + option_name;

                        if (option->dto.type == "input"
                            && option->dto.inputs.size() > 1)
                        {
                            // D-84-17: multi-input 拆为 N 个独立子字段。
                            yyjson::value section_entries(Array());
                            for (const auto& input : option->dto.inputs)
                            {
                                const std::string sub_name =
                                    field_name + "." + input.name;
                                std::string raw;
                                if (current)
                                {
                                    auto it =
                                        current->input_values.find(input.name);
                                    if (it != current->input_values.end())
                                    {
                                        raw = it->second;
                                    }
                                }
                                if (raw.empty() && input.default_value)
                                {
                                    raw = *input.default_value;
                                }
                                item_fields.as_array()->emplace_back(
                                    MakeInputSubField(
                                        sub_name,
                                        input,
                                        raw,
                                        catalog,
                                        current_language));
                                yyjson::value leaf(Object());
                                (*leaf.as_object())[std::string_view(
                                    "fieldName")] = JsonString(sub_name);
                                section_entries.as_array()->emplace_back(
                                    std::move(leaf));
                            }
                            // Section 节点：id 稳定（基于 optionName，单一
                            // 容器内唯一），label 经 i18n 解析。
                            const std::string_view section_label =
                                option->dto.label
                                    ? *option->dto.label
                                    : std::string_view(option->dto.name);
                            const std::string_view resolved_section_label =
                                ResolveI18n(
                                    section_label,
                                    *catalog,
                                    current_language);
                            yyjson::value section(Object());
                            auto          s_obj = section.as_object();
                            (*s_obj)[std::string_view("id")] =
                                JsonString("section_" + option_name);
                            (*s_obj)[std::string_view("label")] =
                                JsonString(resolved_section_label);
                            (*s_obj)[std::string_view("entries")] =
                                std::move(section_entries);
                            item_layout.as_array()->emplace_back(
                                std::move(section));
                            continue;
                        }

                        item_fields.as_array()->emplace_back(
                            MakeOptionPropertyField(
                                field_name,
                                option->dto,
                                current,
                                catalog,
                                current_language));
                        if (emit_layout)
                        {
                            yyjson::value leaf(Object());
                            (*leaf.as_object())[std::string_view("fieldName")] =
                                JsonString(field_name);
                            item_layout.as_array()->emplace_back(
                                std::move(leaf));
                        }
                    }
                }
            }
            (*item_obj)[std::string_view("fields")] = std::move(item_fields);
            if (emit_layout)
            {
                (*item_obj)[std::string_view("fieldLayout")] =
                    std::move(item_layout);
            }
            (*item_obj)[std::string_view("enabled")] = task.enabled;
            // D-84-46: item.debugInfo 是 opaque object，携带 PI task 元数据
            // 便于开发者调试。taskName/entry 不在 label/可见 UI（D-84-45）。
            if (catalog)
            {
                const auto* catalog_task = FindTask(*catalog, task.task_name);
                if (catalog_task)
                {
                    yyjson::value debug_info(Object());
                    auto          d_obj = debug_info.as_object();
                    (*d_obj)[std::string_view("piTaskName")] =
                        JsonString(task.task_name);
                    (*d_obj)[std::string_view("piEntry")] =
                        JsonString(catalog_task->dto.entry);
                    (*item_obj)[std::string_view("debugInfo")] =
                        std::move(debug_info);
                }
            }

            items.as_array()->emplace_back(std::move(item));
        }
        yyjson::value form_sequence(Object());
        (*form_sequence.as_object())[std::string_view("items")] =
            std::move(items);
        (*obj)[std::string_view("formSequence")] = std::move(form_sequence);

        // D-84-31/32/33: actionCatalog 总是出现，便于 UI 在任何状态下都
        // 可以呈现 "添加任务"。Tracer 5 会填入 PI tasks/groups；此处先
        // 给出最小有效结构。entries[] / groups[] 的 label 在 Tracer 5 中
        // 经 i18n 解析。
        yyjson::value action_catalog(Object());
        auto          ac_obj = action_catalog.as_object();
        yyjson::value entries(Array());
        yyjson::value groups(Array());
        if (catalog)
        {
            for (const auto& task : catalog->tasks)
            {
                yyjson::value entry(Object());
                auto          e_obj = entry.as_object();
                (*e_obj)[std::string_view("actionId")] =
                    JsonString(task.dto.name);
                std::string_view label = task.dto.name;
                if (task.dto.label)
                {
                    label = ResolveI18n(
                        *task.dto.label,
                        *catalog,
                        current_language);
                }
                (*e_obj)[std::string_view("label")] = JsonString(label);
                if (task.dto.description)
                {
                    const std::string_view desc = ResolveI18n(
                        *task.dto.description,
                        *catalog,
                        current_language);
                    (*e_obj)[std::string_view("description")] =
                        JsonString(desc);
                }
                if (task.dto.icon)
                {
                    auto resolved = ResolveIcon(*task.dto.icon, catalog);
                    if (!resolved.empty())
                    {
                        (*e_obj)[std::string_view("icon")] =
                            JsonString(resolved);
                    }
                }
                entries.as_array()->emplace_back(std::move(entry));
            }
            for (const auto& group : catalog->groups)
            {
                yyjson::value g(Object());
                auto          g_obj = g.as_object();
                (*g_obj)[std::string_view("id")] = JsonString(group.dto.name);
                std::string_view label = group.dto.name;
                if (group.dto.label)
                {
                    label = ResolveI18n(
                        *group.dto.label,
                        *catalog,
                        current_language);
                }
                (*g_obj)[std::string_view("label")] = JsonString(label);
                if (group.dto.icon)
                {
                    auto resolved = ResolveIcon(*group.dto.icon, catalog);
                    if (!resolved.empty())
                    {
                        (*g_obj)[std::string_view("icon")] =
                            JsonString(resolved);
                    }
                }
                yyjson::value action_ids(Array());
                for (const auto& task_name : catalog->tasks)
                {
                    if (ContainsName(task_name.dto.group, group.dto.name))
                    {
                        action_ids.as_array()->emplace_back(
                            JsonString(task_name.dto.name));
                    }
                }
                (*g_obj)[std::string_view("actionIds")] = std::move(action_ids);
                groups.as_array()->emplace_back(std::move(g));
            }
        }
        (*ac_obj)[std::string_view("entries")] = std::move(entries);
        (*ac_obj)[std::string_view("groups")] = std::move(groups);
        (*obj)[std::string_view("actionCatalog")] = std::move(action_catalog);

        // D-84.2-04: 根级 commands。newPreset 是 D-84-28 通用 invokeCommand
        // 在 MaaPI 的落地（无条件 emit —— preset 创建入口）。UI 触发时由
        // 84-07 注入当前 baseRevision。
        //
        // D-89-04: 追加 checkUpdate / applyUpdate 双按钮。checkUpdate 恒可点；
        // applyUpdate disabled 由后端 pending_update_（checkUpdate 结果暂态）
        // 驱动（Open Question 1 决策：后端状态驱动 commands 投影，前端只按投影
        // disabled 渲染，业务中立）。wire disabled 位于命令对象顶层（89-04
        // 契约"缺失=false"，故仅 disabled 时 emit）。commandId 拼写
        // checkUpdate / applyUpdate 与 89-04 DasNativeUI 锁定一致（Pitfall 8 /
        // T-89-03-02）。
        {
            yyjson::value commands(Array());
            auto          emit_command = [&](std::string_view command_id,
                                             std::string_view label_text,
                                             bool             disabled = false)
            {
                yyjson::value cmd(Object());
                auto          c_obj = cmd.as_object();
                (*c_obj)[std::string_view("id")] = JsonString(command_id);
                (*c_obj)[std::string_view("label")] = JsonString(label_text);
                if (disabled)
                {
                    (*c_obj)[std::string_view("disabled")] = true;
                }
                yyjson::value apply(Object());
                auto          a_obj = apply.as_object();
                (*a_obj)[std::string_view("kind")] =
                    JsonString("invokeCommand");
                yyjson::value payload(Object());
                (*payload.as_object())[std::string_view("commandId")] =
                    JsonString(command_id);
                (*a_obj)[std::string_view("payload")] = std::move(payload);
                (*c_obj)[std::string_view("apply")] = std::move(apply);
                commands.as_array()->emplace_back(std::move(cmd));
            };
            emit_command("newPreset", Translate("command.newPreset"));
            // 命令 label 走 host i18n（DAS chrome），随 GlobalLocale
            // 切换，语言切换时不再硬编码中文。
            const std::string check_label = Translate("command.checkUpdate");
            const std::string apply_label = Translate("command.applyUpdate");
            const std::string discover_label =
                Translate("command.discoverAdbDevices");
            // 后台更新 job 运行中（registry per-pi_dir 状态驱动投影）：两键
            // 都禁用，防 job 期间重复触发；slot idle 后恢复既有条件。
            bool update_running = false;
            if (const auto running_pi_dir = ResolvePiDir(settings))
            {
                update_running =
                    UpdateJobRegistry::Instance().IsRunning(*running_pi_dir);
            }
            emit_command("checkUpdate", check_label, update_running);
            // WR-05: 部分信息路径（MirrorChyan code!=0 但 data.version_name
            // 存在）返回 has_update=true 且 download_url 为空，后端
            // ApplyApplyUpdateCommand 以 "nothing to apply" 拒绝——投影必须与
            // 后端命令可用性一致：无下载链接时按钮 disabled，避免可点但必失败。
            const bool apply_update_disabled =
                (pending_update == nullptr || !pending_update->has_value()
                 || !(*pending_update)->has_update
                 || (*pending_update)->download_url.empty())
                || update_running;
            emit_command("applyUpdate", apply_label, apply_update_disabled);
            // discoverAdbDevices：仅 Adb controller 投影（win32/gamepad 不需
            // 要扫描 ADB 设备）。命令被 controller section fieldLayout row
            // 引用为 {commandId:"discoverAdbDevices"} 叶子，前端 row 横排成
            // 按钮与 controllerName + adbDevice 同行。
            if (catalog != nullptr
                && IsCurrentControllerAdb(settings, *catalog))
            {
                emit_command("discoverAdbDevices", discover_label);
            }
            (*obj)[std::string_view("commands")] = std::move(commands);
        }

        // diagnostics 不在 document 上 (D-84-11：diagnostics 位于 envelope，
        // 和 document 同级)。这里不投影；envelope 由 BuildApplyResult 等调用
        // 方负责。
        (void)diagnostics;
        return document;
    }

    void ApplySetValueChange(
        AcceptedSettingsDto& settings,
        const yyjson::value& change,
        const PiCatalog*     catalog)
    {
        auto obj = change.as_object();
        if (!obj || !obj->contains(std::string_view("payload")))
        {
            return;
        }
        auto payload = (*obj)[std::string_view("payload")].as_object();
        if (!payload)
        {
            return;
        }
        const auto node_id = OptionalString(*payload, "nodeId");
        const auto field = OptionalString(*payload, "field");
        if (node_id && field && payload->contains(std::string_view("value")))
        {
            const auto& item_value = (*payload)[std::string_view("value")];
            const std::string task_name = *node_id;
            auto              ensure_task = [&]() -> MaapiPiTaskSettingsDto*
            {
                auto* existing = FindSelectedTask(settings, task_name);
                if (existing)
                {
                    return existing;
                }
                if (catalog && HasTask(*catalog, task_name))
                {
                    MaapiPiTaskSettingsDto selected;
                    selected.task_name = task_name;
                    // itemId 偶现 bug Layer 2：legacy path 也补 id。
                    selected.item_id = AllocateItemId(settings);
                    settings.pi.tasks.emplace_back(std::move(selected));
                    return &settings.pi.tasks.back();
                }
                return nullptr;
            };
            if (*field == "enabled")
            {
                if (item_value.is_bool())
                {
                    if (auto* task = ensure_task())
                    {
                        task->enabled = item_value.as_bool().value_or(true);
                    }
                }
                return;
            }
            constexpr std::string_view option_field_prefix = "options.";
            if (field->starts_with(option_field_prefix) && catalog)
            {
                auto option_name = field->substr(option_field_prefix.size());
                if (auto* task = ensure_task())
                {
                    if (item_value.is_null())
                    {
                        RemoveOption(task->options, option_name);
                        return;
                    }
                    if (const auto* option = FindOption(*catalog, option_name))
                    {
                        auto parsed = ParseOptionValue(*option, item_value);
                        if (parsed)
                        {
                            RemoveOption(task->options, option_name);
                            UpsertOption(task->options, std::move(*parsed));
                        }
                    }
                }
                return;
            }
            return;
        }
        auto path_opt = OptionalString(*payload, "valuePath");
        if (!path_opt)
        {
            path_opt = OptionalString(*payload, "path");
        }
        auto path = path_opt.value_or("");
        if (path.empty() || !payload->contains(std::string_view("value")))
        {
            return;
        }
        const auto& value = (*payload)[std::string_view("value")];
        if (path == "adapter.interfacePath" && value.is_string())
        {
            settings.adapter.interface_path =
                std::string(value.as_string().value_or(""));
            return;
        }
        if (path == "adapter.proxyUrl" && value.is_string())
        {
            settings.adapter.proxy_url =
                std::string(value.as_string().value_or(""));
            return;
        }
        if (path == "adapter.mirrorchyanCdk" && value.is_string())
        {
            settings.adapter.mirrorchyan_cdk =
                std::string(value.as_string().value_or(""));
            return;
        }
        if (path == "adapter.updateChannel" && value.is_string())
        {
            settings.adapter.update_channel =
                std::string(value.as_string().value_or(""));
            return;
        }
        if (path == "adapter.executionPolicy.failFast" && value.is_bool())
        {
            settings.adapter.execution_policy.fail_fast =
                value.as_bool().value_or(true);
            return;
        }
        if (path == "controllerName" && value.is_string())
        {
            settings.pi.controller_name =
                std::string(value.as_string().value_or(""));
            return;
        }
        if (path == "resourceName" && value.is_string())
        {
            settings.pi.resource_name =
                std::string(value.as_string().value_or(""));
            return;
        }
        if (path == "presetName" && value.is_string())
        {
            const auto name = std::string(value.as_string().value_or(""));
            if (catalog && !name.empty())
            {
                ApplyPreset(settings, *catalog, name);
            }
            else
            {
                settings.pi.preset_name = name;
            }
            return;
        }
        if (path == "language" && value.is_string())
        {
            settings.pi.language = std::string(value.as_string().value_or(""));
            return;
        }
        if (path == "pi.tasks")
        {
            yyjson::value wrapper(Object());
            (*wrapper.as_object())[std::string_view("tasks")] = value;
            settings.pi.tasks =
                ParseTaskSettingsArray(*wrapper.as_object(), "tasks");
            RemoveOrphanPrefix(settings, "pi.tasks:");
            return;
        }
        constexpr std::string_view option_prefix = "options.";
        if (path.starts_with(option_prefix) && catalog)
        {
            const auto option_tail =
                std::string_view(path).substr(option_prefix.size());
            // D-84-17: multi-input 拆分后 sub-field 寻址形如
            // options.{optionName}.{inputName}。找到第一个 '.' 之后，若余下
            // 部分仍是 multi-input option 的 input name，则把值写入对应
            // task option 的 input_values[inputName]。84-07 的严格 setValue
            // payload 还会带 target{kind:formSequenceItem,id,fieldName}，
            // 这里保持 legacy valuePath 兼容路径，让既有 session dispatch
            // 也能在多配置/单配置场景下写回正确实例。
            const auto sub_dot = option_tail.find('.');
            if (sub_dot != std::string_view::npos)
            {
                const auto  option_name = option_tail.substr(0, sub_dot);
                const auto  input_name = option_tail.substr(sub_dot + 1);
                const auto* option = FindOption(*catalog, option_name);
                if (option && option->dto.type == "input"
                    && option->dto.inputs.size() > 1)
                {
                    const bool input_known = std::any_of(
                        option->dto.inputs.begin(),
                        option->dto.inputs.end(),
                        [&](const PiInputDto& inp)
                        { return inp.name == input_name; });
                    if (input_known)
                    {
                        // 单实例 task 找到包含该 option 的 task；找不到则按
                        // catalog 注册的新 task。multi-input sub-field 的 value
                        // 是标量（int/float/string）。
                        auto* task_options = FindTaskOptionContainer(
                            settings,
                            *catalog,
                            option_name);
                        if (task_options)
                        {
                            auto existing = std::find_if(
                                task_options->begin(),
                                task_options->end(),
                                [&](const MaapiPiOptionSettingsDto& o)
                                { return o.option_name == option_name; });
                            std::string str_value;
                            if (value.is_string())
                            {
                                str_value =
                                    std::string(value.as_string().value_or(""));
                            }
                            else if (value.is_sint() || value.is_uint())
                            {
                                const auto n =
                                    value.is_sint()
                                        ? value.as_sint().value_or(0)
                                        : static_cast<std::int64_t>(
                                              value.as_uint().value_or(0));
                                str_value = std::to_string(n);
                            }
                            else if (value.is_real())
                            {
                                str_value = std::to_string(
                                    value.as_real().value_or(0.0));
                            }
                            if (existing == task_options->end())
                            {
                                MaapiPiOptionSettingsDto fresh;
                                fresh.option_name = std::string(option_name);
                                fresh.kind = option->dto.type;
                                fresh.input_values.emplace(
                                    std::string(input_name),
                                    str_value);
                                task_options->emplace_back(std::move(fresh));
                            }
                            else
                            {
                                existing
                                    ->input_values[std::string(input_name)] =
                                    str_value;
                            }
                        }
                        return;
                    }
                }
            }
            ApplyOptionValueChange(settings, *catalog, option_tail, value);
            return;
        }
        constexpr std::string_view task_prefix = "pi.tasks.";
        constexpr std::string_view task_option_separator = ".options.";
        if (path.starts_with(task_prefix) && catalog)
        {
            auto tail = std::string_view(path).substr(task_prefix.size());
            auto option_pos = tail.find(task_option_separator);
            if (option_pos != std::string_view::npos)
            {
                auto task_name = tail.substr(0, option_pos);
                auto option_name =
                    tail.substr(option_pos + task_option_separator.size());
                auto* task = FindSelectedTask(settings, task_name);
                if (!task && HasTask(*catalog, task_name))
                {
                    MaapiPiTaskSettingsDto selected;
                    selected.task_name = std::string(task_name);
                    // itemId 偶现 bug Layer 2：legacy path 也补 id。
                    selected.item_id = AllocateItemId(settings);
                    settings.pi.tasks.emplace_back(std::move(selected));
                    task = &settings.pi.tasks.back();
                }
                if (task)
                {
                    if (value.is_null())
                    {
                        RemoveOption(task->options, option_name);
                        return;
                    }
                    if (const auto* option = FindOption(*catalog, option_name))
                    {
                        auto parsed = ParseOptionValue(*option, value);
                        if (parsed)
                        {
                            RemoveOption(task->options, option_name);
                            UpsertOption(task->options, std::move(*parsed));
                        }
                    }
                }
                return;
            }

            constexpr std::string_view enabled_suffix = ".enabled";
            if (tail.ends_with(enabled_suffix) && value.is_bool())
            {
                auto task_name =
                    tail.substr(0, tail.size() - enabled_suffix.size());
                auto* task = FindSelectedTask(settings, task_name);
                if (!task && HasTask(*catalog, task_name))
                {
                    MaapiPiTaskSettingsDto selected;
                    selected.task_name = std::string(task_name);
                    // itemId 偶现 bug Layer 2：legacy path 也补 id。
                    selected.item_id = AllocateItemId(settings);
                    settings.pi.tasks.emplace_back(std::move(selected));
                    task = &settings.pi.tasks.back();
                }
                if (task)
                {
                    task->enabled = value.as_bool().value_or(true);
                }
                return;
            }
        }
    }

    void ApplyAddSequenceItem(
        AcceptedSettingsDto& settings,
        const PiCatalog*     catalog,
        std::string_view     task_name,
        bool                 enabled)
    {
        if (task_name.empty())
        {
            return;
        }
        if (FindSelectedTask(settings, task_name) != nullptr)
        {
            return;
        }
        if (catalog && HasTask(*catalog, task_name))
        {
            MaapiPiTaskSettingsDto selected;
            selected.task_name = std::string(task_name);
            selected.enabled = enabled;
            // itemId 偶现 bug Layer 2：legacy ApplyAddSequenceItem 也补 id。
            selected.item_id = AllocateItemId(settings);
            settings.pi.tasks.emplace_back(std::move(selected));
            RemoveOrphanPath(
                settings,
                std::string{"pi.tasks:"} + std::string{task_name});
            return;
        }
        settings.pi.orphan_paths.emplace_back(
            std::string{"pi.tasks:"} + std::string{task_name});
    }

    void ApplyMoveSequenceItem(AcceptedSettingsDto& settings, int from, int to)
    {
        const auto size = static_cast<int>(settings.pi.tasks.size());
        if (from < 0 || from >= size || to < 0 || to >= size || from == to)
        {
            return;
        }
        auto item = std::move(settings.pi.tasks[from]);
        settings.pi.tasks.erase(settings.pi.tasks.begin() + from);
        settings.pi.tasks.insert(
            settings.pi.tasks.begin() + to,
            std::move(item));
    }

    void ApplyRemoveSequenceItem(
        AcceptedSettingsDto& settings,
        std::string_view     task_name)
    {
        if (task_name.empty())
        {
            return;
        }
        std::erase_if(
            settings.pi.tasks,
            [&](const MaapiPiTaskSettingsDto& item)
            { return item.task_name == task_name; });
    }

    void ApplyPreset(
        AcceptedSettingsDto& settings,
        const PiCatalog&     catalog,
        std::string_view     preset_name)
    {
        const auto preset = std::find_if(
            catalog.presets.begin(),
            catalog.presets.end(),
            [&](const PiPreset& item) { return item.dto.name == preset_name; });
        if (preset == catalog.presets.end())
        {
            settings.pi.orphan_paths.emplace_back(
                std::string{"presetName:"} + std::string{preset_name});
            return;
        }
        settings.pi.preset_name = std::string(preset_name);
        settings.pi.tasks.clear();
        for (const auto& task : preset->dto.task)
        {
            if (!HasTask(catalog, task.name))
            {
                settings.pi.orphan_paths.emplace_back(
                    std::string{"pi.tasks:"} + task.name);
                continue;
            }
            const auto* catalog_task = FindTask(catalog, task.name);
            if (!catalog_task)
            {
                continue;
            }

            MaapiPiTaskSettingsDto task_settings;
            task_settings.task_name = task.name;
            task_settings.enabled = task.enabled;
            // itemId 偶现 bug Layer 2：preset 展开重建 tasks 时必须为每个
            // 实例分配稳定 item_id，否则投影 doc.item.id 为空，前端报
            // "FormSequenceItem.id must be non-empty"。AllocateItemId 基于
            // settings 当前已用 id 集合分配，避免冲突；preset 展开前的
            // tasks 已被 clear()，故每个实例都拿到全新 id。
            task_settings.item_id = AllocateItemId(settings);

            InitializeOptionDefaults(
                task_settings.options,
                catalog_task->dto.option,
                catalog);

            for (const auto& [option_key, preset_value] : task.option_values)
            {
                auto converted = ConvertPresetOptionValue(
                    option_key,
                    preset_value,
                    catalog,
                    task_settings.options);
                if (converted)
                {
                    UpsertOption(task_settings.options, std::move(*converted));
                }
            }

            settings.pi.tasks.push_back(std::move(task_settings));
        }
    }

    // D-84.2-04: 加载 customPresets 中的用户快照为当前配置（选自定义 preset）。
    // 镜像 ApplyPreset 的"加载 preset 配置"语义，但数据源是 Provider 私有快照；
    // customPresets 数组本身不变（快照不含 customPresets，加载不影响它）。
    void ApplyCustomPreset(
        AcceptedSettingsDto& settings,
        std::string_view     preset_id)
    {
        auto it = std::find_if(
            settings.pi.custom_presets.begin(),
            settings.pi.custom_presets.end(),
            [&](const MaapiPresetSnapshotDto& s) { return s.id == preset_id; });
        if (it == settings.pi.custom_presets.end())
        {
            return;
        }
        settings.pi.controller_name = it->controller_name;
        settings.pi.resource_name = it->resource_name;
        settings.pi.language = it->language;
        settings.pi.global_options = it->global_options;
        settings.pi.resource_options = it->resource_options;
        settings.pi.controller_options = it->controller_options;
        settings.pi.tasks = it->tasks;
        settings.pi.preset_name = std::string(preset_id);
    }

    // ---------------------------------------------------------------------------
    // D-84-07: Strict Phase 84 change dispatch helpers.
    //
    // 每个 handler 严格校验 change payload，在语义成功时修改 settings 并返回
    // true；失败时向 diagnostics 追加一条严格 {severity, message, int_code?,
    // path?} 诊断并返回 false，让 session 放弃 candidate、不递增 revision。
    // ---------------------------------------------------------------------------

    namespace
    {
        void EmitDiagnostic(
            std::vector<PiDiagnosticDto>& diagnostics,
            std::string_view              severity,
            std::string_view              message,
            std::optional<std::int64_t>   int_code = std::nullopt,
            std::string_view              path = {})
        {
            PiDiagnosticDto diag;
            diag.severity = std::string(severity);
            diag.message = std::string(message);
            if (int_code)
            {
                diag.int_code = int_code;
            }
            if (!path.empty())
            {
                diag.path = std::string(path);
            }
            diagnostics.push_back(std::move(diag));
        }

        // 报告 payload 含有未知字段（actual != expected）。返回 true 当且仅当
        // actual 集合等于 expected 集合。
        bool OnlyKeysExact(
            const yyjson::value&          payload,
            std::string_view              context,
            std::vector<std::string>      allowed,
            std::vector<PiDiagnosticDto>& diagnostics)
        {
            auto obj_opt = payload.as_object();
            if (!obj_opt)
            {
                EmitDiagnostic(
                    diagnostics,
                    "error",
                    Translate("diagnostic.payloadMustBeObject", context),
                    DAS_E_INVALID_JSON);
                return false;
            }
            std::set<std::string> actual;
            for (const auto& [key, _] : *obj_opt)
            {
                actual.emplace(key);
            }
            const std::set<std::string> expected(
                allowed.begin(),
                allowed.end());
            if (actual == expected)
            {
                return true;
            }
            std::string allowed_list;
            for (std::size_t i = 0; i < allowed.size(); ++i)
            {
                if (i > 0)
                {
                    allowed_list += ", ";
                }
                allowed_list += allowed[i];
            }
            EmitDiagnostic(
                diagnostics,
                "error",
                Translate(
                    "diagnostic.payloadMustHaveExactly",
                    context,
                    allowed_list),
                DAS_E_INVALID_ARGUMENT);
            return false;
        }

        // 在 object 内取字符串字段；缺失或类型不符返回 nullopt。
        // 接受任意 *_object_base 视图（writer::object_ref /
        // const_object_ref）。
        template <typename TObjectView>
        std::optional<std::string> ObjectStringField(
            const TObjectView& obj,
            std::string_view   key)
        {
            if (!obj.contains(key) || !obj[key].is_string())
            {
                return std::nullopt;
            }
            return std::string(obj[key].as_string().value_or(""));
        }

        MaapiPiTaskSettingsDto* FindTaskByItemId(
            AcceptedSettingsDto& settings,
            std::string_view     item_id)
        {
            auto it = std::find_if(
                settings.pi.tasks.begin(),
                settings.pi.tasks.end(),
                [&](const MaapiPiTaskSettingsDto& task)
                { return task.item_id == item_id; });
            return it == settings.pi.tasks.end() ? nullptr : &*it;
        }

        // D-84-24: setValue 严格 target.kind=document 路径。
        // MaaPI root fields: adapter.interfacePath / adapter.mirrorchyanCdk /
        // adapter.proxyUrl / adapter.updateChannel /
        // adapter.executionPolicy.failFast / controllerName / resourceName /
        // presetName / language / options.{name}。
        bool ApplyDocumentSetValue(
            AcceptedSettingsDto&          settings,
            std::string_view              field_name,
            const yyjson::value&          value,
            const PiCatalog*              catalog,
            std::vector<PiDiagnosticDto>& diagnostics)
        {
            const std::string field(field_name);
            if (field == "adapter.interfacePath")
            {
                if (!value.is_string())
                {
                    EmitDiagnostic(
                        diagnostics,
                        "error",
                        Translate(
                            "diagnostic.valueMustBeString",
                            "adapter.interfacePath"),
                        DAS_E_INVALID_ARGUMENT,
                        field);
                    return false;
                }
                settings.adapter.interface_path =
                    std::string(value.as_string().value_or(""));
                return true;
            }
            if (field == "adapter.proxyUrl")
            {
                if (!value.is_string())
                {
                    EmitDiagnostic(
                        diagnostics,
                        "error",
                        Translate(
                            "diagnostic.valueMustBeString",
                            "adapter.proxyUrl"),
                        DAS_E_INVALID_ARGUMENT,
                        field);
                    return false;
                }
                settings.adapter.proxy_url =
                    std::string(value.as_string().value_or(""));
                return true;
            }
            if (field == "adapter.mirrorchyanCdk")
            {
                if (!value.is_string())
                {
                    EmitDiagnostic(
                        diagnostics,
                        "error",
                        Translate(
                            "diagnostic.valueMustBeString",
                            "adapter.mirrorchyanCdk"),
                        DAS_E_INVALID_ARGUMENT,
                        field);
                    return false;
                }
                settings.adapter.mirrorchyan_cdk =
                    std::string(value.as_string().value_or(""));
                return true;
            }
            if (field == "adapter.updateChannel")
            {
                if (!value.is_string())
                {
                    EmitDiagnostic(
                        diagnostics,
                        "error",
                        Translate(
                            "diagnostic.valueMustBeString",
                            "adapter.updateChannel"),
                        DAS_E_INVALID_ARGUMENT,
                        field);
                    return false;
                }
                const std::string channel =
                    std::string(value.as_string().value_or(""));
                if (!channel.empty() && channel != "mirrorchyan"
                    && channel != "github")
                {
                    EmitDiagnostic(
                        diagnostics,
                        "error",
                        Translate(
                            "diagnostic.valueMustBeOneOfTwo",
                            "adapter.updateChannel",
                            "mirrorchyan",
                            "github"),
                        DAS_E_INVALID_ARGUMENT,
                        field);
                    return false;
                }
                settings.adapter.update_channel = channel;
                return true;
            }
            if (field == "adapter.executionPolicy.failFast")
            {
                if (!value.is_bool())
                {
                    EmitDiagnostic(
                        diagnostics,
                        "error",
                        Translate(
                            "diagnostic.valueMustBeBool",
                            "adapter.executionPolicy.failFast"),
                        DAS_E_INVALID_ARGUMENT,
                        field);
                    return false;
                }
                settings.adapter.execution_policy.fail_fast =
                    value.as_bool().value_or(true);
                return true;
            }
            if (field == "controllerName")
            {
                if (!value.is_string())
                {
                    EmitDiagnostic(
                        diagnostics,
                        "error",
                        Translate(
                            "diagnostic.valueMustBeString",
                            "controllerName"),
                        DAS_E_INVALID_ARGUMENT,
                        field);
                    return false;
                }
                settings.pi.controller_name =
                    std::string(value.as_string().value_or(""));
                return true;
            }
            if (field == "resourceName")
            {
                if (!value.is_string())
                {
                    EmitDiagnostic(
                        diagnostics,
                        "error",
                        Translate(
                            "diagnostic.valueMustBeString",
                            "resourceName"),
                        DAS_E_INVALID_ARGUMENT,
                        field);
                    return false;
                }
                settings.pi.resource_name =
                    std::string(value.as_string().value_or(""));
                return true;
            }
            if (field == "language")
            {
                if (!value.is_string())
                {
                    EmitDiagnostic(
                        diagnostics,
                        "error",
                        Translate("diagnostic.valueMustBeString", "language"),
                        DAS_E_INVALID_ARGUMENT,
                        field);
                    return false;
                }
                settings.pi.language =
                    std::string(value.as_string().value_or(""));
                return true;
            }
            // adbDevice：用户在设备 dropdown 选择的 ADB 设备 address。这是
            // AdbDeviceInfo 六字段中唯一持久化的稳定设备身份（投影端
            // adbDevice.candidates 的 value），其余五字段执行期从扫描结果
            // 取。PI v2 的 controller.adb 不含 address，Adb 控制器执行依赖
            // 本字段。空串合法（清除选择，回退未选状态）。
            if (field == "adbDevice")
            {
                if (!value.is_string())
                {
                    EmitDiagnostic(
                        diagnostics,
                        "error",
                        Translate("diagnostic.valueMustBeString", "adbDevice"),
                        DAS_E_INVALID_ARGUMENT,
                        field);
                    return false;
                }
                const auto address =
                    std::string(value.as_string().value_or(""));
                settings.pi.controller_adb_device =
                    address.empty() ? std::optional<std::string>{}
                                    : std::optional<std::string>{address};
                return true;
            }
            // schedule.* 路由到 acceptedProperties.pi flat optional schedule
            // 字段。 kind 变化触发 ProjectAuthoringDocument 按 kind
            // 重投影（前端 docChanged 重建），无需客户端条件可见性。
            if (field == "schedule.kind")
            {
                if (!value.is_string())
                {
                    EmitDiagnostic(
                        diagnostics,
                        "error",
                        Translate(
                            "diagnostic.valueMustBeString",
                            "schedule.kind"),
                        DAS_E_INVALID_ARGUMENT,
                        field);
                    return false;
                }
                const auto kind = std::string(value.as_string().value_or(""));
                if (kind != "off" && kind != "interval" && kind != "daily")
                {
                    EmitDiagnostic(
                        diagnostics,
                        "error",
                        Translate(
                            "diagnostic.valueMustBeScheduleKind",
                            "schedule.kind"),
                        DAS_E_INVALID_ARGUMENT,
                        field);
                    return false;
                }
                settings.pi.schedule_kind = kind;
                return true;
            }
            if (field == "schedule.intervalMinutes")
            {
                if (!value.is_sint() && !value.is_uint())
                {
                    EmitDiagnostic(
                        diagnostics,
                        "error",
                        Translate(
                            "diagnostic.mustBeInteger",
                            "schedule.intervalMinutes"),
                        DAS_E_INVALID_ARGUMENT,
                        field);
                    return false;
                }
                const auto n = value.is_sint()
                                   ? value.as_sint().value_or(0)
                                   : static_cast<std::int64_t>(
                                         value.as_uint().value_or(0));
                settings.pi.schedule_interval_minutes = static_cast<int>(n);
                return true;
            }
            if (field == "schedule.dailyTime")
            {
                if (!value.is_string())
                {
                    EmitDiagnostic(
                        diagnostics,
                        "error",
                        Translate(
                            "diagnostic.valueMustBeString",
                            "schedule.dailyTime"),
                        DAS_E_INVALID_ARGUMENT,
                        field);
                    return false;
                }
                settings.pi.schedule_daily_time =
                    std::string(value.as_string().value_or(""));
                return true;
            }
            if (field == "schedule.dailyCatchUp")
            {
                if (!value.is_bool())
                {
                    EmitDiagnostic(
                        diagnostics,
                        "error",
                        Translate(
                            "diagnostic.valueMustBeBool",
                            "schedule.dailyCatchUp"),
                        DAS_E_INVALID_ARGUMENT,
                        field);
                    return false;
                }
                settings.pi.schedule_daily_catch_up =
                    value.as_bool().value_or(true);
                return true;
            }
            if (field == "presetName")
            {
                if (!value.is_string())
                {
                    EmitDiagnostic(
                        diagnostics,
                        "error",
                        Translate("diagnostic.valueMustBeString", "presetName"),
                        DAS_E_INVALID_ARGUMENT,
                        field);
                    return false;
                }
                const auto name = std::string(value.as_string().value_or(""));
                if (name.empty())
                {
                    EmitDiagnostic(
                        diagnostics,
                        "error",
                        Translate(
                            "diagnostic.valueMustBeNonEmpty",
                            "presetName"),
                        DAS_E_INVALID_ARGUMENT,
                        field);
                    return false;
                }
                // D-84.2-04: 先查 Provider 私有
                // customPresets（用户创建的快照）；
                // 命中则加载其配置，作为"选自定义 preset"的语义。
                const auto custom_it = std::find_if(
                    settings.pi.custom_presets.begin(),
                    settings.pi.custom_presets.end(),
                    [&](const MaapiPresetSnapshotDto& s)
                    { return s.id == name; });
                if (custom_it != settings.pi.custom_presets.end())
                {
                    ApplyCustomPreset(settings, name);
                    return true;
                }
                // 再查 PI 官方 catalog。
                if (catalog == nullptr)
                {
                    EmitDiagnostic(
                        diagnostics,
                        "error",
                        Translate("diagnostic.presetNameRequiresCatalog"),
                        DAS_E_FAIL,
                        field);
                    return false;
                }
                const auto preset_it = std::find_if(
                    catalog->presets.begin(),
                    catalog->presets.end(),
                    [&](const PiPreset& item)
                    { return item.dto.name == name; });
                if (preset_it == catalog->presets.end())
                {
                    // D-84-10: 未知 preset 是语义失败。session 据此不递增
                    // revision、不修改 acceptedProperties；envelope 携带 error
                    // 诊断。ApplyPreset helper（仍供旧单元测试调用）会写
                    // `presetName:<name>` orphan，但严格 session 路径不走它。
                    EmitDiagnostic(
                        diagnostics,
                        "error",
                        Translate("diagnostic.unknownPresetName", name),
                        DAS_E_NOT_FOUND,
                        field);
                    return false;
                }
                ApplyPreset(settings, *catalog, name);
                return true;
            }
            // options.{name} 或 options.{name}.{input}（多字段 input 子字段）。
            // 委托给既有 ApplyOptionValueChange / 多字段路径处理。
            constexpr std::string_view option_prefix = "options.";
            if (field.starts_with(option_prefix) && catalog)
            {
                ApplyOptionValueChange(
                    settings,
                    *catalog,
                    std::string_view(field).substr(option_prefix.size()),
                    value);
                return true;
            }
            // D-86-04/06: settings.{name} 与 global_option 共享 global_options
            // 存储（D-86-04）。路由到与 options.* 一致的 ApplyOptionValueChange
            // 保证唯一存储位置。
            constexpr std::string_view setting_prefix = "settings.";
            if (field.starts_with(setting_prefix) && catalog)
            {
                ApplyOptionValueChange(
                    settings,
                    *catalog,
                    std::string_view(field).substr(setting_prefix.size()),
                    value);
                return true;
            }
            // D-86-03/06: pretask.{name} 路由到 acceptedProperties.pi.pretasks
            // 私有存储。stable pretask_id 取自首个引用该 option 的 pretask
            // （dto.name 非空→name，否则 exec，与投影端一致）。catalog 缺该
            // option 时报 error 诊断。
            constexpr std::string_view pretask_prefix = "pretask.";
            if (field.starts_with(pretask_prefix) && catalog)
            {
                const auto option_name =
                    std::string_view(field).substr(pretask_prefix.size());
                const auto* option = FindOption(*catalog, option_name);
                if (option == nullptr)
                {
                    EmitDiagnostic(
                        diagnostics,
                        "error",
                        Translate(
                            "diagnostic.setValueFieldReferencesUnknownOption"),
                        DAS_E_INVALID_ARGUMENT,
                        field);
                    return false;
                }
                // 找到首个（含嵌套 option 树）可达该 option 的 pretask 作为
                // 唯一存储载体——嵌套 option（case.option）不在任何
                // pretask.dto.option 顶层列表里，必须沿 option 树找。
                const auto* owner_pretask =
                    FindOwnerPretask(*catalog, option_name);
                if (owner_pretask == nullptr)
                {
                    EmitDiagnostic(
                        diagnostics,
                        "error",
                        Translate(
                            "diagnostic.setValueFieldNotDeclaredOnAnyPretask"),
                        DAS_E_INVALID_ARGUMENT,
                        field);
                    return false;
                }
                const std::string target_pretask_id =
                    StablePretaskId(*owner_pretask);
                if (value.is_null())
                {
                    auto* pretask_settings = FindOrCreatePretaskSettings(
                        settings,
                        target_pretask_id);
                    RemoveOption(pretask_settings->options, option_name);
                    return true;
                }
                auto parsed = ParseOptionValue(*option, value);
                if (!parsed)
                {
                    EmitDiagnostic(
                        diagnostics,
                        "error",
                        Translate("diagnostic.setValueValueTypeIncompatible"),
                        DAS_E_INVALID_ARGUMENT,
                        field);
                    return false;
                }
                auto* pretask_settings =
                    FindOrCreatePretaskSettings(settings, target_pretask_id);
                RemoveOption(pretask_settings->options, option_name);
                UpsertOption(pretask_settings->options, std::move(*parsed));
                return true;
            }
            EmitDiagnostic(
                diagnostics,
                "error",
                Translate("diagnostic.setValueFieldUnknownDocument"),
                DAS_E_INVALID_ARGUMENT,
                field);
            return false;
        }

        // D-84-24/25: setValue 严格 target.kind=formSequenceItem 路径。
        // 内建 fieldName=enabled，或 task 级 options.{name}（按 itemId 找
        // task）。
        bool ApplyFormSequenceItemSetValue(
            AcceptedSettingsDto&          settings,
            std::string_view              item_id,
            std::string_view              field_name,
            const yyjson::value&          value,
            const PiCatalog*              catalog,
            std::vector<PiDiagnosticDto>& diagnostics)
        {
            auto* task = FindTaskByItemId(settings, item_id);
            if (!task)
            {
                EmitDiagnostic(
                    diagnostics,
                    "error",
                    Translate("diagnostic.setValueTargetIdNotFound"),
                    DAS_E_NOT_FOUND,
                    "target.id");
                return false;
            }
            const std::string field(field_name);
            if (field == "enabled")
            {
                if (!value.is_bool())
                {
                    EmitDiagnostic(
                        diagnostics,
                        "error",
                        Translate("diagnostic.valueMustBeBool", "enabled"),
                        DAS_E_INVALID_ARGUMENT,
                        field);
                    return false;
                }
                task->enabled = value.as_bool().value_or(true);
                return true;
            }
            constexpr std::string_view option_prefix = "options.";
            if (field.starts_with(option_prefix) && catalog)
            {
                const auto option_tail =
                    std::string_view(field).substr(option_prefix.size());
                const auto* catalog_task = FindTask(*catalog, task->task_name);
                if (!catalog_task
                    || !ContainsName(catalog_task->dto.option, option_tail))
                {
                    EmitDiagnostic(
                        diagnostics,
                        "error",
                        Translate(
                            "diagnostic.setValueFieldNotDeclaredOnThisTask"),
                        DAS_E_INVALID_ARGUMENT,
                        field);
                    return false;
                }
                const auto* option = FindOption(*catalog, option_tail);
                if (!option)
                {
                    EmitDiagnostic(
                        diagnostics,
                        "error",
                        Translate(
                            "diagnostic.setValueFieldReferencesUnknownOption"),
                        DAS_E_INVALID_ARGUMENT,
                        field);
                    return false;
                }
                if (value.is_null())
                {
                    RemoveOption(task->options, option_tail);
                    return true;
                }
                auto parsed = ParseOptionValue(*option, value);
                if (!parsed)
                {
                    EmitDiagnostic(
                        diagnostics,
                        "error",
                        Translate("diagnostic.setValueValueTypeIncompatible"),
                        DAS_E_INVALID_ARGUMENT,
                        field);
                    return false;
                }
                RemoveOption(task->options, option_tail);
                UpsertOption(task->options, std::move(*parsed));
                return true;
            }
            EmitDiagnostic(
                diagnostics,
                "error",
                Translate("diagnostic.setValueFieldUnknownFormSequenceItem"),
                DAS_E_INVALID_ARGUMENT,
                field);
            return false;
        }
    } // namespace

    bool ApplyStrictSetValueChange(
        AcceptedSettingsDto&          settings,
        const yyjson::value&          payload,
        const PiCatalog*              catalog,
        std::vector<PiDiagnosticDto>& diagnostics)
    {
        if (!OnlyKeysExact(
                payload,
                "setValue",
                {"target", "fieldName", "value"},
                diagnostics))
        {
            return false;
        }
        auto payload_obj_opt = payload.as_object();
        auto payload_obj = *payload_obj_opt;
        // target
        const auto& target_value = payload_obj[std::string_view("target")];
        auto        target_obj_opt = target_value.as_object();
        if (!target_obj_opt
            || !target_obj_opt->contains(std::string_view("kind"))
            || !(*target_obj_opt)[std::string_view("kind")].is_string())
        {
            EmitDiagnostic(
                diagnostics,
                "error",
                Translate("diagnostic.setValueTargetKindRequiredString"),
                DAS_E_INVALID_ARGUMENT,
                "target.kind");
            return false;
        }
        auto       target_obj = *target_obj_opt;
        const auto target_kind = std::string(
            target_obj[std::string_view("kind")].as_string().value_or(""));
        if (target_kind != "document" && target_kind != "formSequenceItem"
            && target_kind != "graphNode")
        {
            EmitDiagnostic(
                diagnostics,
                "error",
                Translate("diagnostic.setValueTargetKindUnsupported"),
                DAS_E_INVALID_ARGUMENT,
                "target.kind");
            return false;
        }
        std::string              target_id;
        std::vector<std::string> allowed_target_keys = {"kind"};
        if (target_kind == "formSequenceItem" || target_kind == "graphNode")
        {
            allowed_target_keys.emplace_back("id");
            std::set<std::string> actual;
            for (const auto& [k, _] : target_obj)
            {
                actual.emplace(k);
            }
            if (actual != std::set<std::string>{"kind", "id"})
            {
                EmitDiagnostic(
                    diagnostics,
                    "error",
                    Translate(
                        "diagnostic.targetMustHaveExactlyKindId",
                        target_kind),
                    DAS_E_INVALID_ARGUMENT,
                    "target");
                return false;
            }
            if (!target_obj[std::string_view("id")].is_string())
            {
                EmitDiagnostic(
                    diagnostics,
                    "error",
                    Translate("diagnostic.mustBeString", "target.id"),
                    DAS_E_INVALID_ARGUMENT,
                    "target.id");
                return false;
            }
            target_id = std::string(
                target_obj[std::string_view("id")].as_string().value_or(""));
        }
        else
        {
            std::set<std::string> actual;
            for (const auto& [k, _] : target_obj)
            {
                actual.emplace(k);
            }
            if (actual != std::set<std::string>{"kind"})
            {
                EmitDiagnostic(
                    diagnostics,
                    "error",
                    Translate("diagnostic.documentTargetMustHaveExactlyKind"),
                    DAS_E_INVALID_ARGUMENT,
                    "target");
                return false;
            }
        }
        // graphNode: MaaPI 没有 graph view，一律拒绝。
        if (target_kind == "graphNode")
        {
            EmitDiagnostic(
                diagnostics,
                "error",
                Translate("diagnostic.graphNodeTargetUnsupported"),
                DAS_E_INVALID_ARGUMENT,
                "target.kind");
            return false;
        }
        // fieldName
        auto field_name = ObjectStringField(payload_obj, "fieldName");
        if (!field_name || field_name->empty())
        {
            EmitDiagnostic(
                diagnostics,
                "error",
                Translate("diagnostic.setValueFieldNameRequired"),
                DAS_E_INVALID_ARGUMENT,
                "fieldName");
            return false;
        }
        // value
        const auto& value = payload_obj[std::string_view("value")];
        if (target_kind == "document")
        {
            return ApplyDocumentSetValue(
                settings,
                *field_name,
                value,
                catalog,
                diagnostics);
        }
        return ApplyFormSequenceItemSetValue(
            settings,
            target_id,
            *field_name,
            value,
            catalog,
            diagnostics);
    }

    bool ApplyAddFormSequenceItem(
        AcceptedSettingsDto&          settings,
        const PiCatalog*              catalog,
        const yyjson::value&          payload,
        std::vector<PiDiagnosticDto>& diagnostics)
    {
        auto payload_obj_opt = payload.as_object();
        if (!payload_obj_opt)
        {
            EmitDiagnostic(
                diagnostics,
                "error",
                Translate("diagnostic.addFormSequenceItemPayloadMustBeObject"),
                DAS_E_INVALID_JSON);
            return false;
        }
        auto payload_obj = *payload_obj_opt;
        // 允许 {actionId} 或 {actionId, insertionIndex}。
        std::set<std::string> actual;
        for (const auto& [k, _] : payload_obj)
        {
            actual.emplace(k);
        }
        const std::set<std::string> allowed_no_index{"actionId"};
        const std::set<std::string> allowed_with_index{
            "actionId",
            "insertionIndex"};
        if (actual != allowed_no_index && actual != allowed_with_index)
        {
            EmitDiagnostic(
                diagnostics,
                "error",
                Translate(
                    "diagnostic.addFormSequenceItemPayloadMustHaveExactly"),
                DAS_E_INVALID_ARGUMENT);
            return false;
        }
        auto action_id = ObjectStringField(payload_obj, "actionId");
        if (!action_id || action_id->empty())
        {
            EmitDiagnostic(
                diagnostics,
                "error",
                Translate("diagnostic.addFormSequenceItemActionIdRequired"),
                DAS_E_INVALID_ARGUMENT,
                "actionId");
            return false;
        }
        std::optional<int> insertion_index;
        if (actual == allowed_with_index)
        {
            const auto& idx_val =
                payload_obj[std::string_view("insertionIndex")];
            if (!idx_val.is_sint() && !idx_val.is_uint())
            {
                EmitDiagnostic(
                    diagnostics,
                    "error",
                    Translate("diagnostic.mustBeInteger", "insertionIndex"),
                    DAS_E_INVALID_ARGUMENT,
                    "insertionIndex");
                return false;
            }
            insertion_index = static_cast<int>(
                idx_val.is_sint()
                    ? idx_val.as_sint().value_or(0)
                    : static_cast<std::int64_t>(idx_val.as_uint().value_or(0)));
        }
        // actionId 必须在 catalog 已注册。
        if (!catalog || !HasTask(*catalog, *action_id))
        {
            EmitDiagnostic(
                diagnostics,
                "error",
                Translate(
                    "diagnostic.addFormSequenceItemActionIdNotRegistered"),
                DAS_E_NOT_FOUND,
                "actionId");
            return false;
        }
        // D-84-34/40: Provider 校验目录、分配实例 ID、生成默认字段。允许同一
        // actionId 被添加多次（不同 itemId）。
        MaapiPiTaskSettingsDto new_task;
        new_task.item_id = AllocateItemId(settings);
        new_task.task_name = std::string(*action_id);
        new_task.enabled = true;
        const auto* catalog_task = FindTask(*catalog, *action_id);
        if (catalog_task)
        {
            InitializeOptionDefaults(
                new_task.options,
                catalog_task->dto.option,
                *catalog);
        }
        RemoveOrphanPath(
            settings,
            std::string{"pi.tasks:"} + std::string{*action_id});
        const auto size = static_cast<int>(settings.pi.tasks.size());
        if (!insertion_index.has_value() || *insertion_index < 0
            || *insertion_index >= size)
        {
            settings.pi.tasks.emplace_back(std::move(new_task));
        }
        else
        {
            settings.pi.tasks.insert(
                settings.pi.tasks.begin() + *insertion_index,
                std::move(new_task));
        }
        return true;
    }

    // uiEvent kind handler：通用 UI 事件 → ui.values kv。
    // payload {type, target, data}（OnlyKeysExact 外层严格；target/data 内部按
    // type）。 当前实现 sectionExpandChanged：target.sectionId + data.expanded
    // → values["sectionExpand.<id>"] = bool。type 驱动；未来加新 type
    // 在此分派， 前端协议不变。不参与 PI 业务语义；ApplyChange 主流程对 uiEvent
    // 短路 document。
    bool ApplyUiEventChange(
        AcceptedSettingsDto&          settings,
        const yyjson::value&          payload,
        std::vector<PiDiagnosticDto>& diagnostics)
    {
        if (!OnlyKeysExact(
                payload,
                "uiEvent",
                {"type", "target", "data"},
                diagnostics))
        {
            return false;
        }
        auto payload_obj = *payload.as_object();
        auto type = ObjectStringField(payload_obj, "type");
        if (!type || type->empty())
        {
            EmitDiagnostic(
                diagnostics,
                "error",
                Translate("diagnostic.uiEventTypeRequired"),
                DAS_E_INVALID_ARGUMENT,
                "type");
            return false;
        }

        const auto& target_val = payload_obj[std::string_view("target")];
        const auto& data_val = payload_obj[std::string_view("data")];

        if (*type == "sectionExpandChanged")
        {
            auto target_obj = target_val.as_object();
            auto section_id = target_obj
                                  ? ObjectStringField(*target_obj, "sectionId")
                                  : std::optional<std::string>{};
            if (!section_id || section_id->empty())
            {
                EmitDiagnostic(
                    diagnostics,
                    "error",
                    Translate(
                        "diagnostic.sectionExpandChangedTargetSectionIdRequired"),
                    DAS_E_INVALID_ARGUMENT,
                    "target.sectionId");
                return false;
            }
            auto data_obj = data_val.as_object();
            if (!data_obj || !data_obj->contains(std::string_view("expanded"))
                || !(*data_obj)[std::string_view("expanded")].is_bool())
            {
                EmitDiagnostic(
                    diagnostics,
                    "error",
                    Translate(
                        "diagnostic.sectionExpandChangedDataExpandedRequiredBool"),
                    DAS_E_INVALID_ARGUMENT,
                    "data.expanded");
                return false;
            }
            bool expanded =
                (*data_obj)[std::string_view("expanded")].as_bool().value_or(
                    true);
            settings.ui.values["sectionExpand." + *section_id] =
                yyjson::value(expanded);
            return true;
        }

        EmitDiagnostic(
            diagnostics,
            "error",
            Translate("diagnostic.uiEventTypeUnsupported", *type),
            DAS_E_INVALID_ARGUMENT,
            "type");
        return false;
    }

    bool ApplyMoveFormSequenceItem(
        AcceptedSettingsDto&          settings,
        const yyjson::value&          payload,
        std::vector<PiDiagnosticDto>& diagnostics)
    {
        if (!OnlyKeysExact(
                payload,
                "moveFormSequenceItem",
                {"formSequenceItemId", "destinationIndex"},
                diagnostics))
        {
            return false;
        }
        auto payload_obj = *payload.as_object();
        auto item_id = ObjectStringField(payload_obj, "formSequenceItemId");
        if (!item_id || item_id->empty())
        {
            EmitDiagnostic(
                diagnostics,
                "error",
                Translate("diagnostic.moveFormSequenceItemIdRequired"),
                DAS_E_INVALID_ARGUMENT,
                "formSequenceItemId");
            return false;
        }
        const auto& idx_val = payload_obj[std::string_view("destinationIndex")];
        if (!idx_val.is_sint() && !idx_val.is_uint())
        {
            EmitDiagnostic(
                diagnostics,
                "error",
                Translate("diagnostic.mustBeInteger", "destinationIndex"),
                DAS_E_INVALID_ARGUMENT,
                "destinationIndex");
            return false;
        }
        const auto destination = static_cast<int>(
            idx_val.is_sint()
                ? idx_val.as_sint().value_or(0)
                : static_cast<std::int64_t>(idx_val.as_uint().value_or(0)));
        const auto size = static_cast<int>(settings.pi.tasks.size());
        int        from = -1;
        for (int i = 0; i < size; ++i)
        {
            if (settings.pi.tasks[i].item_id == *item_id)
            {
                from = i;
                break;
            }
        }
        if (from < 0)
        {
            EmitDiagnostic(
                diagnostics,
                "error",
                Translate("diagnostic.moveFormSequenceItemIdNotFound"),
                DAS_E_NOT_FOUND,
                "formSequenceItemId");
            return false;
        }
        if (destination < 0 || destination >= size)
        {
            EmitDiagnostic(
                diagnostics,
                "error",
                Translate("diagnostic.destinationIndexOutOfRange"),
                DAS_E_OUT_OF_RANGE,
                "destinationIndex");
            return false;
        }
        if (from == destination)
        {
            return true;
        }
        auto moved = std::move(settings.pi.tasks[from]);
        settings.pi.tasks.erase(settings.pi.tasks.begin() + from);
        int adjusted = destination;
        if (from < destination)
        {
            adjusted = destination - 1;
        }
        settings.pi.tasks.insert(
            settings.pi.tasks.begin() + adjusted,
            std::move(moved));
        return true;
    }

    bool ApplyRemoveFormSequenceItem(
        AcceptedSettingsDto&          settings,
        const yyjson::value&          payload,
        std::vector<PiDiagnosticDto>& diagnostics)
    {
        if (!OnlyKeysExact(
                payload,
                "removeFormSequenceItem",
                {"formSequenceItemId"},
                diagnostics))
        {
            return false;
        }
        auto payload_obj = *payload.as_object();
        auto item_id = ObjectStringField(payload_obj, "formSequenceItemId");
        if (!item_id || item_id->empty())
        {
            EmitDiagnostic(
                diagnostics,
                "error",
                Translate("diagnostic.removeFormSequenceItemIdRequired"),
                DAS_E_INVALID_ARGUMENT,
                "formSequenceItemId");
            return false;
        }
        const auto it = std::find_if(
            settings.pi.tasks.begin(),
            settings.pi.tasks.end(),
            [&](const MaapiPiTaskSettingsDto& t)
            { return t.item_id == *item_id; });
        if (it == settings.pi.tasks.end())
        {
            EmitDiagnostic(
                diagnostics,
                "error",
                Translate("diagnostic.removeFormSequenceItemIdNotFound"),
                DAS_E_NOT_FOUND,
                "formSequenceItemId");
            return false;
        }
        // D-84-35: 真实 Provider mutation；按 itemId 精确删除，duplicate task
        // 类型独立保留。同时清理以 `pi.tasks:<taskName>` 形式残留的 orphan
        // path（task 实例不再存在时该 orphan 无意义）。
        const auto removed_task_name = it->task_name;
        settings.pi.tasks.erase(it);
        if (!removed_task_name.empty())
        {
            RemoveOrphanPath(
                settings,
                std::string{"pi.tasks:"} + removed_task_name);
        }
        return true;
    }

    // D-89-03: 网络 seam 解析。测试注入 FakePiHttpClient；生产 seam 为空时
    // 构造 BeastHttpClient（owned 保持存活到本次调用结束）。
    struct ResolvedHttp
    {
        std::unique_ptr<PiHttpClient> owned;
        PiHttpClient*                 value = nullptr;
    };

    ResolvedHttp ResolveHttp(PiHttpClient* seam)
    {
        ResolvedHttp resolved;
        if (seam != nullptr)
        {
            resolved.value = seam;
        }
        else
        {
            resolved.owned = std::make_unique<BeastHttpClient>();
            resolved.value = resolved.owned.get();
        }
        return resolved;
    }

    // 随机不可枚举 SSE 订阅 key：全熵 std::random_device 生成 32 个十六进制
    // 字符（128 bit）。key 是 /authoring/events/{key} 的唯一访问屏障，不用
    // mt19937（非 CSPRNG，收集同进程 624×32bit 输出即可重建状态预测后续
    // key，checkUpdate 天然提供多样本），也不掺可预测的时间戳前缀。
    // checkUpdate 成功响应时创建，前端经 commandResult.data.progressKey 拿到
    // 后拼 SSE URL 订阅；applyUpdate 用同一 key 上报进度。
    std::string GenerateProgressKey()
    {
        std::random_device    rd;
        static constexpr char HEX[] = "0123456789abcdef";
        std::ostringstream    oss;
        for (int i = 0; i < 32; ++i)
        {
            oss << HEX[rd() & 0xF];
        }
        return oss.str();
    }

    // D-89-04: checkUpdate dispatch。调 PiUpdateService::CheckUpdate（读
    // interface.json 顶层 github / mirrorchyan_rid + PiSemver 比较），成功把
    // PiUpdateInfo 暂存 pending_update（commands 投影据此驱动 applyUpdate
    // disabled）。失败 emit 诊断返回 DAS_E_MAAPI_UPDATE_CHECK_FAILED。
    bool ApplyCheckUpdateCommand(
        AcceptedSettingsDto&          settings,
        std::vector<PiDiagnosticDto>& diagnostics,
        std::optional<PiUpdateInfo>*  pending_update,
        PiHttpClient*                 http_seam)
    {
        if (pending_update == nullptr)
        {
            EmitDiagnostic(
                diagnostics,
                "error",
                Translate("diagnostic.checkUpdateRequiresPendingState"),
                DAS_E_INVALID_ARGUMENT,
                "pendingUpdate");
            return false;
        }
        const auto pi_dir = ResolvePiDir(settings);
        if (!pi_dir)
        {
            // IN-02: 复查失败时清空既有 pending_update_（按钮回 disabled），
            // 避免按钮携带过期 has_update/download_url 信息。
            pending_update->reset();
            EmitDiagnostic(
                diagnostics,
                "error",
                Translate("diagnostic.checkUpdateRequiresInterfacePath"),
                DAS_E_MAAPI_UPDATE_CHECK_FAILED,
                "adapter.interfacePath");
            return false;
        }
        auto         http = ResolveHttp(http_seam);
        PiUpdateInfo info;
        const auto   result = PiUpdateService::CheckUpdate(
            *pi_dir,
            *http.value,
            info,
            settings.adapter.proxy_url.value_or(""),
            settings.adapter.mirrorchyan_cdk.value_or(""),
            settings.adapter.update_channel.value_or(""));
        if (IsFailed(result))
        {
            // IN-02: 网络/来源失败时保留旧 pending_update_ 会令 applyUpdate
            // 按钮携带陈旧信息；清空使按钮态与最新检查结果一致。
            pending_update->reset();
            EmitDiagnostic(
                diagnostics,
                "error",
                Translate("diagnostic.checkUpdateFailed"),
                result,
                "checkUpdate");
            return false;
        }
        // 生成随机不可枚举 SSE 订阅 key（经 Apply 响应 commandResult 返回，
        // 不投影 document）。
        info.progress_key = GenerateProgressKey();
        *pending_update = std::move(info);
        return true;
    }

    // applyUpdate 受理异步化 dispatch。受理路径（IPC BusinessThread 上快速
    // 返回）：前置校验（pending engaged + has_update + download_url 非空 +
    // pi_dir 可解析）→ registry TryStart 防重入 → 拷贝 job 数据入后台线程执行
    // PiUpdateService::ApplyUpdate（下载→解压→备份→原子替换），终态经
    // DasProgressReporter::ReportFinal 推送（既有 SSE 字段增量通道）。受理
    // 成功不清 pending_update_（job 完成时 session 可能已被驱逐重建，pending
    // 清理交给下次 checkUpdate 覆盖；按钮防重入由 registry 状态驱动投影）。
    bool ApplyApplyUpdateCommand(
        AcceptedSettingsDto&                       settings,
        std::vector<PiDiagnosticDto>&              diagnostics,
        std::optional<PiUpdateInfo>*               pending_update,
        PiHttpClient*                              http_seam,
        Das::ExportInterface::IDasProgressService* progress_service_seam)
    {
        if (pending_update == nullptr || !pending_update->has_value())
        {
            EmitDiagnostic(
                diagnostics,
                "error",
                Translate("diagnostic.applyUpdateRequiresPendingUpdate"),
                DAS_E_MAAPI_UPDATE_APPLY_FAILED,
                "applyUpdate");
            return false;
        }
        const auto& info = **pending_update;
        if (!info.has_update || info.download_url.empty())
        {
            EmitDiagnostic(
                diagnostics,
                "error",
                Translate("diagnostic.applyUpdateNothingToApply"),
                DAS_E_MAAPI_UPDATE_APPLY_FAILED,
                "applyUpdate");
            return false;
        }
        const auto pi_dir = ResolvePiDir(settings);
        if (!pi_dir)
        {
            EmitDiagnostic(
                diagnostics,
                "error",
                Translate("diagnostic.applyUpdateRequiresInterfacePath"),
                DAS_E_MAAPI_UPDATE_APPLY_FAILED,
                "adapter.interfacePath");
            return false;
        }

        // http 归 shared_ptr 持有：slot 持有保证 registry 析构收尾时
        // RequestCancel 可达。测试注入的 fake 由测试侧 shared_ptr 持有、
        // .get() 喂 seam，此处构造非持有别名（无操作 deleter），杜绝双重
        // 释放。
        std::shared_ptr<PiHttpClient> http;
        if (http_seam != nullptr)
        {
            http =
                std::shared_ptr<PiHttpClient>(http_seam, [](PiHttpClient*) {});
        }
        else
        {
            http = std::make_shared<BeastHttpClient>();
        }

        auto& registry = UpdateJobRegistry::Instance();
        auto  slot = registry.TryStart(*pi_dir, http);
        if (slot == nullptr)
        {
            EmitDiagnostic(
                diagnostics,
                "error",
                Translate("diagnostic.applyUpdateAlreadyRunning"),
                DAS_E_MAAPI_UPDATE_APPLY_FAILED,
                "applyUpdate");
            return false;
        }

        // 受理成功后在后台线程执行；TryStart 之后任何异常都必须回滚
        // running 标记，否则该 pi_dir 的更新入口被永久锁死。
        try
        {
            // 进度上报：seam（测试注入）优先，生产经 GetCurrentIpcContext
            // Resolve 主进程 IDasProgressService。GetCurrentIpcContext 为空 /
            // resolve 失败 / Start 失败 → 降级纯本地更新（不报进度），日志
            // 警告。Start 必须留在受理路径、线程构造之前——started_ 非
            // atomic，线程构造建立 happens-before 后 job 线程只读。
            DasPtr<Das::ExportInterface::IDasProgressService> progress_service;
            if (progress_service_seam != nullptr)
            {
                progress_service =
                    DasPtr<Das::ExportInterface::IDasProgressService>(
                        progress_service_seam);
            }
            else if (
                auto* ctx = Das::Core::IPC::GetCurrentIpcContext();
                ctx != nullptr)
            {
                DasPtr<IDasBase> base;
                const auto       rc = ctx->ResolveMainProcessInterfaceByName(
                    "das.progress",
                    base.Put());
                if (DAS::IsOk(rc) && base)
                {
                    (void)base.As(progress_service);
                }
            }
            if (!progress_service)
            {
                DAS_LOG_WARNING(
                    "ApplyUpdate: progress service unavailable, degrade to "
                    "local update");
            }

            // 更新期间真实经 IPC 上报字段增量（成对
            // AddCallback/RemoveCallback）。 shared_ptr
            // 管理：DasProgressCallbackImpl 内部持 weak_ptr 访问 owner（跨进程
            // proxy 保活使其可能活得比 reporter 久），且析构 RAII 兜底
            // RemoveCallback。
            std::shared_ptr<DasProgressReporter> reporter;
            if (progress_service && pending_update->has_value()
                && !(*pending_update)->progress_key.empty())
            {
                reporter = std::make_shared<DasProgressReporter>(
                    (*pending_update)->progress_key,
                    progress_service);
                if (!reporter->Start())
                {
                    reporter.reset();
                }
            }
            ProgressReporter progress_cb;
            if (reporter)
            {
                progress_cb = [reporter](const UpdateProgress& p)
                { reporter->Report(p); };
            }

            // job 全量值拷贝：job 线程绝不触碰 session / settings /
            // diagnostics 等受理栈对象（session 可能在 job 期间被驱逐销毁）。
            const PiUpdateInfo          job_info = info;
            const std::filesystem::path job_pi_dir = *pi_dir;
            const std::string           proxy_url =
                settings.adapter.proxy_url.value_or("");
            const std::string mirrorchyan_cdk =
                settings.adapter.mirrorchyan_cdk.value_or("");

            slot->worker = std::thread(
                [slot,
                 job_pi_dir,
                 job_info,
                 http,
                 proxy_url,
                 mirrorchyan_cdk,
                 progress_cb,
                 reporter]()
                {
                    // 后台线程绝不让异常逃逸（未捕获 → std::terminate 杀死
                    // 整个 Host 进程）；filesystem 等异常就地映射失败终态。
                    const auto result = [&]()
                    {
                        try
                        {
                            return PiUpdateService::ApplyUpdate(
                                job_pi_dir,
                                job_info,
                                *http,
                                proxy_url,
                                mirrorchyan_cdk,
                                progress_cb);
                        }
                        catch (const std::exception& error)
                        {
                            DAS_LOG_ERROR(
                                DAS::fmt::format(
                                    "ApplyUpdate: background update job "
                                    "exception version={} what={}",
                                    job_info.version_name,
                                    error.what())
                                    .c_str());
                            return DAS_E_FAIL;
                        }
                        catch (...)
                        {
                            DAS_LOG_ERROR(
                                "ApplyUpdate: background update job unknown "
                                "exception");
                            return DAS_E_FAIL;
                        }
                    }();
                    if (IsOk(result))
                    {
                        if (reporter)
                        {
                            reporter->ReportFinal(true, job_info.version_name);
                        }
                    }
                    else
                    {
                        if (reporter)
                        {
                            const std::string failure_detail = DAS::fmt::format(
                                "result={}",
                                static_cast<int>(result));
                            reporter->ReportFinal(false, failure_detail);
                        }
                        DAS_LOG_ERROR(
                            DAS::fmt::format(
                                "ApplyUpdate: background update job failed "
                                "version={} result={}",
                                job_info.version_name,
                                static_cast<int>(result))
                                .c_str());
                    }
                    if (reporter)
                    {
                        reporter->Stop();
                    }
                    UpdateJobRegistry::Instance().Finish(slot);
                });
        }
        catch (...)
        {
            registry.Finish(slot);
            EmitDiagnostic(
                diagnostics,
                "error",
                Translate("diagnostic.applyUpdateFailed"),
                DAS_E_MAAPI_UPDATE_APPLY_FAILED,
                "applyUpdate");
            return false;
        }
        // 受理成功。pending_update_ 不清：job 完成时 session 可能已被
        // Get-after- Apply 驱逐重建，内存清理无意义；pending 的清理由下次
        // checkUpdate 覆盖（缓存失效经 TryParseCatalog
        // 内容感知重解析自然生效）。
        return true;
    }

    // discoverAdbDevices dispatch：调 AdbDeviceFinder::FindAllAdbDevices 扫描
    // 设备 → 填 session.device_cache_（session 内存态，与 pending_update_
    // 同范式不落盘）。设备列表随后经 ProjectAuthoringDocument 投影到
    // adbDevice.candidates（不走 commandResult.data，设备走 document field
    // candidates — 前端 dropdown 现成机制）。零设备返回 ok=true（扫描成功
    // 但无设备，candidates 投影空数组，前端 dropdown 空候补）。
    bool ApplyDiscoverAdbDevicesCommand(
        MaapiAuthoringSession&        session,
        std::vector<PiDiagnosticDto>& diagnostics,
        const AdbDeviceEnumerator&    adb_enumerator)
    {
        const auto enumerator =
            adb_enumerator ? adb_enumerator : DefaultAdbDeviceEnumerator();
        session.device_cache_ = FindAllAdbDevices(enumerator);
        // 诊断区分 toolkit 不可用（warning）vs 扫描成功但 0 设备（info）。
        // 两者都不阻断命令成功（ok=true）— 降级不崩契约保持，但诊断明确
        // 让操作员分辨"MaaToolkit 坏了"与"真没连设备"。
        if (session.device_cache_.empty())
        {
            if (!IsAdbToolkitAvailable())
            {
                EmitDiagnostic(
                    diagnostics,
                    "warning",
                    Translate(
                        "diagnostic.discoverAdbDevicesToolkitUnavailable"),
                    DAS_S_OK,
                    "discoverAdbDevices");
            }
            else
            {
                EmitDiagnostic(
                    diagnostics,
                    "info",
                    Translate("diagnostic.discoverAdbDevicesNoDevices"),
                    DAS_S_OK,
                    "discoverAdbDevices");
            }
        }
        return true;
    }

    InvokeCommandResult ApplyInvokeCommandChange(
        MaapiAuthoringSession&                     session,
        AcceptedSettingsDto&                       settings,
        const yyjson::value&                       payload,
        std::vector<PiDiagnosticDto>&              diagnostics,
        PiHttpClient*                              http_seam,
        Das::ExportInterface::IDasProgressService* progress_service_seam,
        AdbDeviceEnumerator                        adb_enumerator)
    {
        InvokeCommandResult result;
        if (!OnlyKeysExact(
                payload,
                "invokeCommand",
                {"commandId"},
                diagnostics))
        {
            return result;
        }
        auto payload_obj = *payload.as_object();
        auto command_id = ObjectStringField(payload_obj, "commandId");
        if (!command_id || command_id->empty())
        {
            EmitDiagnostic(
                diagnostics,
                "error",
                Translate("diagnostic.invokeCommandIdRequired"),
                DAS_E_INVALID_ARGUMENT,
                "commandId");
            return result;
        }
        // D-84.2-04: newPreset 是 D-84-28 通用 invokeCommand 在 MaaPI 的落地。
        // 把当前 flat pi 字段镜像为 snapshot 追加到 pi.custom_presets。
        if (*command_id == "newPreset")
        {
            ApplyNewPreset(settings);
            result.ok = true;
            return result;
        }
        // D-89-04 / T-89-03-01: checkUpdate / applyUpdate 白名单分支。commandId
        // 拼写 camelCase 与 89-04 DasNativeUI 锁定（T-89-03-02）。未知
        // commandId 仍走下方 DAS_E_NOT_FOUND 拒绝。
        if (*command_id == "checkUpdate")
        {
            // session& 引用直读写 pending_update_（零 T* 出参指针 — DasRef/
            // DasOutPtr 全量替换 T** 出参的风格）。helper 仍接 raw 指针（既有
            // 范式），传 session 成员地址作为 dispatch 内部实现细节，对调用
            // 方暴露的只是 InvokeCommandResult 结构体返回。
            result.ok = ApplyCheckUpdateCommand(
                settings,
                diagnostics,
                &session.pending_update_,
                http_seam);
            // checkUpdate 成功 + pending_update 有 progress_key → 自填
            // command_result {commandId, data:{progressKey}}（SSE 订阅 key，
            // 随机不可枚举；经 commandResult 返回，不投影 document）。
            if (result.ok && session.pending_update_.has_value()
                && !session.pending_update_->progress_key.empty())
            {
                yyjson::value cr(Das::Utils::MakeYyjsonObject());
                auto          cr_obj = cr.as_object();
                (*cr_obj)[std::string_view("commandId")] =
                    JsonString("checkUpdate");
                yyjson::value data(Das::Utils::MakeYyjsonObject());
                auto          data_obj = data.as_object();
                (*data_obj)[std::string_view("progressKey")] =
                    JsonString(session.pending_update_->progress_key);
                (*cr_obj)[std::string_view("data")] = std::move(data);
                result.command_result = std::move(cr);
            }
            return result;
        }
        if (*command_id == "applyUpdate")
        {
            result.ok = ApplyApplyUpdateCommand(
                settings,
                diagnostics,
                &session.pending_update_,
                http_seam,
                progress_service_seam);
            // 受理成功自填 command_result {commandId, data:{accepted:true}}
            // （前端分流：应用 document 但不关 SSE、保留 progressKey——下载
            // 在后台执行，终态经既有 SSE 字段增量通道推送）。
            if (result.ok)
            {
                yyjson::value cr(Das::Utils::MakeYyjsonObject());
                auto          cr_obj = cr.as_object();
                (*cr_obj)[std::string_view("commandId")] =
                    JsonString("applyUpdate");
                yyjson::value data(Das::Utils::MakeYyjsonObject());
                auto          data_obj = data.as_object();
                (*data_obj)[std::string_view("accepted")] = true;
                (*cr_obj)[std::string_view("data")] = std::move(data);
                result.command_result = std::move(cr);
            }
            return result;
        }
        // discoverAdbDevices：扫描设备填 cache，command_result nullopt（设备
        // 进 document field candidates，不走 commandResult.data）。
        if (*command_id == "discoverAdbDevices")
        {
            result.ok = ApplyDiscoverAdbDevicesCommand(
                session,
                diagnostics,
                adb_enumerator);
            result.command_result = std::nullopt;
            return result;
        }
        EmitDiagnostic(
            diagnostics,
            "error",
            Translate("diagnostic.invokeCommandIdNotRegistered"),
            DAS_E_NOT_FOUND,
            "commandId");
        return result;
    }

    // D-84-29: MaaPI 只支持 formSequence；convertView 一律拒绝。
    bool ApplyConvertViewChange(
        const yyjson::value&          payload,
        std::vector<PiDiagnosticDto>& diagnostics)
    {
        if (!OnlyKeysExact(
                payload,
                "convertView",
                {"targetViewKind"},
                diagnostics))
        {
            return false;
        }
        EmitDiagnostic(
            diagnostics,
            "error",
            Translate("diagnostic.convertViewUnsupported"),
            DAS_E_INVALID_ARGUMENT,
            "targetViewKind");
        return false;
    }
} // namespace Das::Plugins::DasMaaPi

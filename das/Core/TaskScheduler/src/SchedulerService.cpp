#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/ErrorLensManager.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/PluginScanner.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/TaskScheduler/SchedulerService.h>
#include <das/Core/TaskScheduler/TaskAuthoringContract.h>
#include <das/Core/Utils/DasJsonImpl.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/IDasBase.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/PathText.h>
#include <das/Utils/StringUtils.h>
#include <das/_autogen/idl/abi/IDasLogRecorder.h>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <das/_autogen/idl/abi/IDasTask.h>
#include <das/_autogen/idl/abi/IDasTaskAuthoring.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <ranges>
#include <sstream>
#include <thread>
#include <unordered_set>

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_SML_WARNING
#include <boost/sml.hpp>
DAS_DISABLE_WARNING_END

namespace Das::Core::TaskScheduler
{

    // boost::sml 转换表。guard lambda 用圆括号包成 event<E>[( [](auto&){...}
    // )]： sml 的 guard 方括号与 lambda 捕获的 [ 会连成 [[，被 C++
    // 当成属性语法拒绝， 中间塞一对 () 就隔开了。
    auto SchedulerService::FsmDefinition::operator()()
    {
        return boost::sml::make_transition_table(
            *boost::sml::state<class Stopped>
                + boost::sml::event<EvtEnable>[(
                      [](SchedulerService& s) -> bool
                      { return s.FsmGuardNotInitialized(); })]
                      / [](SchedulerService& s)
                      { s.FsmActRejectNotInitialized(); },
            boost::sml::state<class Stopped>
                + boost::sml::event<EvtEnable>[(
                      [](SchedulerService& s) -> bool
                      { return s.FsmGuardNoAvailableTask(); })]
                      / [](SchedulerService& s) { s.FsmActRejectNoTask(); },
            boost::sml::state<class Stopped>
                + boost::sml::event<EvtEnable> / [](SchedulerService& s)
                      { s.FsmActEnterRunning(); } =
                boost::sml::state<class Running>,
            boost::sml::state<class Stopped>
                + boost::sml::event<EvtDisable> / [](SchedulerService& s)
                      { s.FsmActRejectFail(); },
            boost::sml::state<class Stopped>
                + boost::sml::event<EvtTickCompleted>
                      / [](SchedulerService&) {},
            boost::sml::state<class Running>
                + boost::sml::event<EvtEnable> / [](SchedulerService& s)
                      { s.FsmActRejectFail(); },
            boost::sml::state<class Running>
                + boost::sml::event<EvtDisable> / [](SchedulerService& s)
                      { s.FsmActEnterStopping(); } =
                boost::sml::state<class Stopping>,
            boost::sml::state<class Running>
                + boost::sml::event<EvtTickCompleted>
                      / [](SchedulerService&) {},
            boost::sml::state<class Stopping>
                + boost::sml::event<EvtEnable> / [](SchedulerService& s)
                      { s.FsmActRejectFail(); },
            boost::sml::state<class Stopping>
                + boost::sml::event<EvtDisable> / [](SchedulerService& s)
                      { s.FsmActRejectIgnored(); },
            boost::sml::state<class Stopping>
                + boost::sml::event<EvtTickCompleted> / [](SchedulerService& s)
                      { s.FsmActEnterStopped(); } =
                boost::sml::state<class Stopped>);
    }

    using Das::Core::ForeignInterfaceHost::FindManifest;
    using Das::Core::ForeignInterfaceHost::PluginPackageDesc;
    using Das::Core::ForeignInterfaceHost::PluginSettingDesc;
    using Das::Core::ForeignInterfaceHost::TaskDescriptor;

    namespace
    {
        using SystemClock = std::chrono::system_clock;

        // Sentinel far-future timestamp used when a task's
        // GetNextExecutionTime() fails. Prevents zero-delay hot loops by
        // pushing the next run far into the future.
        // 2099-12-31T23:59:59 UTC
        static constexpr int64_t SENTINEL_FUTURE_TIMESTAMP = 4102444799LL;

        time_t TimegmPortable(std::tm* tm)
        {
#ifdef _WIN32
            return _mkgmtime(tm);
#else
            return timegm(tm);
#endif
        }

        int64_t DasDateToUnix(const Das::ExportInterface::DasDate& date)
        {
            std::tm tm{};
            tm.tm_year = date.year - 1900;
            tm.tm_mon = date.month - 1;
            tm.tm_mday = date.day;
            tm.tm_hour = date.hour;
            tm.tm_min = date.minute;
            tm.tm_sec = date.second;
            tm.tm_isdst = -1;
            return static_cast<int64_t>(TimegmPortable(&tm));
        }

        std::chrono::steady_clock::duration ClampNextDelay(
            const std::chrono::seconds& delay)
        {
            if (delay <= std::chrono::seconds::zero())
            {
                return std::chrono::steady_clock::duration::zero();
            }
            return std::chrono::duration_cast<
                std::chrono::steady_clock::duration>(delay);
        }

        std::optional<PluginPackageDesc> ReadPluginManifestDesc(
            const std::filesystem::path& manifest_path)
        {
            const std::ifstream ifs(manifest_path);
            if (!ifs.is_open())
            {
                return std::nullopt;
            }

            std::stringstream ss;
            ss << ifs.rdbuf();
            auto parsed = Das::Utils::ParseYyjsonFromString(
                ss.str(),
                yyjson::ReadFlag::AllowComments
                    | yyjson::ReadFlag::AllowTrailingCommas);
            if (!parsed)
            {
                return std::nullopt;
            }

            const auto& const_value = *parsed;
            auto        obj = const_value.as_object();
            if (!obj)
            {
                return std::nullopt;
            }

            PluginPackageDesc desc;
            ParsePluginPackageDescFromJson(*obj, desc);
            return desc;
        }
    } // namespace

    // ----------------------------------------------------------------
    // Helpers
    // ----------------------------------------------------------------

    static std::string GuidToString(const DasGuid& guid)
    {
        return DAS_FMT_NS::format("{}", guid);
    }

    static std::chrono::steady_clock::duration ComputeNextDelay(
        const std::vector<TaskInstanceRecord>& task_instances,
        int64_t                                now_unix)
    {
        bool    has_future_due = false;
        int64_t earliest_due = 0;

        for (const auto& inst : task_instances)
        {
            if (!inst.enabled
                || inst.availability != TaskAvailability::Available
                || !inst.task_instance)
            {
                continue;
            }

            if (!inst.next_execution_time)
            {
                return std::chrono::steady_clock::duration::zero();
            }

            const int64_t due = *inst.next_execution_time;

            if (due <= now_unix)
            {
                return std::chrono::steady_clock::duration::zero();
            }

            if (!has_future_due || due < earliest_due)
            {
                earliest_due = due;
                has_future_due = true;
            }
        }

        if (!has_future_due)
        {
            return std::chrono::seconds(1);
        }

        return ClampNextDelay(std::chrono::seconds(earliest_due - now_unix));
    }

    template <typename Json>
    static yyjson::value CloneJsonValue(const Json& value)
    {
        return Das::Utils::CloneYyjsonValue(value);
    }

    static DasPtr<Das::ExportInterface::IDasJson> WrapJsonValue(
        yyjson::value value)
    {
        return Das::MakeDasPtr<Das::Core::Utils::IDasJsonImpl>(
            std::move(value));
    }

    static yyjson::value ReadJsonInterface(Das::ExportInterface::IDasJson* json)
    {
        if (!json)
        {
            return Das::Utils::MakeYyjsonObject();
        }

        DasPtr<IDasReadOnlyString> text;
        if (DAS::IsFailed(json->ToString(0, text.Put())) || !text)
        {
            return Das::Utils::MakeYyjsonObject();
        }

        const char* raw = nullptr;
        if (DAS::IsFailed(text->GetUtf8(&raw)) || raw == nullptr)
        {
            return Das::Utils::MakeYyjsonObject();
        }

        auto parsed = Das::Utils::ParseYyjsonFromString(raw);
        if (!parsed)
        {
            return Das::Utils::MakeYyjsonObject();
        }
        return std::move(*parsed);
    }

    static yyjson::value MakeAuthoringResponseError(
        std::string_view     kind,
        std::string_view     message,
        const yyjson::value* diagnostics = nullptr)
    {
        yyjson::value result(Das::Utils::MakeYyjsonObject());
        auto          obj = result.as_object();
        (*obj)[std::string_view("ok")] = false;
        (*obj)[std::string_view("errorKind")] = std::string(kind);
        (*obj)[std::string_view("message")] = std::string(message);
        if (diagnostics != nullptr && diagnostics->as_array().has_value())
        {
            (*obj)[std::string_view("diagnostics")] =
                CloneJsonValue(*diagnostics);
        }
        return result;
    }

    static yyjson::value MakeRepositoryResponseError(
        std::string_view     kind,
        std::string_view     message,
        const yyjson::value* diagnostics = nullptr)
    {
        yyjson::value result(Das::Utils::MakeYyjsonObject());
        auto          obj = result.as_object();
        (*obj)[std::string_view("ok")] = false;
        (*obj)[std::string_view("errorKind")] = std::string(kind);
        (*obj)[std::string_view("message")] = std::string(message);
        if (diagnostics != nullptr && diagnostics->as_array().has_value())
        {
            (*obj)[std::string_view("diagnostics")] =
                CloneJsonValue(*diagnostics);
        }
        return result;
    }

    static std::string RepositoryErrorKindFromResult(DasResult result)
    {
        switch (result)
        {
        case DAS_E_INVALID_JSON:
            return "invalidJson";
        case DAS_E_TYPE_ERROR:
            return "invalidProperties";
        case DAS_E_NOT_FOUND:
            return "notFound";
        case DAS_E_TASK_WORKING:
            return "taskWorking";
        case DAS_E_OBJECT_NOT_INIT:
            return "notInitialized";
        default:
            return "repositoryError";
        }
    }

    static bool ParseCreateRepositoryEntryRequest(
        const yyjson::value&                              request,
        Repository::Dto::CreateRepositoryEntryRequestDto& out_request)
    {
        auto obj = request.as_object();
        if (!obj)
        {
            return false;
        }

        auto plugin_guid = (*obj)[std::string_view("pluginGuid")].as_string();
        auto task_type_guid =
            (*obj)[std::string_view("taskTypeGuid")].as_string();
        if (!plugin_guid || !task_type_guid)
        {
            return false;
        }

        out_request.plugin_guid = std::string(*plugin_guid);
        out_request.task_type_guid = std::string(*task_type_guid);

        if (obj->contains(std::string_view("displayName"))
            && !(*obj)[std::string_view("displayName")].is_null())
        {
            auto display_name =
                (*obj)[std::string_view("displayName")].as_string();
            if (!display_name)
            {
                return false;
            }
            out_request.display_name = std::string(*display_name);
        }

        if (obj->contains(std::string_view("initialProperties")))
        {
            out_request.initial_properties =
                CloneJsonValue((*obj)[std::string_view("initialProperties")]);
        }
        else
        {
            out_request.initial_properties = yyjson::value{};
        }

        return true;
    }

    static bool ParseRenameRepositoryEntryRequest(
        const yyjson::value&                              request,
        Repository::Dto::RenameRepositoryEntryRequestDto& out_request)
    {
        auto obj = request.as_object();
        if (!obj)
        {
            return false;
        }

        auto display_name = (*obj)[std::string_view("displayName")].as_string();
        if (!display_name)
        {
            return false;
        }

        out_request.display_name = std::string(*display_name);
        return true;
    }

    static int64_t GetAuthoringRevision(const yyjson::value& task_json)
    {
        auto task_obj = task_json.as_object();
        if (!task_obj || !task_obj->contains(std::string_view("authoring")))
        {
            return 0;
        }
        auto auth_obj = (*task_obj)[std::string_view("authoring")].as_object();
        if (!auth_obj || !auth_obj->contains(std::string_view("revision")))
        {
            return 0;
        }
        auto revision = (*auth_obj)[std::string_view("revision")].as_sint();
        return revision ? *revision : 0;
    }

    // 多语言 label map（{"en": "...", "zh-cn": "..."}，manifest 作者自由
    // 文本）的解析 helper：按 locale → en 顺序查表返回 resolved 文案。
    // 键匹配大小写无关：查询 locale 与 map 键双侧 toLower 归一后比较
    // （与 IDasI18nImpl 装载侧/查询侧双侧归一的先例对齐——manifest 文档
    // 从定义读取、不可变，故键在比较时归一），BCP47 规范大小写（语言
    // 小写、地区大写，如 "zh-CN"）的键与任意大小写的 locale 值互相命中。
    // as_object 失败、候选键缺失、值非 string、空串值一律视为 miss 返回
    // nullopt，由调用方走自己的终级回退（portId / kind / guid）——label
    // 是纯显示数据，畸形不丢端口不失败。返回的 string_view 指向
    // definition 文档 arena，definition 存活期内有效。
    static std::optional<std::string_view> ResolveLocalizedLabel(
        const yyjson::value& label_map,
        std::string_view     locale)
    {
        auto map_obj = label_map.as_object();
        if (!map_obj)
        {
            return std::nullopt;
        }
        std::string normalized_locale(locale);
        DAS::Utils::ToLowerInPlace(normalized_locale);
        for (const std::string_view candidate :
             {std::string_view(normalized_locale), std::string_view("en")})
        {
            for (const auto& [key, value] : *map_obj)
            {
                std::string normalized_key(key);
                DAS::Utils::ToLowerInPlace(normalized_key);
                if (normalized_key != candidate)
                {
                    continue;
                }
                auto text = value.as_string();
                if (text && !text->empty())
                {
                    return *text;
                }
            }
        }
        return std::nullopt;
    }

    // Registry-driven actionCatalog injection. Scheduler
    // holds PluginManager by reference and is the natural owner of the
    // registered IDasTaskComponent component snapshot. It injects the catalog
    // as a top-level `actionCatalog` JSON array of {actionId,label} objects
    // inside every authoring session context: the label resolves from the
    // component manifest definition's top-level localized label map when
    // present (locale→en), falling back to the `kind` string and then the
    // guid, so the authoring UI always shows a friendly renderable name.
    // Plugins that receive the context forward it verbatim to the Core
    // create entry; the Core consumes the `actionCatalog` key at its own
    // context parse point. The array is sorted lexically by actionId for
    // deterministic ordering.
    static std::string CatalogEntryLabel(
        const Das::Core::ForeignInterfaceHost::TaskComponentDefinitionInfo&
                         definition,
        std::string_view locale)
    {
        auto definition_obj = definition.definition.as_object();
        // 组件名 label map（kind 兄弟字段）先行解析；kind 字段本身保持
        // 机器名语义不动（label 是纯显示数据，寻址键恒为 actionId/
        // componentGuid）。
        if (definition_obj
            && definition_obj->contains(std::string_view("label")))
        {
            auto resolved = ResolveLocalizedLabel(
                (*definition_obj)[std::string_view("label")],
                locale);
            if (resolved.has_value())
            {
                return std::string(*resolved);
            }
        }
        if (definition_obj
            && definition_obj->contains(std::string_view("kind")))
        {
            auto kind = (*definition_obj)[std::string_view("kind")].as_string();
            if (kind && !kind->empty())
            {
                return std::string(*kind);
            }
        }
        // Definitions without a usable kind fall back to the guid — the same
        // fallback the Core applies to catalog entries arriving without a
        // label, so the UI always has a renderable value.
        return GuidToString(definition.component_guid);
    }

    // Static port surface of one component, read from the manifest
    // definition's port declaration lists (`inputs`/`outputs`/`signals`).
    // Direction is the list membership — the same authority compile-time
    // port validation uses (edge source ports are looked up in manifest
    // outputs, edge target ports in manifest inputs); the signals list is
    // output-only execution flow. Entries carry {id,type} with an optional
    // multi-locale `label` map resolved against `locale` (miss → en →
    // port-id fallback); a missing type falls back to `fallback_port_type`
    // (the signals list declares bare {id} entries that project as
    // "signal"). Entries without a usable id, or whose resolved type is
    // empty, are skipped — a single malformed declaration must not ride
    // the channel, because the Core consumer rejects the whole catalog on
    // an empty portType. A malformed label never skips the port: it is
    // pure display data, so it degrades to the port id instead. The
    // emitted displayLabel falls back to the port id so every projected
    // port is renderable. Ports are appended directly to the destination
    // array; the array rides the catalog context channel so the Core node
    // port projection can merge it onto nodes by componentGuid.
    static void AppendEntryPortsFromDefinition(
        const yyjson::value&       definition,
        std::string_view           list_key,
        std::string_view           direction,
        std::string_view           fallback_port_type,
        std::string_view           locale,
        yyjson::writer::array_ref& destination)
    {
        auto def_obj = definition.as_object();
        if (!def_obj || !def_obj->contains(list_key))
        {
            return;
        }
        auto list = (*def_obj)[list_key].as_array();
        if (!list)
        {
            return;
        }
        for (const auto& item : list.value())
        {
            auto port_obj = item.as_object();
            if (!port_obj || !port_obj->contains(std::string_view("id")))
            {
                continue;
            }
            auto id = (*port_obj)[std::string_view("id")].as_string();
            if (!id || id->empty())
            {
                continue;
            }
            std::string_view resolved_type = fallback_port_type;
            if (port_obj->contains(std::string_view("type")))
            {
                auto type = (*port_obj)[std::string_view("type")].as_string();
                if (!type)
                {
                    continue;
                }
                resolved_type = *type;
            }
            if (resolved_type.empty())
            {
                continue;
            }
            std::string_view resolved_label;
            if (port_obj->contains(std::string_view("label")))
            {
                // label 是多语言 map：任何 miss（非 object/键缺失/值非
                // string/空串）都留空走 portId 回退，端口照常投影。
                auto resolved = ResolveLocalizedLabel(
                    (*port_obj)[std::string_view("label")],
                    locale);
                if (resolved.has_value())
                {
                    resolved_label = *resolved;
                }
            }
            if (resolved_label.empty())
            {
                resolved_label = *id;
            }
            auto port = Das::Utils::MakeYyjsonObject();
            auto obj = port.as_object();
            (*obj)[std::string_view("portId")] =
                std::make_pair(std::string_view(*id), yyjson::copy_string);
            (*obj)[std::string_view("displayLabel")] =
                std::make_pair(resolved_label, yyjson::copy_string);
            (*obj)[std::string_view("portType")] =
                std::make_pair(resolved_type, yyjson::copy_string);
            (*obj)[std::string_view("direction")] =
                std::make_pair(direction, yyjson::copy_string);
            // loopCarried 声明 engaged-only 发射：仅布尔真值落键（循环携
            // 带口标记，编译器据此合成自环值路由）；非布尔值跳过发射与
            // label/required 同 miss 容忍语义。
            if (port_obj->contains(std::string_view("loopCarried")))
            {
                auto carried =
                    (*port_obj)[std::string_view("loopCarried")].as_bool();
                if (carried.has_value() && *carried)
                {
                    (*obj)[std::string_view("loopCarried")] = true;
                }
            }
            destination.emplace_back(std::move(port));
        }
    }

    // Config field declarations of one component, read from the manifest
    // definition's `config` object
    // ({field_name: {type, label?, description?, default?, taskGuid?,
    // required?, options?}}). Emitted
    // in lexical key order for determinism; entries without a usable non-empty
    // type string are skipped (the same skip-not-poison rule as the port
    // lists — the Core consumer rejects the whole catalog on a malformed
    // channel entry). The optional taskGuid declaration names the target
    // task type of a component-domain field; it rides the channel verbatim
    // (engaged-only) so the Core fields projection can surface it without a
    // component→task reverse lookup. The optional per-field multi-locale
    // `label` map resolves against `locale` (miss → en; any miss keeps the
    // key absent so the element stays four-key) into the resolved display
    // string the Core projection uses as the field label (falling back to
    // the field name there) — label is pure display data, so a malformed
    // map never skips the field. The optional multi-locale `description`
    // map resolves the same way into the field help text (engaged-only;
    // where the label says what the field is, the description is where the
    // manifest documents what configuring it does).
    static void AppendConfigFromDefinition(
        const yyjson::value&       definition,
        std::string_view           locale,
        yyjson::writer::array_ref& destination)
    {
        auto def_obj = definition.as_object();
        if (!def_obj || !def_obj->contains(std::string_view("config")))
        {
            return;
        }
        auto config = (*def_obj)[std::string_view("config")].as_object();
        if (!config)
        {
            return;
        }
        std::vector<std::string_view> field_names;
        field_names.reserve(config->size());
        for (const auto& [key, value] : *config)
        {
            (void)value;
            field_names.push_back(key);
        }
        std::sort(field_names.begin(), field_names.end());
        for (const auto& field_name : field_names)
        {
            auto field_obj = (*config)[field_name].as_object();
            if (!field_obj)
            {
                continue;
            }
            auto type = (*field_obj)[std::string_view("type")].as_string();
            if (!type || type->empty())
            {
                continue;
            }
            std::string_view resolved_label;
            if (field_obj->contains(std::string_view("label")))
            {
                // label 是多语言 map：任何 miss（非 object/键缺失/值非
                // string/空串）都不发该键，元素保持四键现状。
                auto resolved = ResolveLocalizedLabel(
                    (*field_obj)[std::string_view("label")],
                    locale);
                if (resolved.has_value())
                {
                    resolved_label = *resolved;
                }
            }
            std::string_view resolved_description;
            if (field_obj->contains(std::string_view("description")))
            {
                // description 与 label 同一多语言解析/miss 容忍语义。
                auto resolved = ResolveLocalizedLabel(
                    (*field_obj)[std::string_view("description")],
                    locale);
                if (resolved.has_value())
                {
                    resolved_description = *resolved;
                }
            }
            auto config_field = Das::Utils::MakeYyjsonObject();
            auto obj = config_field.as_object();
            (*obj)[std::string_view("name")] =
                std::make_pair(field_name, yyjson::copy_string);
            (*obj)[std::string_view("type")] =
                std::make_pair(std::string_view(*type), yyjson::copy_string);
            if (!resolved_label.empty())
            {
                (*obj)[std::string_view("label")] =
                    std::make_pair(resolved_label, yyjson::copy_string);
            }
            if (!resolved_description.empty())
            {
                (*obj)[std::string_view("description")] =
                    std::make_pair(resolved_description, yyjson::copy_string);
            }
            if (field_obj->contains(std::string_view("default"))
                && !(*field_obj)[std::string_view("default")].is_null())
            {
                (*obj)[std::string_view("default")] =
                    Das::Utils::CloneYyjsonValue(
                        (*field_obj)[std::string_view("default")]);
            }
            if (field_obj->contains(std::string_view("taskGuid")))
            {
                auto task_guid =
                    (*field_obj)[std::string_view("taskGuid")].as_string();
                if (task_guid.has_value() && !task_guid->empty())
                {
                    (*obj)[std::string_view("taskGuid")] = std::make_pair(
                        std::string_view(*task_guid),
                        yyjson::copy_string);
                }
            }
            // required 声明 engaged-only 发射：仅 required:true 落键；非布尔
            // 值跳过发射不报错——与 label 的 miss 容忍语义一致（fail-closed
            // 的形状校验在 Core 消费侧：非布尔 required 整 catalog 拒绝）。
            if (field_obj->contains(std::string_view("required")))
            {
                auto required =
                    (*field_obj)[std::string_view("required")].as_bool();
                if (required.has_value() && *required)
                {
                    (*obj)[std::string_view("required")] = true;
                }
            }
            // options 闭值域声明 engaged-only 发射：仅当 manifest 值是全
            // 非空字符串的非空数组时落键（运算符词表——单一字符串数组即
            // 完整声明，label 机械等于 value）；任何非字符串/空串元素视为
            // 未声明跳过（消费侧对 wire 上出现的 options 做全字符串形状
            // 校验——生产侧 miss 容忍与 label/required 同语义）。
            if (field_obj->contains(std::string_view("options")))
            {
                auto options =
                    (*field_obj)[std::string_view("options")].as_array();
                if (options.has_value() && !options->empty())
                {
                    bool all_strings = true;
                    for (const auto& option : *options)
                    {
                        auto option_text = option.as_string();
                        if (!option_text || option_text->empty())
                        {
                            all_strings = false;
                            break;
                        }
                    }
                    if (all_strings)
                    {
                        auto options_value = Das::Utils::MakeYyjsonArray();
                        auto options_array = options_value.as_array();
                        if (options_array)
                        {
                            for (const auto& option : *options)
                            {
                                options_array->emplace_back(
                                    yyjson::value(
                                        std::string(*option.as_string())));
                            }
                        }
                        (*obj)[std::string_view("options")] =
                            std::move(options_value);
                    }
                }
            }
            destination.emplace_back(std::move(config_field));
        }
    }

    static yyjson::value BuildActionCatalogFromRegistry(
        Das::Core::ForeignInterfaceHost::PluginManager& plugin_manager)
    {
        auto definitions = plugin_manager.GetTaskComponentFactoryManager()
                               .EnumerateDefinitions();
        if (definitions.empty())
        {
            return yyjson::value(Das::Utils::MakeYyjsonArray());
        }

        // label map 解析的 locale 源是进程级 GlobalLocale（DAS 系 locale，
        // 经 DasGetDefaultLocale C API 直调）：函数头取一次传给各解析点，
        // 避免每端口重复查询；读不到或为空时用 en（回退链保证 portId/kind
        // 仍可渲染，catalog 构建不因 locale 缺失失败）。
        std::string locale("en");
        {
            DAS::DasPtr<IDasReadOnlyString> p_locale;
            if (DAS::IsOk(::DasGetDefaultLocale(p_locale.Put()))
                && p_locale.Get() != nullptr)
            {
                const char* locale_utf8 = nullptr;
                p_locale->GetUtf8(&locale_utf8);
                if (locale_utf8 != nullptr && *locale_utf8 != '\0')
                {
                    locale = locale_utf8;
                }
            }
        }

        std::vector<std::size_t> order;
        order.reserve(definitions.size());
        for (std::size_t i = 0; i < definitions.size(); ++i)
        {
            order.push_back(i);
        }
        std::sort(
            order.begin(),
            order.end(),
            [&definitions](std::size_t lhs, std::size_t rhs)
            {
                return GuidToString(definitions[lhs].component_guid)
                       < GuidToString(definitions[rhs].component_guid);
            });

        // Build the catalog as a JSON array of {actionId,label} objects via
        // the yyjson writer: the label is the plugin manifest's kind field
        // (author-controlled free text), so it must go through real JSON
        // escaping instead of raw string concatenation — a kind containing a
        // quote or backslash would otherwise produce invalid JSON and empty
        // the whole catalog. Per das-yyjson-string-safety string members use
        // the (string_view, copy_string) pair overload so every value is
        // deep-copied into the document arena.
        auto catalog = Das::Utils::MakeYyjsonArray();
        auto array_ref = catalog.as_array();
        for (std::size_t index : order)
        {
            const auto&       definition = definitions[index];
            const std::string action_id =
                GuidToString(definition.component_guid);
            const std::string label =
                CatalogEntryLabel(definition, std::string_view(locale));

            auto entry = Das::Utils::MakeYyjsonObject();
            auto obj = entry.as_object();
            (*obj)[std::string_view("actionId")] = std::make_pair(
                std::string_view(action_id),
                yyjson::copy_string);
            (*obj)[std::string_view("label")] =
                std::make_pair(std::string_view(label), yyjson::copy_string);
            // Static port surface: the manifest definition declares
            // inputs[]/outputs[]/signals[]; list membership is the
            // direction (signals are output-only execution flow, entries
            // may omit the type and project as "signal"). Emitted only
            // when the component declares at least one port.
            auto ports = Das::Utils::MakeYyjsonArray();
            auto ports_arr = ports.as_array();
            AppendEntryPortsFromDefinition(
                definition.definition,
                std::string_view("inputs"),
                std::string_view("input"),
                std::string_view(),
                std::string_view(locale),
                *ports_arr);
            AppendEntryPortsFromDefinition(
                definition.definition,
                std::string_view("outputs"),
                std::string_view("output"),
                std::string_view(),
                std::string_view(locale),
                *ports_arr);
            AppendEntryPortsFromDefinition(
                definition.definition,
                std::string_view("signals"),
                std::string_view("output"),
                std::string_view("signal"),
                std::string_view(locale),
                *ports_arr);
            if (ports_arr->size() > 0)
            {
                (*obj)[std::string_view("ports")] = std::move(ports);
            }
            // Config field declarations:
            // {name,type,default?,taskGuid?,label?,description?,required?,
            // options?} in lexical key order — the Core consumer locks the
            // element shape to exactly these keys; label/description are the
            // locale-resolved display string and help text (engaged-only).
            auto config = Das::Utils::MakeYyjsonArray();
            auto config_arr = config.as_array();
            AppendConfigFromDefinition(
                definition.definition,
                std::string_view(locale),
                *config_arr);
            if (config_arr->size() > 0)
            {
                (*obj)[std::string_view("config")] = std::move(config);
            }
            array_ref->emplace_back(std::move(entry));
        }
        return catalog;
    }

    static yyjson::value MakeAuthoringContextJson(
        Das::Core::ForeignInterfaceHost::PluginManager& plugin_manager,
        int64_t                                         task_id,
        const yyjson::value&                            properties,
        const yyjson::value&                            task_json)
    {
        yyjson::value context(Das::Utils::MakeYyjsonObject());
        auto          obj = context.as_object();
        (*obj)[std::string_view("taskId")] = task_id;
        (*obj)[std::string_view("properties")] = CloneJsonValue(properties);
        (*obj)[std::string_view("revision")] = GetAuthoringRevision(task_json);

        auto task_obj = task_json.as_object();
        if (task_obj && task_obj->contains(std::string_view("authoring")))
        {
            (*obj)[std::string_view("authoring")] =
                CloneJsonValue((*task_obj)[std::string_view("authoring")]);
        }
        (*obj)[std::string_view("actionCatalog")] =
            BuildActionCatalogFromRegistry(plugin_manager);
        return context;
    }

    static yyjson::value MakeRepositoryAuthoringContextJson(
        Das::Core::ForeignInterfaceHost::PluginManager& plugin_manager,
        int64_t                                         entry_id,
        const yyjson::value&                            properties,
        const yyjson::value&                            entry_json)
    {
        yyjson::value context(Das::Utils::MakeYyjsonObject());
        auto          obj = context.as_object();
        (*obj)[std::string_view("entryId")] = entry_id;
        (*obj)[std::string_view("properties")] = CloneJsonValue(properties);
        (*obj)[std::string_view("revision")] = GetAuthoringRevision(entry_json);

        auto entry_obj = entry_json.as_object();
        if (entry_obj && entry_obj->contains(std::string_view("authoring")))
        {
            (*obj)[std::string_view("authoring")] =
                CloneJsonValue((*entry_obj)[std::string_view("authoring")]);
        }
        (*obj)[std::string_view("actionCatalog")] =
            BuildActionCatalogFromRegistry(plugin_manager);
        return context;
    }

    // ------------------------------------------------------------------
    // 组件文档口面预备 pass 的静态辅助（member 入口在
    // AppendComponentFacesContextKeys）：图文档节点扫描、绑条目判据、
    // faces 口条目重建与节点作用域 context 装配。
    // ------------------------------------------------------------------

    // 图文档节点列表判定：properties 为 GraphDocumentDto 序列化形状
    // （顶层 nodes 数组）才算图文档；非图文档（formSequence 契约文档等）
    // 返回 false——预备 pass 对其零动作，context 保持现状。
    bool PropertiesHoldGraphNodes(const yyjson::value& properties)
    {
        auto props_obj = properties.as_object();
        return props_obj.has_value()
               && props_obj->contains(std::string_view("nodes"));
    }

    // 节点的 componentRef 组件 guid（targetKind 非 componentRef 或形状
    // 不全时返回空串）。
    std::string ComponentGuidOfNode(const yyjson::value& node_value)
    {
        auto node_obj = node_value.as_object();
        if (!node_obj || !node_obj->contains(std::string_view("target")))
        {
            return {};
        }
        auto target = (*node_obj)[std::string_view("target")].as_object();
        if (!target)
        {
            return {};
        }
        auto target_kind =
            (*target)[std::string_view("targetKind")].as_string();
        if (!target_kind || *target_kind != std::string_view("componentRef"))
        {
            return {};
        }
        if (!target->contains(std::string_view("componentRef")))
        {
            return {};
        }
        const auto& ref = (*target)[std::string_view("componentRef")];
        if (ref.is_null())
        {
            return {};
        }
        auto ref_obj = ref.as_object();
        if (!ref_obj)
        {
            return {};
        }
        auto guid = (*ref_obj)[std::string_view("componentGuid")].as_string();
        if (!guid)
        {
            return {};
        }
        return std::string(*guid);
    }

    // 组件 guid → taskRepositoryRef 配置字段名列表（数据源=context 既有
    // actionCatalog 快照的 config 声明——与编译侧绑条目判据同一目录语义，
    // 业务中立不特判组件）。
    std::unordered_map<std::string, std::vector<std::string>>
    CollectRepositoryRefFields(const yyjson::value& context)
    {
        std::unordered_map<std::string, std::vector<std::string>> fields;
        auto context_obj = context.as_object();
        if (!context_obj
            || !context_obj->contains(std::string_view("actionCatalog")))
        {
            return fields;
        }
        auto catalog =
            (*context_obj)[std::string_view("actionCatalog")].as_array();
        if (!catalog)
        {
            return fields;
        }
        for (const auto& entry_value : *catalog)
        {
            auto entry_obj = entry_value.as_object();
            if (!entry_obj
                || !entry_obj->contains(std::string_view("actionId")))
            {
                continue;
            }
            auto action_id =
                (*entry_obj)[std::string_view("actionId")].as_string();
            if (!action_id || action_id->empty())
            {
                continue;
            }
            if (!entry_obj->contains(std::string_view("config")))
            {
                continue;
            }
            auto config = (*entry_obj)[std::string_view("config")].as_array();
            if (!config)
            {
                continue;
            }
            for (const auto& field_value : *config)
            {
                auto field_obj = field_value.as_object();
                if (!field_obj
                    || !field_obj->contains(std::string_view("type")))
                {
                    continue;
                }
                auto type = (*field_obj)[std::string_view("type")].as_string();
                if (!type || *type != std::string_view("taskRepositoryRef"))
                {
                    continue;
                }
                if (!field_obj->contains(std::string_view("name")))
                {
                    continue;
                }
                auto name = (*field_obj)[std::string_view("name")].as_string();
                if (!name || name->empty())
                {
                    continue;
                }
                fields[std::string(*action_id)].emplace_back(*name);
            }
        }
        return fields;
    }

    // 绑条目判定（判据=编译侧 BoundRepositoryEntryIdOf 同款语义：声明的
    // taskRepositoryRef 字段持正整数值）——返回该正整数，nullopt=未绑。
    std::optional<int64_t> BoundEntryIdOfNode(
        const yyjson::value&            node_value,
        const std::vector<std::string>& repository_field_names)
    {
        auto node_obj = node_value.as_object();
        if (!node_obj || !node_obj->contains(std::string_view("settings")))
        {
            return std::nullopt;
        }
        auto settings_obj =
            (*node_obj)[std::string_view("settings")].as_object();
        if (!settings_obj)
        {
            return std::nullopt;
        }
        for (const auto& field_name : repository_field_names)
        {
            if (!settings_obj->contains(std::string_view(field_name)))
            {
                continue;
            }
            const auto& value = (*settings_obj)[std::string_view(field_name)];
            if (value.is_int())
            {
                const auto entry_id = value.as_int();
                if (entry_id.has_value() && *entry_id > 0)
                {
                    return *entry_id;
                }
            }
        }
        return std::nullopt;
    }

    // faces 口条目重建：从 Provider 文档根 ports section 提取
    // {id,type,direction[,label]}（仅取四键、逐元素校验、畸形元素跳过——
    // 输出恒符合 Core context 键解析的锁形规则）。缺 section 或全部
    // 畸形/为空 → nullopt（跳过该面，不产诊断）。
    std::optional<yyjson::value> ExtractFacePortsValue(
        const yyjson::value& document)
    {
        auto doc_obj = document.as_object();
        if (!doc_obj || !doc_obj->contains(std::string_view("ports")))
        {
            return std::nullopt;
        }
        auto ports_arr = (*doc_obj)[std::string_view("ports")].as_array();
        if (!ports_arr)
        {
            return std::nullopt;
        }
        yyjson::value ports(Das::Utils::MakeYyjsonArray());
        auto          ports_out = ports.as_array();
        for (const auto& port_value : *ports_arr)
        {
            auto port_obj = port_value.as_object();
            if (!port_obj || !port_obj->contains(std::string_view("id"))
                || !port_obj->contains(std::string_view("type"))
                || !port_obj->contains(std::string_view("direction")))
            {
                continue;
            }
            auto id = (*port_obj)[std::string_view("id")].as_string();
            auto type = (*port_obj)[std::string_view("type")].as_string();
            auto direction =
                (*port_obj)[std::string_view("direction")].as_string();
            if (!id || id->empty() || !type || type->empty() || !direction
                || (*direction != std::string_view("input")
                    && *direction != std::string_view("output")))
            {
                continue;
            }
            yyjson::value port(Das::Utils::MakeYyjsonObject());
            auto          port_out = port.as_object();
            (*port_out)[std::string_view("id")] =
                std::make_pair(std::string_view(*id), yyjson::copy_string);
            (*port_out)[std::string_view("type")] =
                std::make_pair(std::string_view(*type), yyjson::copy_string);
            (*port_out)[std::string_view("direction")] = std::make_pair(
                std::string_view(*direction),
                yyjson::copy_string);
            if (port_obj->contains(std::string_view("label")))
            {
                if (auto label =
                        (*port_obj)[std::string_view("label")].as_string();
                    label.has_value())
                {
                    (*port_out)[std::string_view("label")] = std::make_pair(
                        std::string_view(*label),
                        yyjson::copy_string);
                }
            }
            ports_out->emplace_back(std::move(port));
        }
        if (ports_out->empty())
        {
            return std::nullopt;
        }
        return ports;
    }

    // faces 注册类型重建：文档根 types section → {name[,compatibleWith]}
    // （name 必填非空；compatibleWith 仅取字符串元素重建）。缺 section 或
    // 全畸形 → nullopt。
    std::optional<yyjson::value> ExtractFaceTypesValue(
        const yyjson::value& document)
    {
        auto doc_obj = document.as_object();
        if (!doc_obj || !doc_obj->contains(std::string_view("types")))
        {
            return std::nullopt;
        }
        auto types_arr = (*doc_obj)[std::string_view("types")].as_array();
        if (!types_arr)
        {
            return std::nullopt;
        }
        yyjson::value types(Das::Utils::MakeYyjsonArray());
        auto          types_out = types.as_array();
        for (const auto& type_value : *types_arr)
        {
            auto type_obj = type_value.as_object();
            if (!type_obj || !type_obj->contains(std::string_view("name")))
            {
                continue;
            }
            auto name = (*type_obj)[std::string_view("name")].as_string();
            if (!name || name->empty())
            {
                continue;
            }
            yyjson::value type(Das::Utils::MakeYyjsonObject());
            auto          type_out = type.as_object();
            (*type_out)[std::string_view("name")] =
                std::make_pair(std::string_view(*name), yyjson::copy_string);
            if (type_obj->contains(std::string_view("compatibleWith")))
            {
                auto compatible =
                    (*type_obj)[std::string_view("compatibleWith")].as_array();
                if (compatible)
                {
                    yyjson::value compatible_value(
                        Das::Utils::MakeYyjsonArray());
                    auto compatible_out = compatible_value.as_array();
                    for (const auto& compatible_name : *compatible)
                    {
                        auto compatible_text = compatible_name.as_string();
                        if (!compatible_text)
                        {
                            continue;
                        }
                        compatible_out->emplace_back(
                            yyjson::value(std::string(*compatible_text)));
                    }
                    (*type_out)[std::string_view("compatibleWith")] =
                        std::move(compatible_value);
                }
            }
            types_out->emplace_back(std::move(type));
        }
        if (types_out->empty())
        {
            return std::nullopt;
        }
        return types;
    }

    // 节点作用域组件会话 context：节点 settings 平铺 + 宿主 revision +
    // componentContext 标记（true 时投影器按组件语境门控）。平铺形状与
    // 边界会话的 context 契约一致（顶层 revision 剥离、其余键进
    // settings），也兼容以 properties/平铺双形状解析的插件工厂。
    yyjson::value MakeNodeComponentContextJson(
        const yyjson::value& node_value,
        int64_t              host_revision)
    {
        yyjson::value context(Das::Utils::MakeYyjsonObject());
        auto          obj = context.as_object();
        auto          node_obj = node_value.as_object();
        if (node_obj && node_obj->contains(std::string_view("settings")))
        {
            const auto& settings_ref =
                (*node_obj)[std::string_view("settings")];
            if (auto settings_obj = settings_ref.as_object())
            {
                for (const auto& [key, value] : *settings_obj)
                {
                    (*obj)[std::string_view(key)] = CloneJsonValue(value);
                }
            }
        }
        (*obj)[std::string_view("revision")] = host_revision;
        (*obj)[std::string_view("componentContext")] = true;
        return context;
    }

    // 空 get 请求体（Provider GetDocument 的 {} 形态）。
    yyjson::value MakeEmptyAuthoringGetRequest()
    {
        return Das::Utils::MakeYyjsonObject();
    }

    static std::optional<int64_t> GetBaseRevision(const yyjson::value& change)
    {
        auto obj = change.as_object();
        if (!obj || !obj->contains(std::string_view("baseRevision")))
        {
            return std::nullopt;
        }
        auto revision = (*obj)[std::string_view("baseRevision")].as_sint();
        if (!revision)
        {
            return std::nullopt;
        }
        return *revision;
    }

    static std::string GetStringField(
        const yyjson::value& value,
        std::string_view     key,
        std::string_view     fallback)
    {
        auto obj = value.as_object();
        if (!obj || !obj->contains(key))
        {
            return std::string(fallback);
        }
        auto str = (*obj)[key].as_string();
        if (!str)
        {
            return std::string(fallback);
        }
        return std::string(*str);
    }

    static bool IsExplicitFalse(const yyjson::value& value)
    {
        auto bool_value = value.as_bool();
        return bool_value && !*bool_value;
    }

    // provider compile 结果是否显式声明 ok=false：键缺席（各 provider 契约
    // 不同，如 graph 的 compile 只带 executionInput）视为未声明——不抛、
    // 不拒（cpp_yyjson object operator[] 对缺失键 throw out_of_range，
    // 消费侧必须 contains 先行，同 RepositoryInvokeCompiler 判定形态）。
    static bool ProviderCompileExplicitFalse(const yyjson::value& compile)
    {
        auto compile_obj = compile.as_object();
        return compile_obj && compile_obj->contains(std::string_view("ok"))
               && IsExplicitFalse((*compile_obj)[std::string_view("ok")]);
    }

    static bool GetBoolField(
        const yyjson::value& value,
        std::string_view     key,
        bool                 fallback)
    {
        auto obj = value.as_object();
        if (!obj || !obj->contains(key))
        {
            return fallback;
        }

        auto bool_value = (*obj)[key].as_bool();
        return bool_value ? *bool_value : fallback;
    }

    // ===================================================================
    // Phase 84 shared Authoring result validation/persistence helpers
    // -------------------------------------------------------------------
    // D-84-08/09/10/11: Scheduler is a transparent validator that preserves
    // the Provider result envelope. taskId and repository entryId adapters
    // share the same parsing, diagnostic validation, returned-revision
    // assertion, and prepare-then-write order; they differ only in the
    // target lookup and store adapters.
    // ===================================================================

    struct AuthoringProviderGet
    {
        yyjson::value document;
        yyjson::value diagnostics;
        bool          has_document = false;
        std::string   error_kind;
        std::string   error_message;
    };

    static AuthoringProviderGet ParseAuthoringProviderGet(
        const yyjson::value& provider_result)
    {
        AuthoringProviderGet outcome;
        auto validation = ValidateAuthoringGetResult(provider_result);
        if (!validation.valid)
        {
            outcome.error_kind = validation.error_kind;
            outcome.error_message = validation.message;
            return outcome;
        }

        auto obj = provider_result.as_object();
        if (obj && obj->contains(std::string_view("document")))
        {
            outcome.document =
                CloneJsonValue((*obj)[std::string_view("document")]);
            outcome.has_document = true;
            auto document_validation =
                ValidateAuthoringDocument(outcome.document);
            if (!document_validation.valid)
            {
                outcome.error_kind = document_validation.error_kind;
                outcome.error_message = document_validation.message;
                return outcome;
            }
        }
        outcome.diagnostics =
            CloneJsonValue((*obj)[std::string_view("diagnostics")]);
        return outcome;
    }

    struct AuthoringProviderApply
    {
        yyjson::value accepted_properties;
        yyjson::value document;
        yyjson::value diagnostics;
        yyjson::value command_result; // 可选：业务命令结果（如 checkUpdate 的
                                      // progress_key）
        bool        has_command_result = false;
        int64_t     returned_revision = 0;
        std::string error_kind;
        std::string error_message;
    };

    // 短路 kind：Apply 响应不带 document，前端据有无 document 分流
    // （UiEventApplied/ChangeApplied）。目前仅 uiEvent；其余为文档编辑 kind，
    // 缺 document 视为 provider 未执行变更（missingField，不落盘）。
    static bool IsShortCircuitAuthoringKind(std::string_view kind)
    {
        return kind == "uiEvent";
    }

    static AuthoringProviderApply ParseAuthoringProviderApply(
        const yyjson::value& provider_result,
        int64_t              base_revision,
        std::string_view     change_kind)
    {
        AuthoringProviderApply outcome;
        // Capture diagnostics before validation so a provider-rejected apply
        // (error diagnostics) can surface its real reason even if the envelope
        // later fails a stricter check.
        auto pre_obj = provider_result.as_object();
        if (pre_obj && pre_obj->contains(std::string_view("diagnostics")))
        {
            outcome.diagnostics =
                CloneJsonValue((*pre_obj)[std::string_view("diagnostics")]);
        }
        auto validation = ValidateAuthoringApplyResult(
            provider_result,
            base_revision,
            change_kind);
        if (!validation.valid)
        {
            outcome.error_kind = validation.error_kind;
            outcome.error_message = validation.message;
            return outcome;
        }

        auto obj = provider_result.as_object();
        outcome.accepted_properties =
            CloneJsonValue((*obj)[std::string_view("acceptedProperties")]);
        // 可选 commandResult（业务命令结果，如 checkUpdate 返回的
        // progress_key）：provider 带则转发到 HTTP 响应。
        if (obj->contains(std::string_view("commandResult")))
        {
            outcome.command_result =
                CloneJsonValue((*obj)[std::string_view("commandResult")]);
            outcome.has_command_result = true;
        }

        // document 对短路 kind（uiEvent 等）可选：其响应不带 document（前端
        // 据有无 document 分流）。文档编辑 kind 必须回传 document——有则解析
        // revision + 校验 document；无则按 base+1 推进 revision（与前端一致，
        // ui.values 已变更）。
        const bool has_document =
            obj->contains(std::string_view("document"))
            && !(*obj)[std::string_view("document")].is_null();
        if (has_document)
        {
            outcome.document =
                CloneJsonValue((*obj)[std::string_view("document")]);
            auto       doc_obj = outcome.document.as_object();
            const auto revision_view = (*doc_obj)[std::string_view("revision")];
            outcome.returned_revision =
                revision_view.is_sint()
                    ? revision_view.as_sint().value()
                    : static_cast<int64_t>(revision_view.as_uint().value());

            auto document_validation =
                ValidateAuthoringDocument(outcome.document);
            if (!document_validation.valid)
            {
                outcome.error_kind = document_validation.error_kind;
                outcome.error_message = document_validation.message;
                return outcome;
            }
        }
        else if (!IsShortCircuitAuthoringKind(change_kind))
        {
            outcome.error_kind = "missingField";
            outcome.error_message =
                "AuthoringApplyResult.document required for kind "
                + std::string(change_kind);
            return outcome;
        }
        else
        {
            outcome.returned_revision = base_revision + 1;
        }
        return outcome;
    }

    struct AuthoringProviderCompile
    {
        yyjson::value compile;
        yyjson::value diagnostics;
        std::string   error_kind;
        std::string   error_message;
    };

    static AuthoringProviderCompile ParseAuthoringProviderCompile(
        const yyjson::value& provider_result)
    {
        AuthoringProviderCompile outcome;
        auto validation = ValidateAuthoringCompileResult(provider_result);
        if (!validation.valid)
        {
            outcome.error_kind = validation.error_kind;
            outcome.error_message = validation.message;
            return outcome;
        }

        auto obj = provider_result.as_object();
        outcome.compile = CloneJsonValue((*obj)[std::string_view("compile")]);
        outcome.diagnostics =
            CloneJsonValue((*obj)[std::string_view("diagnostics")]);
        return outcome;
    }

    // Builds the persisted authoring metadata object. Per D-84-02/D-84-04 the
    // metadata records only revision and an optional lastChangeKind; migration
    // and migrationState are removed from the authority. sourceFingerprint is
    // preserved best-effort because the RepositoryInvokeCompiler still consumes
    // it as an entry-identity guard, and RepositoryAuthoringMetadataDto still
    // requires the key when reading older entries.
    static yyjson::value MakePersistedAuthoringMetadata(
        int64_t              revision,
        std::string_view     last_change_kind,
        const yyjson::value& existing_authoring)
    {
        yyjson::value authoring(Das::Utils::MakeYyjsonObject());
        auto          obj = authoring.as_object();
        (*obj)[std::string_view("revision")] = revision;
        if (!last_change_kind.empty())
        {
            (*obj)[std::string_view("lastChangeKind")] =
                std::string(last_change_kind);
        }
        else if (
            auto prev = existing_authoring.as_object();
            prev && prev->contains(std::string_view("lastChangeKind")))
        {
            (*obj)[std::string_view("lastChangeKind")] =
                CloneJsonValue((*prev)[std::string_view("lastChangeKind")]);
        }
        if (auto prev = existing_authoring.as_object();
            prev && prev->contains(std::string_view("sourceFingerprint")))
        {
            (*obj)[std::string_view("sourceFingerprint")] =
                CloneJsonValue((*prev)[std::string_view("sourceFingerprint")]);
        }
        else
        {
            (*obj)[std::string_view("sourceFingerprint")] = std::string("");
        }
        return authoring;
    }

    static yyjson::value ExtractProviderExecutionInput(
        const yyjson::value& compile)
    {
        auto obj = compile.as_object();
        if (obj && obj->contains(std::string_view("executionInput")))
        {
            return CloneJsonValue((*obj)[std::string_view("executionInput")]);
        }
        return yyjson::value{};
    }

    static yyjson::value MakeExecutionCompileRequest()
    {
        yyjson::value request(Das::Utils::MakeYyjsonObject());
        auto          obj = request.as_object();
        (*obj)[std::string_view("purpose")] = "execution";
        return request;
    }

    static DasResult CreateAuthoringSessionWithContext(
        Das::Core::ForeignInterfaceHost::PluginManager&         plugin_manager,
        const TaskAuthoringCapability&                          capability,
        yyjson::value                                           context,
        DasPtr<Das::PluginInterface::IDasTaskAuthoringSession>& session)
    {
        DasPtr<Das::PluginInterface::IDasTaskAuthoringSessionFactory> factory;
        auto factory_features = plugin_manager.GetFeaturesByType(
            Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY);
        for (auto* feature_info : factory_features)
        {
            if (!feature_info
                || feature_info->plugin_guid != capability.plugin_guid
                || !feature_info->interface_ptr)
            {
                continue;
            }

            DasPtr<Das::PluginInterface::IDasTaskAuthoringSessionFactory>
                 candidate;
            auto query_result = feature_info->interface_ptr->QueryInterface(
                DasIidOf<
                    Das::PluginInterface::IDasTaskAuthoringSessionFactory>(),
                reinterpret_cast<void**>(candidate.Put()));
            if (DAS::IsFailed(query_result) || !candidate)
            {
                continue;
            }

            DasGuid candidate_guid{};
            auto    guid_result = candidate->GetGuid(&candidate_guid);
            if (DAS::IsFailed(guid_result))
            {
                continue;
            }

            if (candidate_guid == capability.authoring_factory_guid)
            {
                factory = std::move(candidate);
                break;
            }
        }

        if (!factory)
        {
            return DAS_E_NOT_FOUND;
        }

        auto context_json = WrapJsonValue(std::move(context));
        return factory->CreateSession(
            capability.task_guid,
            context_json.Get(),
            session.Put());
    }

    // 进程级 GlobalLocale 的原始读数，作为 authoring session 缓存的
    // locale 陈旧性戳：读取失败/未设置时为空串，是合法戳值（两次读数
    // 一致即视为 locale 未变），不做 en 回退——回退是 catalog label
    // 解析侧的渲染策略，戳侧只需忠反映射「创建时 vs 现在」的读数差异。
    // 与 BuildActionCatalogFromRegistry 的读法同源（同一 C API）。
    static std::string ReadGlobalLocaleStamp()
    {
        std::string                     locale;
        Das::DasPtr<IDasReadOnlyString> p_locale;
        if (DAS::IsOk(::DasGetDefaultLocale(p_locale.Put()))
            && p_locale.Get() != nullptr)
        {
            const char* locale_utf8 = nullptr;
            p_locale->GetUtf8(&locale_utf8);
            if (locale_utf8 != nullptr)
            {
                locale = locale_utf8;
            }
        }
        return locale;
    }

    // Cache core, shared by the member GetOrCreateAuthoringSession (Task/Repo
    // Get + Compile callers, which resolve the session before any pipeline)
    // and by the Apply pipeline, which must resolve the session AFTER its ③④
    // envelope/revision pre-checks (D-85-02c: an envelope failure must not
    // create a session). `mutex` guards only find/emplace; session creation
    // happens outside the lock (v1.3 short-critical-section rule). Returns
    // null on create failure without inserting an empty entry.
    //
    // Read-side staleness has two stamps, both checked under `mutex`:
    // - revision (Get path only, enforce_fresh): the on-disk authoring
    //   revision advanced since creation → rebuild.
    // - locale (both paths): the process GlobalLocale changed since the
    //   session's context was computed → rebuild. Callers recompute the
    //   context on every Get/Apply (MakeAuthoringContextJson →
    //   BuildActionCatalogFromRegistry reads the locale live), so a cache
    //   hit merely skips session rebuild — after the erase the rebuild
    //   consumes the caller's freshly-built context and the new language
    //   takes effect naturally. NOT gated on enforce_fresh: the Apply path
    //   reuses a session on the premise that its view is still fresh, and a
    //   locale change makes the label view stale there too. Stamp comparison
    //   is byte-wise (case-sensitive): a zh-cn → ZH-CN switch triggers one
    //   extra harmless rebuild because label resolution normalizes case on
    //   both sides — erring toward one redundant rebuild, never toward
    //   serving a stale language.
    static std::shared_ptr<AuthoringSessionEntry> ResolveCachedAuthoringSession(
        std::unordered_map<int64_t, std::shared_ptr<AuthoringSessionEntry>>&
                                                        cache,
        std::mutex&                                     mutex,
        Das::Core::ForeignInterfaceHost::PluginManager& plugin_manager,
        const TaskAuthoringCapability&                  capability,
        int64_t                                         id,
        yyjson::value                                   context,
        int64_t                                         current_revision,
        bool                                            enforce_fresh)
    {
        // 取一次贯穿本次解析：命中比对与（若陈旧而重建的）新条目赋值用
        // 同一读数，重建后的新 session 戳的就是触发这次重建的 locale。
        const std::string current_locale = ReadGlobalLocaleStamp();
        {
            const std::scoped_lock lock(mutex);
            auto                   it = cache.find(id);
            if (it != cache.end())
            {
                // Read-side freshness: a Get following an Apply must rebuild
                // because the provider's document view is built from the
                // context captured at session creation. Apply skips this
                // (enforce_fresh=false) so consecutive Applies reuse one
                // session — unless the locale stamp diverged (see above).
                if ((enforce_fresh
                     && it->second->creation_revision != current_revision)
                    || it->second->creation_locale != current_locale)
                {
                    cache.erase(it);
                }
                else
                {
                    return it->second;
                }
            }
        }

        DasPtr<Das::PluginInterface::IDasTaskAuthoringSession> session;
        auto session_result = CreateAuthoringSessionWithContext(
            plugin_manager,
            capability,
            std::move(context),
            session);
        if (DAS::IsFailed(session_result) || !session)
        {
            return nullptr;
        }

        auto entry = std::make_shared<AuthoringSessionEntry>();
        entry->session = std::move(session);
        entry->creation_revision = current_revision;
        entry->creation_locale = current_locale;

        // emplace (not operator[]) so a concurrent inserter wins and ours is
        // dropped: avoids overwriting a live entry.
        const std::scoped_lock lock(mutex);
        auto                   emplace_result = cache.emplace(id, entry);
        return emplace_result.first->second;
    }

    // Structured result of the shared Apply pipeline. The caller owns response
    // construction (taskId vs entryId) and path-specific error message wording
    // for revisionConflict / sessionCreateFailed / providerFailed kinds.
    // error_locator carries "change[i](kind)" on batch failure — the caller
    // prefixes it onto the final message so the failing element is locatable.
    // rollback is set when an earlier change in the batch already mutated the
    // session document: nothing was persisted, so the caller must tombstone +
    // evict the cache entry (next resolve rebuilds from the persisted
    // pre-batch state).
    struct AuthoringApplyPipelineResult
    {
        bool                   ok = false;
        bool                   rollback = false;
        std::string            error_kind;
        std::string            error_message;
        std::string            error_locator;
        int64_t                current_revision = 0;
        int64_t                next_revision = 0;
        AuthoringProviderApply parsed;
        yyjson::value          authoring;
    };

    // 批量失败定位串：change[i](kind) ——响应消息前缀，标明失败元素在
    // changes[] 中的下标与 kind。
    static std::string MakeBatchChangeLocator(
        std::size_t      change_index,
        std::string_view kind)
    {
        std::string locator = "change[";
        locator += std::to_string(change_index);
        locator += "](";
        locator.append(kind);
        locator += ")";
        return locator;
    }

    // 批内逐 change 诊断聚合：全批（含失败 change）的 diagnostics 按序
    // 拼接，失败响应一次携带完整原因链。
    static void AppendDiagnosticsInto(
        std::vector<yyjson::value>& aggregated,
        const yyjson::value&        diagnostics)
    {
        auto array = diagnostics.as_array();
        if (!array)
        {
            return;
        }
        for (const auto& diagnostic : *array)
        {
            aggregated.push_back(CloneJsonValue(diagnostic));
        }
    }

    static yyjson::value BuildAggregatedDiagnostics(
        const std::vector<yyjson::value>& aggregated)
    {
        yyjson::value diagnostics = Das::Utils::MakeYyjsonArray();
        auto          array_view = diagnostics.as_array();
        for (const auto& diagnostic : aggregated)
        {
            array_view->emplace_back(CloneJsonValue(diagnostic));
        }
        return diagnostics;
    }

    // 批内 kinds 按 "+" 连接为 lastChangeKind（顺序保留）。消费方按自由
    // 文本读取该字段（无定长假设），changes 上限 256 约束了最长情形。
    static std::string JoinChangeKinds(const std::vector<std::string>& kinds)
    {
        std::string joined;
        for (std::size_t index = 0; index < kinds.size(); ++index)
        {
            if (index > 0)
            {
                joined += "+";
            }
            joined += kinds[index];
        }
        return joined;
    }

    // Locked core of the Apply pipeline. The caller has already done the
    // envelope validation, the revision check on a fresh settings read, and the
    // cache resolve, and holds `entry->lock` (unique_lock) so the whole batch
    // — per-change Provider ApplyChange (M), parse, rejection check, and
    // persisted-metadata build — runs atomically with the caller's single
    // trailing persist (W). The request carries the batch wire
    // {baseRevision, changes:[...]}; each element is re-wrapped into the
    // single-change wire {baseRevision, kind, payload} the Provider session
    // contract expects, with the iteration base advancing to the previous
    // change's returned revision. Cross-kind dependencies (an id allocated by
    // an earlier addGraphNode referenced by a later change) are why the batch
    // cannot be validated ahead of mutation — execution order is semantic,
    // atomicity comes from all-or-nothing persist plus the caller's
    // eviction rollback on failure. The whole M-W section lives outside
    // mutex_ and inside entry->lock (per-session serialization — the mutex
    // is not abused as the session lock).
    // Returns true on success (result.parsed/next_revision/authoring filled:
    // acceptedProperties/diagnostics from the last change, document from the
    // last document-bearing change); false on provider/parse/rejection error
    // (result.error_kind/message set, result.error_locator locates the
    // failing change, result.rollback set when an earlier change already
    // mutated the session document, and result.parsed.diagnostics carries the
    // aggregated batch diagnostics so an applyRejected response can surface
    // the real rejection reason).
    static bool RunAuthoringApplyPipelineLocked(
        const std::shared_ptr<AuthoringSessionEntry>& entry,
        const yyjson::value&                          change,
        const yyjson::value&                          source_json,
        int64_t                                       current_revision,
        AuthoringApplyPipelineResult&                 result)
    {
        auto& session = entry->session;

        // 契约层（ValidateAuthoringChange）已保证 changes 形状；此处防线
        // 仅为异常不穿越 ABI 边界——cpp_yyjson object operator[] 对缺失键
        // throw out_of_range，消费侧必须 contains 先行。
        auto request_object = change.as_object();
        if (!request_object
            || !request_object->contains(std::string_view("changes")))
        {
            result.error_kind = "missingField";
            result.error_message = "AuthoringChange.changes required";
            return false;
        }
        auto changes_array =
            (*request_object)[std::string_view("changes")].as_array();
        if (!changes_array)
        {
            result.error_kind = "invalidChanges";
            result.error_message = "AuthoringChange.changes must be an array";
            return false;
        }

        int64_t                    revision_cursor = current_revision;
        std::vector<std::string>   applied_kinds;
        std::vector<yyjson::value> aggregated_diagnostics;
        yyjson::value              last_accepted_properties;
        yyjson::value              last_document;
        yyjson::value              last_command_result;
        bool                       has_command_result = false;

        std::size_t change_index = 0;
        for (const auto& element : *changes_array)
        {
            const std::string change_kind = GetStringField(element, "kind", "");
            result.error_locator =
                MakeBatchChangeLocator(change_index, change_kind);

            yyjson::value single_change(Das::Utils::MakeYyjsonObject());
            auto          single_object = single_change.as_object();
            (*single_object)[std::string_view("baseRevision")] =
                revision_cursor;
            (*single_object)[std::string_view("kind")] = std::make_pair(
                std::string_view(change_kind),
                yyjson::copy_string);
            auto element_object = element.as_object();
            if (!element_object
                || !element_object->contains(std::string_view("payload")))
            {
                result.error_kind = "missingField";
                result.error_message = "changes element payload required";
                result.rollback = change_index > 0;
                return false;
            }
            (*single_object)[std::string_view("payload")] =
                CloneJsonValue((*element_object)[std::string_view("payload")]);

            auto change_json = WrapJsonValue(std::move(single_change));
            DasPtr<Das::ExportInterface::IDasJson> apply_result_json;
            auto apply_result = session->ApplyChange(
                change_json.Get(),
                apply_result_json.Put());
            if (DAS::IsFailed(apply_result) || !apply_result_json)
            {
                result.error_kind = "providerFailed";
                result.error_message = "Authoring provider rejected change";
                result.rollback = change_index > 0;
                result.parsed.diagnostics =
                    BuildAggregatedDiagnostics(aggregated_diagnostics);
                return false;
            }

            auto provider_result = ReadJsonInterface(apply_result_json.Get());
            auto parsed = ParseAuthoringProviderApply(
                provider_result,
                revision_cursor,
                change_kind);
            AppendDiagnosticsInto(aggregated_diagnostics, parsed.diagnostics);
            if (!parsed.error_kind.empty())
            {
                result.error_kind = parsed.error_kind;
                result.error_message = parsed.error_message;
                result.rollback = change_index > 0;
                result.parsed.diagnostics =
                    BuildAggregatedDiagnostics(aggregated_diagnostics);
                return false;
            }

            // Provider-rejected change: the envelope is valid but the provider
            // declined this change (error diagnostics present). Stop the batch
            // — no persist, no store revision advance; surface the real
            // rejection reason via the aggregated diagnostics.
            if (HasErrorDiagnostic(parsed.diagnostics))
            {
                result.error_kind = "applyRejected";
                result.error_message =
                    FirstErrorDiagnosticMessage(parsed.diagnostics);
                if (result.error_message.empty())
                {
                    result.error_message =
                        "Authoring change rejected by provider";
                }
                result.rollback = change_index > 0;
                result.parsed.diagnostics =
                    BuildAggregatedDiagnostics(aggregated_diagnostics);
                return false;
            }

            revision_cursor = parsed.returned_revision;
            applied_kinds.push_back(change_kind);
            last_accepted_properties = std::move(parsed.accepted_properties);
            if (!parsed.document.is_null())
            {
                last_document = std::move(parsed.document);
            }
            if (parsed.has_command_result)
            {
                last_command_result = std::move(parsed.command_result);
                has_command_result = true;
            }
            ++change_index;
        }

        result.parsed.accepted_properties = std::move(last_accepted_properties);
        result.parsed.document = std::move(last_document);
        result.parsed.diagnostics =
            BuildAggregatedDiagnostics(aggregated_diagnostics);
        result.parsed.command_result = std::move(last_command_result);
        result.parsed.has_command_result = has_command_result;
        result.parsed.returned_revision = revision_cursor;
        result.next_revision = revision_cursor;
        result.error_locator.clear();

        yyjson::value previous_authoring;
        if (auto src_obj = source_json.as_object();
            src_obj && src_obj->contains(std::string_view("authoring")))
        {
            previous_authoring =
                CloneJsonValue((*src_obj)[std::string_view("authoring")]);
        }
        result.authoring = MakePersistedAuthoringMetadata(
            result.next_revision,
            JoinChangeKinds(applied_kinds),
            previous_authoring);

        result.ok = true;
        return true;
    }

    // 回滚墓碑命中后的会话重建：构造一个不经缓存的替代 entry（session 从
    // 持久层重建 = 批前状态）。不回填缓存的原因：回填会引入第二个可并发
    // 进入的锁域，破坏 per-session 写锁串行化；不回填只让下一个请求多一
    // 次会话创建（仅批量失败路径可达）。
    static std::shared_ptr<AuthoringSessionEntry> BuildUncachedAuthoringSession(
        Das::Core::ForeignInterfaceHost::PluginManager& plugin_manager,
        const TaskAuthoringCapability&                  capability,
        yyjson::value                                   context,
        int64_t                                         current_revision)
    {
        DasPtr<Das::PluginInterface::IDasTaskAuthoringSession> session;
        auto session_result = CreateAuthoringSessionWithContext(
            plugin_manager,
            capability,
            std::move(context),
            session);
        if (DAS::IsFailed(session_result) || !session)
        {
            return nullptr;
        }
        auto entry = std::make_shared<AuthoringSessionEntry>();
        entry->session = std::move(session);
        entry->creation_revision = current_revision;
        entry->creation_locale = ReadGlobalLocaleStamp();
        return entry;
    }

    // Structured result of the shared Get pipeline. The caller owns response
    // construction (taskId vs entryId/taskTypeGuid/revision) and path-specific
    // error message wording for sessionCreateFailed / providerFailed kinds.
    struct AuthoringGetPipelineResult
    {
        bool                 ok = false;
        std::string          error_kind;
        std::string          error_message;
        AuthoringProviderGet parsed;
    };

    // Shared Get pipeline (Provider GetDocument -> parse). The caller owns
    // ①② locks/IO/context-build and resolves the cached session via
    // GetOrCreateAuthoringSession; this pipeline owns ⑥⑦(GetDocument + parse).
    static AuthoringGetPipelineResult RunAuthoringGetPipeline(
        std::shared_ptr<AuthoringSessionEntry> entry,
        const yyjson::value&                   request)
    {
        AuthoringGetPipelineResult result;

        if (!entry || !entry->session)
        {
            result.error_kind = "sessionCreateFailed";
            result.error_message = "Failed to create authoring session";
            return result;
        }

        auto& session = entry->session;

        auto request_json = WrapJsonValue(CloneJsonValue(request));
        DasPtr<Das::ExportInterface::IDasJson> document_json;
        auto                                   document_result =
            session->GetDocument(request_json.Get(), document_json.Put());
        if (DAS::IsFailed(document_result) || !document_json)
        {
            result.error_kind = "providerFailed";
            result.error_message =
                "Authoring provider failed to create document";
            return result;
        }

        auto provider_result = ReadJsonInterface(document_json.Get());
        auto parsed = ParseAuthoringProviderGet(provider_result);
        if (!parsed.error_kind.empty())
        {
            result.error_kind = parsed.error_kind;
            result.error_message = parsed.error_message;
            return result;
        }
        result.parsed = std::move(parsed);

        result.ok = true;
        return result;
    }

    // Structured result of the shared Compile pipeline. The caller owns
    // response construction (taskId vs entryId) and path-specific error message
    // wording for sessionCreateFailed / providerFailed kinds.
    struct AuthoringCompilePipelineResult
    {
        bool                     ok = false;
        std::string              error_kind;
        std::string              error_message;
        AuthoringProviderCompile parsed;
    };

    // Shared Compile pipeline (session create -> Provider Compile -> parse).
    // Caller owns ①② locks/IO/context-build; pipeline owns ⑤⑥⑦. Session is
    // created inside the pipeline, mirroring the Apply/Get pipeline ordering.
    static AuthoringCompilePipelineResult RunAuthoringCompilePipeline(
        std::shared_ptr<AuthoringSessionEntry> entry,
        const yyjson::value&                   request)
    {
        AuthoringCompilePipelineResult result;

        if (!entry || !entry->session)
        {
            result.error_kind = "sessionCreateFailed";
            result.error_message = "Failed to create authoring session";
            return result;
        }
        auto& session = entry->session;

        auto request_json = WrapJsonValue(CloneJsonValue(request));
        DasPtr<Das::ExportInterface::IDasJson> compile_json;
        auto                                   compile_result =
            session->Compile(request_json.Get(), compile_json.Put());
        if (DAS::IsFailed(compile_result) || !compile_json)
        {
            result.error_kind = "providerFailed";
            result.error_message =
                "Authoring provider failed to compile document";
            return result;
        }

        auto provider_result = ReadJsonInterface(compile_json.Get());
        auto parsed = ParseAuthoringProviderCompile(provider_result);
        if (!parsed.error_kind.empty())
        {
            result.error_kind = parsed.error_kind;
            result.error_message = parsed.error_message;
            return result;
        }
        result.parsed = std::move(parsed);

        result.ok = true;
        return result;
    }

    struct RepositoryProviderCompileResult
    {
        yyjson::value compile;
        std::string   error_kind;
        std::string   message;

        bool Succeeded() const { return error_kind.empty(); }
    };

    static RepositoryProviderCompileResult CompileRepositoryEntryProvider(
        std::shared_ptr<AuthoringSessionEntry> entry,
        const yyjson::value&                   request)
    {
        if (!entry || !entry->session)
        {
            return {
                {},
                "sessionCreateFailed",
                "Failed to create repository authoring session"};
        }
        auto& session = entry->session;

        auto request_json = WrapJsonValue(CloneJsonValue(request));
        DasPtr<Das::ExportInterface::IDasJson> compile_json;
        auto                                   compile_result =
            session->Compile(request_json.Get(), compile_json.Put());
        if (DAS::IsFailed(compile_result) || !compile_json)
        {
            return {
                {},
                "providerFailed",
                "Repository authoring provider failed to compile document"};
        }

        return {ReadJsonInterface(compile_json.Get()), {}, {}};
    }

    static TaskInstanceRecord* FindNextRunnableTask(
        std::vector<TaskInstanceRecord>& task_instances,
        int64_t                          now_unix)
    {
        TaskInstanceRecord* selected = nullptr;
        int64_t             selected_due = 0;
        bool                selected_has_due = false;

        for (auto& inst : task_instances)
        {
            if (!inst.enabled
                || inst.availability != TaskAvailability::Available
                || !inst.task_instance)
            {
                continue;
            }

            if (!inst.next_execution_time)
            {
                return &inst;
            }

            const int64_t due = *inst.next_execution_time;

            if (due > now_unix)
            {
                continue;
            }

            if (!selected || !selected_has_due || due < selected_due)
            {
                selected = &inst;
                selected_due = due;
                selected_has_due = true;
            }
        }

        return selected;
    }

    TaskTypeRecord* SchedulerService::FindTaskType(const DasGuid& task_guid)
    {
        for (auto& ttr : task_types_)
        {
            if (ttr->task_guid == task_guid)
            {
                return ttr.get();
            }
        }
        return nullptr;
    }

    TaskInstanceRecord* SchedulerService::FindTaskInstance(int64_t task_id)
    {
        for (auto& inst : task_instances_)
        {
            if (inst.id == task_id)
            {
                return &inst;
            }
        }
        return nullptr;
    }

    const TaskInstanceRecord* SchedulerService::FindTaskInstance(
        int64_t task_id) const
    {
        for (const auto& inst : task_instances_)
        {
            if (inst.id == task_id)
            {
                return &inst;
            }
        }
        return nullptr;
    }

    Repository::Dto::RepositoryAvailabilityDto
    SchedulerService::DeriveRepositoryAvailabilityLocked(
        const Repository::Dto::RepositoryEntryDto& entry)
    {
        Repository::Dto::RepositoryAvailabilityDto availability;

        DasGuid plugin_guid{};
        DasGuid task_guid{};
        try
        {
            plugin_guid =
                Das::Core::ForeignInterfaceHost::MakeDasGuid(entry.plugin_guid);
            task_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                entry.task_type_guid);
        }
        catch (const std::exception&)
        {
            availability.state = "unavailable";
            availability.reason = "pluginUnavailable";
            availability.message =
                "Repository entry contains an invalid plugin or task GUID";
            return availability;
        }

        if (auto unavailable_plugin =
                repository_plugin_availability_.find(plugin_guid);
            unavailable_plugin != repository_plugin_availability_.end())
        {
            availability.state = "unavailable";
            availability.reason = unavailable_plugin->second.reason;
            availability.message = unavailable_plugin->second.message;
            return availability;
        }

        if (plugin_manager_.FindPluginPackageByGuid(plugin_guid) == nullptr)
        {
            availability.state = "unavailable";
            availability.reason = "pluginUnavailable";
            availability.message =
                "Plugin package is not loaded for the initialized profile";
            return availability;
        }

        auto* task_record = capability_registry_.FindTask(task_guid);
        if (!task_record || task_record->plugin_guid != plugin_guid)
        {
            availability.state = "unavailable";
            availability.reason = "taskTypeUnavailable";
            availability.message =
                "Task type is not declared by the initialized plugin package";
            return availability;
        }

        if (capability_registry_.FindAuthoring(task_guid) == nullptr)
        {
            availability.state = "unavailable";
            availability.reason = "capabilityMissing";
            availability.message =
                "Task type does not declare an authoring capability";
            return availability;
        }

        availability.state = "available";
        return availability;
    }

    bool SchedulerService::SchedulerBusyLocked() const
    {
        return state_.load() != SchedulerState::Stopped
               || current_task_id_.has_value();
    }

    std::optional<AuthoringGuardError>
    SchedulerService::CheckSchedulerIdleLocked() const
    {
        if (SchedulerBusyLocked())
        {
            return AuthoringGuardError{
                "taskWorking",
                "Scheduler is running, stopping or a task is executing"};
        }
        return std::nullopt;
    }

    std::optional<AuthoringGuardError>
    SchedulerService::ResolveAuthoringCapabilityLocked(
        const DasGuid&           task_guid,
        TaskAuthoringCapability& out_capability)
    {
        auto* authoring = capability_registry_.FindAuthoring(task_guid);
        if (!authoring)
        {
            return AuthoringGuardError{
                "capabilityMissing",
                "Task does not declare authoring capability"};
        }
        out_capability = *authoring;
        return std::nullopt;
    }

    // ----------------------------------------------------------------
    // Construction
    // ----------------------------------------------------------------

    SchedulerService::SchedulerService(
        Das::Core::ForeignInterfaceHost::PluginManager&       plugin_manager,
        Das::DasRef<DAS::Core::IPC::MainProcess::IIpcContext> ipc_context)
        : plugin_manager_(plugin_manager), ipc_context_{std::move(ipc_context)},
          bt_scheduler_(
              Das::Core::IPC::schedule_on_business_thread(ipc_context_.get()))
    {
        fsm_ = std::make_unique<boost::sml::sm<FsmDefinition>>(*this);
        config_persist_thread_ =
            std::thread(&SchedulerService::ConfigPersistThreadLoop, this);
        // D-87-08: 注册为 PluginManager 的 lifecycle listener，plugin
        // load/unload 后全清 authoring cache。SchedulerService 持有
        // PluginManager& 引用，结构上 PluginManager 存活长于
        // SchedulerService，裸指针注册安全。
        plugin_manager_.SetPluginLifecycleListener(this);
    }

    SchedulerService::~SchedulerService()
    {
        // 防御：清注册。PluginManager 结构上存活长于 SchedulerService（持有
        // PluginManager& 引用），此调用成本为零；若实际析构顺序变化也避免
        // PluginManager 持悬空 listener 指针。
        plugin_manager_.SetPluginLifecycleListener(nullptr);

        // 禁止在 BusinessThread 上析构（生产 HTTP 路径全程跑在 BT 上，本
        // 析构只发生在主线程/测试线程）：下面是阻塞排空，BT 上自等即死锁。
        // 先等 tick 生产者物理停产——顺序就是修复核心，先关 persist 消费者
        // 会丢最后一条事件。
        static_cast<void>(stdexec::sync_wait(scope_.on_empty()));

        // asio timer 析构 cancel 未决 wait，handler 以 aborted 完成且其
        // !ec 检查使其不触碰 this。
        tick_timer_.reset();

        ShutdownConfigPersistQueue();
    }

    void SchedulerService::OnPluginLifecycleEvent(
        Das::Core::ForeignInterfaceHost::PluginLifecyclePhase /*phase*/,
        const DasGuid& /*plugin_guid*/)
    {
        // Initialize() has already cleared both caches while holding mutex_.
        // PluginManager may notify synchronously from the initiating thread or
        // concurrently from a Host callback. In either case, acquiring mutex_
        // here is redundant and may deadlock with the active unload path.
        if (suppress_plugin_lifecycle_invalidation_.load())
        {
            return;
        }

        // Any plugin change outside the SS-initiated window invalidates every
        // authoring session so the next access rebuilds against fresh state.
        InvalidateAuthoringSessions();
    }

    DasResult SchedulerService::CreateTaskInstance(
        const TaskTypeRecord&            task_type,
        Das::PluginInterface::IDasTask** pp_out_task)
    {
        if (!pp_out_task)
        {
            return DAS_E_INVALID_POINTER;
        }

        DasOutPtr<Das::PluginInterface::IDasTask> result(pp_out_task);

        auto create_result = plugin_manager_.CreateFeatureInterface(
            task_type.plugin_guid,
            task_type.feature_index,
            DasIidOf<Das::PluginInterface::IDasTask>(),
            reinterpret_cast<void**>(result.Put()));
        if (IsOk(create_result) && result)
        {
            result.Keep();
            return create_result;
        }

        if (create_result != DAS_E_NOT_FOUND || !task_type.prototype_task)
        {
            return create_result;
        }

        result.Set(task_type.prototype_task.Get());
        result.Keep();
        return DAS_S_OK;
    }

    // ----------------------------------------------------------------
    // Initialize
    // ----------------------------------------------------------------

    DasResult SchedulerService::Initialize(
        const std::filesystem::path& plugin_dir,
        const std::vector<DasGuid>&  disabled_guids)
    {
        // Phase 1: Short lock for state validation and cleanup
        {
            const std::scoped_lock lock(mutex_);

            if (state_.load() == SchedulerState::Running
                || state_.load() == SchedulerState::Stopping)
            {
                DAS_CORE_LOG_ERROR(
                    "SchedulerService::Initialize: reject, scheduler is "
                    "Running or Stopping");
                return DAS_E_TASK_WORKING;
            }

            if (initialized_)
            {
                // Re-initialize: clear existing runtime state
                task_types_.clear();
                task_instances_.clear();
                task_repository_store_.reset();
                capability_registry_.Clear();
                repository_plugin_availability_.clear();
                InvalidateAuthoringSessionsLocked();

                const Das::Utils::ScopeGuardVoid lifecycle_invalidation_guard{
                    [this]()
                    { suppress_plugin_lifecycle_invalidation_.store(true); },
                    [this]()
                    { suppress_plugin_lifecycle_invalidation_.store(false); }};
                for (auto& loaded_plugin_path :
                     std::views::reverse(loaded_plugin_paths_))
                {
                    plugin_manager_.UnloadPlugin(loaded_plugin_path);
                }
                loaded_plugin_paths_.clear();
                initialized_ = false;
            }
        } // lock released

        // Phase 2: Lock-free I/O — directory traversal, manifest reading,
        // plugin loading, and SettingsManager reads all execute outside
        // mutex_.
        std::error_code ec;
        auto            dir_iter = std::filesystem::directory_iterator(
            plugin_dir,
            std::filesystem::directory_options::skip_permission_denied,
            ec);
        if (ec)
        {
            DAS_CORE_LOG_WARN(
                "SchedulerService::Initialize: failed to iterate "
                "plugin directory {}: {}",
                Das::Utils::PathToUtf8(plugin_dir),
                ec.message());
            return DAS_E_FAIL;
        }

        // Temp vectors to accumulate loaded data outside the lock
        std::vector<std::filesystem::path>           temp_paths;
        std::vector<std::unique_ptr<TaskTypeRecord>> temp_types;
        std::unordered_map<DasGuid, RepositoryPluginAvailabilityState>
            temp_repository_plugin_availability;

        // Load allowed plugin packages
        for (const auto& entry : dir_iter)
        {
            std::filesystem::path manifest_path;
            if (entry.is_directory())
            {
                auto dirname = Das::Utils::PathToUtf8(entry.path().filename());
                auto marker =
                    entry.path()
                    / Das::Utils::PathFromUtf8(dirname + ".willBeDelete");
                if (std::filesystem::exists(marker))
                {
                    auto banned_manifest_path = FindManifest(entry.path());
                    if (!banned_manifest_path.empty())
                    {
                        try
                        {
                            if (auto desc = ReadPluginManifestDesc(
                                    banned_manifest_path))
                            {
                                temp_repository_plugin_availability[desc->guid] =
                                    {"pluginUnavailable",
                                     "Plugin package is marked for deletion"};
                            }
                        }
                        catch (const std::exception& e)
                        {
                            DAS_CORE_LOG_WARN(
                                "SchedulerService::Initialize: failed to "
                                "parse banned plugin manifest: {}",
                                e.what());
                        }
                    }
                    continue;
                }

                manifest_path = FindManifest(entry.path());
            }
            else if (entry.path().extension() == DAS_PATH_LITERAL(".json"))
            {
                manifest_path = entry.path();
            }

            if (manifest_path.empty())
            {
                continue;
            }

            try
            {
                auto desc_opt = ReadPluginManifestDesc(manifest_path);
                if (!desc_opt)
                {
                    DAS_CORE_LOG_WARN(
                        "SchedulerService::Initialize: failed to "
                        "parse manifest: {}",
                        Das::Utils::PathToUtf8(manifest_path));
                    continue;
                }

                PluginPackageDesc desc = std::move(*desc_opt);

                if (std::find(
                        disabled_guids.begin(),
                        disabled_guids.end(),
                        desc.guid)
                    != disabled_guids.end())
                {
                    temp_repository_plugin_availability[desc.guid] = {
                        "pluginUnavailable",
                        "Plugin package is disabled for scheduler initialize"};
                    DAS_CORE_LOG_INFO(
                        "SchedulerService::Initialize: skipping "
                        "disabled plugin: {}",
                        desc.name);
                    continue;
                }

                auto load_result = plugin_manager_.LoadPlugin(manifest_path);
                if (DAS::IsFailed(load_result))
                {
                    temp_repository_plugin_availability[desc.guid] = {
                        "pluginLoadFailed",
                        "Plugin package failed to load during scheduler "
                        "initialize"};
                    DAS_CORE_LOG_WARN(
                        "SchedulerService::Initialize: failed to "
                        "load plugin {}, result={}",
                        desc.name,
                        load_result);
                    continue;
                }

                temp_paths.push_back(manifest_path);
            }
            catch (const std::exception& e)
            {
                DAS_CORE_LOG_WARN(
                    "SchedulerService::Initialize: failed to parse "
                    "manifest: {}",
                    e.what());
            }
        }

        // Collect IDasTask features and build available task types
        auto task_features = plugin_manager_.GetFeaturesByType(
            Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK);
        for (auto* feature_info : task_features)
        {
            DasPtr<Das::PluginInterface::IDasTask> task;
            auto qi_result = feature_info->interface_ptr->QueryInterface(
                DasIidOf<Das::PluginInterface::IDasTask>(),
                reinterpret_cast<void**>(task.Put()));
            if (!IsOk(qi_result) || !task)
            {
                continue;
            }

            auto type_record = std::make_unique<TaskTypeRecord>();
            type_record->prototype_task = task;
            type_record->plugin_guid = feature_info->plugin_guid;
            type_record->feature_index = feature_info->feature_index;

            // Get task type GUID from IDasTypeInfo
            DasGuid task_guid{};
            auto    guid_result = task->GetGuid(&task_guid);
            if (!IsOk(guid_result))
            {
                DAS_CORE_LOG_WARN(
                    "SchedulerService::Initialize: failed to get task "
                    "GUID, result={}",
                    guid_result);
                continue;
            }
            type_record->task_guid = task_guid;

            // Find manifest task descriptor metadata
            auto* pkg_desc = plugin_manager_.FindPluginPackageByGuid(
                feature_info->plugin_guid);
            if (pkg_desc)
            {
                auto td_it = pkg_desc->task_descriptors.find(task_guid);
                if (td_it != pkg_desc->task_descriptors.end())
                {
                    const auto& td = td_it->second;
                    type_record->name = td.name;
                    type_record->description = td.description;
                    type_record->game_name = td.game_name;
                    type_record->descriptors = td.descriptors;
                    capability_registry_.AddTaskDescriptor(
                        feature_info->plugin_guid,
                        task_guid,
                        feature_info->feature_index,
                        td);
                }
            }

            temp_types.push_back(std::move(type_record));
        }

        DAS_CORE_LOG_INFO(
            "SchedulerService::Initialize: discovered {} task types",
            temp_types.size());

        // Phase 3: SettingsManager reads for task instances (lock-free
        // from SchedulerService::mutex_ perspective; SettingsManager uses
        // per-key mutex internally).
        auto& settings = plugin_manager_.GetSettingsManager();
        auto  scheduler_index = settings.GetSchedulerIndexJson("0");

        std::vector<TaskInstanceRecord> temp_instances;

        auto sched_obj = scheduler_index.as_object();
        if (sched_obj && sched_obj->contains(std::string_view("taskOrder"))
            && sched_obj->operator[](std::string_view("taskOrder")).is_array())
        {
            auto task_order_arr =
                *sched_obj->operator[](std::string_view("taskOrder"))
                     .as_array();
            for (auto&& task_id_val : task_order_arr)
            {
                if (!task_id_val.is_int())
                {
                    continue;
                }
                const int64_t tid = static_cast<int64_t>(task_id_val);

                auto instance_json = settings.GetTaskInstanceJson("0", tid);
                auto inst_obj_opt = instance_json.as_object();
                TaskInstanceRecord rec;
                rec.id = tid;

                if (!inst_obj_opt || !instance_json.is_object())
                {
                    rec.availability = TaskAvailability::Invalid;
                    rec.unavailability_reason =
                        "task instance file is missing or corrupt";
                    temp_instances.push_back(std::move(rec));
                    continue;
                }
                const auto& inst_obj = *inst_obj_opt;

                // Parse taskGuid
                const auto task_guid_key = std::string_view("taskGuid");
                if (!inst_obj.contains(task_guid_key))
                {
                    rec.availability = TaskAvailability::Invalid;
                    rec.unavailability_reason = "missing taskGuid";
                    temp_instances.push_back(std::move(rec));
                    continue;
                }

                auto tg_val = inst_obj[task_guid_key];
                auto tg_str = tg_val.as_string();
                if (!tg_str)
                {
                    rec.availability = TaskAvailability::Invalid;
                    rec.unavailability_reason = "invalid taskGuid";
                    temp_instances.push_back(std::move(rec));
                    continue;
                }

                try
                {
                    rec.task_guid =
                        Das::Core::ForeignInterfaceHost::MakeDasGuid(
                            std::string(*tg_str));
                }
                catch (const std::exception& e)
                {
                    rec.availability = TaskAvailability::Invalid;
                    rec.unavailability_reason =
                        std::string("invalid taskGuid: ") + e.what();
                    temp_instances.push_back(std::move(rec));
                    continue;
                }

                // Parse pluginGuid
                const auto plugin_guid_key = std::string_view("pluginGuid");
                if (inst_obj.contains(plugin_guid_key))
                {
                    auto pg_val = inst_obj[plugin_guid_key];
                    auto pg_str = pg_val.as_string();
                    if (pg_str)
                    {
                        try
                        {
                            rec.plugin_guid =
                                Das::Core::ForeignInterfaceHost::MakeDasGuid(
                                    std::string(*pg_str));
                        }
                        catch (const std::exception& e)
                        {
                            rec.availability = TaskAvailability::Invalid;
                            rec.unavailability_reason =
                                std::string("invalid pluginGuid: ") + e.what();
                            temp_instances.push_back(std::move(rec));
                            continue;
                        }
                    }
                    else if (!pg_val.is_null())
                    {
                        rec.availability = TaskAvailability::Invalid;
                        rec.unavailability_reason = "invalid pluginGuid";
                        temp_instances.push_back(std::move(rec));
                        continue;
                    }
                }

                // Parse nextExecutionTime
                auto net_key = std::string_view("nextExecutionTime");
                if (inst_obj.contains(net_key) && !inst_obj[net_key].is_null())
                {
                    const auto& net_val = inst_obj[net_key];
                    auto        net_int = net_val.as_sint();
                    if (net_int)
                    {
                        rec.next_execution_time = *net_int;
                    }
                }

                // Parse enabled (defaults to true when absent for backward
                // compatibility with persisted instances that predate the
                // field). Never throws: a malformed value falls back to true.
                rec.enabled = GetBoolField(instance_json, "enabled", true);

                // Parse properties
                auto props_key = std::string_view("properties");
                if (inst_obj.contains(props_key)
                    && inst_obj[props_key].is_object())
                {
                    rec.properties = CloneJsonValue(inst_obj[props_key]);
                }
                else
                {
                    rec.properties = Das::Utils::MakeYyjsonObject();
                }

                auto authoring_key = std::string_view("authoring");
                if (inst_obj.contains(authoring_key)
                    && inst_obj[authoring_key].is_object())
                {
                    rec.authoring = CloneJsonValue(inst_obj[authoring_key]);
                }

                // Link to available task type — note: FindTaskType
                // searches temp_types (not yet committed), so we
                // search it manually here
                TaskTypeRecord* type_rec = nullptr;
                for (auto& ttr : temp_types)
                {
                    if (ttr->task_guid == rec.task_guid)
                    {
                        type_rec = ttr.get();
                        break;
                    }
                }

                if (type_rec)
                {
                    rec.task_type = type_rec;
                    rec.plugin_guid = type_rec->plugin_guid;
                    auto create_result =
                        CreateTaskInstance(*type_rec, rec.task_instance.Put());
                    if (IsOk(create_result) && rec.task_instance)
                    {
                        rec.availability = TaskAvailability::Available;
                    }
                    else
                    {
                        rec.availability = TaskAvailability::Unavailable;
                        rec.unavailability_reason =
                            "failed to create task instance";
                    }
                }
                else
                {
                    rec.availability = TaskAvailability::Unavailable;
                    rec.unavailability_reason =
                        "task type not found among loaded plugins";
                }

                temp_instances.push_back(std::move(rec));
            }
        }

        // Phase 4: Short lock to commit results to runtime state
        {
            const std::scoped_lock lock(mutex_);
            task_types_ = std::move(temp_types);
            task_instances_ = std::move(temp_instances);
            task_repository_store_ = std::make_shared<TaskRepositoryStore>(
                plugin_manager_.GetSettingsManager(),
                "0");
            loaded_plugin_paths_ = std::move(temp_paths);
            repository_plugin_availability_ =
                std::move(temp_repository_plugin_availability);
            initialized_ = true;
        }

        DAS_CORE_LOG_INFO(
            "SchedulerService::Initialize: materialized {} task instances",
            task_instances_.size());

        return DAS_S_OK;
    }

    // ----------------------------------------------------------------
    // Enable / Disable / Status
    // ----------------------------------------------------------------

    DasResult SchedulerService::Enable()
    {
        {
            const std::scoped_lock lock(mutex_);
            // 手动立即执行期间拒绝启动调度：执行槽被占时一切状态切换
            // 与 task 变更都视为 busy（卡住语义）。
            if (current_task_id_.has_value())
            {
                return DAS_E_TASK_WORKING;
            }
            fsm_->process_event(EvtEnable{});
        }
        // FsmActEnterRunning 置 Running 但不起 tick：先用最新 schedule 重算所有
        // task 的 next_execution_time（覆盖非 Running 态 apply 的 schedule 变更
        // 与 off→daily 残留的 sentinel），再起 tick，避免拿旧值把 tick
        // 定到远期。
        RefreshNextExecutionTimes();
        {
            const std::scoped_lock lock(mutex_);
            const int64_t now_unix = SystemClock::to_time_t(SystemClock::now());
            StartTickTimer(ComputeNextDelay(task_instances_, now_unix));
        }
        NotifyStateChange();
        return fsm_last_result_;
    }

    DasResult SchedulerService::Disable()
    {
        bool task_idle = false;
        {
            const std::scoped_lock lock(mutex_);
            fsm_->process_event(EvtDisable{});
            // 无 task 在跑：立即收尾 Stopped。有 task：保持 Stopping，等 OnTick
            // task 完成时 process EvtTickCompleted 收尾。
            task_idle = !current_task_id_.has_value();
            if (task_idle)
            {
                fsm_->process_event(EvtTickCompleted{});
            }
        }
        NotifyStateChange();

        // 排空等待仅限无任务在执行的路径（与上面的 FSM 分支同判据）：此刻
        // 无人再驱动 tick 收尾，不排空就把在飞 tick 留给调用方竞速（丢
        // 最后一条 persist 事件的窗口）。任务在执行的路径不等待——当前
        // tick 的收尾由 OnTick 的 EvtTickCompleted 链驱动，且调用方约定
        // 在 Stopping 窗口继续与调度器交互，等待会与其形成双等（任务执行
        // 中的 stop 请求还会被 Do 的跨进程同步调用经嵌套泵拉进 OnTick
        // 内部，此时等待等的是自己栈下层的完成）。生产 stop 路径经
        // Router 派发在 BusinessThread 上调用本方法：BT 上
        // sync_wait(on_empty) 自等死锁（HostLauncher Pitfall 4），改泵化
        // 等待——等待期间继续处理入站队列，排空链上的 tick 就在本线程泵
        // 内执行，最后一个 op 完成即 scope 归零、continuation 就地置
        // drain_complete_（泵嵌套是递归设计）。禁止把泵化等待换回
        // sync_wait。非 BT 调用方（测试线程）直接阻塞等待。
        if (task_idle)
        {
            if (Das::Core::IPC::IsOnBusinessThread())
            {
                drain_complete_.store(false);
                stdexec::start_detached(
                    scope_.on_empty()
                    | stdexec::then([this] { drain_complete_.store(true); }));
                static_cast<void>(
                    Das::Core::IPC::PumpBusinessThreadUntilPredicate(
                        "scheduler_disable_drain",
                        [this] { return drain_complete_.load(); }));
            }
            else
            {
                static_cast<void>(stdexec::sync_wait(scope_.on_empty()));
            }
        }
        return fsm_last_result_;
    }

    DasResult SchedulerService::RunTaskNow(int64_t task_id)
    {
        {
            const std::scoped_lock lock(mutex_);

            if (!initialized_)
            {
                return DAS_E_OBJECT_NOT_INIT;
            }

            // 状态隔离：Running/Stopping 是调度器的时间，拒绝手动插队；
            // Stopped 且执行槽空闲才允许立即执行。
            if (SchedulerBusyLocked())
            {
                return DAS_E_TASK_WORKING;
            }

            auto* inst = FindTaskInstance(task_id);
            if (!inst)
            {
                return DAS_E_NOT_FOUND;
            }
            if (inst->availability != TaskAvailability::Available)
            {
                DAS_CORE_LOG_WARN(
                    "SchedulerService::RunTaskNow: task {} unavailable",
                    task_id);
                return DAS_E_FAIL;
            }
            if (!inst->enabled)
            {
                DAS_CORE_LOG_WARN(
                    "SchedulerService::RunTaskNow: task {} disabled",
                    task_id);
                return DAS_E_FAIL;
            }
            if (!inst->task_instance)
            {
                DAS_CORE_LOG_WARN(
                    "SchedulerService::RunTaskNow: task {} has no materialized "
                    "instance",
                    task_id);
                return DAS_E_FAIL;
            }

            // 手动执行不进调度链：直接投递单任务执行操作，点谁跑谁、
            // 跑完即止——不经 FindNextRunnableTask 全局选任务，也不触
            // 发排空，其他任务的历史残留 overdue 不被连带清算。
        }

        // 投递 BusinessThread 后立即返回；ExecuteTaskOnce 落地时在锁内
        // 复检 scheduler 状态与任务可用性（投递窗口内被 Enable/删除/
        // 禁用则作废），并在 BusinessThread 串行执行，不阻塞调用线程。
        SpawnExecuteOnce(task_id);
        return DAS_S_OK;
    }

    SchedulerState SchedulerService::Status() const { return state_.load(); }

    void SchedulerService::NotifyStateChange()
    {
        std::shared_ptr<StateNotify> notify;
        {
            const std::scoped_lock lock(state_notify_mutex_);
            notify = state_notify_;
        }
        if (!notify || !*notify)
        {
            return;
        }
        auto state_json = Get();

        auto event = Das::Utils::MakeYyjsonObject();
        auto obj = *event.as_object();
        obj[std::string_view("code")] = static_cast<int64_t>(DAS_S_OK);
        obj[std::string_view("msg")] = std::string_view("");
        obj[std::string_view("data")] = state_json;

        auto state_serialized = Das::Utils::SerializeYyjsonValue(event, false);
        if (state_serialized)
        {
            (*notify)(state_serialized->c_str());
        }
    }

    bool SchedulerService::FsmGuardNotInitialized() const
    {
        return !initialized_;
    }

    bool SchedulerService::FsmGuardNoAvailableTask() const
    {
        for (const auto& inst : task_instances_)
        {
            if (inst.availability == TaskAvailability::Available)
            {
                return false;
            }
        }
        return true;
    }

    void SchedulerService::FsmActEnterRunning()
    {
        state_.store(SchedulerState::Running);
        if (!tick_timer_)
        {
            tick_timer_ = std::make_unique<boost::asio::steady_timer>(
                ipc_context_.get().GetIoContext());
        }
        // tick 由 Enable 锁外段在 RefreshNextExecutionTimes 之后启动，用刷新后
        // 的 next_execution_time 算 delay，避免拿非 Running 态 apply 残留的
        // sentinel/旧值把 tick 定到远期。
        fsm_last_result_ = DAS_S_OK;
        DAS_CORE_LOG_INFO("SchedulerService::Enable: scheduler started");
    }

    void SchedulerService::FsmActEnterStopping()
    {
        state_.store(SchedulerState::Stopping);
        if (tick_timer_)
        {
            tick_timer_->cancel();
        }
        if (stop_token_)
        {
            auto* impl = static_cast<Das::Core::Utils::DasStopTokenImpl*>(
                stop_token_.Get());
            if (impl)
            {
                impl->RequestStop();
            }
        }
        fsm_last_result_ = DAS_S_OK;
        DAS_CORE_LOG_INFO("SchedulerService::Disable: scheduler stopping");
    }

    void SchedulerService::FsmActEnterStopped()
    {
        stop_token_.Reset();
        state_.store(SchedulerState::Stopped);
        fsm_last_result_ = DAS_S_OK;
        DAS_CORE_LOG_INFO("SchedulerService::Disable: scheduler stopped");
    }

    void SchedulerService::FsmActRejectNotInitialized()
    {
        fsm_last_result_ = DAS_E_OBJECT_NOT_INIT;
        DAS_CORE_LOG_ERROR(
            "SchedulerService::Enable: not initialized, call Initialize first");
    }

    void SchedulerService::FsmActRejectNoTask()
    {
        fsm_last_result_ = DAS_E_OBJECT_NOT_INIT;
        DAS_CORE_LOG_ERROR(
            "SchedulerService::Enable: no available task instances");
    }

    void SchedulerService::FsmActRejectFail()
    {
        fsm_last_result_ = DAS_E_FAIL;
        DAS_CORE_LOG_WARN("SchedulerService: invalid state for operation");
    }

    void SchedulerService::FsmActRejectIgnored()
    {
        fsm_last_result_ = DAS_S_OK;
        DAS_CORE_LOG_WARN(
            "SchedulerService::Disable: already stopping, ignored");
    }

    std::optional<DasGuid> SchedulerService::FindTaskExecutionComponent(
        const DasGuid& task_guid) const
    {
        const std::scoped_lock lock(mutex_);
        auto*                  execution_component =
            capability_registry_.FindExecutionComponent(task_guid);
        if (execution_component == nullptr)
        {
            return std::nullopt;
        }
        return *execution_component;
    }

    // ----------------------------------------------------------------
    // Get: merged scheduler state JSON
    // ----------------------------------------------------------------

    yyjson::value SchedulerService::Get()
    {
        const std::scoped_lock lock(mutex_);

        auto result = Das::Utils::MakeYyjsonObject();
        auto result_obj = *result.as_object();

        result_obj[std::string_view("state")] = std::string_view(
            state_.load() == SchedulerState::Running    ? "running"
            : state_.load() == SchedulerState::Stopping ? "stopping"
                                                        : "stopped");

        // 执行中才输出 runningTaskId，空闲时省略该 key（UI 视缺 key 为
        // 无任务执行）。
        if (current_task_id_)
        {
            result_obj[std::string_view("runningTaskId")] = *current_task_id_;
        }

        // Available task types
        auto types_arr = Das::Utils::MakeYyjsonArray();
        auto types_arr_ref = *types_arr.as_array();
        for (const auto& ttr : task_types_)
        {
            auto type_obj = Das::Utils::MakeYyjsonObject();
            auto type_ref = *type_obj.as_object();
            type_ref[std::string_view("taskGuid")] =
                yyjson::value(GuidToString(ttr->task_guid));
            type_ref[std::string_view("pluginGuid")] =
                yyjson::value(GuidToString(ttr->plugin_guid));
            type_ref[std::string_view("name")] = yyjson::value(ttr->name);
            type_ref[std::string_view("description")] =
                yyjson::value(ttr->description);
            if (ttr->game_name)
            {
                type_ref[std::string_view("gameName")] =
                    yyjson::value(*ttr->game_name);
            }

            if (auto* authoring =
                    capability_registry_.FindAuthoring(ttr->task_guid))
            {
                type_ref[std::string_view("configurationMode")] =
                    std::string_view("authoring");
                type_ref[std::string_view("propertiesWritable")] = false;

                yyjson::value authoring_obj(Das::Utils::MakeYyjsonObject());
                auto          authoring_ref = authoring_obj.as_object();
                (*authoring_ref)[std::string_view("factoryGuid")] =
                    yyjson::value(
                        GuidToString(authoring->authoring_factory_guid));

                yyjson::value supported_kinds(Das::Utils::MakeYyjsonArray());
                auto          supported_ref = supported_kinds.as_array();
                for (const auto& kind : authoring->supported_kinds)
                {
                    supported_ref->emplace_back(yyjson::value(kind));
                }
                (*authoring_ref)[std::string_view("supportedKinds")] =
                    std::move(supported_kinds);
                type_ref[std::string_view("authoring")] =
                    std::move(authoring_obj);
            }
            else
            {
                type_ref[std::string_view("configurationMode")] =
                    std::string_view("properties");
                type_ref[std::string_view("propertiesWritable")] = true;
            }

            // Include descriptors
            auto desc_arr = Das::Utils::MakeYyjsonArray();
            auto desc_arr_ref = *desc_arr.as_array();
            for (const auto& d : ttr->descriptors)
            {
                auto desc_obj = Das::Utils::MakeYyjsonObject();
                auto desc_ref = *desc_obj.as_object();
                desc_ref[std::string_view("name")] = yyjson::value(d.name);
                desc_ref[std::string_view("type")] =
                    static_cast<int64_t>(d.type);
                desc_ref[std::string_view("required")] = d.required;
                if (d.description)
                {
                    desc_ref[std::string_view("description")] =
                        yyjson::value(*d.description);
                }
                desc_arr_ref.emplace_back(std::move(desc_obj));
            }
            type_ref[std::string_view("descriptors")] = std::move(desc_arr);
            types_arr_ref.emplace_back(std::move(type_obj));
        }
        result_obj[std::string_view("availableTaskTypes")] =
            std::move(types_arr);

        // Queued task instances
        auto tasks_arr = Das::Utils::MakeYyjsonArray();
        auto tasks_arr_ref = *tasks_arr.as_array();
        for (const auto& inst : task_instances_)
        {
            auto task_obj = Das::Utils::MakeYyjsonObject();
            auto task_ref = *task_obj.as_object();
            task_ref[std::string_view("id")] = inst.id;
            task_ref[std::string_view("taskGuid")] =
                yyjson::value(GuidToString(inst.task_guid));
            task_ref[std::string_view("pluginGuid")] =
                yyjson::value(GuidToString(inst.plugin_guid));
            const bool has_authoring =
                capability_registry_.FindAuthoring(inst.task_guid) != nullptr;
            task_ref[std::string_view("configurationMode")] =
                has_authoring ? std::string_view("authoring")
                              : std::string_view("properties");
            task_ref[std::string_view("propertiesWritable")] = !has_authoring;
            if (has_authoring && !inst.authoring.is_null())
            {
                task_ref[std::string_view("authoring")] =
                    CloneJsonValue(inst.authoring);
            }

            if (inst.availability == TaskAvailability::Available)
            {
                task_ref[std::string_view("availability")] =
                    std::string_view("available");
            }
            else if (inst.availability == TaskAvailability::Unavailable)
            {
                task_ref[std::string_view("availability")] =
                    std::string_view("unavailable");
                task_ref[std::string_view("unavailabilityReason")] =
                    yyjson::value(inst.unavailability_reason);
            }
            else
            {
                task_ref[std::string_view("availability")] =
                    std::string_view("invalid");
                task_ref[std::string_view("unavailabilityReason")] =
                    yyjson::value(inst.unavailability_reason);
            }

            task_ref[std::string_view("enabled")] = inst.enabled;

            if (inst.next_execution_time)
            {
                task_ref[std::string_view("nextExecutionTime")] =
                    *inst.next_execution_time;
            }
            else
            {
                task_ref[std::string_view("nextExecutionTime")] = nullptr;
            }

            if (!inst.properties.is_null())
            {
                task_ref[std::string_view("properties")] =
                    CloneJsonValue(inst.properties);
            }
            else
            {
                task_ref[std::string_view("properties")] =
                    Das::Utils::MakeYyjsonObject();
            }
            tasks_arr_ref.emplace_back(std::move(task_obj));
        }

        result_obj[std::string_view("tasks")] = std::move(tasks_arr);
        return result;
    }

    yyjson::value SchedulerService::GetTaskRepository()
    {
        std::shared_ptr<TaskRepositoryStore> store;
        {
            const std::scoped_lock lock(mutex_);
            if (!initialized_ || !task_repository_store_)
            {
                return MakeRepositoryResponseError(
                    "notInitialized",
                    "Scheduler profile has not been initialized");
            }
            store = task_repository_store_;
        }

        Repository::Dto::RepositoryGetResponseDto response;
        response.entries = store->ListEntries();
        {
            const std::scoped_lock lock(mutex_);
            for (auto& entry : response.entries)
            {
                entry.availability = DeriveRepositoryAvailabilityLocked(entry);
            }
        }
        return CloneJsonValue(yyjson::object(response));
    }

    yyjson::value SchedulerService::CreateRepositoryEntry(
        const yyjson::value& request)
    {
        Repository::Dto::CreateRepositoryEntryRequestDto create_request;
        if (!ParseCreateRepositoryEntryRequest(request, create_request))
        {
            return MakeRepositoryResponseError(
                "invalidJson",
                "Repository create request must include pluginGuid and "
                "taskTypeGuid strings");
        }

        DasGuid plugin_guid{};
        DasGuid task_guid{};
        try
        {
            plugin_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                create_request.plugin_guid);
            task_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                create_request.task_type_guid);
        }
        catch (const std::exception&)
        {
            return MakeRepositoryResponseError(
                "invalidGuid",
                "Repository create request contains an invalid GUID");
        }

        std::shared_ptr<TaskRepositoryStore> store;
        TaskTypeRecord                       type_snapshot;
        {
            const std::scoped_lock lock(mutex_);

            if (state_.load() == SchedulerState::Running
                || state_.load() == SchedulerState::Stopping)
            {
                return MakeRepositoryResponseError(
                    "taskWorking",
                    "Scheduler is running or stopping");
            }

            if (!initialized_ || !task_repository_store_)
            {
                return MakeRepositoryResponseError(
                    "notInitialized",
                    "Scheduler profile has not been initialized");
            }

            auto* type_record = FindTaskType(task_guid);
            if (!type_record || type_record->plugin_guid != plugin_guid)
            {
                return MakeRepositoryResponseError(
                    "notFound",
                    "Task type was not found in the initialized registry");
            }

            type_snapshot = *type_record;
            store = task_repository_store_;
        }

        Repository::Dto::RepositoryEntryDto entry;
        auto                                create_result = store->CreateEntry(
            create_request,
            type_snapshot.name,
            type_snapshot.descriptors,
            entry);
        if (DAS::IsFailed(create_result))
        {
            return MakeRepositoryResponseError(
                RepositoryErrorKindFromResult(create_result),
                "Failed to create repository entry");
        }

        return CloneJsonValue(yyjson::object(entry));
    }

    DasResult SchedulerService::DeleteRepositoryEntry(int64_t entry_id)
    {
        std::shared_ptr<TaskRepositoryStore> store;
        {
            const std::scoped_lock lock(mutex_);
            if (!initialized_ || !task_repository_store_)
            {
                return DAS_E_OBJECT_NOT_INIT;
            }
            store = task_repository_store_;
        }

        auto result = store->DeleteEntry(entry_id);
        if (DAS::IsFailed(result))
        {
            return result;
        }
        // Entry deleted — drop its cached authoring session so a recreated
        // entry id rebuilds against fresh state.
        {
            const std::scoped_lock lock(mutex_);
            EvictAuthoringSession(repository_authoring_sessions_, entry_id);
        }
        return DAS_S_OK;
    }

    yyjson::value SchedulerService::RenameRepositoryEntry(
        int64_t              entry_id,
        const yyjson::value& request)
    {
        Repository::Dto::RenameRepositoryEntryRequestDto rename_request;
        if (!ParseRenameRepositoryEntryRequest(request, rename_request))
        {
            return MakeRepositoryResponseError(
                "invalidJson",
                "Repository rename request must include displayName string");
        }

        std::shared_ptr<TaskRepositoryStore> store;
        {
            const std::scoped_lock lock(mutex_);
            if (!initialized_ || !task_repository_store_)
            {
                return MakeRepositoryResponseError(
                    "notInitialized",
                    "Scheduler profile has not been initialized");
            }
            store = task_repository_store_;
        }

        Repository::Dto::RepositoryEntryDto entry;
        auto                                rename_result =
            store->RenameEntry(entry_id, rename_request, entry);
        if (DAS::IsFailed(rename_result))
        {
            return MakeRepositoryResponseError(
                RepositoryErrorKindFromResult(rename_result),
                "Failed to rename repository entry");
        }

        {
            const std::scoped_lock lock(mutex_);
            entry.availability = DeriveRepositoryAvailabilityLocked(entry);
        }

        return CloneJsonValue(yyjson::object(entry));
    }

    std::shared_ptr<AuthoringSessionEntry>
    SchedulerService::GetOrCreateAuthoringSession(
        std::unordered_map<int64_t, std::shared_ptr<AuthoringSessionEntry>>&
                                       cache,
        const TaskAuthoringCapability& capability,
        int64_t                        id,
        yyjson::value                  context,
        int64_t                        current_revision,
        bool                           enforce_fresh)
    {
        return ResolveCachedAuthoringSession(
            cache,
            mutex_,
            plugin_manager_,
            capability,
            id,
            std::move(context),
            current_revision,
            enforce_fresh);
    }

    int64_t SchedulerService::AppendComponentFacesContextKeys(
        yyjson::value&       context,
        const yyjson::value& properties,
        int64_t              host_id)
    {
        int64_t host_revision = -1;
        if (auto context_obj = context.as_object();
            context_obj && context_obj->contains(std::string_view("revision")))
        {
            if (auto revision =
                    (*context_obj)[std::string_view("revision")].as_sint())
            {
                host_revision = *revision;
            }
        }

        // 非图文档（无 nodes 数组）：零 faces 键，context 与现状逐字节一致。
        auto props_obj = properties.as_object();
        if (!PropertiesHoldGraphNodes(properties))
        {
            return host_revision;
        }
        const auto& nodes_ref = (*props_obj)[std::string_view("nodes")];
        auto        nodes_arr = nodes_ref.as_array();
        if (!nodes_arr)
        {
            return host_revision;
        }

        // 宿主会话缓存的有效 revision 戳（见头文件注释）：宿主 revision 与
        // 全部绑条目 revision 的混合，任一变化即触发宿主会话重建。
        uint64_t stamp = static_cast<uint64_t>(host_revision);

        const auto repository_ref_fields = CollectRepositoryRefFields(context);

        auto& settings = plugin_manager_.GetSettingsManager();

        yyjson::value               node_faces(Das::Utils::MakeYyjsonArray());
        yyjson::value               entry_faces(Das::Utils::MakeYyjsonArray());
        std::unordered_set<int64_t> seen_entry_ids;
        std::unordered_set<std::string> seen_node_ids;

        for (const auto& node_value : *nodes_arr)
        {
            auto node_obj = node_value.as_object();
            if (!node_obj || !node_obj->contains(std::string_view("nodeId")))
            {
                continue;
            }
            auto node_id = (*node_obj)[std::string_view("nodeId")].as_string();
            if (!node_id || node_id->empty()
                || !seen_node_ids.insert(std::string(*node_id)).second)
            {
                continue;
            }

            const std::string component_guid = ComponentGuidOfNode(node_value);
            if (component_guid.empty())
            {
                continue;
            }

            // ── 绑条目分支：entryFaces——条目文档口面经既有缓存会话现算
            //    （缺条目/无 capability/无 ports section 一律跳过不产诊断）。
            const auto field_it = repository_ref_fields.find(component_guid);
            if (field_it != repository_ref_fields.end())
            {
                auto bound_entry =
                    BoundEntryIdOfNode(node_value, field_it->second);
                if (bound_entry.has_value())
                {
                    const int64_t entry_id = *bound_entry;
                    if (entry_id == host_id
                        || !seen_entry_ids.insert(entry_id).second)
                    {
                        // 自引用防护（宿主条目会话锁自死锁）+ wire 契约
                        // （entryFaces 每条目一条）。
                        continue;
                    }
                    auto entry_json =
                        settings.GetTaskRepositoryEntryJson("0", entry_id);
                    if (entry_json.is_null() || !entry_json.is_object())
                    {
                        continue;
                    }
                    Repository::Dto::RepositoryEntryDto entry;
                    try
                    {
                        entry =
                            yyjson::cast<Repository::Dto::RepositoryEntryDto>(
                                entry_json);
                    }
                    catch (const std::exception&)
                    {
                        continue;
                    }
                    // 条目 revision 并入宿主会话失效戳：该条目的持久层
                    // revision 推进（对条目自身的 apply）必须连带重建宿主
                    // 图会话，否则缓存会话带着旧 entryFaces 续命。
                    {
                        const uint64_t entry_revision = static_cast<uint64_t>(
                            GetAuthoringRevision(entry_json));
                        const uint64_t entry_key =
                            static_cast<uint64_t>(entry_id);
                        stamp ^= entry_key * 0x9E3779B97F4A7C15ULL;
                        stamp *= 0xBF58476D1CE4E5B9ULL;
                        stamp ^= entry_revision * 0x94D049BB133111EBULL;
                        stamp *= 0xBF58476D1CE4E5B9ULL;
                    }
                    DasGuid entry_task_guid{};
                    try
                    {
                        entry_task_guid =
                            Das::Core::ForeignInterfaceHost::MakeDasGuid(
                                entry.task_type_guid);
                    }
                    catch (const std::exception&)
                    {
                        continue;
                    }
                    TaskAuthoringCapability entry_capability;
                    {
                        const std::scoped_lock lock(mutex_);
                        if (ResolveAuthoringCapabilityLocked(
                                entry_task_guid,
                                entry_capability))
                        {
                            continue;
                        }
                    }
                    auto entry_session = GetOrCreateAuthoringSession(
                        repository_authoring_sessions_,
                        entry_capability,
                        entry_id,
                        MakeRepositoryAuthoringContextJson(
                            plugin_manager_,
                            entry_id,
                            entry.accepted_properties,
                            entry_json),
                        GetAuthoringRevision(entry_json),
                        /*enforce_fresh=*/true);
                    if (!entry_session || !entry_session->session)
                    {
                        continue;
                    }
                    AuthoringGetPipelineResult pipeline;
                    {
                        const std::shared_lock<std::shared_mutex> read_lock(
                            entry_session->lock);
                        pipeline = RunAuthoringGetPipeline(
                            entry_session,
                            MakeEmptyAuthoringGetRequest());
                    }
                    if (!pipeline.ok || !pipeline.parsed.has_document)
                    {
                        continue;
                    }
                    auto ports =
                        ExtractFacePortsValue(pipeline.parsed.document);
                    if (!ports.has_value())
                    {
                        continue;
                    }
                    yyjson::value face(Das::Utils::MakeYyjsonObject());
                    auto          face_obj = face.as_object();
                    (*face_obj)[std::string_view("entryId")] = entry_id;
                    (*face_obj)[std::string_view("ports")] = std::move(*ports);
                    if (auto types =
                            ExtractFaceTypesValue(pipeline.parsed.document);
                        types.has_value())
                    {
                        (*face_obj)[std::string_view("types")] =
                            std::move(*types);
                    }
                    entry_faces.as_array()->emplace_back(std::move(face));
                    continue;
                }
            }

            // ── 未绑 componentRef 分支：nodeFaces——经注册面反查命中既有
            //    QI 链（begin/end 无特判，与其它组件同一条链），无
            //    authoring capability（纯 manifest 组件）跳过。
            DasGuid component_guid_value{};
            try
            {
                component_guid_value =
                    Das::Core::ForeignInterfaceHost::MakeDasGuid(
                        component_guid);
            }
            catch (const std::exception&)
            {
                continue;
            }
            TaskAuthoringCapability node_capability;
            bool                    has_node_capability = false;
            {
                const std::scoped_lock lock(mutex_);
                const auto*            record =
                    capability_registry_.FindByExecutionComponentGuid(
                        component_guid_value);
                if (record != nullptr && record->authoring.has_value())
                {
                    node_capability = *record->authoring;
                    has_node_capability = true;
                }
            }
            if (!has_node_capability)
            {
                continue;
            }
            // 节点会话瞬态现建（不经缓存——零新缓存层），context=节点
            // settings + 宿主 revision + 组件语境标记。
            auto node_session = BuildUncachedAuthoringSession(
                plugin_manager_,
                node_capability,
                MakeNodeComponentContextJson(node_value, host_revision),
                host_revision);
            if (!node_session || !node_session->session)
            {
                continue;
            }
            AuthoringGetPipelineResult node_pipeline;
            {
                const std::shared_lock<std::shared_mutex> read_lock(
                    node_session->lock);
                node_pipeline = RunAuthoringGetPipeline(
                    node_session,
                    MakeEmptyAuthoringGetRequest());
            }
            if (!node_pipeline.ok || !node_pipeline.parsed.has_document)
            {
                continue;
            }
            auto node_ports =
                ExtractFacePortsValue(node_pipeline.parsed.document);
            if (!node_ports.has_value())
            {
                continue;
            }
            yyjson::value face(Das::Utils::MakeYyjsonObject());
            auto          face_obj = face.as_object();
            (*face_obj)[std::string_view("nodeId")] =
                std::make_pair(std::string_view(*node_id), yyjson::copy_string);
            (*face_obj)[std::string_view("ports")] = std::move(*node_ports);
            node_faces.as_array()->emplace_back(std::move(face));
        }

        auto context_obj = context.as_object();
        if (context_obj)
        {
            if (!node_faces.as_array()->empty())
            {
                (*context_obj)[std::string_view("nodeFaces")] =
                    std::move(node_faces);
            }
            if (!entry_faces.as_array()->empty())
            {
                (*context_obj)[std::string_view("entryFaces")] =
                    std::move(entry_faces);
            }
        }
        return static_cast<int64_t>(stamp);
    }

    std::shared_ptr<AuthoringSessionEntry>
    SchedulerService::FindCachedAuthoringSession(
        std::unordered_map<int64_t, std::shared_ptr<AuthoringSessionEntry>>&
                cache,
        int64_t id)
    {
        const std::scoped_lock lock(mutex_);
        auto                   it = cache.find(id);
        if (it == cache.end())
        {
            return nullptr;
        }
        return it->second;
    }

    void SchedulerService::EvictAuthoringSession(
        std::unordered_map<int64_t, std::shared_ptr<AuthoringSessionEntry>>&
                cache,
        int64_t id)
    {
        // mutex_ held by caller. Direct erase — no drain. The map's shared_ptr
        // releases its reference, but any in-flight caller that already took a
        // copy keeps the entry (and its session) alive until it finishes
        // (D-87-07 shared_ptr upgrade). That is what roots out the UAF a drain
        // cycle was designed to prevent, so the drain is unnecessary here.
        auto it = cache.find(id);
        if (it == cache.end())
        {
            return;
        }
        cache.erase(it);
    }

    void SchedulerService::InvalidateAuthoringSessions()
    {
        const std::scoped_lock lock(mutex_);
        InvalidateAuthoringSessionsLocked();
    }

    void SchedulerService::InvalidateAuthoringSessionsLocked()
    {
        // No per-entry drain. In-flight callers keep their entries alive via
        // shared_ptr copies (D-87-07), so clearing the maps cannot dangle a
        // session a caller is still using. The caller must hold mutex_.
        task_authoring_sessions_.clear();
        repository_authoring_sessions_.clear();
    }

    yyjson::value SchedulerService::GetRepositoryEntryAuthoringDocument(
        int64_t              entry_id,
        const yyjson::value& request)
    {
        TaskAuthoringCapability                    capability;
        Repository::Dto::RepositoryEntryDto        entry;
        Repository::Dto::RepositoryAvailabilityDto availability;
        {
            const std::scoped_lock lock(mutex_);
            auto                   idle_guard = CheckSchedulerIdleLocked();
            if (idle_guard)
            {
                return MakeRepositoryResponseError(
                    idle_guard->error_kind,
                    idle_guard->message);
            }
            if (!initialized_ || !task_repository_store_)
            {
                return MakeRepositoryResponseError(
                    "notInitialized",
                    "Scheduler profile has not been initialized");
            }
        }

        auto& settings = plugin_manager_.GetSettingsManager();
        auto  entry_json = settings.GetTaskRepositoryEntryJson("0", entry_id);
        if (entry_json.is_null() || !entry_json.is_object())
        {
            return MakeRepositoryResponseError(
                "notFound",
                "Repository entry was not found");
        }

        try
        {
            entry =
                yyjson::cast<Repository::Dto::RepositoryEntryDto>(entry_json);
        }
        catch (const std::exception&)
        {
            return MakeRepositoryResponseError(
                "invalidJson",
                "Repository entry could not be parsed");
        }

        DasGuid task_guid{};
        try
        {
            task_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                entry.task_type_guid);
        }
        catch (const std::exception&)
        {
            return MakeRepositoryResponseError(
                "invalidGuid",
                "Repository entry contains an invalid task GUID");
        }

        {
            const std::scoped_lock lock(mutex_);
            availability = DeriveRepositoryAvailabilityLocked(entry);
            if (availability.state != "available")
            {
                return MakeRepositoryResponseError(
                    availability.reason.empty() ? "unavailable"
                                                : availability.reason,
                    availability.message.empty()
                        ? "Repository entry is unavailable"
                        : availability.message);
            }

            auto capability_guard =
                ResolveAuthoringCapabilityLocked(task_guid, capability);
            if (capability_guard)
            {
                // Repository-side capability message preserved byte-level
                // (Task path uses the helper's generic message verbatim).
                return MakeRepositoryResponseError(
                    capability_guard->error_kind,
                    "Repository entry task type does not declare authoring "
                    "capability");
            }
        }

        // Envelope guard: the contract requires the server to validate the
        // {kind, payload} envelope before invoking the Provider. The shared
        // Get pipeline owns ⑤ session -> ⑥⑦(GetDocument + parse); this caller
        // owns ①② locks/IO, and ⑩ entryId/taskTypeGuid/revision response
        // assembly.
        // 预备 pass 在锁外（上方 scoped_lock 段已闭合）现算组件文档口面，
        // 随 context 注入（actionCatalog 旁）。返回的 revision 戳（宿主+
        // 绑条目混合）作会话缓存失效键——faces 只在会话创建时播种。
        auto context = MakeRepositoryAuthoringContextJson(
            plugin_manager_,
            entry_id,
            entry.accepted_properties,
            entry_json);
        const int64_t faces_revision = AppendComponentFacesContextKeys(
            context,
            entry.accepted_properties,
            entry_id);
        auto session_entry = GetOrCreateAuthoringSession(
            repository_authoring_sessions_,
            capability,
            entry_id,
            std::move(context),
            faces_revision,
            /*enforce_fresh=*/true);
        if (!session_entry)
        {
            return MakeRepositoryResponseError(
                "sessionCreateFailed",
                "Failed to create repository authoring session");
        }
        AuthoringGetPipelineResult pipeline;
        {
            const std::shared_lock<std::shared_mutex> read_lock(
                session_entry->lock);
            pipeline = RunAuthoringGetPipeline(session_entry, request);
        }
        if (!pipeline.ok)
        {
            std::string message = std::move(pipeline.error_message);
            if (pipeline.error_kind == "sessionCreateFailed")
            {
                message = "Failed to create repository authoring session";
            }
            else if (pipeline.error_kind == "providerFailed")
            {
                message =
                    "Repository authoring provider failed to create document";
            }
            return MakeRepositoryResponseError(pipeline.error_kind, message);
        }

        yyjson::value result(Das::Utils::MakeYyjsonObject());
        auto          obj = result.as_object();
        (*obj)[std::string_view("entryId")] = entry_id;
        (*obj)[std::string_view("taskTypeGuid")] =
            std::string(entry.task_type_guid);
        (*obj)[std::string_view("revision")] = entry.authoring.revision;
        if (pipeline.parsed.has_document)
        {
            (*obj)[std::string_view("document")] =
                std::move(pipeline.parsed.document);
        }
        (*obj)[std::string_view("diagnostics")] =
            std::move(pipeline.parsed.diagnostics);
        // 持久化源文档随行：Provider 会话侧的调用节点边界端口推导需要从
        // 条目的 GraphDocumentDto 原文做悬空分析（投影 document 不携带
        // 完整拓扑原文，推导不可靠）。
        (*obj)[std::string_view("acceptedProperties")] =
            CloneJsonValue(entry.accepted_properties);
        return result;
    }

    yyjson::value SchedulerService::ApplyRepositoryEntryAuthoringChange(
        int64_t              entry_id,
        const yyjson::value& change)
    {
        TaskAuthoringCapability              capability;
        Repository::Dto::RepositoryEntryDto  entry;
        std::shared_ptr<TaskRepositoryStore> store;
        {
            const std::scoped_lock lock(mutex_);
            auto                   idle_guard = CheckSchedulerIdleLocked();
            if (idle_guard)
            {
                return MakeRepositoryResponseError(
                    idle_guard->error_kind,
                    idle_guard->message);
            }
            if (!initialized_ || !task_repository_store_)
            {
                return MakeRepositoryResponseError(
                    "notInitialized",
                    "Scheduler profile has not been initialized");
            }
            store = task_repository_store_;
        }

        auto& settings = plugin_manager_.GetSettingsManager();
        auto  entry_json = settings.GetTaskRepositoryEntryJson("0", entry_id);
        if (entry_json.is_null() || !entry_json.is_object())
        {
            return MakeRepositoryResponseError(
                "notFound",
                "Repository entry was not found");
        }

        try
        {
            entry =
                yyjson::cast<Repository::Dto::RepositoryEntryDto>(entry_json);
        }
        catch (const std::exception&)
        {
            return MakeRepositoryResponseError(
                "invalidJson",
                "Repository entry could not be parsed");
        }

        DasGuid task_guid{};
        try
        {
            task_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                entry.task_type_guid);
        }
        catch (const std::exception&)
        {
            return MakeRepositoryResponseError(
                "invalidGuid",
                "Repository entry contains an invalid task GUID");
        }

        {
            const std::scoped_lock lock(mutex_);
            auto availability = DeriveRepositoryAvailabilityLocked(entry);
            if (availability.state != "available")
            {
                return MakeRepositoryResponseError(
                    availability.reason.empty() ? "unavailable"
                                                : availability.reason,
                    availability.message.empty()
                        ? "Repository entry is unavailable"
                        : availability.message);
            }

            auto capability_guard =
                ResolveAuthoringCapabilityLocked(task_guid, capability);
            if (capability_guard)
            {
                // Repository-side capability message preserved byte-level
                // (Task path uses the helper's generic message verbatim).
                return MakeRepositoryResponseError(
                    capability_guard->error_kind,
                    "Repository entry task type does not declare authoring "
                    "capability");
            }
        }

        // Envelope guard (③): validate the {kind, payload} envelope before any
        // session resolve so an envelope failure does not create a session
        // (D-85-02c) and does not increment the plugin's
        // authoring_session_count.
        auto validation = ValidateAuthoringChange(change);
        if (!validation.valid)
        {
            return MakeRepositoryResponseError(
                validation.error_kind,
                validation.message);
        }

        // Peek the cache (no create). A hit runs the authoritative revision
        // check inside the per-session write lock below (concurrent Apply —
        // REQ-07a/b). A miss runs an early revision check first so a stale
        // baseRevision never creates a session (D-85-02c, byte-level for
        // SchedulerRepositoryAuthoringApplyStaleBaseRevisionReturnsConflict).
        auto cached_entry = FindCachedAuthoringSession(
            repository_authoring_sessions_,
            entry_id);
        if (!cached_entry)
        {
            const int64_t early_revision = GetAuthoringRevision(entry_json);
            const auto    base_revision = GetBaseRevision(change);
            if (!base_revision || *base_revision != early_revision)
            {
                yyjson::value err = MakeRepositoryResponseError(
                    "revisionConflict",
                    "Repository authoring change baseRevision is stale");
                (*err.as_object())[std::string_view("currentRevision")] =
                    early_revision;
                return err;
            }
        }

        // Resolve the session through the cache (hit: reuse; miss: create).
        // 预备 pass 在锁外（上方 scoped_lock 段已闭合）现算组件文档口面。
        // 返回的 revision 戳（宿主+绑条目混合）作会话缓存失效键。
        auto context = MakeRepositoryAuthoringContextJson(
            plugin_manager_,
            entry_id,
            entry.accepted_properties,
            entry_json);
        const int64_t faces_revision = AppendComponentFacesContextKeys(
            context,
            entry.accepted_properties,
            entry_id);
        auto session_entry = cached_entry ? cached_entry
                                          : GetOrCreateAuthoringSession(
                                                repository_authoring_sessions_,
                                                capability,
                                                entry_id,
                                                std::move(context),
                                                faces_revision,
                                                /*enforce_fresh=*/false);
        if (!session_entry || !session_entry->session)
        {
            return MakeRepositoryResponseError(
                "sessionCreateFailed",
                "Failed to create repository authoring session");
        }

        // Per-session serialization (D-87-07): unique_lock wraps the
        // authoritative revision check (fresh settings read), Provider
        // ApplyChange (M), and the store persist (W). The M-W runs outside
        // mutex_ and inside session_entry->lock. Lock order mutex_ ->
        // entry->lock is forward (mutex_ never held when entry->lock is taken).
        AuthoringApplyPipelineResult pipeline;
        bool                         persistence_failed = false;
        {
            const std::unique_lock<std::shared_mutex> write_lock(
                session_entry->lock);

            auto fresh_json =
                settings.GetTaskRepositoryEntryJson("0", entry_id);
            if (fresh_json.is_null() || !fresh_json.is_object())
            {
                return MakeRepositoryResponseError(
                    "notFound",
                    "Repository entry was not found");
            }
            pipeline.current_revision = GetAuthoringRevision(fresh_json);
            const auto base_revision = GetBaseRevision(change);
            if (!base_revision || *base_revision != pipeline.current_revision)
            {
                yyjson::value err = MakeRepositoryResponseError(
                    "revisionConflict",
                    "Repository authoring change baseRevision is stale");
                (*err.as_object())[std::string_view("currentRevision")] =
                    pipeline.current_revision;
                return err;
            }

            if (session_entry->invalidated_by_rollback)
            {
                // 本请求在批量失败逐出之前已解析到该 entry（写锁等待者）。
                // 其 session 内存文档携带未持久化的半批突变，不能继续使用
                // ——从持久层重建替代会话（不经缓存，原因见 helper 注释）。
                auto rebuilt = BuildUncachedAuthoringSession(
                    plugin_manager_,
                    capability,
                    MakeRepositoryAuthoringContextJson(
                        plugin_manager_,
                        entry_id,
                        entry.accepted_properties,
                        fresh_json),
                    pipeline.current_revision);
                if (!rebuilt || !rebuilt->session)
                {
                    return MakeRepositoryResponseError(
                        "sessionCreateFailed",
                        "Failed to create repository authoring session");
                }
                session_entry = std::move(rebuilt);
            }

            if (!RunAuthoringApplyPipelineLocked(
                    session_entry,
                    change,
                    fresh_json,
                    pipeline.current_revision,
                    pipeline))
            {
                if (pipeline.rollback)
                {
                    // 中途失败回滚：墓碑 + 逐出都在写锁内完成——若在锁外
                    // 逐出，并发请求可在窗口内重新解析到带半批突变的
                    // session 并成功提交（半批幽灵状态落盘）。逐出需要
                    // mutex_，形成 entry->lock → mutex_ 的受控逆向嵌套：
                    // 全库不存在持 mutex_ 再取 entry->lock 的路径（缓存
                    // resolve 只在 mutex_ 内做 find/emplace/erase），无
                    // AB-BA 环。
                    session_entry->invalidated_by_rollback = true;
                    const std::scoped_lock cache_lock(mutex_);
                    EvictAuthoringSession(
                        repository_authoring_sessions_,
                        entry_id);
                }
                std::string message = std::move(pipeline.error_message);
                if (pipeline.error_kind == "sessionCreateFailed")
                {
                    message = "Failed to create repository authoring session";
                }
                else if (pipeline.error_kind == "providerFailed")
                {
                    message = "Repository authoring provider rejected change";
                }
                // applyRejected: keep pipeline.error_message (the real
                // diagnostic) and surface the provider's diagnostics array.
                if (!pipeline.error_locator.empty())
                {
                    message = pipeline.error_locator + ": " + message;
                }
                const yyjson::value* diags =
                    (pipeline.error_kind == "applyRejected")
                        ? &pipeline.parsed.diagnostics
                        : nullptr;
                return MakeRepositoryResponseError(
                    pipeline.error_kind,
                    message,
                    diags);
            }

            yyjson::value updated_entry_json;
            auto          persist_result = store->UpdateAuthoring(
                entry_id,
                pipeline.parsed.accepted_properties,
                pipeline.authoring,
                updated_entry_json);
            if (DAS::IsFailed(persist_result))
            {
                persistence_failed = true;
            }
        }

        if (persistence_failed)
        {
            return MakeRepositoryResponseError(
                "persistenceFailed",
                "Failed to persist accepted repository authoring change");
        }

        yyjson::value result(Das::Utils::MakeYyjsonObject());
        auto          result_obj = result.as_object();
        (*result_obj)[std::string_view("ok")] = true;
        (*result_obj)[std::string_view("entryId")] = entry_id;
        (*result_obj)[std::string_view("revision")] = pipeline.next_revision;
        (*result_obj)[std::string_view("acceptedProperties")] =
            std::move(pipeline.parsed.accepted_properties);
        (*result_obj)[std::string_view("document")] =
            std::move(pipeline.parsed.document);
        (*result_obj)[std::string_view("diagnostics")] =
            std::move(pipeline.parsed.diagnostics);
        if (pipeline.parsed.has_command_result)
        {
            (*result_obj)[std::string_view("commandResult")] =
                std::move(pipeline.parsed.command_result);
        }
        return result;
    }

    yyjson::value SchedulerService::CompileRepositoryEntryAuthoring(
        int64_t              entry_id,
        const yyjson::value& request)
    {
        TaskAuthoringCapability             capability;
        Repository::Dto::RepositoryEntryDto entry;
        {
            const std::scoped_lock lock(mutex_);
            auto                   idle_guard = CheckSchedulerIdleLocked();
            if (idle_guard)
            {
                return MakeRepositoryResponseError(
                    idle_guard->error_kind,
                    idle_guard->message);
            }
            if (!initialized_ || !task_repository_store_)
            {
                return MakeRepositoryResponseError(
                    "notInitialized",
                    "Scheduler profile has not been initialized");
            }
        }

        auto& settings = plugin_manager_.GetSettingsManager();
        auto  entry_json = settings.GetTaskRepositoryEntryJson("0", entry_id);
        if (entry_json.is_null() || !entry_json.is_object())
        {
            return MakeRepositoryResponseError(
                "notFound",
                "Repository entry was not found");
        }

        try
        {
            entry =
                yyjson::cast<Repository::Dto::RepositoryEntryDto>(entry_json);
        }
        catch (const std::exception&)
        {
            return MakeRepositoryResponseError(
                "invalidJson",
                "Repository entry could not be parsed");
        }

        DasGuid task_guid{};
        try
        {
            task_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                entry.task_type_guid);
        }
        catch (const std::exception&)
        {
            return MakeRepositoryResponseError(
                "invalidGuid",
                "Repository entry contains an invalid task GUID");
        }

        {
            const std::scoped_lock lock(mutex_);
            auto availability = DeriveRepositoryAvailabilityLocked(entry);
            if (availability.state != "available")
            {
                return MakeRepositoryResponseError(
                    availability.reason.empty() ? "unavailable"
                                                : availability.reason,
                    availability.message.empty()
                        ? "Repository entry is unavailable"
                        : availability.message);
            }

            auto capability_guard =
                ResolveAuthoringCapabilityLocked(task_guid, capability);
            if (capability_guard)
            {
                // Repository-side capability message preserved byte-level
                // (Task path uses the helper's generic message verbatim).
                return MakeRepositoryResponseError(
                    capability_guard->error_kind,
                    "Repository entry task type does not declare authoring "
                    "capability");
            }
        }

        // D-84-08/09: Compile sends {} to Provider and returns {compile,
        // diagnostics}; Scheduler does not rewrap, project, or accept a client
        // document. D-84-09 forbids client properties/document in Compile.
        // The shared Compile pipeline owns ⑤ session -> ⑥⑦(Compile + parse);
        // this caller owns ①② locks/IO, and ⑩ entryId response assembly.
        // 预备 pass 在锁外（上方 scoped_lock 段已闭合）现算组件文档口面。
        // 返回的 revision 戳（宿主+绑条目混合）作会话缓存失效键。
        auto context = MakeRepositoryAuthoringContextJson(
            plugin_manager_,
            entry_id,
            entry.accepted_properties,
            entry_json);
        const int64_t faces_revision = AppendComponentFacesContextKeys(
            context,
            entry.accepted_properties,
            entry_id);
        auto compile_entry = GetOrCreateAuthoringSession(
            repository_authoring_sessions_,
            capability,
            entry_id,
            std::move(context),
            faces_revision);
        if (!compile_entry)
        {
            return MakeRepositoryResponseError(
                "sessionCreateFailed",
                "Failed to create repository authoring session");
        }
        AuthoringCompilePipelineResult pipeline;
        {
            const std::shared_lock<std::shared_mutex> read_lock(
                compile_entry->lock);
            pipeline = RunAuthoringCompilePipeline(compile_entry, request);
        }
        if (!pipeline.ok)
        {
            std::string message = std::move(pipeline.error_message);
            if (pipeline.error_kind == "sessionCreateFailed")
            {
                message = "Failed to create repository authoring session";
            }
            else if (pipeline.error_kind == "providerFailed")
            {
                message =
                    "Repository authoring provider failed to compile document";
            }
            return MakeRepositoryResponseError(pipeline.error_kind, message);
        }

        // D-84-08/09: Compile response preserves the Provider envelope
        // `{compile, diagnostics}`. canExecute/summary rewrapping was removed;
        // entryId is echoed purely as a routing ack, matching Get/Apply.
        //
        // 插件侧调用方无组件注册表访问：把条目任务类型解析出的执行组件
        // guid 附到 compile 段（engaged-only——解析不到缺省不发），调用方
        // 仅凭编译响应即可组装子执行快照。
        if (auto execution_component =
                BuildRepositoryInvokeCompileServices().find_execution_component(
                    entry);
            execution_component.has_value())
        {
            if (auto compile_object = pipeline.parsed.compile.as_object())
            {
                // 左值 std::string 直接赋值只存裸指针（源串析构即悬挂），
                // 必须经 copy_string 拷入 yyjson 文档池。
                (*compile_object)[std::string_view("executionComponentGuid")] =
                    std::make_pair(
                        std::string_view(*execution_component),
                        yyjson::copy_string);
            }
        }
        yyjson::value result(Das::Utils::MakeYyjsonObject());
        auto          obj = result.as_object();
        (*obj)[std::string_view("entryId")] = entry_id;
        (*obj)[std::string_view("compile")] =
            std::move(pipeline.parsed.compile);
        (*obj)[std::string_view("diagnostics")] =
            std::move(pipeline.parsed.diagnostics);
        return result;
    }

    RepositoryInvoke::RepositoryInvokeCompileResult
    SchedulerService::ResolveRepositoryInvokeSnapshot(
        const RepositoryInvoke::Dto::RepositoryTaskRefDto& repository_ref)
    {
        return ResolveRepositoryInvokeSnapshot(
            repository_ref,
            RepositoryInvoke::RepositoryInvokeSourceContext{});
    }

    RepositoryInvoke::RepositoryInvokeCompileResult
    SchedulerService::ResolveRepositoryInvokeSnapshot(
        const RepositoryInvoke::Dto::RepositoryTaskRefDto&     repository_ref,
        const RepositoryInvoke::RepositoryInvokeSourceContext& source_context)
    {
        auto fail = [](std::string code, std::string message)
            -> RepositoryInvoke::RepositoryInvokeCompileResult
        {
            RepositoryInvoke::RepositoryInvokeCompileDiagnostic diagnostic;
            diagnostic.code = std::move(code);
            diagnostic.message = std::move(message);

            RepositoryInvoke::RepositoryInvokeCompileResult result;
            result.ok = false;
            result.diagnostics.push_back(std::move(diagnostic));
            return result;
        };

        {
            const std::scoped_lock lock(mutex_);
            if (state_.load() == SchedulerState::Running
                || state_.load() == SchedulerState::Stopping)
            {
                return fail("task-working", "Scheduler is running or stopping");
            }
            if (!initialized_ || !task_repository_store_)
            {
                return fail(
                    "not-initialized",
                    "Scheduler profile has not been initialized");
            }
        }

        return RepositoryInvoke::ResolveRepositoryInvokeSnapshot(
            BuildRepositoryInvokeCompileServices(),
            repository_ref,
            source_context);
    }

    RepositoryInvoke::RepositoryInvokeCompileServices
    SchedulerService::BuildRepositoryInvokeCompileServices()
    {
        RepositoryInvoke::RepositoryInvokeCompileServices services;
        services.load_entry = [this](int64_t entry_id)
            -> std::optional<Repository::Dto::RepositoryEntryDto>
        {
            auto& settings = plugin_manager_.GetSettingsManager();
            auto  entry_json =
                settings.GetTaskRepositoryEntryJson("0", entry_id);
            if (entry_json.is_null() || !entry_json.is_object())
            {
                return std::nullopt;
            }

            Repository::Dto::RepositoryEntryDto entry;
            try
            {
                entry = yyjson::cast<Repository::Dto::RepositoryEntryDto>(
                    entry_json);
            }
            catch (const std::exception&)
            {
                return std::nullopt;
            }

            const std::scoped_lock lock(mutex_);
            entry.availability = DeriveRepositoryAvailabilityLocked(entry);
            return entry;
        };
        services.find_execution_component =
            [this](const Repository::Dto::RepositoryEntryDto& entry)
            -> std::optional<std::string>
        {
            DasGuid task_guid{};
            try
            {
                task_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                    entry.task_type_guid);
            }
            catch (const std::exception&)
            {
                return std::nullopt;
            }

            const std::scoped_lock lock(mutex_);
            auto*                  execution_component =
                capability_registry_.FindExecutionComponent(task_guid);
            if (execution_component == nullptr)
            {
                return std::nullopt;
            }
            return GuidToString(*execution_component);
        };
        services.compile_authoring =
            [this](
                const Repository::Dto::RepositoryEntryDto& entry,
                const yyjson::value&                       request)
            -> RepositoryInvoke::RepositoryInvokeProviderCompileResult
        {
            DasGuid task_guid{};
            try
            {
                task_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                    entry.task_type_guid);
            }
            catch (const std::exception&)
            {
                RepositoryInvoke::RepositoryInvokeProviderCompileResult result;
                result.ok = false;
                return result;
            }

            TaskAuthoringCapability capability;
            {
                const std::scoped_lock lock(mutex_);
                auto* authoring = capability_registry_.FindAuthoring(task_guid);
                if (authoring == nullptr)
                {
                    RepositoryInvoke::RepositoryInvokeProviderCompileResult
                        result;
                    result.ok = false;
                    return result;
                }
                capability = *authoring;
            }

            auto entry_json = CloneJsonValue(yyjson::object(entry));
            auto compile_entry = GetOrCreateAuthoringSession(
                repository_authoring_sessions_,
                capability,
                entry.entry_id,
                MakeRepositoryAuthoringContextJson(
                    plugin_manager_,
                    entry.entry_id,
                    entry.accepted_properties,
                    entry_json));
            if (!compile_entry)
            {
                RepositoryInvoke::RepositoryInvokeProviderCompileResult result;
                result.ok = false;
                RepositoryInvoke::RepositoryInvokeCompileDiagnostic diagnostic;
                diagnostic.code = "provider-compile-failed";
                diagnostic.message =
                    "Failed to create repository authoring session";
                result.diagnostics.push_back(std::move(diagnostic));
                return result;
            }
            RepositoryProviderCompileResult provider_compile;
            {
                const std::shared_lock<std::shared_mutex> read_lock(
                    compile_entry->lock);
                provider_compile =
                    CompileRepositoryEntryProvider(compile_entry, request);
            }

            RepositoryInvoke::RepositoryInvokeProviderCompileResult result;
            result.ok = provider_compile.Succeeded();
            if (!result.ok)
            {
                RepositoryInvoke::RepositoryInvokeCompileDiagnostic diagnostic;
                diagnostic.code = "provider-compile-failed";
                diagnostic.message = provider_compile.message;
                result.diagnostics.push_back(std::move(diagnostic));
                return result;
            }

            // Phase 84 strict Compile result envelope is `{compile,
            // diagnostics}`. RepositoryInvokeCompiler consumes the inner
            // compile payload, so unwrap the validated envelope here rather
            // than asking every downstream consumer to understand the new wire.
            auto parsed =
                ParseAuthoringProviderCompile(provider_compile.compile);
            if (!parsed.error_kind.empty())
            {
                result.ok = false;
                RepositoryInvoke::RepositoryInvokeCompileDiagnostic diagnostic;
                diagnostic.code = "provider-compile-failed";
                diagnostic.message = parsed.error_message;
                result.diagnostics.push_back(std::move(diagnostic));
                return result;
            }
            result.compile_result = std::move(parsed.compile);
            return result;
        };

        return services;
    }

    // ----------------------------------------------------------------
    // AddTask
    // ----------------------------------------------------------------

    DasResult SchedulerService::AddTask(
        const DasGuid& task_guid,
        int64_t*       out_task_id)
    {
        if (out_task_id == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }

        *out_task_id = 0;

        // Phase 1: Short lock for validation — copy type data to avoid
        // dangling pointer if Initialize/Disable clears task_types_.
        TaskTypeRecord type_snapshot;
        {
            const std::scoped_lock lock(mutex_);

            if (SchedulerBusyLocked())
            {
                return DAS_E_TASK_WORKING;
            }

            if (!initialized_)
            {
                return DAS_E_OBJECT_NOT_INIT;
            }

            auto* type_rec = FindTaskType(task_guid);
            if (!type_rec)
            {
                return DAS_E_NOT_FOUND;
            }

            type_snapshot = *type_rec;
        } // lock released

        // Phase 2: Lock-free persistence — all SettingsManager calls
        // and file I/O execute outside mutex_. Uses type_snapshot
        // (value copy) so no dangling pointer risk.
        auto&   settings = plugin_manager_.GetSettingsManager();
        auto    scheduler_index = settings.GetSchedulerIndexJson("0");
        int64_t next_id = 0;
        {
            auto si_obj = scheduler_index.as_object();
            if (si_obj)
            {
                auto nid_val =
                    si_obj->operator[](std::string_view("nextTaskId"));
                auto nid = nid_val.as_sint();
                if (nid)
                {
                    next_id = *nid;
                }
            }
        }

        // Build instance with descriptor defaults
        TaskInstanceRecord rec;
        rec.id = next_id;
        rec.task_guid = task_guid;
        rec.plugin_guid = type_snapshot.plugin_guid;
        rec.availability = TaskAvailability::Available;
        rec.properties = Das::Utils::MakeYyjsonObject();
        auto create_result =
            CreateTaskInstance(type_snapshot, rec.task_instance.Put());
        if (IsFailed(create_result) || !rec.task_instance)
        {
            return IsFailed(create_result) ? create_result : DAS_E_FAIL;
        }

        // Initialize properties from descriptor defaults
        {
            auto props_obj = rec.properties.as_object();
            for (const auto& desc : type_snapshot.descriptors)
            {
                if (!std::holds_alternative<std::monostate>(desc.default_value))
                {
                    std::visit(
                        [&props_obj, &desc](const auto& val)
                        {
                            if constexpr (!std::is_same_v<
                                              std::decay_t<decltype(val)>,
                                              std::monostate>)
                            {
                                auto property = props_obj->operator[](
                                    std::string_view(desc.name));
                                if constexpr (
                                    std::is_same_v<
                                        std::decay_t<decltype(val)>,
                                        std::string>)
                                {
                                    property = std::string(val);
                                }
                                else
                                {
                                    property = val;
                                }
                            }
                        },
                        desc.default_value);
                }
            }
        }

        // Persist task instance file
        auto instance_json = Das::Utils::MakeYyjsonObject();
        auto inst_obj = *instance_json.as_object();
        inst_obj[std::string_view("id")] = rec.id;
        inst_obj[std::string_view("taskGuid")] =
            yyjson::value(GuidToString(rec.task_guid));
        inst_obj[std::string_view("pluginGuid")] =
            yyjson::value(GuidToString(rec.plugin_guid));

        // 首跑语义 B：含 authoring + schedule 的 task 在 AddTask 时算首次
        // next_execution_time（scheduler 到点才首次跑）。off / 无 authoring
        // 的 task 保持 null（既有 descriptor-driven 行为不变）。仅在 scheduler
        // 运行态下计算——非运行态 AddTask 后用户调 Start 时 OnTick 会自决首跑
        // 时间（null → 立即跑一次后按 schedule 排），避免改写大量依赖"AddTask
        // 后 nextExecutionTime=null"的既有测试与并发 Apply 测试场景的 IPC 争
        // 用。
        std::optional<int64_t> initial_next_time;
        if (rec.task_instance && state_.load() == SchedulerState::Running)
        {
            initial_next_time = ComputeInitialNextExecutionTime(
                task_guid,
                rec.id,
                rec.properties,
                Das::Utils::MakeYyjsonObject(),
                rec.task_instance.Get());
        }
        if (initial_next_time)
        {
            inst_obj[std::string_view("nextExecutionTime")] =
                *initial_next_time;
            rec.next_execution_time = *initial_next_time;
        }
        else
        {
            inst_obj[std::string_view("nextExecutionTime")] = nullptr;
        }
        inst_obj[std::string_view("enabled")] = true;

        if (!rec.properties.is_null())
        {
            inst_obj[std::string_view("properties")] =
                CloneJsonValue(rec.properties);
        }
        else
        {
            inst_obj[std::string_view("properties")] =
                Das::Utils::MakeYyjsonObject();
        }

        auto persist_result =
            settings.UpdateTaskInstanceJson("0", rec.id, instance_json);
        if (IsFailed(persist_result))
        {
            return persist_result;
        }

        // Update scheduler index
        {
            auto si_obj = scheduler_index.as_object();
            if (si_obj)
            {
                si_obj->operator[](std::string_view("nextTaskId")) =
                    next_id + 1;
                auto order_arr =
                    si_obj->operator[](std::string_view("taskOrder"))
                        .as_array();
                if (order_arr)
                {
                    order_arr->emplace_back(next_id);
                }
            }
        }
        auto index_result =
            settings.UpdateSchedulerIndexJson("0", scheduler_index);
        if (IsFailed(index_result))
        {
            // Rollback task instance file (best-effort)
            settings.DeleteTaskInstanceJson("0", rec.id);
            return index_result;
        }

        // Phase 3: Short lock to commit to runtime state
        {
            const std::scoped_lock lock(mutex_);
            auto*                  type_rec = FindTaskType(task_guid);
            rec.task_type = type_rec;
            task_instances_.push_back(std::move(rec));
        }

        *out_task_id = next_id;
        return DAS_S_OK;
    }

    // ----------------------------------------------------------------
    // DeleteTask
    // ----------------------------------------------------------------

    DasResult SchedulerService::DeleteTask(int64_t task_id)
    {
        // Phase 1: Short lock to validate state and check existence
        {
            const std::scoped_lock lock(mutex_);

            if (SchedulerBusyLocked())
            {
                return DAS_E_TASK_WORKING;
            }

            auto it = std::find_if(
                task_instances_.begin(),
                task_instances_.end(),
                [task_id](const TaskInstanceRecord& r)
                { return r.id == task_id; });

            if (it == task_instances_.end())
            {
                return DAS_E_NOT_FOUND;
            }
        } // lock released — Config side validation done

        // Phase 2: Lock-free SettingsManager persistence with retry.
        // All file I/O and sleep_for execute outside mutex_.
        auto& settings = plugin_manager_.GetSettingsManager();

        auto scheduler_index = settings.GetSchedulerIndexJson("0");
        {
            auto si_obj = scheduler_index.as_object();
            if (si_obj && si_obj->contains(std::string_view("taskOrder"))
                && si_obj->operator[](std::string_view("taskOrder")).is_array())
            {
                auto order_arr =
                    *si_obj->operator[](std::string_view("taskOrder"))
                         .as_array();
                // Build new array without the deleted task
                auto new_arr = Das::Utils::MakeYyjsonArray();
                auto new_arr_ref = *new_arr.as_array();
                for (auto&& oit : order_arr)
                {
                    if (!oit.is_int() || static_cast<int64_t>(oit) != task_id)
                    {
                        new_arr_ref.emplace_back(CloneJsonValue(oit));
                    }
                }
                si_obj->operator[](std::string_view("taskOrder")) =
                    std::move(new_arr);
            }
        }

        constexpr int MAX_RETRIES = 3;
        constexpr int BASE_DELAY_MS = 100;
        DasResult     last_error = DAS_S_OK;

        for (int attempt = 0; attempt < MAX_RETRIES; ++attempt)
        {
            if (attempt > 0)
            {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(BASE_DELAY_MS << (attempt - 1)));
            }

            auto delete_result = settings.DeleteTaskInstanceJson("0", task_id);
            auto index_result =
                settings.UpdateSchedulerIndexJson("0", scheduler_index);

            if (!DAS::IsFailed(delete_result) && !DAS::IsFailed(index_result))
            {
                // Persistence succeeded — remove from runtime state
                const std::scoped_lock commit_lock(mutex_);
                auto                   it = std::find_if(
                    task_instances_.begin(),
                    task_instances_.end(),
                    [task_id](const TaskInstanceRecord& r)
                    { return r.id == task_id; });
                if (it != task_instances_.end())
                {
                    task_instances_.erase(it);
                }
                // Drop the cached authoring session so a later re-created task
                // id rebuilds against fresh state. commit_lock holds mutex_;
                // EvictAuthoringSession assumes the caller holds it.
                EvictAuthoringSession(task_authoring_sessions_, task_id);
                return DAS_S_OK;
            }

            last_error =
                DAS::IsFailed(delete_result) ? delete_result : index_result;
        }

        // All retries failed: runtime state unchanged (task remains in
        // task_instances_), no rollback needed.
        DAS_CORE_LOG_ERROR(
            "SchedulerService::DeleteTask: persistence failed for task {} "
            "after {} retries, result={}",
            task_id,
            MAX_RETRIES,
            last_error);

        return last_error;
    }

    // ----------------------------------------------------------------
    // UpdateTaskProperties
    // ----------------------------------------------------------------

    DasResult SchedulerService::UpdateTaskProperties(
        int64_t              task_id,
        const yyjson::value& properties)
    {
        // Phase 1: Short lock for validation and snapshot — copy
        // descriptors to avoid dangling pointer across lock-free phases.
        std::vector<Das::Core::ForeignInterfaceHost::PluginSettingDesc>
                      descriptors;
        yyjson::value current_properties;
        {
            const std::scoped_lock lock(mutex_);

            if (SchedulerBusyLocked())
            {
                return DAS_E_TASK_WORKING;
            }

            auto* inst = FindTaskInstance(task_id);
            if (!inst)
            {
                return DAS_E_NOT_FOUND;
            }

            // Invalid/unavailable instances cannot have properties updated
            if (inst->availability != TaskAvailability::Available)
            {
                return DAS_E_FAIL;
            }

            if (!inst->task_type)
            {
                return DAS_E_FAIL;
            }

            if (capability_registry_.FindAuthoring(inst->task_guid) != nullptr)
            {
                return DAS_E_ACCESS_DENIED;
            }

            descriptors = inst->task_type->descriptors;
            if (!inst->properties.is_null())
            {
                current_properties = CloneJsonValue(inst->properties);
            }
            else
            {
                current_properties = Das::Utils::MakeYyjsonObject();
            }
        } // lock released

        if (!properties.is_object())
        {
            return DAS_E_INVALID_JSON;
        }

        // Validate property names and types against descriptors
        auto props_obj = properties.as_object();
        for (auto it = props_obj->begin(); it != props_obj->end(); ++it)
        {
            const auto& prop_name = it->first;
            const auto& prop_val = it->second;
            bool        found = false;
            for (const auto& desc : descriptors)
            {
                if (desc.name == std::string_view(prop_name))
                {
                    // Type validation
                    switch (desc.type)
                    {
                    case Das::ExportInterface::DAS_TYPE_BOOL:
                        if (!prop_val.is_bool())
                        {
                            return DAS_E_TYPE_ERROR;
                        }
                        break;
                    case Das::ExportInterface::DAS_TYPE_INT:
                        if (!prop_val.is_int())
                        {
                            return DAS_E_TYPE_ERROR;
                        }
                        break;
                    case Das::ExportInterface::DAS_TYPE_FLOAT:
                        if (!prop_val.is_num())
                        {
                            return DAS_E_TYPE_ERROR;
                        }
                        break;
                    case Das::ExportInterface::DAS_TYPE_STRING:
                        if (!prop_val.is_string())
                        {
                            return DAS_E_TYPE_ERROR;
                        }
                        break;
                    default:
                        break;
                    }
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                return DAS_E_NOT_FOUND;
            }
        }

        // Apply validated properties to snapshot (mutable copy)
        {
            auto cur_obj = current_properties.as_object();
            for (auto it = props_obj->begin(); it != props_obj->end(); ++it)
            {
                const auto& prop_name = it->first;
                const auto& prop_val = it->second;
                cur_obj->operator[](std::string_view(prop_name)) =
                    CloneJsonValue(prop_val);
            }
        }

        // Phase 2: Lock-free SettingsManager persistence
        auto& settings = plugin_manager_.GetSettingsManager();
        auto  instance_json = settings.GetTaskInstanceJson("0", task_id);
        if (instance_json.is_null() || !instance_json.is_object())
        {
            instance_json = Das::Utils::MakeYyjsonObject();
        }
        {
            auto inst_obj = instance_json.as_object();
            inst_obj->operator[](std::string_view("properties")) =
                CloneJsonValue(current_properties);
        }
        auto persist_result =
            settings.UpdateTaskInstanceJson("0", task_id, instance_json);
        if (IsFailed(persist_result))
        {
            return persist_result;
        }

        // Phase 3: Short lock to commit properties to runtime state
        {
            const std::scoped_lock lock(mutex_);
            auto*                  inst = FindTaskInstance(task_id);
            if (inst)
            {
                inst->properties = std::move(current_properties);
            }
        }

        return DAS_S_OK;
    }

    // ----------------------------------------------------------------
    // UpdateTaskInternalProperties
    // ----------------------------------------------------------------

    DasResult SchedulerService::UpdateTaskInternalProperties(
        int64_t              task_id,
        const yyjson::value& internal_props)
    {
        // Phase 1: Short lock for validation and snapshot
        std::optional<int64_t> new_next_time;
        bool                   has_update = false;
        {
            const std::scoped_lock lock(mutex_);

            if (SchedulerBusyLocked())
            {
                return DAS_E_TASK_WORKING;
            }

            auto* inst = FindTaskInstance(task_id);
            if (!inst)
            {
                return DAS_E_NOT_FOUND;
            }

            // Parse nextExecutionTime if provided
            auto ip_obj = internal_props.as_object();
            auto net_key = std::string_view("nextExecutionTime");
            if (ip_obj && ip_obj->contains(net_key))
            {
                const auto& val = ip_obj->operator[](net_key);
                if (val.is_null())
                {
                    new_next_time = std::nullopt;
                    has_update = true;
                }
                else if (val.is_int())
                {
                    auto net_int = val.as_sint();
                    if (net_int)
                    {
                        new_next_time = *net_int;
                        has_update = true;
                    }
                    else
                    {
                        return DAS_E_TYPE_ERROR;
                    }
                }
                else
                {
                    return DAS_E_TYPE_ERROR;
                }
            }
        } // lock released

        if (!has_update)
        {
            return DAS_S_OK;
        }

        // Phase 2: Lock-free SettingsManager persistence
        auto& settings = plugin_manager_.GetSettingsManager();
        auto  instance_json = settings.GetTaskInstanceJson("0", task_id);
        if (instance_json.is_null() || !instance_json.is_object())
        {
            instance_json = Das::Utils::MakeYyjsonObject();
        }
        {
            auto inst_obj = instance_json.as_object();
            if (new_next_time.has_value())
            {
                inst_obj->operator[](std::string_view("nextExecutionTime")) =
                    *new_next_time;
            }
            else
            {
                inst_obj->operator[](std::string_view("nextExecutionTime")) =
                    nullptr;
            }
        }
        auto persist_result =
            settings.UpdateTaskInstanceJson("0", task_id, instance_json);
        if (IsFailed(persist_result))
        {
            return persist_result;
        }

        // Phase 3: Short lock to commit to runtime state
        {
            const std::scoped_lock lock(mutex_);
            auto*                  inst = FindTaskInstance(task_id);
            if (inst)
            {
                inst->next_execution_time = new_next_time;
            }
        }

        return DAS_S_OK;
    }

    // ----------------------------------------------------------------
    // SetTaskEnabled
    // ----------------------------------------------------------------

    DasResult SchedulerService::SetTaskEnabled(int64_t task_id, bool enabled)
    {
        // Phase 1: Short lock to validate existence and capture current value.
        // No Running/Stopping guard: toggling enabled is a runtime-safe intent
        // switch — the dispatch loop reads the flag on the next tick, so it is
        // safe to flip while the scheduler is running (that is the feature's
        // core value: pause/resume a single task without stopping the world).
        {
            const std::scoped_lock lock(mutex_);

            if (!initialized_)
            {
                return DAS_E_OBJECT_NOT_INIT;
            }

            auto* inst = FindTaskInstance(task_id);
            if (!inst)
            {
                return DAS_E_NOT_FOUND;
            }

            if (inst->enabled == enabled)
            {
                // No-op: avoid a redundant persistence write.
                return DAS_S_OK;
            }
        }

        // Phase 2: Lock-free SettingsManager persistence. Read-modify-write
        // the persisted instance so the toggle survives restart.
        auto& settings = plugin_manager_.GetSettingsManager();
        auto  instance_json = settings.GetTaskInstanceJson("0", task_id);
        if (instance_json.is_null() || !instance_json.is_object())
        {
            instance_json = Das::Utils::MakeYyjsonObject();
        }
        {
            auto inst_obj = instance_json.as_object();
            inst_obj->operator[](std::string_view("enabled")) = enabled;
        }
        auto persist_result =
            settings.UpdateTaskInstanceJson("0", task_id, instance_json);
        if (IsFailed(persist_result))
        {
            return persist_result;
        }

        // Phase 3: Short lock to commit to runtime state.
        {
            const std::scoped_lock lock(mutex_);
            auto*                  inst = FindTaskInstance(task_id);
            if (inst)
            {
                inst->enabled = enabled;
            }
        }

        return DAS_S_OK;
    }

    yyjson::value SchedulerService::GetTaskAuthoringDocument(
        int64_t              task_id,
        const yyjson::value& request)
    {
        TaskAuthoringCapability capability;
        DasGuid                 task_guid{};
        yyjson::value           properties;
        {
            const std::scoped_lock lock(mutex_);
            auto*                  inst = FindTaskInstance(task_id);
            if (!inst)
            {
                return MakeAuthoringResponseError(
                    "notFound",
                    "Task instance was not found");
            }
            if (inst->availability != TaskAvailability::Available)
            {
                return MakeAuthoringResponseError(
                    "unavailable",
                    "Task instance is unavailable");
            }
            auto capability_guard =
                ResolveAuthoringCapabilityLocked(inst->task_guid, capability);
            if (capability_guard)
            {
                return MakeAuthoringResponseError(
                    capability_guard->error_kind,
                    capability_guard->message);
            }
            task_guid = inst->task_guid;
            properties = CloneJsonValue(inst->properties);
        }

        auto& settings = plugin_manager_.GetSettingsManager();
        auto  task_json = settings.GetTaskInstanceJson("0", task_id);

        // Envelope guard: the contract requires the server to validate the
        // {kind, payload} envelope before invoking the Provider. The shared
        // Get pipeline owns ⑤ session -> ⑥⑦(GetDocument + parse); this caller
        // owns ①② lock/IO, and ⑩ taskId response assembly.
        // 预备 pass 在锁外（上方 scoped_lock 段已闭合）现算组件文档口面，
        // 随 context 注入（actionCatalog 旁）。返回的 revision 戳（宿主+
        // 绑条目混合）作会话缓存失效键——faces 只在会话创建时播种。
        auto context = MakeAuthoringContextJson(
            plugin_manager_,
            task_id,
            properties,
            task_json);
        const int64_t faces_revision =
            AppendComponentFacesContextKeys(context, properties, task_id);
        auto entry = GetOrCreateAuthoringSession(
            task_authoring_sessions_,
            capability,
            task_id,
            std::move(context),
            faces_revision,
            /*enforce_fresh=*/true);
        if (!entry)
        {
            return MakeAuthoringResponseError(
                "sessionCreateFailed",
                "Failed to create task authoring session");
        }
        AuthoringGetPipelineResult pipeline;
        {
            const std::shared_lock<std::shared_mutex> read_lock(entry->lock);
            pipeline = RunAuthoringGetPipeline(entry, request);
        }
        if (!pipeline.ok)
        {
            std::string message = std::move(pipeline.error_message);
            if (pipeline.error_kind == "sessionCreateFailed")
            {
                message = "Failed to create task authoring session";
            }
            else if (pipeline.error_kind == "providerFailed")
            {
                message = "Task authoring provider failed to create document";
            }
            return MakeAuthoringResponseError(pipeline.error_kind, message);
        }

        yyjson::value result(Das::Utils::MakeYyjsonObject());
        auto          obj = result.as_object();
        (*obj)[std::string_view("taskId")] = task_id;
        (*obj)[std::string_view("taskGuid")] = GuidToString(task_guid);
        if (pipeline.parsed.has_document)
        {
            (*obj)[std::string_view("document")] =
                std::move(pipeline.parsed.document);
        }
        (*obj)[std::string_view("diagnostics")] =
            std::move(pipeline.parsed.diagnostics);
        return result;
    }

    std::optional<int64_t> SchedulerService::ComputeInitialNextExecutionTime(
        const DasGuid&                  task_guid,
        int64_t                         task_id,
        const yyjson::value&            properties,
        const yyjson::value&            task_json,
        Das::PluginInterface::IDasTask* task_ptr)
    {
        if (task_ptr == nullptr)
        {
            return std::nullopt;
        }

        // 无 authoring capability → descriptor-driven，保持既有
        // next_execution_time （Core OnTick 自决）。
        TaskAuthoringCapability capability;
        {
            const std::scoped_lock lock(mutex_);
            auto* authoring = capability_registry_.FindAuthoring(task_guid);
            if (authoring == nullptr)
            {
                return std::nullopt;
            }
            capability = *authoring;
        }

        // Compile 走 IPC 长耗时，必须在 mutex_ 外执行。
        auto entry = GetOrCreateAuthoringSession(
            task_authoring_sessions_,
            capability,
            task_id,
            MakeAuthoringContextJson(
                plugin_manager_,
                task_id,
                properties,
                task_json));
        if (!entry || !entry->session)
        {
            DAS_CORE_LOG_WARN(
                "SchedulerService::ComputeInitialNextExecutionTime: task {} "
                "authoring session create failed",
                task_id);
            return std::nullopt;
        }

        DasPtr<Das::ExportInterface::IDasJson> p_props_json;
        {
            const std::shared_lock<std::shared_mutex> read_lock(entry->lock);
            auto compiled_pipeline = RunAuthoringCompilePipeline(
                entry,
                MakeExecutionCompileRequest());
            if (!compiled_pipeline.ok)
            {
                DAS_CORE_LOG_WARN(
                    "SchedulerService::ComputeInitialNextExecutionTime: task {} "
                    "authoring compile failed: {}",
                    task_id,
                    compiled_pipeline.error_kind);
                return std::nullopt;
            }

            if (ProviderCompileExplicitFalse(compiled_pipeline.parsed.compile))
            {
                DAS_CORE_LOG_WARN(
                    "SchedulerService::ComputeInitialNextExecutionTime: task {} "
                    "authoring compile returned ok=false",
                    task_id);
                return std::nullopt;
            }

            try
            {
                p_props_json = Das::MakeDasPtr<Das::Core::Utils::IDasJsonImpl>(
                    ExtractProviderExecutionInput(
                        compiled_pipeline.parsed.compile));
            }
            catch (const std::bad_alloc&)
            {
                DAS_CORE_LOG_WARN(
                    "SchedulerService::ComputeInitialNextExecutionTime: task {} "
                    "failed to allocate IDasJson for compiled input",
                    task_id);
                return std::nullopt;
            }
        }

        Das::ExportInterface::DasDate next_date{};
        const auto                    net_result =
            task_ptr->GetNextExecutionTime(p_props_json.Get(), &next_date);
        if (IsFailed(net_result))
        {
            // schedule=off 或 GetNextExecutionTime 失败：保持既有
            // next_execution_time（caller 默认行为）。
            return std::nullopt;
        }
        return DasDateToUnix(next_date);
    }

    void SchedulerService::RefreshNextExecutionTimes()
    {
        // Enable 锁外段调用：用最新 schedule 重算所有 task 的
        // next_execution_time， 覆盖非 Running 态 apply 残留的 sentinel/旧
        // schedule 值（off→daily 变更 后 task 卡在 sentinel，Enable
        // 若不刷新直接拿 sentinel 算 delay 会把 tick
        // 定到远期，任务永不调度）。ComputeInitialNextExecutionTime 内部
        // Compile 走 IPC 长耗时且自管 mutex_，故本方法不持锁：先 snapshot
        // inputs，锁外算， 再回锁更新内存 + PostPersistEvent 持久化。
        struct RecalcInput
        {
            DasGuid       task_guid;
            int64_t       task_id;
            yyjson::value properties;
            // DasPtr 拷贝持有：锁外 Compile 窗口期间 task_instances_ 可能
            // 被并发变更（Disable 收尾后 DeleteTask 放行），引用计数保证
            // 任务对象活到 GetNextExecutionTime 返回。
            Das::DasPtr<Das::PluginInterface::IDasTask> task_holder;
        };
        std::vector<RecalcInput> inputs;
        {
            const std::scoped_lock lock(mutex_);
            for (const auto& inst : task_instances_)
            {
                if (!inst.task_instance)
                {
                    continue;
                }
                // 仅刷新 sentinel（GetNextExecutionTime 失败兜底）的
                // task：sentinel 表示上次算下次时间失败，schedule 变更（如
                // off→daily）后应重试。 null（未算过 / 立即执行语义）与正常 due
                // 时间不动——避免覆盖 mock plugin 的固定返回值或破坏
                // null→立即执行的契约。
                if (!inst.next_execution_time
                    || *inst.next_execution_time != SENTINEL_FUTURE_TIMESTAMP)
                {
                    continue;
                }
                inputs.push_back(
                    {inst.task_guid,
                     inst.id,
                     CloneJsonValue(inst.properties),
                     inst.task_instance});
            }
        }

        auto& settings = plugin_manager_.GetSettingsManager();
        std::vector<std::pair<int64_t, int64_t>> updates;
        for (const auto& in : inputs)
        {
            auto refreshed = ComputeInitialNextExecutionTime(
                in.task_guid,
                in.task_id,
                in.properties,
                settings.GetTaskInstanceJson("0", in.task_id),
                in.task_holder.Get());
            if (refreshed)
            {
                updates.emplace_back(in.task_id, *refreshed);
            }
        }

        {
            const std::scoped_lock lock(mutex_);
            for (const auto& [id, time] : updates)
            {
                auto* inst = FindTaskInstance(id);
                if (inst)
                {
                    inst->next_execution_time = time;
                }
            }
        }
        for (const auto& [id, time] : updates)
        {
            PostPersistEvent(id, time);
        }
    }

    yyjson::value SchedulerService::ApplyTaskAuthoringChange(
        int64_t              task_id,
        const yyjson::value& change)
    {
        TaskAuthoringCapability capability;
        yyjson::value           properties;
        {
            const std::scoped_lock lock(mutex_);
            auto                   idle_guard = CheckSchedulerIdleLocked();
            if (idle_guard)
            {
                return MakeAuthoringResponseError(
                    idle_guard->error_kind,
                    idle_guard->message);
            }

            auto* inst = FindTaskInstance(task_id);
            if (!inst)
            {
                return MakeAuthoringResponseError(
                    "notFound",
                    "Task instance was not found");
            }
            if (inst->availability != TaskAvailability::Available)
            {
                return MakeAuthoringResponseError(
                    "unavailable",
                    "Task instance is unavailable");
            }
            auto capability_guard =
                ResolveAuthoringCapabilityLocked(inst->task_guid, capability);
            if (capability_guard)
            {
                return MakeAuthoringResponseError(
                    capability_guard->error_kind,
                    capability_guard->message);
            }
            properties = CloneJsonValue(inst->properties);
        }

        auto& settings = plugin_manager_.GetSettingsManager();
        auto  task_json = settings.GetTaskInstanceJson("0", task_id);
        if (task_json.is_null() || !task_json.is_object())
        {
            return MakeAuthoringResponseError(
                "notFound",
                "Task instance file was not found");
        }

        // Envelope guard (③): validate the {kind, payload} envelope before any
        // session resolve so an envelope failure does not create a session
        // (D-85-02c) and does not increment the plugin's
        // authoring_session_count.
        auto validation = ValidateAuthoringChange(change);
        if (!validation.valid)
        {
            return MakeAuthoringResponseError(
                validation.error_kind,
                validation.message);
        }

        // Peek the cache (no create). A hit means a session already exists, so
        // the authoritative revision check runs inside the per-session write
        // lock below (handles concurrent Apply on the same task — REQ-07a/b). A
        // miss runs an early revision check first so a stale baseRevision never
        // creates a session (D-85-02c, preserved byte-level for the
        // revisionConflict regression cases).
        auto cached_entry =
            FindCachedAuthoringSession(task_authoring_sessions_, task_id);
        if (!cached_entry)
        {
            const int64_t early_revision = GetAuthoringRevision(task_json);
            const auto    base_revision = GetBaseRevision(change);
            if (!base_revision || *base_revision != early_revision)
            {
                yyjson::value err = MakeAuthoringResponseError(
                    "revisionConflict",
                    "Authoring change baseRevision is stale");
                (*err.as_object())[std::string_view("currentRevision")] =
                    early_revision;
                return err;
            }
        }

        // Resolve the session through the cache (hit: reuse; miss: create — the
        // early check above already passed). Apply passes enforce_fresh=false
        // so consecutive Applies share one session (D-87-06 cache reuse).
        // 预备 pass 在锁外（上方 scoped_lock 段已闭合）现算组件文档口面。
        // 返回的 revision 戳（宿主+绑条目混合）作会话缓存失效键。
        auto context = MakeAuthoringContextJson(
            plugin_manager_,
            task_id,
            properties,
            task_json);
        const int64_t faces_revision =
            AppendComponentFacesContextKeys(context, properties, task_id);
        auto entry = cached_entry ? cached_entry
                                  : GetOrCreateAuthoringSession(
                                        task_authoring_sessions_,
                                        capability,
                                        task_id,
                                        std::move(context),
                                        faces_revision,
                                        /*enforce_fresh=*/false);
        if (!entry || !entry->session)
        {
            return MakeAuthoringResponseError(
                "sessionCreateFailed",
                "Failed to create task authoring session");
        }

        // Per-session serialization (D-87-07): the unique_lock wraps the
        // authoritative revision check (on a fresh settings read), the Provider
        // ApplyChange (M), and the settings persist (W). The whole M-W runs
        // outside mutex_ and inside entry->lock; mutex_ is re-acquired only for
        // the short in-memory inst metadata update after the lock releases.
        // Lock order is mutex_ -> entry->lock (forward): mutex_ is never held
        // when entry->lock is acquired, so there is no AB-BA with the Get path
        // (which also never holds them simultaneously).
        AuthoringApplyPipelineResult pipeline;
        bool                         persistence_failed = false;
        {
            const std::unique_lock<std::shared_mutex> write_lock(entry->lock);

            auto fresh_json = settings.GetTaskInstanceJson("0", task_id);
            if (fresh_json.is_null() || !fresh_json.is_object())
            {
                return MakeAuthoringResponseError(
                    "notFound",
                    "Task instance file was not found");
            }
            pipeline.current_revision = GetAuthoringRevision(fresh_json);
            const auto base_revision = GetBaseRevision(change);
            if (!base_revision || *base_revision != pipeline.current_revision)
            {
                yyjson::value err = MakeAuthoringResponseError(
                    "revisionConflict",
                    "Authoring change baseRevision is stale");
                (*err.as_object())[std::string_view("currentRevision")] =
                    pipeline.current_revision;
                return err;
            }

            if (entry->invalidated_by_rollback)
            {
                // 本请求在批量失败逐出之前已解析到该 entry（写锁等待者）。
                // 其 session 内存文档携带未持久化的半批突变，不能继续使用
                // ——从持久层重建替代会话（不经缓存，原因见 helper 注释）。
                auto rebuilt = BuildUncachedAuthoringSession(
                    plugin_manager_,
                    capability,
                    MakeAuthoringContextJson(
                        plugin_manager_,
                        task_id,
                        properties,
                        fresh_json),
                    pipeline.current_revision);
                if (!rebuilt || !rebuilt->session)
                {
                    return MakeAuthoringResponseError(
                        "sessionCreateFailed",
                        "Failed to create task authoring session");
                }
                entry = std::move(rebuilt);
            }

            if (!RunAuthoringApplyPipelineLocked(
                    entry,
                    change,
                    fresh_json,
                    pipeline.current_revision,
                    pipeline))
            {
                if (pipeline.rollback)
                {
                    // 中途失败回滚：墓碑 + 逐出都在写锁内完成（Repository
                    // 路径同款注释：锁外逐出会让并发请求在窗口内重新解析
                    // 到带半批突变的 session；entry->lock → mutex_ 嵌套无
                    // AB-BA——全库不存在反向获取路径）。
                    entry->invalidated_by_rollback = true;
                    const std::scoped_lock cache_lock(mutex_);
                    EvictAuthoringSession(task_authoring_sessions_, task_id);
                }
                std::string message = std::move(pipeline.error_message);
                if (pipeline.error_kind == "providerFailed")
                {
                    message = "Task authoring provider rejected change";
                }
                else if (pipeline.error_kind == "sessionCreateFailed")
                {
                    message = "Failed to create task authoring session";
                }
                // applyRejected: keep pipeline.error_message (the real
                // diagnostic); also surface the provider's diagnostics array so
                // the UI shows the precise rejection reason.
                if (!pipeline.error_locator.empty())
                {
                    message = pipeline.error_locator + ": " + message;
                }
                const yyjson::value* diags =
                    (pipeline.error_kind == "applyRejected")
                        ? &pipeline.parsed.diagnostics
                        : nullptr;
                return MakeAuthoringResponseError(
                    pipeline.error_kind,
                    message,
                    diags);
            }

            auto fresh_obj = fresh_json.as_object();
            (*fresh_obj)[std::string_view("properties")] =
                CloneJsonValue(pipeline.parsed.accepted_properties);
            (*fresh_obj)[std::string_view("authoring")] =
                CloneJsonValue(pipeline.authoring);
            if (DAS::IsFailed(
                    settings.UpdateTaskInstanceJson("0", task_id, fresh_json)))
            {
                persistence_failed = true;
            }
        }

        if (persistence_failed)
        {
            return MakeAuthoringResponseError(
                "persistenceFailed",
                "Failed to persist accepted authoring change");
        }

        // Apply 成功后重算 next_execution_time（schedule 变更实时反映）。
        // 失败（Compile/GetNextExecutionTime 任一）→ 保留既有
        // next_execution_time 不变 + 日志警告（不硬失败 apply 响应，apply
        // 已成功）。 在 mutex_ 内拷贝所需上下文后释放锁，再走 Compile（IPC
        // 长耗时不能持 mutex_），最后短锁内写回 next_execution_time +
        // PostPersistEvent。
        DasGuid       task_guid_for_recalc{};
        yyjson::value properties_for_recalc;
        // DasPtr 拷贝持有：锁外 Compile 窗口期间对象保持存活（窗口内
        // Disable 可收尾为 Stopped，随后 DeleteTask 放行即可析构对象）。
        Das::DasPtr<PluginInterface::IDasTask> task_holder_for_recalc;
        bool                                   can_recalc = false;
        {
            const std::scoped_lock lock(mutex_);
            auto*                  inst = FindTaskInstance(task_id);
            if (inst)
            {
                inst->properties =
                    CloneJsonValue(pipeline.parsed.accepted_properties);
                inst->authoring = CloneJsonValue(pipeline.authoring);
                task_guid_for_recalc = inst->task_guid;
                properties_for_recalc = CloneJsonValue(inst->properties);
                task_holder_for_recalc = inst->task_instance;
                can_recalc = true;
            }
        }

        // 仅在 scheduler 运行态下重算 next_execution_time：生产场景 schedule
        // 变更需实时反映到下次 tick。非运行态（Initialize 后未 Start / 测试
        // 场景）OnTick 不会触发，重算无意义且可能与并发 Apply 路径争用 IPC
        // （SchedulerAuthoringConcurrentApplySerialized 场景）。
        if (can_recalc && state_.load() == SchedulerState::Running)
        {
            auto refreshed_next = ComputeInitialNextExecutionTime(
                task_guid_for_recalc,
                task_id,
                properties_for_recalc,
                settings.GetTaskInstanceJson("0", task_id),
                task_holder_for_recalc.Get());
            if (refreshed_next)
            {
                bool should_persist = false;
                {
                    const std::scoped_lock lock(mutex_);
                    auto*                  inst = FindTaskInstance(task_id);
                    if (inst)
                    {
                        inst->next_execution_time = *refreshed_next;
                        should_persist = true;
                    }
                }
                if (should_persist)
                {
                    PostPersistEvent(task_id, *refreshed_next);
                }
            }
            else
            {
                DAS_CORE_LOG_WARN(
                    "SchedulerService::ApplyTaskAuthoringChange: task {} "
                    "next_execution_time recompute skipped (no authoring "
                    "capability or schedule off), keeping existing value",
                    task_id);
            }
        }

        yyjson::value result(Das::Utils::MakeYyjsonObject());
        auto          result_obj = result.as_object();
        (*result_obj)[std::string_view("ok")] = true;
        (*result_obj)[std::string_view("taskId")] = task_id;
        (*result_obj)[std::string_view("revision")] = pipeline.next_revision;
        (*result_obj)[std::string_view("acceptedProperties")] =
            std::move(pipeline.parsed.accepted_properties);
        (*result_obj)[std::string_view("document")] =
            std::move(pipeline.parsed.document);
        (*result_obj)[std::string_view("diagnostics")] =
            std::move(pipeline.parsed.diagnostics);
        if (pipeline.parsed.has_command_result)
        {
            (*result_obj)[std::string_view("commandResult")] =
                std::move(pipeline.parsed.command_result);
        }
        return result;
    }

    yyjson::value SchedulerService::CompileTaskAuthoring(
        int64_t              task_id,
        const yyjson::value& request)
    {
        TaskAuthoringCapability capability;
        yyjson::value           properties;
        {
            const std::scoped_lock lock(mutex_);
            auto*                  inst = FindTaskInstance(task_id);
            if (!inst)
            {
                return MakeAuthoringResponseError(
                    "notFound",
                    "Task instance was not found");
            }
            auto capability_guard =
                ResolveAuthoringCapabilityLocked(inst->task_guid, capability);
            if (capability_guard)
            {
                return MakeAuthoringResponseError(
                    capability_guard->error_kind,
                    capability_guard->message);
            }
            properties = CloneJsonValue(inst->properties);
        }

        auto& settings = plugin_manager_.GetSettingsManager();
        auto  task_json = settings.GetTaskInstanceJson("0", task_id);

        // Envelope guard: the contract requires the server to validate the
        // {kind, payload} envelope before invoking the Provider. The shared
        // Compile pipeline owns ⑤ session -> ⑥⑦(Compile + parse); this caller
        // owns ①② lock/IO, and ⑩ taskId response assembly.
        // 预备 pass 在锁外（上方 scoped_lock 段已闭合）现算组件文档口面。
        // 返回的 revision 戳（宿主+绑条目混合）作会话缓存失效键。
        auto context = MakeAuthoringContextJson(
            plugin_manager_,
            task_id,
            properties,
            task_json);
        const int64_t faces_revision =
            AppendComponentFacesContextKeys(context, properties, task_id);
        auto compile_entry = GetOrCreateAuthoringSession(
            task_authoring_sessions_,
            capability,
            task_id,
            std::move(context),
            faces_revision);
        if (!compile_entry)
        {
            return MakeAuthoringResponseError(
                "sessionCreateFailed",
                "Failed to create task authoring session");
        }
        AuthoringCompilePipelineResult pipeline;
        {
            const std::shared_lock<std::shared_mutex> read_lock(
                compile_entry->lock);
            pipeline = RunAuthoringCompilePipeline(compile_entry, request);
        }
        if (!pipeline.ok)
        {
            std::string message = std::move(pipeline.error_message);
            if (pipeline.error_kind == "sessionCreateFailed")
            {
                message = "Failed to create task authoring session";
            }
            else if (pipeline.error_kind == "providerFailed")
            {
                message = "Task authoring provider failed to compile document";
            }
            return MakeAuthoringResponseError(pipeline.error_kind, message);
        }

        yyjson::value result(Das::Utils::MakeYyjsonObject());
        auto          obj = result.as_object();
        (*obj)[std::string_view("taskId")] = task_id;
        (*obj)[std::string_view("compile")] =
            std::move(pipeline.parsed.compile);
        (*obj)[std::string_view("diagnostics")] =
            std::move(pipeline.parsed.diagnostics);
        return result;
    }

    // ----------------------------------------------------------------
    // Timer and tick
    // ----------------------------------------------------------------

    // 任务执行失败的用户可见出口：经 focus 渠道推 toast（错误说明文本），
    // log 渠道随 recorder 落 user.jsonl。只挂 OnTick 的 Do 失败一处——
    // 发射语义即"任务失败了"，成功路径零条目（非每 tick）。任何一步失败
    // 都降级为 DAS_CORE_LOG_WARN，不阻塞调度。
    static void EmitTaskFailureFocusToast(int64_t task_id, DasResult do_result)
    {
        // 主进程服务解析必须走 C ABI（in-process 快路径；inline
        // thread_local 直读跨 DLL 分裂恒空）。
        DAS::DasPtr<IDasBase> raw_object;
        DasResult             hr = ::DasQueryMainProcessInterfaceByName(
            "das.focus-log-recorder",
            raw_object.Put());
        if (DAS::IsFailed(hr) || !raw_object)
        {
            DAS_CORE_LOG_WARN(
                "TaskFailureFocusToast: resolve das.focus-log-recorder "
                "failed, result={} (task {})",
                hr,
                task_id);
            return;
        }
        DAS::DasPtr<Das::ExportInterface::IDasProcessLogRecorder> recorder;
        hr = raw_object.As(recorder.Put());
        if (DAS::IsFailed(hr) || !recorder)
        {
            DAS_CORE_LOG_WARN(
                "TaskFailureFocusToast: recorder interface cast failed, "
                "result={} (task {})",
                hr,
                task_id);
            return;
        }

        // 错误说明文本三级回落：ErrorLens 扫描按码人话 → Core 预定义表
        // → 数值文本。locale 读进程级 GlobalLocale（与 catalog 构建同源）。
        std::string message;
        {
            DAS::DasPtr<IDasReadOnlyString> locale_string;
            if (DAS::IsFailed(::DasGetDefaultLocale(locale_string.Put())))
            {
                locale_string.Reset();
            }
            const auto* active_lens_manager =
                Das::Core::ForeignInterfaceHost::GetActiveErrorLensManager();
            if (active_lens_manager != nullptr)
            {
                auto lens_message = active_lens_manager->GetErrorMessageForCode(
                    locale_string.Get(),
                    do_result);
                if (lens_message.has_value() && lens_message.value())
                {
                    const char* message_utf8 = nullptr;
                    if (DAS::IsOk(lens_message.value()->GetUtf8(&message_utf8))
                        && message_utf8 != nullptr && *message_utf8 != '\0')
                    {
                        message = message_utf8;
                    }
                }
            }
        }
        if (message.empty())
        {
            DAS::DasPtr<IDasReadOnlyString> predefined_message;
            if (DAS::IsOk(
                    ::DasGetPredefinedErrorMessage(
                        do_result,
                        predefined_message.Put()))
                && predefined_message)
            {
                const char* message_utf8 = nullptr;
                if (DAS::IsOk(predefined_message->GetUtf8(&message_utf8))
                    && message_utf8 != nullptr)
                {
                    message = message_utf8;
                }
            }
        }
        if (message.empty())
        {
            message = DAS_FMT_NS::format("result={}", do_result);
        }
        const std::string entry_text =
            DAS_FMT_NS::format("{} (task {})", message, task_id);

        // 条目构造照 MaaPi focus display 形态：type=error + 文本 +
        // displayChannels ["toast","log"]（toast 推送 + log 渠道落盘）。
        DAS::DasPtr<Das::ExportInterface::IDasLogEntry> entry;
        hr = ::CreateIDasLogEntry(entry.Put());
        if (DAS::IsFailed(hr) || !entry)
        {
            DAS_CORE_LOG_WARN(
                "TaskFailureFocusToast: CreateIDasLogEntry failed, "
                "result={} (task {})",
                hr,
                task_id);
            return;
        }
        hr = entry->SetType(Das::ExportInterface::DAS_LOG_TYPE_ERROR);
        if (DAS::IsFailed(hr))
        {
            DAS_CORE_LOG_WARN(
                "TaskFailureFocusToast: SetType failed, result={} (task {})",
                hr,
                task_id);
            return;
        }
        DasU8StringOnStack text_value{entry_text.c_str()};
        hr = entry->AddText(&text_value);
        if (DAS::IsFailed(hr))
        {
            DAS_CORE_LOG_WARN(
                "TaskFailureFocusToast: AddText failed, result={} (task {})",
                hr,
                task_id);
            return;
        }
        // channels JSON 构造照 FormatDisplayChannelsJson 产出形态（yyjson
        // 数组 + copy_string，拷贝语义防悬挂）。
        std::string display_channels_json;
        {
            auto channels_array = Das::Utils::MakeYyjsonArray();
            auto array_ref = channels_array.as_array();
            if (array_ref)
            {
                array_ref->emplace_back(
                    std::string_view{"toast"},
                    yyjson::copy_string);
                array_ref->emplace_back(
                    std::string_view{"log"},
                    yyjson::copy_string);
            }
            auto serialized = Das::Utils::SerializeYyjsonValue(channels_array);
            display_channels_json = serialized ? *serialized : "[]";
        }
        DasU8StringOnStack channels_key{"displayChannels"};
        DasU8StringOnStack channels_value{display_channels_json.c_str()};
        hr = entry->SetField(&channels_key, &channels_value);
        if (DAS::IsFailed(hr))
        {
            DAS_CORE_LOG_WARN(
                "TaskFailureFocusToast: SetField failed, result={} (task {})",
                hr,
                task_id);
            return;
        }

        hr = recorder->Record(entry.Get());
        if (DAS::IsFailed(hr))
        {
            // 降级兜底：recorder 失败时文本不进日志黑洞，落调试日志。
            DAS_CORE_LOG_WARN(
                "TaskFailureFocusToast: Record failed, result={} (task {}): "
                "{}",
                hr,
                task_id,
                entry_text);
        }
    }

    void SchedulerService::SpawnTick()
    {
        // async_scope::spawn 的默认 receiver 遇 set_error 直接 rethrow
        // terminate 进程，OnTick 的逃逸异常必须就地吞掉记日志（下一 tick
        // 照常调度）。
        scope_.spawn(
            stdexec::schedule(bt_scheduler_)
            | stdexec::then(
                [this]
                {
                    try
                    {
                        OnTick();
                    }
                    catch (const std::exception& exception)
                    {
                        DAS_CORE_LOG_ERROR(
                            "SchedulerService::OnTick threw exception: {}",
                            exception.what());
                    }
                    catch (...)
                    {
                        DAS_CORE_LOG_ERROR(
                            "SchedulerService::OnTick threw unknown exception");
                    }
                }));
    }

    void SchedulerService::SpawnExecuteOnce(int64_t task_id)
    {
        // 与 SpawnTick 同型的异常兜底：执行段的插件边界已由窄 catch
        // 就地转错误码，这里的逃逸异常仅剩框架自身缺陷面，吞掉记日志
        // 防 spawn receiver rethrow terminate 进程。
        scope_.spawn(
            stdexec::schedule(bt_scheduler_)
            | stdexec::then(
                [this, task_id]
                {
                    try
                    {
                        ExecuteTaskOnce(task_id);
                    }
                    catch (const std::exception& exception)
                    {
                        DAS_CORE_LOG_ERROR(
                            "SchedulerService::ExecuteTaskOnce threw "
                            "exception: {}",
                            exception.what());
                    }
                    catch (...)
                    {
                        DAS_CORE_LOG_ERROR(
                            "SchedulerService::ExecuteTaskOnce threw unknown "
                            "exception");
                    }
                }));
    }

    void SchedulerService::StartTickTimer(
        std::chrono::steady_clock::duration delay)
    {
        if (!tick_timer_)
        {
            return;
        }

        tick_timer_->expires_after(delay);
        tick_timer_->async_wait(
            [this](const boost::system::error_code& ec)
            {
                if (!ec && state_.load() == SchedulerState::Running)
                {
                    SpawnTick();
                }
            });
    }

    void SchedulerService::OnTick()
    {
        // Running 专属的常规调度 tick（全局最早 due）。手动立即执行走
        // ExecuteTaskOnce 单任务通路，与本通路状态互斥：非 Running 落地
        // 一律放弃，杜绝调度与手动双通路并发执行。
        if (state_.load() != SchedulerState::Running)
        {
            return;
        }

        // Select a runnable task instance under lock, then release lock
        // before calling plugin Do. task_holder 持引用计数：锁外窗口内
        // 即使 task_instances_ 被变更（扩容/删除），任务对象也活到
        // GetNextExecutionTime 返回。
        Das::DasPtr<Das::PluginInterface::IDasTask> task_holder;
        yyjson::value                               properties_copy;
        std::optional<TaskAuthoringCapability>      authoring_capability;
        int64_t                                     selected_id = -1;

        int64_t now_unix = SystemClock::to_time_t(SystemClock::now());

        {
            const std::scoped_lock lock(mutex_);

            if (state_.load() != SchedulerState::Running)
            {
                return;
            }

            auto* selected_inst =
                FindNextRunnableTask(task_instances_, now_unix);

            if (!selected_inst)
            {
                DAS_CORE_LOG_DEBUG(
                    "SchedulerService::OnTick: no runnable task");
                StartTickTimer(ComputeNextDelay(task_instances_, now_unix));
                return;
            }

            task_holder = selected_inst->task_instance;
            properties_copy = selected_inst->properties;
            selected_id = selected_inst->id;
            if (auto* authoring = capability_registry_.FindAuthoring(
                    selected_inst->task_guid))
            {
                authoring_capability = *authoring;
            }

            current_task_id_ = selected_id;

            // Create or reuse stop token for cooperative cancellation
            if (!stop_token_)
            {
                stop_token_ = Das::Core::Utils::DasStopTokenImpl::Make();
            }
        }

        ExecuteSelectedTask(
            selected_id,
            std::move(task_holder),
            std::move(properties_copy),
            authoring_capability);

        DAS_CORE_LOG_DEBUG("SchedulerService::OnTick: tick complete");
    }

    void SchedulerService::ExecuteTaskOnce(int64_t task_id)
    {
        // RunTaskNow 投递的单任务执行落地体。与 OnTick 分工且状态互斥：
        // 本通路仅在 Stopped 执行（RunTaskNow 的接受条件），OnTick 仅在
        // Running 执行——锁内复检双向堵死「调度与手动并发执行」。
        Das::DasPtr<Das::PluginInterface::IDasTask> task_holder;
        yyjson::value                               properties_copy;
        std::optional<TaskAuthoringCapability>      authoring_capability;

        {
            const std::scoped_lock lock(mutex_);

            // 投递到落地之间被 Enable 抢先转 Running（或被 Disable /
            // 析构推进 Stopping）：手动执行作废，交调度器接管。
            if (state_.load() != SchedulerState::Stopped)
            {
                DAS_CORE_LOG_WARN(
                    "SchedulerService::ExecuteTaskOnce: scheduler left "
                    "stopped before task {} ran, manual run dropped",
                    task_id);
                return;
            }

            // 复检任务可用性：投递窗口内任务可能被删除或禁用。
            auto* inst = FindTaskInstance(task_id);
            if (!inst || !inst->enabled
                || inst->availability != TaskAvailability::Available
                || !inst->task_instance)
            {
                DAS_CORE_LOG_WARN(
                    "SchedulerService::ExecuteTaskOnce: task {} no longer "
                    "runnable, manual run dropped",
                    task_id);
                return;
            }

            task_holder = inst->task_instance;
            properties_copy = inst->properties;
            if (auto* authoring =
                    capability_registry_.FindAuthoring(inst->task_guid))
            {
                authoring_capability = *authoring;
            }

            current_task_id_ = task_id;

            // Create or reuse stop token for cooperative cancellation
            if (!stop_token_)
            {
                stop_token_ = Das::Core::Utils::DasStopTokenImpl::Make();
            }
        }

        ExecuteSelectedTask(
            task_id,
            std::move(task_holder),
            std::move(properties_copy),
            authoring_capability);
    }

    void SchedulerService::ExecuteSelectedTask(
        int64_t                                     selected_id,
        Das::DasPtr<Das::PluginInterface::IDasTask> task_holder,
        yyjson::value                               properties_copy,
        std::optional<TaskAuthoringCapability>      authoring_capability)
    {
        // 执行开始即推送（锁外）：Get() 携带 runningTaskId，UI 立即感知
        // 「任务执行中」并禁用相关操作。Do 可能执行分钟级，若只在收尾
        // 推送，整个执行期间 UI 完全看不到占用状态。结束推送由收尾的
        // NotifyStateChange 负责。
        NotifyStateChange();

        // Create IDasJson inputs outside the lock (no serialization round-trip)
        DasPtr<Das::ExportInterface::IDasJson> p_env_json;
        auto env_cr = CreateEmptyDasJson(p_env_json.Put());
        if (IsFailed(env_cr))
        {
            p_env_json.Reset();
        }

        DasPtr<Das::ExportInterface::IDasJson> p_props_json;
        bool                                   compile_failed = false;
        if (authoring_capability)
        {
            auto task_json =
                plugin_manager_.GetSettingsManager().GetTaskInstanceJson(
                    "0",
                    selected_id);

            auto ontick_entry = GetOrCreateAuthoringSession(
                task_authoring_sessions_,
                *authoring_capability,
                selected_id,
                MakeAuthoringContextJson(
                    plugin_manager_,
                    selected_id,
                    properties_copy,
                    task_json));
            if (!ontick_entry || !ontick_entry->session)
            {
                compile_failed = true;
                DAS_CORE_LOG_WARN(
                    "SchedulerService::ExecuteSelectedTask: task {} authoring session "
                    "create failed",
                    selected_id);
            }
            else
            {
                const std::shared_lock<std::shared_mutex> read_lock(
                    ontick_entry->lock);
                auto& session = ontick_entry->session;
                auto  request_json =
                    WrapJsonValue(MakeExecutionCompileRequest());
                DasPtr<Das::ExportInterface::IDasJson> compile_json;
                auto                                   compile_result =
                    session->Compile(request_json.Get(), compile_json.Put());
                if (DAS::IsFailed(compile_result) || !compile_json)
                {
                    compile_failed = true;
                    DAS_CORE_LOG_WARN(
                        "SchedulerService::ExecuteSelectedTask: task {} authoring compile "
                        "failed, result={}",
                        selected_id,
                        compile_result);
                }
                else
                {
                    auto compiled = ReadJsonInterface(compile_json.Get());
                    auto parsed_compile =
                        ParseAuthoringProviderCompile(compiled);
                    if (!parsed_compile.error_kind.empty())
                    {
                        compile_failed = true;
                        DAS_CORE_LOG_WARN(
                            "SchedulerService::ExecuteSelectedTask: task {} authoring "
                            "compile result rejected: {}",
                            selected_id,
                            parsed_compile.error_kind);
                    }
                    else if (
                        ProviderCompileExplicitFalse(parsed_compile.compile))
                    {
                        compile_failed = true;
                        DAS_CORE_LOG_WARN(
                            "SchedulerService::ExecuteSelectedTask: task {} authoring "
                            "compile returned ok=false",
                            selected_id);
                    }
                    else
                    {
                        // The provider compile's executionInput reaches the
                        // task verbatim. Graph plans keep their
                        // {"fingerprint","plan"} shell — the execution side
                        // unwraps and fingerprint-checks it before running,
                        // so the scheduler must not pre-cast or rewrite it.
                        auto execution_input = ExtractProviderExecutionInput(
                            parsed_compile.compile);

                        try
                        {
                            p_props_json =
                                Das::MakeDasPtr<Das::Core::Utils::IDasJsonImpl>(
                                    std::move(execution_input));
                        }
                        catch (const std::bad_alloc&)
                        {
                            compile_failed = true;
                            DAS_CORE_LOG_WARN(
                                "Failed to allocate IDasJson for compiled "
                                "task input");
                        }
                    }
                }
            }
        }
        else
        {
            try
            {
                p_props_json = Das::MakeDasPtr<Das::Core::Utils::IDasJsonImpl>(
                    std::move(properties_copy));
            }
            catch (const std::bad_alloc&)
            {
                DAS_CORE_LOG_WARN(
                    "Failed to allocate IDasJson for task properties");
            }
        }

        if (!compile_failed)
        {
            // Call IDasTask::Do WITHOUT holding the mutex.
            // 窄 catch 而非依赖调度循环外层兜底：兜底接住异常时 tick 的
            // 清理段（执行槽清零、收尾状态机）已被跳过，调度器会永久停留
            // 执行中——插件边界异常必须就地转错误码让 tick 走完。
            DasResult do_result = DAS_E_FAIL;
            try
            {
                do_result = task_holder.Get()->Do(
                    stop_token_.Get(),
                    p_env_json ? p_env_json.Get() : nullptr,
                    p_props_json ? p_props_json.Get() : nullptr);
            }
            catch (const std::exception& exception)
            {
                do_result = DAS_E_INTERNAL_FATAL_ERROR;
                DAS_CORE_LOG_ERROR(
                    "SchedulerService::ExecuteSelectedTask: task {} Do threw exception: {}",
                    selected_id,
                    exception.what());
            }
            catch (...)
            {
                do_result = DAS_E_INTERNAL_FATAL_ERROR;
                DAS_CORE_LOG_ERROR(
                    "SchedulerService::ExecuteSelectedTask: task {} Do threw unknown "
                    "exception",
                    selected_id);
            }

            if (IsFailed(do_result))
            {
                DAS_CORE_LOG_WARN(
                    "SchedulerService::ExecuteSelectedTask: task {} Do returned result={}",
                    selected_id,
                    do_result);
                // 失败的用户可见出口：focus toast + log 渠道（内部自带
                // 降级，失败不阻塞调度）。
                EmitTaskFailureFocusToast(selected_id, do_result);
            }
        }

        // Refresh nextExecutionTime from the task
        int64_t                       refreshed_time = 0;
        bool                          has_refreshed_time = false;
        Das::ExportInterface::DasDate next_date{};
        if (compile_failed)
        {
            refreshed_time = SENTINEL_FUTURE_TIMESTAMP;
            has_refreshed_time = true;
            DAS_CORE_LOG_WARN(
                "SchedulerService::ExecuteSelectedTask: task {} authoring compile failed, "
                "setting sentinel nextExecutionTime={}",
                selected_id,
                SENTINEL_FUTURE_TIMESTAMP);
        }
        else
        {
            // 与 Do 调用点同理的窄 catch：插件抛异常时按失败结果处理，
            // 保证后续哨兵分支、执行槽清理与收尾恒可达。
            DasResult net_result = DAS_E_FAIL;
            try
            {
                net_result = task_holder.Get()->GetNextExecutionTime(
                    p_props_json ? p_props_json.Get() : nullptr,
                    &next_date);
            }
            catch (const std::exception& exception)
            {
                net_result = DAS_E_INTERNAL_FATAL_ERROR;
                DAS_CORE_LOG_ERROR(
                    "SchedulerService::ExecuteSelectedTask: task {} GetNextExecutionTime "
                    "threw exception: {}",
                    selected_id,
                    exception.what());
            }
            catch (...)
            {
                net_result = DAS_E_INTERNAL_FATAL_ERROR;
                DAS_CORE_LOG_ERROR(
                    "SchedulerService::ExecuteSelectedTask: task {} GetNextExecutionTime "
                    "threw unknown exception",
                    selected_id);
            }
            if (IsOk(net_result))
            {
                refreshed_time = DasDateToUnix(next_date);
                has_refreshed_time = true;
            }
            else
            {
                // GetNextExecutionTime failed: use sentinel to prevent hot loop
                refreshed_time = SENTINEL_FUTURE_TIMESTAMP;
                has_refreshed_time = true;
                DAS_CORE_LOG_WARN(
                    "SchedulerService::ExecuteSelectedTask: task {} GetNextExecutionTime "
                    "failed (result={}), setting sentinel nextExecutionTime={}",
                    selected_id,
                    net_result,
                    SENTINEL_FUTURE_TIMESTAMP);
            }
        }

        // Re-acquire lock to update in-memory state only.
        // Persistence is posted to config persist thread (no SettingsManager
        // call under mutex_).
        bool                                should_persist = false;
        int64_t                             persist_id = 0;
        int64_t                             persist_time = 0;
        bool                                should_restart_timer = false;
        std::chrono::steady_clock::duration next_delay =
            std::chrono::seconds(1);
        {
            const std::scoped_lock lock(mutex_);

            // 写回经 id 现查：锁外窗口不持有 record 指针。执行期间
            // task 变更入口都被 SchedulerBusyLocked 拒绝，正常必中；
            // 竞态漏防查不到则跳过写回（对象由 task_holder 保活，
            // 不会 UAF）。
            if (auto* inst = FindTaskInstance(selected_id))
            {
                if (has_refreshed_time)
                {
                    inst->next_execution_time = refreshed_time;
                    should_persist = true;
                    persist_id = selected_id;
                    persist_time = refreshed_time;
                }
            }

            current_task_id_.reset();

            if (state_.load() == SchedulerState::Running)
            {
                const int64_t now_unix =
                    SystemClock::to_time_t(SystemClock::now());
                next_delay = ComputeNextDelay(task_instances_, now_unix);
                should_restart_timer = true;
            }
            else if (state_.load() == SchedulerState::Stopping)
            {
                // task 完成立即收尾 Stopped：FsmActEnterStopped 置
                // state/stop_token + NotifyStateChange。等活由执行通路
                // 自身完成。
                fsm_->process_event(EvtTickCompleted{});
            }
            // Stopped（手动单任务执行完成）：无收尾动作，跑完即止。
        }

        // 重排判定在 mutex_ 临界区内做出：锁外重读 state_ 会撞上 Disable
        // 置 Stopped 后的陈旧 Running 窗口，把 timer 重排在停机之后。
        if (should_restart_timer)
        {
            StartTickTimer(next_delay);
        }

        // Post persistence event to config-side thread (outside mutex_).
        // Failures are logged by the persist thread and do not affect
        // runtime state.
        if (should_persist)
        {
            PostPersistEvent(persist_id, persist_time);
        }

        // 周期推送当前状态（UI task 列表刷新）；状态变化的即时推送由
        // Enable/Disable/执行收尾调 NotifyStateChange 负责。
        NotifyStateChange();
    }

    // ----------------------------------------------------------------
    // Config-side persistence thread
    // ----------------------------------------------------------------

    void SchedulerService::SetStateNotifyCallback(
        SchedulerNotifyFunc func,
        void*               user_data)
    {
        const std::scoped_lock lock(state_notify_mutex_);
        state_notify_ =
            std::make_shared<StateNotify>(StateNotify{func, user_data});
    }

    void SchedulerService::PostPersistEvent(int64_t task_id, int64_t next_time)
    {
        {
            const std::scoped_lock lock(config_persist_mutex_);
            config_persist_queue_.push({task_id, next_time});
        }
        config_persist_cv_.notify_one();
    }

    void SchedulerService::ConfigPersistThreadLoop()
    {
        while (true)
        {
            std::unique_lock<std::mutex> lock(config_persist_mutex_);
            config_persist_cv_.wait(
                lock,
                [this]
                {
                    return !config_persist_queue_.empty()
                           || config_persist_shutdown_.load();
                });

            // Drain all pending events before checking shutdown
            while (!config_persist_queue_.empty())
            {
                auto event = config_persist_queue_.front();
                config_persist_queue_.pop();
                lock.unlock();

                // Execute SettingsManager I/O without holding any
                // SchedulerService::mutex_
                auto& settings = plugin_manager_.GetSettingsManager();
                auto  instance_json =
                    settings.GetTaskInstanceJson("0", event.task_id);
                if (instance_json.is_null() || !instance_json.is_object())
                {
                    instance_json = Das::Utils::MakeYyjsonObject();
                }
                {
                    auto inst_obj = instance_json.as_object();
                    inst_obj->operator[](std::string_view(
                        "nextExecutionTime")) = event.next_execution_time;
                }
                auto result = settings.UpdateTaskInstanceJson(
                    "0",
                    event.task_id,
                    instance_json);

                if (DAS::IsFailed(result))
                {
                    DAS_CORE_LOG_ERROR(
                        "SchedulerService::ConfigPersistThread: failed to "
                        "persist nextExecutionTime for task {}, result={}",
                        event.task_id,
                        result);
                }

                lock.lock();
            }

            if (config_persist_shutdown_.load())
            {
                return;
            }
        }
    }

    void SchedulerService::ShutdownConfigPersistQueue()
    {
        config_persist_shutdown_.store(true);
        config_persist_cv_.notify_all();
        if (config_persist_thread_.joinable())
        {
            config_persist_thread_.join();
        }
    }

} // namespace Das::Core::TaskScheduler

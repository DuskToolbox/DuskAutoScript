#define DAS_BUILD_SHARED

#include <das/Plugins/DasGraphTask/DasGraphTaskAdapter.h>

#include <das/Core/GraphRuntime/GraphBoundaryComponents.h>
#include <das/Core/Utils/PortableTime.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Plugins/DasGraphTask/DasGraphTaskImpl.h>
#include <das/Utils/TaskComponentPorts.h>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>

#include <cpp_yyjson.hpp>

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

DAS_NS_BEGIN
namespace Plugins::DasGraphTask
{
    namespace
    {
        // end 组件 compiled_settings 三件套键（manifest config 字段 id 镜像）。
        constexpr std::string_view NEXT_INTERVAL_KEY = "nextInterval";
        constexpr std::string_view NEXT_UNIT_KEY = "nextUnit";
        constexpr std::string_view NEXT_ANCHOR_KEY = "nextAnchorTime";

        // D-B7 缺省一次性上界：9999-12-31T23:59:59 UTC——可安全经
        // TimegmPortable/time_t 往返的远未来时刻（字段类型字面最大值会溢出
        // 换算）。调度器持久化该值后任务不再触发（一次性语义）。
        constexpr ExportInterface::DasDate
            ONE_SHOT_MAX_DATE{9999, 12, 31, 23, 59, 59};

        // 超窗推进上限：interval 极小值+大偏移组合的近无限循环防线（超限
        // warn+INVALID_ARGUMENT → Core sentinel 兜底）。
        constexpr int MAX_SKIPPED_SLOTS = 10000;

        // end 组件 compiled_settings 的下次执行时间三件套（解析中间态；
        // anchor 可选——缺省=跟随本次钟点）。
        struct EndNextSettings
        {
            std::int64_t interval = 0;
            std::string  unit;
            std::string  anchor_time;
            bool         has_interval = false;
            bool         has_unit = false;
        };

        // 三态解析结果：Found=三件套在场；Default=缺省族（外壳/快照/字段
        // 任一缺失——fail-closed 不猜测具体时间，走一次性 max 填充）；
        // Invalid=键在场但值非法（错型等）——显性报错防热循环。
        enum class EndSettingsStatus
        {
            Found,
            Default,
            Invalid
        };

        // unix → DasDate（UTC 字段，PortableGmtime 反填——Core 的 DasDateToUnix
        // 用 TimegmPortable 把 DasDate 当 UTC 解释，出字段必须走同链才能
        // 往返一致）。
        bool NowUtcDasDate(std::time_t now, ExportInterface::DasDate& out)
        {
            std::tm utc{};
            if (Das::Core::Utils::PortableGmtime(&now, &utc) == nullptr)
            {
                return false;
            }
            out.year = static_cast<std::uint16_t>(utc.tm_year + 1900);
            out.month = static_cast<std::uint8_t>(utc.tm_mon + 1);
            out.day = static_cast<std::uint8_t>(utc.tm_mday);
            out.hour = static_cast<std::uint8_t>(utc.tm_hour);
            out.minute = static_cast<std::uint8_t>(utc.tm_min);
            out.second = static_cast<std::uint8_t>(utc.tm_sec);
            return true;
        }

        // 解析锚定钟点 "HH:MM"（照 ParseDailyTime 纪律：sscanf 两段 + 范围
        // 校验 hour 0-23 / minute 0-59，不夹紧、拒绝非法）。
        bool ParseAnchorTime(const std::string& text, int& hour, int& minute)
        {
            if (std::sscanf(text.c_str(), "%d:%d", &hour, &minute) != 2)
            {
                return false;
            }
            return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
        }

        // DasDate(UTC 字段) → unix → 本地 tm → 锚点钟点改写（秒清零）→
        // mktime → unix → UTC 字段回写。锚点是宿主机本地钟点（「本地算
        // slot」——时区铁律：本地钟点不能直接写进 UTC 字段，须经本地域
        // mktime 换算；直接改 UTC 字段会偏移一个 UTC 偏移量）。
        bool ApplyAnchorClockTime(
            ExportInterface::DasDate& date,
            int                       hour,
            int                       minute)
        {
            std::tm utc{};
            utc.tm_year = date.year - 1900;
            utc.tm_mon = date.month - 1;
            utc.tm_mday = date.day;
            utc.tm_hour = date.hour;
            utc.tm_min = date.minute;
            utc.tm_sec = date.second;
            utc.tm_isdst = 0;
            const std::time_t unix_time =
                Das::Core::Utils::TimegmPortable(&utc);
            if (unix_time == static_cast<std::time_t>(-1))
            {
                return false;
            }
            std::tm local{};
            if (Das::Core::Utils::PortableLocaltime(&unix_time, &local)
                == nullptr)
            {
                return false;
            }
            local.tm_hour = hour;
            local.tm_min = minute;
            local.tm_sec = 0;
            local.tm_isdst = -1;
            const std::time_t anchored = std::mktime(&local);
            if (anchored == static_cast<std::time_t>(-1))
            {
                return false;
            }
            return NowUtcDasDate(anchored, date);
        }

        // 单位折算 → DasDate delta 字段（间隔装字段：零字段合法，111-01
        // ABI 谓词）。minute/hour 定长单位超名义域时折算进位（90 分钟 =
        // {hour:1,minute:30}、25 小时 = {day:1,hour:1}——UTC timegm 链无
        // DST，进位精确）；day/week/month/year 是日历单位（月长不齐，折算
        // 即错）不折算，超出名义域返回 false 由上层拒绝。返回 false = 单位
        // 词非法或间隔超出可表达域。
        bool MakeIntervalDelta(
            const std::string&        unit,
            std::int64_t              interval,
            ExportInterface::DasDate& out)
        {
            out = ExportInterface::DasDate{};
            if (interval <= 0)
            {
                return false;
            }
            if (unit == "minute")
            {
                constexpr std::int64_t kMinutesPerDay = 24 * 60;
                const std::int64_t     days = interval / kMinutesPerDay;
                if (days > 31)
                {
                    return false;
                }
                const std::int64_t remainder = interval % kMinutesPerDay;
                out.day = static_cast<std::uint8_t>(days);
                out.hour = static_cast<std::uint8_t>(remainder / 60);
                out.minute = static_cast<std::uint8_t>(remainder % 60);
                return true;
            }
            if (unit == "hour")
            {
                const std::int64_t days = interval / 24;
                if (days > 31)
                {
                    return false;
                }
                out.day = static_cast<std::uint8_t>(days);
                out.hour = static_cast<std::uint8_t>(interval % 24);
                return true;
            }
            if (unit == "day")
            {
                if (interval > 31)
                {
                    return false;
                }
                out.day = static_cast<std::uint8_t>(interval);
                return true;
            }
            if (unit == "week")
            {
                const std::int64_t days = interval * 7;
                if (days > 31)
                {
                    return false;
                }
                out.day = static_cast<std::uint8_t>(days);
                return true;
            }
            if (unit == "month")
            {
                if (interval > 12)
                {
                    return false;
                }
                out.month = static_cast<std::uint8_t>(interval);
                return true;
            }
            if (unit == "year")
            {
                if (interval > 65535)
                {
                    return false;
                }
                out.year = static_cast<std::uint16_t>(interval);
                return true;
            }
            return false;
        }

        // task_settings（编译产物 {"fingerprint","plan"} 外壳序列化，文档
        // 往返数据非受信）→ end 快照 compiled_settings 三件套。下钻先例照
        // ResolveTaskReturnCode：plan.nodeSnapshots 按 componentGuid 匹配
        // GRAPH_END_COMPONENT_GUID。任一形状缺失 → Default（缺省族）；
        // 键在场但值非法 → Invalid。
        EndSettingsStatus FindEndNextSettings(
            ExportInterface::IDasJson* p_task_settings_json,
            EndNextSettings&           out)
        {
            if (p_task_settings_json == nullptr)
            {
                return EndSettingsStatus::Default;
            }
            DAS::DasPtr<IDasReadOnlyString> artifact;
            if (DAS::IsFailed(p_task_settings_json->ToString(0, artifact.Put()))
                || !artifact)
            {
                return EndSettingsStatus::Default;
            }
            const char* plan_utf8 = nullptr;
            if (DAS::IsFailed(artifact->GetUtf8(&plan_utf8))
                || plan_utf8 == nullptr)
            {
                return EndSettingsStatus::Default;
            }
            try
            {
                auto shell = yyjson::read(std::string(plan_utf8));
                auto shell_obj = shell.as_object();
                if (!shell_obj
                    || !shell_obj->contains(std::string_view("plan")))
                {
                    return EndSettingsStatus::Default;
                }
                const auto& plan_subtree =
                    (*shell_obj)[std::string_view("plan")];
                auto plan_obj = plan_subtree.as_object();
                if (!plan_obj
                    || !plan_obj->contains(std::string_view("nodeSnapshots")))
                {
                    return EndSettingsStatus::Default;
                }
                auto snapshots =
                    (*plan_obj)[std::string_view("nodeSnapshots")].as_array();
                if (!snapshots.has_value())
                {
                    return EndSettingsStatus::Default;
                }
                for (const auto& snapshot : *snapshots)
                {
                    auto snapshot_obj = snapshot.as_object();
                    if (!snapshot_obj.has_value())
                    {
                        continue;
                    }
                    const auto component_guid =
                        (*snapshot_obj)[std::string_view("componentGuid")]
                            .as_string();
                    if (!component_guid.has_value()
                        || *component_guid
                               != Das::Core::GraphRuntime::
                                   GRAPH_END_COMPONENT_GUID)
                    {
                        continue;
                    }
                    auto settings =
                        (*snapshot_obj)[std::string_view("compiledSettings")]
                            .as_object();
                    if (!settings.has_value())
                    {
                        // end 在场但 compiled_settings 缺席 → 无三件套键
                        // → 缺省族。
                        return EndSettingsStatus::Default;
                    }
                    if (!settings->contains(NEXT_INTERVAL_KEY)
                        || !settings->contains(NEXT_UNIT_KEY))
                    {
                        // D-B7：无 nextInterval 或 nextUnit 键 → 缺省分支
                        // （一次性 max 填充，不猜测具体时间）。
                        return EndSettingsStatus::Default;
                    }
                    const auto interval_value =
                        (*settings)[NEXT_INTERVAL_KEY].as_int();
                    if (!interval_value.has_value())
                    {
                        return EndSettingsStatus::Invalid;
                    }
                    out.interval = *interval_value;
                    out.has_interval = true;
                    const auto unit_value =
                        (*settings)[NEXT_UNIT_KEY].as_string();
                    if (!unit_value.has_value())
                    {
                        return EndSettingsStatus::Invalid;
                    }
                    out.unit = *unit_value;
                    out.has_unit = true;
                    if (settings->contains(NEXT_ANCHOR_KEY))
                    {
                        const auto& anchor_subtree =
                            (*settings)[NEXT_ANCHOR_KEY];
                        if (!anchor_subtree.is_null())
                        {
                            const auto anchor_value =
                                anchor_subtree.as_string();
                            if (!anchor_value.has_value())
                            {
                                return EndSettingsStatus::Invalid;
                            }
                            out.anchor_time = *anchor_value;
                        }
                    }
                    return EndSettingsStatus::Found;
                }
                // 无 end 快照 → 缺省族。
                return EndSettingsStatus::Default;
            }
            catch (...)
            {
                // 解析异常按形状缺失处理（缺省族），不在解析层抛出。
                return EndSettingsStatus::Default;
            }
        }
    } // namespace

    DasGraphTaskAdapter::DasGraphTaskAdapter(
        PluginInterface::IDasTaskComponentHost* p_host)
        : host_(DasPtr<PluginInterface::IDasTaskComponentHost>(p_host))
    {
    }

    DasResult DasGraphTaskAdapter::Do(
        PluginInterface::IDasStopToken* stop_token,
        ExportInterface::IDasJson*      p_environment_json,
        ExportInterface::IDasJson*      p_task_settings_json)
    {
        std::ignore = p_environment_json;

        // 本次执行推进调度周期：基准失效，执行后首次 GetNextExecutionTime
        // 以执行完成时刻重锚定（「执行到 End 后记录」——D-B7）。无 Do 伴随
        // 的重算不推槽（同 base 同返回值）由 last_planned_ 持有态保证。
        last_planned_.reset();

        // task_settings is the CompiledGraphPlanDto serialization the
        // scheduler extracted from the authoring compile envelope's
        // compile.executionInput — pure pass-through, no field extraction.
        DAS::DasPtr<IDasReadOnlyString> artifact;
        if (p_task_settings_json != nullptr)
        {
            const DasResult to_string_hr =
                p_task_settings_json->ToString(0, artifact.Put());
            if (DAS::IsFailed(to_string_hr))
            {
                return DAS_E_INVALID_JSON;
            }
        }

        // 形状适配：IDasTask 的 settings JSON → 内核组件的 executionInput
        // 输入口（与调用组件喂线同键；task_settings 为 null 时不设口，保持
        // null artifact 语义）。
        DAS::DasPtr<ExportInterface::IDasPortMap> input_map;
        const auto create_map_hr = CreateIDasPortMap(input_map.Put());
        if (DAS::IsFailed(create_map_hr))
        {
            return create_map_hr;
        }
        if (artifact.Get() != nullptr)
        {
            const char* plan_utf8 = nullptr;
            const auto  get_hr = artifact->GetUtf8(&plan_utf8);
            if (DAS::IsFailed(get_hr))
            {
                return get_hr;
            }
            if (plan_utf8 == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            Das::Utils::SetPortString(
                input_map.Get(),
                "executionInput",
                plan_utf8);
        }

        // 委托同模块直接构造的组件内核执行（GraphRuntime 驱动代码仅
        // DasGraphTaskImpl 一份）；输出 portmap 丢弃——IDasTask 无结构化
        // 返回值（fire-and-forget，与 MaapiTask 同语义）。无 host 时内核
        // 自身以结构化错误失败，hostless 回退分支随之消亡。
        DasGraphTaskImpl                          component(host_.Get());
        DAS::DasPtr<ExportInterface::IDasPortMap> output;
        return component.Do(stop_token, input_map.Get(), output.Put());
    }

    DasResult DasGraphTaskAdapter::GetNextExecutionTime(
        ExportInterface::IDasJson* p_task_settings_json,
        ExportInterface::DasDate*  p_out_date)
    {
        if (p_out_date == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }

        // 调度器窄 catch 之外自兜：全程无 throw 出口（T-111-07），意外异常
        // 按结构化失败收口（Core 既有窄 catch 亦兜底，双保险）。
        try
        {
            // ---- 静态通道：end 节点 compiled_settings 三件套 ----
            EndNextSettings         end_settings;
            const EndSettingsStatus settings_status =
                FindEndNextSettings(p_task_settings_json, end_settings);
            if (settings_status == EndSettingsStatus::Default)
            {
                // 缺省族（null json/无外壳/无 end 快照/缺 nextInterval 或
                // nextUnit 键）→ 一次性语义：最大值填充返回 OK——调度器
                // 持久化远未来时间后任务不再触发，不落 now 热循环
                // （D-B7 用户裁决 2026-09-19，修订原 NO_IMPLEMENTATION 设计）。
                *p_out_date = ONE_SHOT_MAX_DATE;
                return DAS_S_OK;
            }
            if (settings_status == EndSettingsStatus::Invalid)
            {
                DAS_LOG_WARNING(
                    DAS_FMT_NS::format(
                        "DasGraphTaskAdapter::GetNextExecutionTime: end "
                        "next config has invalid value type, result={}",
                        static_cast<std::int64_t>(DAS_E_INVALID_ARGUMENT))
                        .c_str());
                return DAS_E_INVALID_ARGUMENT;
            }

            // 非法配置防热循环：间隔≤0/单位词非法/间隔超出可表达域 →
            // warn（result 十进制）+ INVALID_ARGUMENT。
            ExportInterface::DasDate delta{};
            if (end_settings.interval <= 0
                || !MakeIntervalDelta(
                    end_settings.unit,
                    end_settings.interval,
                    delta))
            {
                DAS_LOG_WARNING(
                    DAS_FMT_NS::format(
                        "DasGraphTaskAdapter::GetNextExecutionTime: invalid "
                        "end next config (interval={}, unit=\"{}\"), "
                        "result={}",
                        end_settings.interval,
                        end_settings.unit,
                        static_cast<std::int64_t>(DAS_E_INVALID_ARGUMENT))
                        .c_str());
                return DAS_E_INVALID_ARGUMENT;
            }

            const std::time_t now = now_();

            // 基准 = 本次原计划时刻；无值（首次计算/Do 执行后首算）以 now
            // 重锚定并提交（UTC 字段，PortableGmtime 反填）——此后无 Do 伴
            // 随的重算同 base 同返回值（幂等，防重算推挤）。
            ExportInterface::DasDate base{};
            if (last_planned_.has_value())
            {
                base = *last_planned_;
            }
            else
            {
                if (!NowUtcDasDate(now, base))
                {
                    DAS_LOG_WARNING(
                        DAS_FMT_NS::format(
                            "DasGraphTaskAdapter::GetNextExecutionTime: "
                            "clock conversion failed, result={}",
                            static_cast<std::int64_t>(DAS_E_INVALID_ARGUMENT))
                            .c_str());
                    return DAS_E_INVALID_ARGUMENT;
                }
                last_planned_ = base;
            }

            // 锚定钟点（配置在场且非空）：base 的本地钟点改写为锚点、秒清
            // 零（「定时 8 点执行，下次还是八点」）；非法值拒绝。
            if (!end_settings.anchor_time.empty())
            {
                int anchor_hour = 0;
                int anchor_minute = 0;
                if (!ParseAnchorTime(
                        end_settings.anchor_time,
                        anchor_hour,
                        anchor_minute)
                    || !ApplyAnchorClockTime(base, anchor_hour, anchor_minute))
                {
                    DAS_LOG_WARNING(
                        DAS_FMT_NS::format(
                            "DasGraphTaskAdapter::GetNextExecutionTime: "
                            "invalid nextAnchorTime \"{}\", result={}",
                            end_settings.anchor_time,
                            static_cast<std::int64_t>(DAS_E_INVALID_ARGUMENT))
                            .c_str());
                    return DAS_E_INVALID_ARGUMENT;
                }
            }

            // next = base + delta（111-01 C ABI 直调，UTC 字段算术+归一化
            // 进位——月末/闰年语义由 ABI 测试背书）。
            ExportInterface::DasDate next{};
            DasResult arithmetic_result = DasDateAdd(&base, &delta, &next);
            if (DAS::IsFailed(arithmetic_result))
            {
                DAS_LOG_WARNING(
                    DAS_FMT_NS::format(
                        "DasGraphTaskAdapter::GetNextExecutionTime: "
                        "DasDateAdd failed, result={}",
                        static_cast<std::int64_t>(arithmetic_result))
                        .c_str());
                return arithmetic_result;
            }

            // 超窗：本次越过下个槽点（停机/延迟）→ 跳到未来的下一个槽，
            // 不补跑；skipped 计跳过槽数（focus 记录链路在跳槽日志改造接
            // 通后发布 skipped>0 条目）。
            ExportInterface::DasDate now_utc{};
            if (!NowUtcDasDate(now, now_utc))
            {
                DAS_LOG_WARNING(
                    DAS_FMT_NS::format(
                        "DasGraphTaskAdapter::GetNextExecutionTime: now "
                        "clock conversion failed, result={}",
                        static_cast<std::int64_t>(DAS_E_INVALID_ARGUMENT))
                        .c_str());
                return DAS_E_INVALID_ARGUMENT;
            }
            int skipped = 0;
            for (;;)
            {
                std::int32_t compare = 0;
                arithmetic_result = DasDateCompare(&next, &now_utc, &compare);
                if (DAS::IsFailed(arithmetic_result))
                {
                    DAS_LOG_WARNING(
                        DAS_FMT_NS::format(
                            "DasGraphTaskAdapter::GetNextExecutionTime: "
                            "DasDateCompare failed, result={}",
                            static_cast<std::int64_t>(arithmetic_result))
                            .c_str());
                    return arithmetic_result;
                }
                if (compare > 0)
                {
                    break;
                }
                ExportInterface::DasDate advanced{};
                arithmetic_result = DasDateAdd(&next, &delta, &advanced);
                if (DAS::IsFailed(arithmetic_result))
                {
                    DAS_LOG_WARNING(
                        DAS_FMT_NS::format(
                            "DasGraphTaskAdapter::GetNextExecutionTime: "
                            "over-window DasDateAdd failed, result={}",
                            static_cast<std::int64_t>(arithmetic_result))
                            .c_str());
                    return arithmetic_result;
                }
                next = advanced;
                ++skipped;
                if (skipped > MAX_SKIPPED_SLOTS)
                {
                    DAS_LOG_WARNING(
                        DAS_FMT_NS::format(
                            "DasGraphTaskAdapter::GetNextExecutionTime: "
                            "over-window advance exceeded {} slots, "
                            "result={}",
                            MAX_SKIPPED_SLOTS,
                            static_cast<std::int64_t>(DAS_E_INVALID_ARGUMENT))
                            .c_str());
                    return DAS_E_INVALID_ARGUMENT;
                }
            }

            *p_out_date = next;
            return DAS_S_OK;
        }
        catch (...)
        {
            DAS_LOG_WARNING(
                DAS_FMT_NS::format(
                    "DasGraphTaskAdapter::GetNextExecutionTime: unexpected "
                    "exception, result={}",
                    static_cast<std::int64_t>(DAS_E_INTERNAL_FATAL_ERROR))
                    .c_str());
            return DAS_E_INTERNAL_FATAL_ERROR;
        }
    }
} // namespace Plugins::DasGraphTask
DAS_NS_END

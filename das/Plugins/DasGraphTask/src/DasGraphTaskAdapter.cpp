#define DAS_BUILD_SHARED

#include <das/Plugins/DasGraphTask/DasGraphTaskAdapter.h>

#include "GraphTaskFocusLog.h"

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

        // end 组件「下次时间」string 口 id（manifest end definition 的输入/
        // 输出镜像口对——动态通道数据源口）。
        constexpr std::string_view NEXT_TIME_PORT_ID = "下次时间";

        // 动态通道串长上限防线：wire 契约是 ISO 8601 UTC 定长 19 字符
        // （YYYY-MM-DDTHH:MM:SS）；异常超长串在此截断（解析本身按 19 定长
        // 拒绝，本上限只防日志/持有面的病态串）。
        constexpr std::size_t MAX_CAPTURED_NEXT_TIME_LENGTH = 64;

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

        // 解析锚定钟点 "HH:MM"（照 ParseDailyTime 纪律：from_chars 两段 +
        // 范围校验 hour 0-23 / minute 0-59，不夹紧、拒绝非法）。
        bool ParseAnchorTime(const std::string& text, int& hour, int& minute)
        {
            const char* const text_end = text.data() + text.size();
            const auto hour_result =
                std::from_chars(text.data(), text_end, hour);
            if (
                hour_result.ec != std::errc{} || hour_result.ptr == text_end
                || *hour_result.ptr != ':')
            {
                return false;
            }
            const auto minute_result =
                std::from_chars(hour_result.ptr + 1, text_end, minute);
            if (minute_result.ec != std::errc{} || minute_result.ptr != text_end)
            {
                return false;
            }
            return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
        }

        // 严格 ISO 8601 UTC 定长解析（D-B5 wire 契约）：YYYY-MM-DDTHH:MM:SS
        // 19 字符定长 + 分隔符定位 + 逐位数字 + 分段范围校验（month 1-12/
        // day 1-31/hour 0-23/minute 0-59/second 0-59，year ≥ 1）——不夹紧、
        // 不猜测，任何偏差整串拒绝。字符串是插件产出端口值（非受信），
        // 解析后字段再经 DasDateCompare ABI 谓词复核（日历域 + timegm 往
        // 返——T-111-05：恶意/损坏端口值不能直接驱动调度器）。直填 DasDate
        // UTC 字段与静态通道 base 的 PortableGmtime 出字段同语义，经
        // SchedulerService DasDateToUnix（TimegmPortable）还原同一 unix，
        // 往返精确。
        bool ParseIso8601UtcStrict(
            const std::string&        text,
            ExportInterface::DasDate& out)
        {
            const auto digit_of = [](char c) -> int
            { return c >= '0' && c <= '9' ? c - '0' : -1; };
            if (text.size() != 19)
            {
                return false;
            }
            if (text[4] != '-' || text[7] != '-' || text[10] != 'T'
                || text[13] != ':' || text[16] != ':')
            {
                return false;
            }
            int fields[6] = {0, 0, 0, 0, 0, 0};
            // 每字段起止位（year 0-3 / month 5-6 / day 8-9 / hour 11-12 /
            // minute 14-15 / second 17-18），其余位为分隔符（已校验）。
            const std::size_t field_spans[6][2] =
                {{0, 3}, {5, 6}, {8, 9}, {11, 12}, {14, 15}, {17, 18}};
            for (int field = 0; field < 6; ++field)
            {
                int value = 0;
                for (std::size_t pos = field_spans[field][0];
                     pos <= field_spans[field][1];
                     ++pos)
                {
                    const int digit = digit_of(text[pos]);
                    if (digit < 0)
                    {
                        return false;
                    }
                    value = value * 10 + digit;
                }
                fields[field] = value;
            }
            if (fields[0] < 1)
            {
                // year 0 经 timegm 落在可表示域边缘且语义荒谬，拒绝。
                return false;
            }
            if (fields[1] < 1 || fields[1] > 12 || fields[2] < 1
                || fields[2] > 31)
            {
                return false;
            }
            if (fields[3] > 23 || fields[4] > 59 || fields[5] > 59)
            {
                return false;
            }
            ExportInterface::DasDate parsed{};
            parsed.year = static_cast<std::uint16_t>(fields[0]);
            parsed.month = static_cast<std::uint8_t>(fields[1]);
            parsed.day = static_cast<std::uint8_t>(fields[2]);
            parsed.hour = static_cast<std::uint8_t>(fields[3]);
            parsed.minute = static_cast<std::uint8_t>(fields[4]);
            parsed.second = static_cast<std::uint8_t>(fields[5]);
            // ABI 谓词防线（111-01 日历域校验 + timegm 往返）。
            std::int32_t validation = 0;
            if (DAS::IsFailed(DasDateCompare(&parsed, &parsed, &validation)))
            {
                return false;
            }
            out = parsed;
            return true;
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
        PluginInterface::IDasTaskComponentCreator* p_host)
        : host_(DasPtr<PluginInterface::IDasTaskComponentCreator>(p_host))
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
        // 动态通道捕获值同步清空——每次执行重捕获，陈旧值不跨执行残留
        // （D-B5）。
        last_planned_.reset();
        captured_next_time_.reset();

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
        // DasGraphTaskImpl 一份）。无 host 时内核自身以结构化错误失败，
        // hostless 回退分支随之消亡。
        DasGraphTaskImpl                          component(host_.Get());
        DAS::DasPtr<ExportInterface::IDasPortMap> output;
        const DasResult                           do_result =
            component.Do(stop_token, input_map.Get(), output.Put());

        // 动态通道捕获（D-B5）：执行成功后从 end「下次时间」口读字符串
        // （111-03 MaaPI brain 经 PortableGmtime 格式化的 ISO 8601 UTC 定长
        // 19 字符串）。口缺省/读取失败 = 无动态值（GetNextExecutionTime
        // 回落静态通道），不视为错误、不影响 Do 返回码。端组件把已连线的
        // 输入口转写进出参面（「下次时间」口对镜像），此处读到的即图内组
        // 件连线产出（MaaPI nextTime 或任意日期源）。
        if (DAS::IsOk(do_result) && output.Get() != nullptr)
        {
            const std::string captured =
                Das::Utils::GetPortString(output.Get(), NEXT_TIME_PORT_ID);
            if (!captured.empty())
            {
                if (captured.size() <= MAX_CAPTURED_NEXT_TIME_LENGTH)
                {
                    captured_next_time_ = captured;
                }
            }
        }
        return do_result;
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
            // ---- 动态通道优先（D-B5）：Do 捕获的 end「下次时间」口值 ----
            // 串是插件产出端口值经 PortableGmtime 格式化的 UTC 字段
            // （111-01 wire 契约三端同语义），直填 DasDate 后经
            // SchedulerService DasDateToUnix（TimegmPortable）还原同一
            // unix，往返精确；与静态通道 base 的出字段同归 UTC 字段。解析
            // 失败 fail-safe 回落静态通道（A1 裁决）。
            if (captured_next_time_.has_value())
            {
                ExportInterface::DasDate dynamic_date{};
                if (ParseIso8601UtcStrict(*captured_next_time_, dynamic_date))
                {
                    *p_out_date = dynamic_date;
                    return DAS_S_OK;
                }
                DAS_LOG_WARNING(
                    DAS_FMT_NS::format(
                        "DasGraphTaskAdapter::GetNextExecutionTime: captured "
                        "next time port value is not strict ISO 8601 UTC "
                        "(value=\"{}\"), falling back to static channel, "
                        "result={}",
                        captured_next_time_->substr(0, 32),
                        static_cast<std::int64_t>(DAS_E_INVALID_ARGUMENT))
                        .c_str());
                // 回落静态通道（下方继续）。
            }

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

            // 超窗跳槽经 focus 记录链路（i18n graphTask.scheduleSkipped +
            // user.jsonl）记录跳过经过——发布失败只落调试日志，不影响本
            // 函数返回码（Publish 语义）。
            if (skipped > 0)
            {
                PublishGraphTaskScheduleSkippedFocusEntry(
                    static_cast<std::int64_t>(skipped));
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

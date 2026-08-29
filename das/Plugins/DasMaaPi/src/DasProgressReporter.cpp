#define DAS_BUILD_SHARED

#include "DasProgressReporter.h"

#include <das/DasApi.h>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/StringUtils.h>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasProgressCallback.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasProgressUpdate.Implements.hpp>

#include <boost/dll/runtime_symbol_info.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    namespace
    {
        // 字节数格式化为人类可读文本（"12.3 MB"）。1024 进制。
        std::string FormatBytes(std::uint64_t bytes)
        {
            constexpr double   bytes_per_kib = 1024.0;
            constexpr double   bytes_per_mib = 1024.0 * 1024.0;
            constexpr double   bytes_per_gib = 1024.0 * 1024.0 * 1024.0;
            std::ostringstream oss;
            if (bytes >= static_cast<std::uint64_t>(bytes_per_gib))
            {
                oss << std::fixed << std::setprecision(1)
                    << (bytes / bytes_per_gib) << " GB";
            }
            else if (bytes >= static_cast<std::uint64_t>(bytes_per_mib))
            {
                oss << std::fixed << std::setprecision(1)
                    << (bytes / bytes_per_mib) << " MB";
            }
            else if (bytes >= static_cast<std::uint64_t>(bytes_per_kib))
            {
                oss << std::fixed << std::setprecision(1)
                    << (bytes / bytes_per_kib) << " KB";
            }
            else
            {
                oss << bytes << " B";
            }
            return oss.str();
        }

        // 下载速率文本（"2.5 MB/s"）。
        std::string FormatRate(double rate_mbps)
        {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1) << rate_mbps << " MB/s";
            return oss.str();
        }

        // 下载耗时文本（秒/分秒）。
        std::string FormatElapsed(std::chrono::milliseconds elapsed)
        {
            const auto total_seconds = elapsed.count() / 1000;
            const auto minutes = total_seconds / 60;
            const auto seconds = total_seconds % 60;
            if (minutes > 0)
            {
                return std::to_string(minutes) + "m " + std::to_string(seconds)
                       + "s";
            }
            return std::to_string(seconds) + "s";
        }

        // 阶段 → updateProgress 字段值文本。下载阶段有 total
        // → 字节对 + 百分比；无 total → 阶段态；解压/替换只报阶段。
        // （实际实现迁入 DasProgressReporter::BuildProgressText，经 IDasI18n
        // Translate + vformat 填充 {N}，此处仅保留 FormatBytes 等格式化辅助。）

        // 经 i18n 翻译 key；i18n 为空 / key miss 返回空串（调用方回退硬编码）。
        std::string TranslateKey(
            Das::ExportInterface::IDasI18n* i18n,
            const char*                     key)
        {
            if (i18n == nullptr)
            {
                return {};
            }
            DasPtr<IDasReadOnlyString> key_str;
            if (DAS::IsFailed(
                    ::CreateIDasReadOnlyStringFromUtf8(key, key_str.Put())))
            {
                return {};
            }
            DasPtr<IDasReadOnlyString> text;
            if (DAS::IsFailed(i18n->Translate(key_str.Get(), text.Put())))
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

        // boost::filesystem::path（this_line_location 返回）→ UTF-8
        // std::string。 经 std::filesystem::path 桥接：Windows 走
        // wstring（UTF-16），Unix 走 string（native narrow），再 u8string() 出
        // UTF-8 字节。boost path 没有 u8string()，.string() 在 Windows 是 ACP
        // narrow（中文路径乱码）。
        std::string BoostPathToUtf8(const boost::filesystem::path& boost_path)
        {
#ifdef _WIN32
            const std::filesystem::path std_path{boost_path.wstring()};
#else
            const std::filesystem::path std_path{boost_path.string()};
#endif
            return std::string{DAS::Utils::U8AsString(std_path.u8string())};
        }

        // 插件侧 IDasJson 构造惯例（SchedulerTestPluginImpl / DasFlowControl
        // 先例）：IDasJsonImpl 符号未从 DasCore.dll 导出，插件无法链接；改用
        // 导出的 ParseDasJsonFromString 经序列化往返创建。
        // 注意必须带 copy_string：裸 yyjson::value{std::string} 会被
        // initializer_list 构造解析成单元素数组（["..."]），字段值会变成
        // 数组而非标量，service 侧装配会跳过。
        DasPtr<Das::ExportInterface::IDasJson> MakeJsonFieldValue(
            const std::string& text)
        {
            yyjson::value value{std::string(text), yyjson::copy_string};
            auto          serialized = Das::Utils::SerializeYyjsonValue(value);
            if (!serialized)
            {
                return {};
            }
            DasPtr<Das::ExportInterface::IDasJson> result;
            ParseDasJsonFromString(serialized->c_str(), result.Put());
            return result;
        }

        // 只读字段增量条目（IDasProgressUpdate 本地实现）。承载 SSE
        // 路由 key + authoring 字段增量（fieldName → IDasJson value），无
        // percent/status/stage 语义。service 侧经反向代理逐字段 Get 读回。
        class DasProgressUpdateImpl final
            : public Das::ExportInterface::DasProgressUpdateImplBase<
                  DasProgressUpdateImpl>
        {
        public:
            using Field =
                std::pair<std::string, DasPtr<Das::ExportInterface::IDasJson>>;

            DasProgressUpdateImpl(std::string key, std::vector<Field> fields)
                : key_(std::move(key)), fields_(std::move(fields))
            {
            }

            DasResult GetKey(IDasReadOnlyString** pp_out_key) override
            {
                if (pp_out_key == nullptr)
                {
                    return DAS_E_INVALID_POINTER;
                }
                *pp_out_key = nullptr;
                return CreateIDasReadOnlyStringFromUtf8(
                    key_.c_str(),
                    pp_out_key);
            }

            DasResult GetFieldCount(size_t* p_out_count) override
            {
                if (p_out_count == nullptr)
                {
                    return DAS_E_INVALID_POINTER;
                }
                *p_out_count = fields_.size();
                return DAS_S_OK;
            }

            DasResult GetFieldByIndex(
                size_t                           index,
                IDasReadOnlyString**             pp_out_name,
                Das::ExportInterface::IDasJson** pp_out_value) override
            {
                if (pp_out_name == nullptr || pp_out_value == nullptr)
                {
                    return DAS_E_INVALID_POINTER;
                }
                *pp_out_name = nullptr;
                *pp_out_value = nullptr;
                if (index >= fields_.size())
                {
                    return DAS_E_OUT_OF_RANGE;
                }
                auto rc = CreateIDasReadOnlyStringFromUtf8(
                    fields_[index].first.c_str(),
                    pp_out_name);
                if (DAS::IsFailed(rc))
                {
                    return rc;
                }
                *pp_out_value = fields_[index].second.Get();
                // MakeJsonFieldValue 失败产生的空条目守卫：对空指针 AddRef
                // 是解引用崩溃，返回失败而非崩溃（防御路径，正常序列化不触发）。
                if (*pp_out_value == nullptr)
                {
                    return DAS_E_FAIL;
                }
                (*pp_out_value)->AddRef();
                return DAS_S_OK;
            }

        private:
            std::string        key_;
            std::vector<Field> fields_;
        };
    } // namespace

    // 反向通知回调（IDasProgressCallback 本地实现）。service 在
    // 该 key 所有 SSE 订阅断开时调用；own_key 过滤命中才停止上报。
    // owner_ 持 weak_ptr：跨进程 proxy 去重使本回调可能活得比 reporter
    // 久（DasHttp 持 proxy 引用），lock() 判空后访问 owner，杜绝 UAF。
    class DasProgressReporter::DasProgressCallbackImpl final
        : public Das::ExportInterface::DasProgressCallbackImplBase<
              DasProgressCallbackImpl>
    {
    public:
        DasProgressCallbackImpl(
            std::string                        own_key,
            std::weak_ptr<DasProgressReporter> owner)
            : own_key_(std::move(own_key)), owner_(std::move(owner))
        {
        }

        DasResult OnAllSubscribersDisconnected(IDasReadOnlyString* key) override
        {
            if (key == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            const char* raw = nullptr;
            if (DAS::IsFailed(key->GetUtf8(&raw)) || raw == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (own_key_ == raw)
            {
                if (auto owner = owner_.lock())
                {
                    owner->OnSubscribersDisconnected();
                }
            }
            return DAS_S_OK;
        }

    private:
        std::string                        own_key_;
        std::weak_ptr<DasProgressReporter> owner_;
    };

    DasProgressReporter::DasProgressReporter(
        std::string                                       key,
        DasPtr<Das::ExportInterface::IDasProgressService> service)
        : key_(std::move(key)), service_(std::move(service))
    {
    }

    DasProgressReporter::~DasProgressReporter()
    {
        // RAII 兜底：ApplyUpdate 抛异常跳过正常路径的显式 Stop() 时，
        // 析构保证 RemoveCallback 执行，不泄漏跨进程 callback 注册。
        Stop();
    }

    bool DasProgressReporter::Start()
    {
        if (started_)
        {
            return true;
        }
        if (!service_)
        {
            DAS_LOG_ERROR(
                DAS::fmt::format(
                    "DasProgressReporter: Start without progress service "
                    "key={}",
                    key_)
                    .c_str());
            return false;
        }
        callback_ = DasProgressCallbackImpl::Make(key_, shared_from_this());
        const auto rc = service_->AddCallback(callback_.Get());
        if (DAS::IsFailed(rc))
        {
            DAS_LOG_ERROR(
                DAS::fmt::format(
                    "DasProgressReporter: AddCallback failed key={} result={}",
                    key_,
                    static_cast<int>(rc))
                    .c_str());
            callback_.Reset();
            return false;
        }
        started_ = true;
        DAS_LOG_INFO(
            DAS::fmt::format("DasProgressReporter: AddCallback ok key={}", key_)
                .c_str());
        return true;
    }

    void DasProgressReporter::Stop()
    {
        if (!started_)
        {
            return;
        }
        if (service_ && callback_)
        {
            const auto rc = service_->RemoveCallback(callback_.Get());
            if (DAS::IsFailed(rc))
            {
                DAS_LOG_ERROR(
                    DAS::fmt::format(
                        "DasProgressReporter: RemoveCallback failed key={} "
                        "result={}",
                        key_,
                        static_cast<int>(rc))
                        .c_str());
            }
            else
            {
                DAS_LOG_INFO(
                    DAS::fmt::format(
                        "DasProgressReporter: RemoveCallback ok key={}",
                        key_)
                        .c_str());
            }
        }
        callback_.Reset();
        started_ = false;
    }

    Das::ExportInterface::IDasI18n* DasProgressReporter::GetI18n()
    {
        if (i18n_ != nullptr)
        {
            return i18n_.Get();
        }
        if (i18n_load_attempted_)
        {
            return nullptr;
        }
        i18n_load_attempted_ = true;

        // <plugin_dir>/i18n 经 boost::dll::this_line_location().parent_path()
        // 推导（cwd != 插件安装目录，与 MaaFW LogDir 同范式）。目录缺失 /
        // 装载失败永久降级硬编码（不重试），this_line_location 异平台抛时
        // 静默兜底。
        try
        {
            const auto plugin_module = boost::dll::this_line_location();
            const auto boost_i18n_dir = plugin_module.parent_path() / "i18n";
            const auto dir_utf8 = BoostPathToUtf8(boost_i18n_dir);

            DasPtr<IDasReadOnlyString> dir_str;
            if (DAS::IsFailed(
                    ::CreateIDasReadOnlyStringFromUtf8(
                        dir_utf8.c_str(),
                        dir_str.Put())))
            {
                return nullptr;
            }
            const auto rc =
                ::CreateIDasI18nFromDirectory(dir_str.Get(), i18n_.Put());
            if (DAS::IsFailed(rc) || i18n_ == nullptr)
            {
                i18n_.Reset();
                return nullptr;
            }
            return i18n_.Get();
        }
        catch (...)
        {
            return nullptr;
        }
    }

    std::string DasProgressReporter::BuildProgressText(
        const UpdateProgress& progress)
    {
        auto* const i18n = GetI18n();

        if (progress.stage == UpdateStage::Extract)
        {
            auto text = TranslateKey(i18n, "progress.extracting");
            if (!text.empty())
            {
                return text;
            }
            return "解压中…";
        }
        if (progress.stage == UpdateStage::Replace)
        {
            auto text = TranslateKey(i18n, "progress.replacing");
            if (!text.empty())
            {
                return text;
            }
            return "替换中…";
        }
        if (!progress.total_bytes || *progress.total_bytes == 0)
        {
            auto text = TranslateKey(i18n, "progress.downloading");
            if (!text.empty())
            {
                return text;
            }
            return "正在下载…";
        }
        double        ratio = static_cast<double>(progress.bytes_received)
                              / static_cast<double>(*progress.total_bytes);
        std::uint64_t percent = static_cast<std::uint64_t>(ratio * 100.0);
        if (percent > 100)
        {
            percent = 100;
        }
        // make_format_args 持引用而非拥有：参数须是具名局部变量，生命周期覆盖
        // vformat 调用（禁临时量，Pitfall 5 悬垂引用）。
        const std::string bytes_str = FormatBytes(progress.bytes_received);
        const std::string total_str = FormatBytes(*progress.total_bytes);
        const std::string percent_str = std::to_string(percent);

        auto tmpl = TranslateKey(i18n, "progress.downloading_with_size");
        if (!tmpl.empty())
        {
            return DAS_FMT_NS::vformat(
                std::string_view(tmpl),
                DAS_FMT_NS::make_format_args(
                    bytes_str,
                    total_str,
                    percent_str));
        }
        return "正在下载（" + bytes_str + " / " + total_str + "）" + percent_str
               + "%";
    }

    void DasProgressReporter::Report(const UpdateProgress& progress)
    {
        // 断开后不再浪费 IPC（反向通知语义）。
        if (!started_ || disconnected_.load())
        {
            return;
        }
        if (!service_)
        {
            return;
        }
        std::vector<DasProgressUpdateImpl::Field> fields;
        fields.emplace_back(
            "updateProgress",
            MakeJsonFieldValue(BuildProgressText(progress)));
        // 非下载阶段不投影速率/耗时：解压/替换期 0 值是陈旧数据，UI 不应
        // 在「解压中…」旁出现 "0.0 MB/s" 的误导字段。
        if (progress.stage == UpdateStage::Download)
        {
            fields.emplace_back(
                "downloadRate",
                MakeJsonFieldValue(FormatRate(progress.rate_mbps)));
            fields.emplace_back(
                "downloadElapsed",
                MakeJsonFieldValue(FormatElapsed(progress.elapsed)));
        }

        DasPtr<Das::ExportInterface::IDasProgressUpdate> entry =
            DasProgressUpdateImpl::Make(key_, std::move(fields));
        const auto rc = service_->Report(entry.Get());
        if (DAS::IsFailed(rc))
        {
            DAS_LOG_ERROR(
                DAS::fmt::format(
                    "DasProgressReporter: Report failed key={} result={}",
                    key_,
                    static_cast<int>(rc))
                    .c_str());
        }
    }

    void DasProgressReporter::ReportFinal(bool ok, const std::string& detail)
    {
        // 与 Report 同款早退链：断开后不再浪费 IPC，未 Start / 无 service
        // 直接返回。
        if (!started_ || disconnected_.load())
        {
            return;
        }
        if (!service_)
        {
            return;
        }
        auto* const i18n = GetI18n();
        auto        tmpl = TranslateKey(
            i18n,
            ok ? "progress.updateDone" : "progress.updateFailed");
        std::string text;
        if (!tmpl.empty())
        {
            // make_format_args 持引用而非拥有：参数须是具名局部变量，生命
            // 周期覆盖 vformat 调用（禁临时量悬垂引用）。
            const std::string& detail_text = detail;
            text = DAS_FMT_NS::vformat(
                std::string_view(tmpl),
                DAS_FMT_NS::make_format_args(detail_text));
        }
        else
        {
            text = ok ? "更新完成（" + detail + "）" : "更新失败：" + detail;
        }
        // 终态三字段恒推：文案 + 速率/耗时清空（陈旧下载指标不残留 UI）。
        // 字段值必须经 MakeJsonFieldValue（copy_string——裸 brace init 变单
        // 元素数组，service 侧标量校验会跳过字段）。
        std::vector<DasProgressUpdateImpl::Field> fields;
        fields.emplace_back("updateProgress", MakeJsonFieldValue(text));
        fields.emplace_back("downloadRate", MakeJsonFieldValue(""));
        fields.emplace_back("downloadElapsed", MakeJsonFieldValue(""));

        DasPtr<Das::ExportInterface::IDasProgressUpdate> entry =
            DasProgressUpdateImpl::Make(key_, std::move(fields));
        const auto rc = service_->Report(entry.Get());
        if (DAS::IsFailed(rc))
        {
            DAS_LOG_ERROR(
                DAS::fmt::format(
                    "DasProgressReporter: ReportFinal failed key={} result={}",
                    key_,
                    static_cast<int>(rc))
                    .c_str());
        }
    }

    void DasProgressReporter::ReportCheckOutcome(
        CheckOutcomeKind    kind,
        const PiUpdateInfo& info,
        const std::string&  current_version,
        const std::string&  failure_detail)
    {
        // 与 ReportFinal 同款早退链：断开后不再浪费 IPC，未 Start / 无
        // service 直接返回。
        if (!started_ || disconnected_.load())
        {
            return;
        }
        if (!service_)
        {
            return;
        }
        auto* const i18n = GetI18n();

        // 三终局的模板键、{0} 绑定值与缺译硬编码文案（FoundUpdate 绑目标
        // 版本；UpToDate 绑当前版本；Failed 绑失败分类文案）。
        const char* tmpl_key = "";
        std::string bound_value;
        std::string fallback_text;
        switch (kind)
        {
        case CheckOutcomeKind::FoundUpdate:
            tmpl_key = "progress.updateFound";
            bound_value = info.version_name;
            fallback_text = "发现新版本（" + info.version_name + "）";
            break;
        case CheckOutcomeKind::UpToDate:
            tmpl_key = "progress.upToDate";
            bound_value = current_version;
            fallback_text = "已是最新（" + current_version + "）";
            break;
        case CheckOutcomeKind::Failed:
            tmpl_key = "progress.checkFailed";
            bound_value = failure_detail;
            fallback_text = "检查失败：" + failure_detail;
            break;
        }
        const auto  tmpl = TranslateKey(i18n, tmpl_key);
        std::string text;
        if (!tmpl.empty())
        {
            // make_format_args 持引用而非拥有：参数须是具名局部变量，生命
            // 周期覆盖 vformat 调用（禁临时量悬垂引用）。
            const std::string& bound_text = bound_value;
            text = DAS_FMT_NS::vformat(
                std::string_view(tmpl),
                DAS_FMT_NS::make_format_args(bound_text));
        }
        else
        {
            text = fallback_text;
        }

        // updateVersion：FoundUpdate 推「cur → next」三元退化构造（空侧
        // 退化单值，照投影五字段构造逻辑复写于 reporter 侧）；UpToDate /
        // Failed 推空串。检查链无下载指标，不推 downloadRate /
        // downloadElapsed。
        std::string version_text;
        if (kind == CheckOutcomeKind::FoundUpdate)
        {
            if (current_version.empty())
            {
                version_text = info.version_name;
            }
            else if (info.version_name.empty())
            {
                version_text = current_version;
            }
            else
            {
                version_text = current_version + " → " + info.version_name;
            }
        }

        // 字段值必须经 MakeJsonFieldValue（copy_string——裸 brace init 变单
        // 元素数组，service 侧标量校验会跳过字段）。
        std::vector<DasProgressUpdateImpl::Field> fields;
        fields.emplace_back("updateVersion", MakeJsonFieldValue(version_text));
        if (kind == CheckOutcomeKind::FoundUpdate)
        {
            // releaseNote：日志原文（可为空串——无更新无日志，投影侧兜底
            // 「暂无更新日志」属文档职责，推送只带原文）。
            fields.emplace_back(
                "releaseNote",
                MakeJsonFieldValue(info.release_note));
        }
        fields.emplace_back("updateProgress", MakeJsonFieldValue(text));

        DasPtr<Das::ExportInterface::IDasProgressUpdate> entry =
            DasProgressUpdateImpl::Make(key_, std::move(fields));
        const auto rc = service_->Report(entry.Get());
        if (DAS::IsFailed(rc))
        {
            DAS_LOG_ERROR(
                DAS::fmt::format(
                    "DasProgressReporter: ReportCheckOutcome failed key={} "
                    "result={}",
                    key_,
                    static_cast<int>(rc))
                    .c_str());
        }
    }

    void DasProgressReporter::OnSubscribersDisconnected()
    {
        disconnected_.store(true);
        DAS_LOG_INFO(
            DAS::fmt::format(
                "DasProgressReporter: all subscribers disconnected key={}",
                key_)
                .c_str());
    }
} // namespace Plugins::DasMaaPi
DAS_NS_END

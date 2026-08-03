#define DAS_BUILD_SHARED

#include "DasProgressReporter.h"

#include <das/DasApi.h>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasProgressCallback.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasProgressUpdate.Implements.hpp>

#include <chrono>
#include <cstdint>
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
        std::string BuildUpdateProgressText(const UpdateProgress& progress)
        {
            if (progress.stage == UpdateStage::Extract)
            {
                return "解压中…";
            }
            if (progress.stage == UpdateStage::Replace)
            {
                return "替换中…";
            }
            if (!progress.total_bytes || *progress.total_bytes == 0)
            {
                return "正在下载…";
            }
            double        ratio = static_cast<double>(progress.bytes_received)
                                  / static_cast<double>(*progress.total_bytes);
            std::uint64_t percent = static_cast<std::uint64_t>(ratio * 100.0);
            if (percent > 100)
            {
                percent = 100;
            }
            return "正在下载（" + FormatBytes(progress.bytes_received) + " / "
                   + FormatBytes(*progress.total_bytes) + "）"
                   + std::to_string(percent) + "%";
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
            MakeJsonFieldValue(BuildUpdateProgressText(progress)));
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

#pragma once

#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/abi/IDasProgressService.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasProgressService.Implements.hpp>

#include "../SseHub.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Das::Http
{
    namespace detail
    {
        // setValue payload 中 value 必须是合法 JSON
        // 标量（string/number/bool）。 对齐 authoring FieldDescriptor.value
        // 类型语义；null/object/array 字段增量视为非法，安全跳过（不广播坏
        // payload）。
        inline bool IsValidPayloadValue(const yyjson::value& value)
        {
            if (value.is_string())
            {
                return true;
            }
            if (value.is_uint() || value.is_sint() || value.is_real())
            {
                return true;
            }
            if (value.is_bool())
            {
                return true;
            }
            return false;
        }

        // 装配 authoring setValue change payload：
        // { "target": { "kind": "document" }, "fieldName": ..., "value": ... }
        // 复用 authoring 规范格式，SSE 事件体零新协议。
        inline std::optional<std::string> BuildSetValuePayload(
            const std::string&   field_name,
            const yyjson::value& value)
        {
            yyjson::value event = Das::Utils::MakeYyjsonObject();
            auto          event_obj = event.as_object();
            if (!event_obj)
            {
                return std::nullopt;
            }

            yyjson::value target = Das::Utils::MakeYyjsonObject();
            auto          target_obj = target.as_object();
            if (!target_obj)
            {
                return std::nullopt;
            }
            (*target_obj)[std::string_view("kind")] =
                std::string_view("document");
            (*event_obj)[std::string_view("target")] = std::move(target);

            const std::string field_name_copy{field_name};
            (*event_obj)[std::string_view("fieldName")] =
                yyjson::value{field_name_copy, yyjson::copy_string};
            (*event_obj)[std::string_view("value")] =
                Das::Utils::CloneYyjsonValue(value);

            return Das::Utils::SerializeYyjsonValue(event);
        }
    } // namespace detail

    /**
     * @brief IDasProgressService 主进程实现。
     *
     * DasHttp 侧实现 IDL 接口并经 RegisterServiceByName("das.progress")
     * 按名注册。Report(entry) 读只读 entry 的字段增量，装配 authoring setValue
     * payload，经 SseHub 按 key 广播到前端 SSE；AddCallback/RemoveCallback
     * 管理跨进程 callback 生命周期（RemoveCallback 释放 proxy
     * 触发远端释放链）； SseHub key 订阅归零时反向通知
     * OnAllSubscribersDisconnected。
     *
     * @thread_safety 所有公开方法线程安全（callbacks_ 有互斥保护）。
     */
    class DasProgressServiceImpl final
        : public Das::ExportInterface::DasProgressServiceImplBase<
              DasProgressServiceImpl>
    {
    public:
        explicit DasProgressServiceImpl(std::shared_ptr<SseHub> hub)
            : hub_(std::move(hub))
        {
        }

        // 快照总量上限（never-subscribed key 不触发 key-empty，FIFO 逐出
        // 最旧兜底保证「内存有界」字面成立）。
        static constexpr std::size_t MAX_SNAPSHOT_KEYS = 256;

        DasResult AddCallback(
            Das::ExportInterface::IDasProgressCallback* callback) override
        {
            if (!callback)
            {
                return DAS_E_INVALID_POINTER;
            }
            const std::unique_lock lock(mutex_);
            callbacks_.emplace_back(callback);
            return DAS_S_OK;
        }

        DasResult RemoveCallback(
            Das::ExportInterface::IDasProgressCallback* callback) override
        {
            if (!callback)
            {
                return DAS_E_INVALID_POINTER;
            }
            const std::unique_lock lock(mutex_);
            const auto             it = std::find_if(
                callbacks_.begin(),
                callbacks_.end(),
                [callback](
                    const Das::DasPtr<
                        Das::ExportInterface::IDasProgressCallback>& stored)
                { return stored.Get() == callback; });
            if (it == callbacks_.end())
            {
                return DAS_E_NOT_FOUND;
            }
            // DasPtr 析构 Release → 触发远端释放链（防 DOM 泄漏）。
            callbacks_.erase(it);
            return DAS_S_OK;
        }

        DasResult Report(
            Das::ExportInterface::IDasProgressUpdate* entry) override
        {
            if (!entry)
            {
                return DAS_E_INVALID_POINTER;
            }

            Das::DasPtr<IDasReadOnlyString> key_str;
            auto                            rc = entry->GetKey(key_str.Put());
            if (DAS::IsFailed(rc) || !key_str)
            {
                DAS_LOG_ERROR(
                    DAS_FMT_NS::format(
                        "DasProgressServiceImpl: GetKey failed result={}",
                        rc)
                        .c_str());
                return rc;
            }
            const char* key_raw = nullptr;
            rc = key_str->GetUtf8(&key_raw);
            if (DAS::IsFailed(rc) || !key_raw)
            {
                return rc;
            }
            const std::string key{key_raw};

            size_t field_count = 0;
            rc = entry->GetFieldCount(&field_count);
            if (DAS::IsFailed(rc))
            {
                return rc;
            }

            for (size_t index = 0; index < field_count; ++index)
            {
                Das::DasPtr<IDasReadOnlyString>             name;
                Das::DasPtr<Das::ExportInterface::IDasJson> value;
                rc = entry->GetFieldByIndex(index, name.Put(), value.Put());
                if (DAS::IsFailed(rc))
                {
                    // 越界/异常字段：安全跳过，不中断整条 entry。
                    DAS_LOG_ERROR(
                        DAS_FMT_NS::format(
                            "DasProgressServiceImpl: skip field index={} "
                            "result={}",
                            index,
                            rc)
                            .c_str());
                    continue;
                }
                if (!name || !value)
                {
                    continue;
                }
                const char* name_raw = nullptr;
                if (DAS::IsFailed(name->GetUtf8(&name_raw)) || !name_raw)
                {
                    continue;
                }
                const std::string field_name{name_raw};

                auto payload = ReadFieldPayload(field_name, value.Get());
                if (!payload)
                {
                    continue;
                }
                // 零订阅者告警：推送打到无人订阅的 key（前端未订阅/订阅已
                // 断开/注册未完成），帧丢失的确定性信号。
                if (hub_->SubscriberCount(key) == 0)
                {
                    DAS_LOG_ERROR(
                        DAS_FMT_NS::format(
                            "SSE progress push with no subscriber: key={} "
                            "field={}",
                            key,
                            field_name)
                            .c_str());
                }
                hub_->Broadcast(
                    key,
                    std::make_shared<const std::string>(std::move(*payload)));
            }
            return DAS_S_OK;
        }

        /**
         * @brief SseHub key 订阅归零时反向通知全部已注册 callback。
         *
         * @param key 订阅归零的业务 key；callback 侧按 own_key 过滤。
         */
        void OnKeySubscribersEmpty(const std::string& key)
        {
            std::vector<Das::DasPtr<Das::ExportInterface::IDasProgressCallback>>
                callbacks;
            {
                const std::unique_lock lock(mutex_);
                callbacks = callbacks_;
            }
            Das::DasPtr<IDasReadOnlyString> key_str;
            if (DAS::IsFailed(CreateIDasReadOnlyStringFromUtf8(
                    key.c_str(),
                    key_str.Put()))
                || !key_str)
            {
                return;
            }
            for (const auto& callback : callbacks)
            {
                const auto rc =
                    callback->OnAllSubscribersDisconnected(key_str.Get());
                if (DAS::IsFailed(rc))
                {
                    DAS_LOG_ERROR(
                        DAS_FMT_NS::format(
                            "DasProgressServiceImpl: "
                            "OnAllSubscribersDisconnected failed key={} "
                            "result={}",
                            key,
                            rc)
                            .c_str());
                }
            }
        }

        /**
         * @brief key 是否已知（订阅拒绝谓词）。
         *
         * 双信号：快照命中 OR 存在活跃上报者（callbacks 非空）。后者是
         * IDL 硬约束（callbacks 无 per-key 信息）下的放行信号——受理路径
         * reporter Start 先于订阅到达，check job 期间零 Report 零快照的
         * 合法首订阅不被误杀。
         */
        [[nodiscard]]
        bool IsKeyKnown(const std::string& key) const
        {
            return false;
        }

        /**
         * @brief 取 key 的字段级快照 payload（按 fieldName 字典序）。
         *
         * 返回该 key 每个字段最后一次广播的 payload，供订阅建立处逐条
         * 重放；重放字段序不具语义（字段值幂等覆盖）。未知 key 返回空。
         */
        [[nodiscard]]
        std::vector<std::shared_ptr<const std::string>> TakeSnapshot(
            const std::string& key) const
        {
            return {};
        }

    private:
        // 读 entry 字段的 IDasJson value，装配 payload；解析失败/非法类型
        // 返回 nullopt（安全跳过）。
        static std::optional<std::string> ReadFieldPayload(
            const std::string&              field_name,
            Das::ExportInterface::IDasJson* value)
        {
            if (!value)
            {
                return std::nullopt;
            }
            Das::DasPtr<IDasReadOnlyString> value_str;
            if (DAS::IsFailed(value->ToString(0, value_str.Put()))
                || !value_str)
            {
                return std::nullopt;
            }
            const char* value_raw = nullptr;
            if (DAS::IsFailed(value_str->GetUtf8(&value_raw)) || !value_raw)
            {
                return std::nullopt;
            }
            auto parsed = Das::Utils::ParseYyjsonFromString(value_raw);
            if (!parsed)
            {
                return std::nullopt;
            }
            if (!detail::IsValidPayloadValue(*parsed))
            {
                return std::nullopt;
            }
            return detail::BuildSetValuePayload(field_name, *parsed);
        }

        std::shared_ptr<SseHub> hub_;
        std::mutex              mutex_;
        std::vector<Das::DasPtr<Das::ExportInterface::IDasProgressCallback>>
            callbacks_;
    };

} // namespace Das::Http

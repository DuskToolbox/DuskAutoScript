#ifndef DAS_CORE_LOGGER_IDASLOGREQUESTERIMPL_H
#define DAS_CORE_LOGGER_IDASLOGREQUESTERIMPL_H

#include "das/DasString.hpp"
#include <das/_autogen/idl/abi/DasLogger.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/fmt.h>
#include <boost/circular_buffer.hpp>
#include <mutex>
#include <spdlog/sinks/base_sink.h>

template <typename Mutex>
class DasLogRequesterSink;

class IDasLogRequesterImpl final : public Das::ExportInterface::IDasLogRequester
{
    using Type = DasReadOnlyString;
    using SpLogRequesterSink = std::shared_ptr<DasLogRequesterSink<std::mutex>>;
    std::mutex                                         mutex_{};
    boost::circular_buffer<Type, std::allocator<Type>> buffer_;
    SpLogRequesterSink                                 sp_log_requester_sink_;

public:
    IDasLogRequesterImpl(uint32_t max_buffer_size, SpLogRequesterSink sp_sink);
    ~IDasLogRequesterImpl();
    // IDasBase
    uint32_t AddRef() override;
    uint32_t Release() override;
    DasResult QueryInterface(const DasGuid& iid, void** pp_out_object) override;    // IDasLogRequester
    DasResult RequestOne(Das::ExportInterface::IDasLogReader* p_reader) override;
    // IDasLogRequesterImpl
    void Accept(const std::shared_ptr<std::string>& sp_message);

private:
    uint32_t ref_counter_{};
};

template <typename Mutex>
class DasLogRequesterSink final : public spdlog::sinks::base_sink<Mutex>
{
private:
    Mutex                                          mutex_;
    std::vector<DAS::DasPtr<IDasLogRequesterImpl>> logger_requester_vector_ =
        DAS::Utils::MakeEmptyContainerOfReservedSize<
            decltype(logger_requester_vector_)>(5);

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {

        // log_msg is a struct containing the log entry info like level,
        // timestamp, thread id etc. msg.raw contains pre formatted log

        // If needed (very likely but not mandatory), the sink formats
        // the message before sending it to its final destination:
        spdlog::memory_buf_t formatted;
        spdlog::sinks::base_sink<Mutex>::formatter_->format(msg, formatted);
        auto       message = DAS::FmtCommon::to_string(formatted);
        const auto sp_message =
            std::make_shared<std::string>(std::move(message));
        std::lock_guard guard{mutex_};
        for (const auto& p_requester : logger_requester_vector_)
        {
            p_requester->Accept(sp_message);
        }
    }

    void flush_() override
    {
        // ! 不存在需要flush_的地方
    }

public:
    void Remove(IDasLogRequesterImpl* p_requester)
    {
        std::lock_guard lock_guard{mutex_};
        std::erase(logger_requester_vector_, p_requester);
    }

    void Add(IDasLogRequesterImpl* p_requester)
    {
        std::lock_guard lock_guard{mutex_};
        logger_requester_vector_.emplace_back(p_requester);
    }
};

extern std::shared_ptr<DasLogRequesterSink<std::mutex>>
    g_das_log_requester_sink;

#endif // DAS_CORE_LOGGER_IDASLOGREQUESTERIMPL_H

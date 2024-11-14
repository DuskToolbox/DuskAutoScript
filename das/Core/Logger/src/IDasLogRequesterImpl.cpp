#include "IDasLogRequesterImpl.h"
#include <das/Core/Logger/Logger.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/QueryInterface.hpp>

IDasLogRequesterImpl::IDasLogRequesterImpl(
    uint32_t           max_buffer_size,
    SpLogRequesterSink sp_sink)
    : buffer_{max_buffer_size}, sp_log_requester_sink_{sp_sink}
{
    sp_sink->Add(this);
    DAS_CORE_LOG_INFO(
        "Initialize IDasLogRequesterImpl successfully! This = {}. max_buffer_size = {}.",
        DAS::Utils::VoidP(this),
        max_buffer_size);
}

IDasLogRequesterImpl::~IDasLogRequesterImpl()
{
    sp_log_requester_sink_->Remove(this);
}

DasResult IDasLogRequesterImpl::QueryInterface(
    const DasGuid& iid,
    void**         pp_object)
{
    return DAS::Utils::QueryInterface<IDasLogRequester>(this, iid, pp_object);
}

DasResult IDasLogRequesterImpl::RequestOne(IDasLogReader* p_reader)
{
    std::lock_guard guard{mutex_};

    if (buffer_.empty())
    {
        return DAS_E_OUT_OF_RANGE;
    }

    DAS_UTILS_CHECK_POINTER(p_reader);
    const auto& message = buffer_.front();
    const auto  result = p_reader->ReadOne(message->c_str(), message->size());
    buffer_.pop_front();

    return result;
}

void IDasLogRequesterImpl::Accept(std::shared_ptr<std::string> sp_message)
{
    buffer_.push_back(std::move(sp_message));
}

DasResult CreateIDasLogRequester(
    uint32_t           max_line_count,
    IDasLogRequester** pp_out_requester)
{
    DAS_UTILS_CHECK_POINTER(pp_out_requester)

    try
    {
        const auto p_result =
            new IDasLogRequesterImpl{max_line_count, g_das_log_requester_sink};
        *pp_out_requester = p_result;
        p_result->AddRef();
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DAS_DEFINE_VARIABLE(g_das_log_requester_sink);

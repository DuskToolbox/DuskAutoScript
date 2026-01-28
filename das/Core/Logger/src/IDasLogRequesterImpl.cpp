#include "IDasLogRequesterImpl.h"
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/Utils/CommonUtils.hpp>

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

uint32_t IDasLogRequesterImpl::AddRef()
{
    ++ref_counter_;
    return ref_counter_;
}

uint32_t IDasLogRequesterImpl::Release()
{
    --ref_counter_;
    return ref_counter_;
}

DasResult IDasLogRequesterImpl::QueryInterface(
    const DasGuid& iid,
    void**         pp_object)
{
    if (pp_object == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    // 检查IID_IDasLogRequester
    if (iid == DasIidOf<Das::ExportInterface::IDasLogRequester>())
    {
        *pp_object = static_cast<Das::ExportInterface::IDasLogRequester*>(this);
        this->AddRef();
        return DAS_S_OK;
    }

    // 检查IID_IDasBase
    if (iid == DasIidOf<IDasBase>())
    {
        *pp_object = static_cast<IDasBase*>(this);
        this->AddRef();
        return DAS_S_OK;
    }

    *pp_object = nullptr;
    return DAS_E_NO_INTERFACE;
}

DasResult IDasLogRequesterImpl::RequestOne(
    Das::ExportInterface::IDasLogReader* p_reader)
{
    std::lock_guard guard{mutex_};

    if (buffer_.empty())
    {
        return DAS_E_OUT_OF_RANGE;
    }

    DAS_UTILS_CHECK_POINTER(p_reader);
    const auto& message = buffer_.front();
    buffer_.pop_front();
    const auto result = p_reader->ReadOne(message.Get());

    return result;
}

void IDasLogRequesterImpl::Accept(
    const std::shared_ptr<std::string>& sp_message)
{
    buffer_.push_back({sp_message->c_str()});
}

DasResult CreateIDasLogRequester(
    uint32_t                                 max_line_count,
    Das::ExportInterface::IDasLogRequester** pp_out_requester)
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

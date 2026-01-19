#include "DasHttpLogReader.h"

DasResult DasHttpLogReader::ReadOne(const char* message, size_t size)
{
    message_ = {message, size};
    return DAS_S_OK;
}

DasResult DasHttpLogReader::QueryInterface(const DasGuid& iid, void** pp_object)
{
    if (pp_object == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    // 检查IID_IDasLogReader
    if (iid == DasIidOf<IDasLogReader>())
    {
        *pp_object = static_cast<Das::ExportInterface::IDasLogReader*>(this);
        this->AddRef();
        return DAS_S_OK;
    }

    // 检查IID_IDasBase
    if (iid == DAS_IID_BASE)
    {
        *pp_object = static_cast<Das::ExportInterface::IDasBase*>(this);
        this->AddRef();
        return DAS_S_OK;
    }

    *pp_object = nullptr;
    return DAS_E_NO_INTERFACE;
}

auto DasHttpLogReader::GetLog() const noexcept -> std::string_view
{
    return message_;
}
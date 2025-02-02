#include "DasHttpLogReader.h"
#include "das/Utils/QueryInterface.hpp"

DasResult DasHttpLogReader::ReadOne(const char* message, size_t size)
{
    message_ = {message, size};
    return DAS_S_OK;
}

DasResult DasHttpLogReader::QueryInterface(const DasGuid& iid, void** pp_object)
{
    return DAS::Utils::QueryInterface<IDasLogReader>(this, iid, pp_object);
}

auto DasHttpLogReader::GetLog() const noexcept -> std::string_view
{
    return message_;
}
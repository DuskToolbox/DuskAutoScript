#include "DasHttpLogReader.h"

DasResult DasHttpLogReader::ReadOne(IDasReadOnlyString* message)
{
    const char* log;
    const auto  result = message->GetUtf8(&log);
    if (DAS::IsOk(result))
    {
        message_ = log;
    }
    return result;
}

std::string_view DasHttpLogReader::GetLog() const noexcept { return message_; }

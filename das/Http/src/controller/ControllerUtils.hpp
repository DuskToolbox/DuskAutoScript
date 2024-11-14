#ifndef DAS_HTTP_CONTROLLER_CONTROLLERUTILS_HPP
#define DAS_HTTP_CONTROLLER_CONTROLLERUTILS_HPP

#include <memory>
#include <string>

#include "das/ExportInterface/DasLogger.h"
#include "das/IDasBase.h"
#include "das/Utils/CommonUtils.hpp"
#include "das/Utils/QueryInterface.hpp"

class DasHttpLogReader : public IDasLogReader
{
    std::string message_{};

    DasResult ReadOne(const char* message, size_t size) override
    {
        message_ = {message, size};
        return DAS_S_OK;
    }

    DAS_UTILS_IDASBASE_AUTO_IMPL(DasHttpLogReader)

    DAS_IMPL QueryInterface(const DasGuid& iid, void** pp_object) override
    {
        return DAS::Utils::QueryInterface<IDasLogReader>(this, iid, pp_object);
    };

    auto GetMessage() const noexcept -> std::string_view { return message_; }
};

#endif // DAS_HTTP_CONTROLLER_CONTROLLERUTILS_HPP

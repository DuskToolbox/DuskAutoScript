#ifndef DAS_HTTP_CONTROLLER_DASHTTPLOGREADER_H
#define DAS_HTTP_CONTROLLER_DASHTTPLOGREADER_H

#include <memory>
#include <string>

#include "das/ExportInterface/DasLogger.h"
#include "das/IDasBase.h"
#include "das/Utils/CommonUtils.hpp"

class DasHttpLogReader final : public IDasLogReader
{
    std::string message_{};

    DasResult ReadOne(const char* message, size_t size) override;

    DAS_UTILS_IDASBASE_AUTO_IMPL(DasHttpLogReader)

    DAS_IMPL QueryInterface(const DasGuid& iid, void** pp_object) override;

    [[nodiscard]]
    auto GetMessage() const noexcept -> std::string_view;
};

#endif // DAS_HTTP_CONTROLLER_DASHTTPLOGREADER_H

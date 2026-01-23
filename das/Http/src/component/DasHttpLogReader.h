#ifndef DAS_HTTP_COMPONENT_DASHTTPLOGREADER_H
#define DAS_HTTP_COMPONENT_DASHTTPLOGREADER_H

#include <memory>
#include <string>

#include "das/DasApi.h"
#include "das/IDasBase.h"
#include "das/Utils/CommonUtils.hpp"
#include "das/_autogen/idl/wrapper/Das.ExportInterface.IDasLogReader.Implements.hpp"

class DasHttpLogReader final
    : public DAS::ExportInterface::DasLogReaderImplBase<DasHttpLogReader>
{
    std::string message_{};

    DAS_IMPL ReadOne(::IDasReadOnlyString* message);

    DAS_UTILS_IDASBASE_AUTO_IMPL(DasHttpLogReader)

    DAS_IMPL QueryInterface(const DasGuid& iid, void** pp_object) override;

    [[nodiscard]]
    auto GetLog() const noexcept -> std::string_view;
};

#endif // DAS_HTTP_COMPONENT_DASHTTPLOGREADER_H

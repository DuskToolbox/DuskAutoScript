#ifndef DAS_HTTP_COMPONENT_DASHTTPLOGREADER_H
#define DAS_HTTP_COMPONENT_DASHTTPLOGREADER_H

#include <memory>
#include <string>
#include <string_view>

#include "das/IDasBase.h"
#include "das/_autogen/idl/wrapper/Das.ExportInterface.IDasLogReader.Implements.hpp"

class DasHttpLogReader final
    : public DAS::ExportInterface::DasLogReaderImplBase<DasHttpLogReader>
{
    std::string message_{};

    DAS_IMPL ReadOne(::IDasReadOnlyString* message);

    [[nodiscard]]
    auto GetLog() const noexcept -> std::string_view;
};

#endif // DAS_HTTP_COMPONENT_DASHTTPLOGREADER_H

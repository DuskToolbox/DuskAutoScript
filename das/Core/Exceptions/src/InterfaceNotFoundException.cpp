#include <das/Core/Exceptions/InterfaceNotFoundException.h>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>

DAS_CORE_EXCEPTIONS_NS_BEGIN

InterfaceNotFoundException::InterfaceNotFoundException(const DasGuid& iid)
    : Base{DAS_FMT_NS::format(
        "Attempt to convert interface to one that not exist. Interface id = {} .",
        iid)}
{
}

DAS_CORE_EXCEPTIONS_NS_END

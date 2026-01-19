#include <das/Core/Exceptions/InterfaceNotFoundException.h>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Utils/fmt.h>

DAS_CORE_EXCEPTIONS_NS_BEGIN

InterfaceNotFoundException::InterfaceNotFoundException(const DasGuid& iid)
    : DasException{
          DAS_E_NO_INTERFACE,
          DAS_FMT_NS::format(
              "Attempt to convert interface to one that not exist. Interface id = {} .",
              iid)}
{
}

DAS_CORE_EXCEPTIONS_NS_END

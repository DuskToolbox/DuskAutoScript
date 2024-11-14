#ifndef DAS_CORE_EXCEPTIONS_INTERFACENOTFOUNDEXCEPTION_H
#define DAS_CORE_EXCEPTIONS_INTERFACENOTFOUNDEXCEPTION_H

#include <das/Core/Exceptions/Config.h>
#include <das/IDasBase.h>

DAS_CORE_EXCEPTIONS_NS_BEGIN

class InterfaceNotFoundException : public std::runtime_error
{
    using Base = std::runtime_error;

public:
    explicit InterfaceNotFoundException(const DasGuid& iid);
};

DAS_CORE_EXCEPTIONS_NS_END

#endif // DAS_CORE_EXCEPTIONS_INTERFACENOTFOUNDEXCEPTION_H
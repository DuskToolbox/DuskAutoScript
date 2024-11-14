#ifndef DAS_CORE_EXCEPTIONS_PYTHONEXCEPTION_H
#define DAS_CORE_EXCEPTIONS_PYTHONEXCEPTION_H

#include <das/Core/Exceptions/Config.h>
#include <das/IDasBase.h>

DAS_CORE_EXCEPTIONS_NS_BEGIN

class PythonException : public std::runtime_error
{
    using Base = std::runtime_error;

public:
    DAS_USING_BASE_CTOR(Base);
};

DAS_CORE_EXCEPTIONS_NS_END

#endif // DAS_CORE_EXCEPTIONS_PYTHONEXCEPTION_H

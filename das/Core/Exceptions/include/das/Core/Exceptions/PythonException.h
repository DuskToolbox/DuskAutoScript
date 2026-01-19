#ifndef DAS_CORE_EXCEPTIONS_PYTHONEXCEPTION_H
#define DAS_CORE_EXCEPTIONS_PYTHONEXCEPTION_H

#include <das/Core/Exceptions/Config.h>
#include <das/DasException.hpp>
#include <das/IDasBase.h>

DAS_CORE_EXCEPTIONS_NS_BEGIN

class PythonException : public DasException
{
public:
    explicit PythonException(const std::string& message);
};

DAS_CORE_EXCEPTIONS_NS_END

#endif // DAS_CORE_EXCEPTIONS_PYTHONEXCEPTION_H

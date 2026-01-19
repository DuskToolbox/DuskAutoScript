#include <das/Core/Exceptions/PythonException.h>

DAS_CORE_EXCEPTIONS_NS_BEGIN

PythonException::PythonException(const std::string& message)
    : DasException{DAS_E_PYTHON_ERROR, message}
{
}

DAS_CORE_EXCEPTIONS_NS_END

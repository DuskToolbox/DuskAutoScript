#ifndef DAS_CORE_EXCEPTIONS_PYTHONEXCEPTION_H
#define DAS_CORE_EXCEPTIONS_PYTHONEXCEPTION_H

#include <das/Core/Exceptions/Config.h>
#include <das/DasException.hpp>
#include <das/IDasBase.h>

#include <string>

DAS_CORE_EXCEPTIONS_NS_BEGIN

class PythonException : public DasException
{
public:
    // 从字符串消息构造（现有接口）
    explicit PythonException(const std::string& message);

    // 从当前 Python 错误状态构造（调用 PyErr_Fetch）
    explicit PythonException();

    // 访问器
    const std::string& GetStackTrace() const { return stack_trace_; }
    const std::string& GetExceptionType() const { return exception_type_; }
    const std::string& GetExceptionValue() const { return exception_value_; }

    // 格式化输出
    const char* what() const noexcept override;

private:
    std::string stack_trace_;      // 完整堆栈跟踪
    std::string exception_type_;   // 如 "ValueError", "TypeError"
    std::string exception_value_;  // 异常消息

    // 从 PyErr_Fetch 解析信息
    void ParseCurrentException();
};

DAS_CORE_EXCEPTIONS_NS_END

#endif // DAS_CORE_EXCEPTIONS_PYTHONEXCEPTION_H

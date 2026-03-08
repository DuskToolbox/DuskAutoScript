#include <das/Core/Exceptions/PythonException.h>

// Python.h 需要在禁用警告后包含
DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_UNUSED_PARAMETER
#include <Python.h>
DAS_DISABLE_WARNING_END

#include <algorithm>
#include <sstream>
#include <vector>

DAS_CORE_EXCEPTIONS_NS_BEGIN

PythonException::PythonException(const std::string& message)
    : DasException{DAS_E_PYTHON_ERROR, message}
{
}

PythonException::PythonException()
    : DasException{DAS_E_PYTHON_ERROR, ""}
{
    ParseCurrentException();
}

void PythonException::ParseCurrentException()
{
    PyObject* p_type{nullptr};
    PyObject* p_value{nullptr};
    PyObject* p_trace_back{nullptr};

    // 获取当前 Python 异常
    ::PyErr_Fetch(&p_type, &p_value, &p_trace_back);

    if (p_type == nullptr)
    {
        exception_type_ = "Unknown";
        exception_value_ = "No Python exception set";
        return;
    }

    // 标准化异常
    ::PyErr_NormalizeException(&p_type, &p_value, &p_trace_back);

    // 解析异常类型名
    if (p_type != nullptr)
    {
        PyObject* type_name = ::PyObject_GetAttrString(p_type, "__name__");
        if (type_name != nullptr)
        {
            // PyUnicode_AsUTF8AndSize 是 Stable ABI 兼容的 (Python 3.10+)
            const char* name = ::PyUnicode_AsUTF8AndSize(type_name, nullptr);
            if (name != nullptr)
            {
                exception_type_ = name;
            }
            ::Py_DECREF(type_name);
        }
    }

    // 解析异常值（消息）
    if (p_value != nullptr)
    {
        PyObject* str_value = ::PyObject_Str(p_value);
        if (str_value != nullptr)
        {
            // PyUnicode_AsUTF8AndSize 是 Stable ABI 兼容的 (Python 3.10+)
            const char* msg = ::PyUnicode_AsUTF8AndSize(str_value, nullptr);
            if (msg != nullptr)
            {
                exception_value_ = msg;
            }
            ::Py_DECREF(str_value);
        }
    }

    // 解析堆栈跟踪
    if (p_type != nullptr && p_trace_back != nullptr)
    {
        // 使用 traceback.format_exception 获取完整堆栈
        PyObject* traceback_module = ::PyImport_ImportModule("traceback");
        if (traceback_module != nullptr)
        {
            PyObject* format_exception =
                ::PyObject_GetAttrString(traceback_module, "format_exception");
            if (format_exception != nullptr && ::PyCallable_Check(format_exception))
            {
                // 设置 traceback 到 value
                ::PyException_SetTraceback(p_value, p_trace_back);

                // 构建参数元组
                PyObject* args = ::PyTuple_New(3);
                ::PyTuple_SetItem(args, 0, p_type);
                ::Py_INCREF(p_type);
                ::PyTuple_SetItem(args, 1, p_value);
                ::Py_INCREF(p_value);
                ::PyTuple_SetItem(args, 2, p_trace_back);
                ::Py_INCREF(p_trace_back);

                PyObject* formatted_list = ::PyObject_Call(format_exception, args, nullptr);
                ::Py_DECREF(args);
                ::Py_DECREF(format_exception);

                if (formatted_list != nullptr && ::PyList_Check(formatted_list))
                {
                    std::ostringstream oss;
                    const Py_ssize_t list_size = ::PyList_Size(formatted_list);

                    for (Py_ssize_t i = 0; i < list_size; ++i)
                    {
                        PyObject* formatted_string = ::PyList_GetItem(formatted_list, i);
                        if (::PyUnicode_Check(formatted_string))
                        {
                            // PyUnicode_AsUTF8AndSize 是 Stable ABI 兼容的 (Python 3.10+)
                            const char* str = ::PyUnicode_AsUTF8AndSize(formatted_string, nullptr);
                            if (str != nullptr)
                            {
                                oss << str;
                            }
                        }
                    }
                    stack_trace_ = oss.str();
                }
                ::Py_XDECREF(formatted_list);
            }
            ::Py_XDECREF(format_exception);
            ::Py_DECREF(traceback_module);
        }
    }

    // 恢复异常状态（保持与 PyErr_Fetch 契约）
    ::PyErr_Restore(p_type, p_value, p_trace_back);
}

const char* PythonException::what() const noexcept
{
    // 缓存格式化消息
    static thread_local std::string formatted;

    formatted = "Python Exception: " + exception_type_;
    if (!exception_value_.empty())
    {
        formatted += ": " + exception_value_;
    }
    if (!stack_trace_.empty())
    {
        formatted += "\nStack Trace:\n" + stack_trace_;
    }

    return formatted.c_str();
}

DAS_CORE_EXCEPTIONS_NS_END

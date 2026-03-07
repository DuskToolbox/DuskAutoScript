#include <das/Utils/StringUtils.h>

#ifdef DAS_EXPORT_PYTHON

// #include "TemporaryPluginObjectStorage.h"  // TODO: 未来将重写 PythonHost
#include <das/Core/Exceptions/PythonException.h>
#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Core/ForeignInterfaceHost/PythonHost.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/TaskScheduler/TaskScheduler.h>
#include <das/Utils/CommonUtils.hpp>

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_UNUSED_PARAMETER

// Avoid MSVC Warning C4100
// This warning exists in some specific versions of Python header files
// 一些特定版本的Python头文件中存在这一警告
#include <Python.h>

DAS_DISABLE_WARNING_END

#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

static_assert(
    std::is_same_v<_object*, PyObject*>,
    "Type PyObject is not type _object. Consider to check if \"Python.h\" change "
    "the type declaration.");

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DAS_NS_PYTHONHOST_BEGIN

// ============================================================================
// PyGILGuard 实现
// ============================================================================

PyGILGuard::PyGILGuard() : state_(::PyGILState_Ensure()) {}

PyGILGuard::~PyGILGuard() { ::PyGILState_Release(state_); }

// ============================================================================
// 工厂函数
// ============================================================================

auto CreateForeignLanguageRuntime(
    [[maybe_unused]] const ForeignLanguageRuntimeFactoryDesc& desc)
    -> DAS::Utils::Expected<DasPtr<IForeignLanguageRuntime>>
{
    const auto                      p_runtime = new PythonRuntime{};
    DasPtr<IForeignLanguageRuntime> result{p_runtime};
    return result;
}

PyObjectPtr::PyObjectPtr(
    PyObject* ptr,
    [[maybe_unused]] PyObjectPtr::AttachOnly) noexcept
    : ptr_(ptr)
{
}

PyObjectPtr::PyObjectPtr(decltype(nullptr) p) noexcept : ptr_(p) {}

PyObjectPtr::PyObjectPtr(PyObject* ptr) : ptr_(ptr) { Py_INCREF(ptr_); }

PyObjectPtr::PyObjectPtr(const PyObjectPtr& other) : ptr_(other.ptr_)
{
    Py_XINCREF(ptr_);
}

PyObjectPtr& PyObjectPtr::operator=(const PyObjectPtr& other)
{
    ptr_ = other.ptr_;
    Py_XINCREF(ptr_);
    return *this;
}

PyObjectPtr::PyObjectPtr(PyObjectPtr&& other) noexcept
    : ptr_(std::exchange(other.ptr_, nullptr))
{
}

PyObjectPtr& PyObjectPtr::operator=(PyObjectPtr&& other) noexcept
{
    ptr_ = std::exchange(other.ptr_, nullptr);
    return *this;
}

PyObjectPtr::~PyObjectPtr() { Py_XDECREF(ptr_); }

PyObjectPtr PyObjectPtr::Attach(PyObject* ptr)
{
    return PyObjectPtr(ptr, AttachOnly{});
}

PyObject** PyObjectPtr::Put() noexcept
{
    Py_XDECREF(ptr_);
    ptr_ = nullptr;
    return &ptr_;
}

bool PyObjectPtr::operator==(const PyObject* const p) const noexcept
{
    return p == ptr_;
}

bool PyObjectPtr::operator==(PyObjectPtr p) const noexcept
{
    return p.Get() == ptr_;
}

PyObject* PyObjectPtr::Get() const noexcept { return ptr_; }

PyObjectPtr::operator bool() const noexcept
{
    return !(Get() == PyObjectPtr());
}

PyObject* PyObjectPtr::Detach() noexcept
{
    return std::exchange(ptr_, nullptr);
}

DAS_NS_ANONYMOUS_DETAILS_BEGIN

PyObjectPtr PyUnicodeFromU8String(const char8_t* u8_string)
{
    return PyObjectPtr::Attach(
        PyUnicode_FromString(reinterpret_cast<const char*>(u8_string)));
}

template <class T>
bool IsSubDirectory(T path, T root)
{
    while (path != T{})
    {
        if (path == root)
        {
            return true;
        }
        path = path.parent_path();
    }
    return false;
}

auto GetPreferredSeparator() -> const std::u8string&
{
    static std::u8string result{
        []() -> decltype(result)
        {
            std::string                               tmp_result;
            const U_NAMESPACE_QUALIFIER UnicodeString icu_string{
                std::filesystem::path::preferred_separator};
            icu_string.toUTF8String(tmp_result);
            return {DAS_FULL_RANGE_OF(tmp_result)};
        }()};
    return result;
}

DAS_NS_ANONYMOUS_DETAILS_END

// ============================================================================
// LoadPluginConfig - 解析插件配置文件
// ============================================================================

DasResult LoadPluginConfig(
    const std::filesystem::path& json_path,
    std::string&                 out_entry_point)
{
    std::ifstream file(json_path);
    if (!file.is_open())
    {
        DAS_CORE_LOG_ERROR("Failed to open plugin config: {}", json_path.string());
        return DAS_E_FILE_NOT_FOUND;
    }

    try
    {
        nlohmann::json config = nlohmann::json::parse(file);

        if (config.contains("entryPoint"))
        {
            config["entryPoint"].get_to(out_entry_point);
        }
        else
        {
            DAS_CORE_LOG_ERROR(
                "Plugin config missing 'entryPoint' field: {}",
                json_path.string());
            return DAS_E_FAIL;
        }

        return DAS_S_OK;
    }
    catch (const nlohmann::json::exception& e)
    {
        DAS_CORE_LOG_ERROR("Failed to parse plugin config: {}", e.what());
        return DAS_E_FAIL;
    }
}

// ============================================================================
// ParseEntryPoint - 解析入口点字符串
// ============================================================================

std::pair<std::string, std::string> ParseEntryPoint(const std::string& entry_point)
{
    // "my_package.main.create_plugin" -> ("my_package.main", "create_plugin")
    auto last_dot = entry_point.rfind('.');
    if (last_dot == std::string::npos)
    {
        DAS_CORE_LOG_ERROR("Invalid entry point format: {}", entry_point);
        throw PythonException("Invalid entry point format: " + entry_point);
    }

    return {
        entry_point.substr(0, last_dot),     // module_path
        entry_point.substr(last_dot + 1)     // function_name
    };
}

class PythonResult : public DAS::Utils::NonCopyableAndNonMovable
{
    PyObjectPtr ptr_;
    bool        owned_;

    void DetachIfNotOwned()
    {
        if (!owned_)
        {
            std::ignore = ptr_.Detach();
        }
    }

    void CheckPointer() const
    {
        if (!ptr_)
        {
            RaiseIfError();
        }
    }

    constexpr static PyObject* TEST_PTR = nullptr;

public:
    explicit PythonResult(PyObjectPtr ptr) : ptr_{ptr}, owned_{true}
    {
        CheckPointer();
    }
    explicit PythonResult(PyObject* ptr) : ptr_{ptr}, owned_{false}
    {
        CheckPointer();
    }
    ~PythonResult() { DetachIfNotOwned(); }
    /**
     * @brief 验证构造此方法的指针是否为空，若为空，抛出
     PythonError异常； 否则将指针作为参数执行传入的函数
     *
     * @tparam F Callable对象的类型
     * @param func Callable对象，接收一个类型为PyObject*的输入，
        输出为PyObject*时，暗示这一用户不具有对象所有权；
        输出为PyObjectPtr时，暗示用户已经妥善处理的对象引用问题；
        输出为其它时，暗示下一次调用then时仍然使用当前的PyObject。
     * @return PythonResult& 本对象的引用
     */
    template <class F>
    PythonResult& then(F func)
    {
        if constexpr (std::is_same_v<decltype(func(TEST_PTR)), PyObject*>)
        {
            auto func_result = func(ptr_.Get());
            DetachIfNotOwned();
            ptr_ = PyObjectPtr::Attach(func_result);
            owned_ = false;
        }
        else if constexpr (std::
                               is_same_v<decltype(func(TEST_PTR)), PyObjectPtr>)
        {
            auto func_result = func(ptr_.Get());
            DetachIfNotOwned();
            ptr_ = func_result;
            owned_ = true;
        }
        else
        {
            func(ptr_.Get());
        }

        CheckPointer();
        return *this;
    }

    auto CheckAndGet() -> PyObjectPtr
    {
        CheckPointer();
        return ptr_;
    }

    /**
     * @brief If internal pointer is null, it indicates that an error occurred
     * in the last operation。 Then this function will THROW an exception.
     */
    void Check() { CheckPointer(); };

    [[noreturn]]
    static void RaiseIfError()
    {
        PyObject* p_type{nullptr};
        PyObject* p_value{nullptr};
        PyObject* p_trace_back{nullptr};

        // TODO: 这些C_API已经在Python 3.12中弃用
        // TODO: 请在未来提供对Python3.12版本的支持
        ::PyErr_Fetch(&p_type, &p_value, &p_trace_back);

        const auto traceback =
            PyObjectPtr::Attach(::PyImport_ImportModule("traceback"));
        const auto traceback_format_exception = PyObjectPtr::Attach(
            ::PyObject_GetAttrString(traceback.Get(), "format_exception"));

        const auto format_args = PyObjectPtr::Attach(PyTuple_New(3));

        if (p_type == nullptr)
        {
            // 未知错误，直接Print试试
            ::PyErr_Print();
            throw PythonException{
                "Error happened when calling python code,"
                "but it seems that no error is set in Python."};
        }
        ::PyErr_NormalizeException(&p_type, &p_value, &p_trace_back);
        Py_INCREF(p_type);
        ::PyTuple_SetItem(format_args.Get(), 0, p_type);
        Py_IncRef(p_value);
        ::PyTuple_SetItem(format_args.Get(), 1, p_value);
        if (p_trace_back != nullptr)
        {
            Py_INCREF(p_trace_back);
            ::PyException_SetTraceback(p_value, p_trace_back);
            ::PyTuple_SetItem(format_args.Get(), 2, p_trace_back);
        }
        else
        {
            Py_INCREF(Py_None);
            ::PyTuple_SetItem(format_args.Get(), 2, Py_None);
        }
        const auto formatted_list = PyObjectPtr::Attach(PyObject_Call(
            traceback_format_exception.Get(),
            format_args.Get(),
            nullptr));
        if (::PyList_Check(formatted_list.Get()) == false)
        {
            throw PythonException{"Error happened when calling python code,"
                                  "but formatted_list is not a list."};
        }

        size_t stack_trace_message_size{0};
        auto   string_list = DAS::Utils::MakeEmptyContainerOfReservedSize<
              std::vector<std::pair<const char*, size_t>>>(10);

        const auto list_size = ::PyList_Size(formatted_list.Get());
        for (Py_ssize_t i = 0; i < list_size; ++i)
        {
            const auto formatted_string =
                ::PyList_GetItem(formatted_list.Get(), i);
            if (::PyUnicode_Check(formatted_string) == false)
            {
                continue;
            }
            Py_ssize_t a_line_message_size{0};
            const auto a_line_message = ::PyUnicode_AsUTF8AndSize(
                formatted_string,
                &a_line_message_size);
            if (a_line_message != nullptr)
            {
                string_list.emplace_back(a_line_message, a_line_message_size);
                stack_trace_message_size += a_line_message_size;
            }
        }
        std::string stack_trace_message{};
        stack_trace_message.reserve(stack_trace_message_size);
        std::for_each(
            DAS_FULL_RANGE_OF(string_list),
            [&stack_trace_message](auto string_size_pair)
            {
                stack_trace_message.append(
                    string_size_pair.first,
                    string_size_pair.second);
            });

        ::PyErr_Restore(p_type, p_value, p_trace_back);

        throw PythonException{stack_trace_message};
    }
};

// ============================================================================
// PythonManager 实现
// ============================================================================

PythonManager& PythonManager::GetInstance()
{
    static PythonManager instance;
    return instance;
}

bool PythonManager::Initialize()
{
    if (!::Py_IsInitialized())
    {
        // 使用 Py_InitializeEx(0) 而非 Py_Initialize()
        // 参数 0 表示不注册信号处理程序，避免在子进程中产生问题
        ::Py_InitializeEx(0);
    }
    return ::Py_IsInitialized();
}

bool PythonManager::IsInitialized() const { return ::Py_IsInitialized(); }

void RaisePythonInterpreterException() { PythonResult::RaiseIfError(); }

// ============================================================================
// PythonPluginHolder 实现
// ============================================================================

PythonPluginHolder::PythonPluginHolder(PyObject* py_obj, IDasBase* cpp_ptr)
    : py_obj_(py_obj), cpp_ptr_(cpp_ptr)
{
    Py_INCREF(py_obj_);
    cpp_ptr_->AddRef();
}

PythonPluginHolder::~PythonPluginHolder()
{
    PyGILGuard gil;
    Py_DECREF(py_obj_);
    cpp_ptr_->Release();
}

uint32_t PythonPluginHolder::AddRef() { return ++ref_count_; }

uint32_t PythonPluginHolder::Release()
{
    const auto count = --ref_count_;
    if (count == 0)
    {
        delete this;
        return 0;
    }
    return count;
}

DasResult PythonPluginHolder::QueryInterface(const DasGuid& iid, void** pp_object)
{
    if (!pp_object)
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    return cpp_ptr_->QueryInterface(iid, pp_object);
}

// ============================================================================
// ExtractIDasBaseFromDasRetBase 实现
// ============================================================================

/**
 * @brief 从 SWIG 包装的 DasRetBase 对象中提取 IDasBase 指针
 *
 * 使用 Python 数字协议 hack (int() 转换) 从 SwigPyObject 获取指针。
 * 不直接访问 SwigPyObject 结构体成员，因为 ptr 是 C 成员而非 Python 属性。
 *
 * @param das_ret_base Python DasRetBase 对象
 * @return DasPtr<IDasBase> 包装后的指针，失败返回空指针
 */
auto ExtractIDasBaseFromDasRetBase(PyObject* das_ret_base) -> DasPtr<IDasBase>
{
    if (!das_ret_base)
    {
        DAS_CORE_LOG_ERROR("das_ret_base is null");
        return {};
    }

    // 1. 获取 value 属性
    PyObjectPtr value = PyObjectPtr::Attach(
        PyObject_GetAttrString(das_ret_base, "value"));
    if (!value)
    {
        DAS_CORE_LOG_ERROR("Failed to get 'value' attribute from DasRetBase");
        return {};
    }

    // 2. 获取 this 属性（SwigPyObject）
    PyObjectPtr this_attr = PyObjectPtr::Attach(
        PyObject_GetAttrString(value.Get(), "this"));
    if (!this_attr)
    {
        DAS_CORE_LOG_ERROR("Failed to get 'this' attribute from IDasBase");
        return {};
    }

    // 3. int(this) 获取指针值（触发 SwigPyObject_long()）
    PyObjectPtr ptr_long = PyObjectPtr::Attach(
        PyNumber_Long(this_attr.Get()));
    if (!ptr_long)
    {
        DAS_CORE_LOG_ERROR("Failed to convert SwigPyObject to long (int() conversion failed)");
        return {};
    }

    // 4. 转换为 C++ 指针
    void* raw_ptr = PyLong_AsVoidPtr(ptr_long.Get());
    if (!raw_ptr)
    {
        DAS_CORE_LOG_ERROR("Pointer value is null or conversion failed");
        return {};
    }

    IDasBase* cpp_ptr = static_cast<IDasBase*>(raw_ptr);

    // 5. 创建 PythonPluginHolder 包装
    // 使用 Attach 因为 PythonPluginHolder 构造时已持有初始引用计数 1
    return DasPtr<IDasBase>::Attach(
        new PythonPluginHolder(value.Get(), cpp_ptr));
}

// ============================================================================
// PythonRuntime 实现
// ============================================================================

PythonRuntime::PythonRuntime()
{
    PythonManager::GetInstance().Initialize();
}

PythonRuntime::~PythonRuntime() = default;

uint32_t PythonRuntime::AddRef() { return ++ref_count_; }

uint32_t PythonRuntime::Release()
{
    const auto count = --ref_count_;
    if (count == 0)
    {
        delete this;
    }
    return count;
}

DasResult PythonRuntime::QueryInterface(const DasGuid& iid, void** pp_object)
{
    if (!pp_object)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    // 只检查 DAS_IID_BASE，与 JavaHost 保持一致
    if (iid == DAS_IID_BASE)
    {
        *pp_object = static_cast<IForeignLanguageRuntime*>(this);
        AddRef();
        return DAS_S_OK;
    }

    return DAS_E_NO_INTERFACE;
}

auto PythonRuntime::LoadPlugin(const std::filesystem::path& path)
    -> DAS::Utils::Expected<DasPtr<IDasBase>>
{
    // 1. 获取 GIL
    PyGILGuard gil_guard;

    // 2. 确保解释器已初始化
    if (!PythonManager::GetInstance().IsInitialized())
    {
        DAS_CORE_LOG_ERROR("Python interpreter not initialized");
        return tl::make_unexpected(DAS_E_OBJECT_NOT_INIT);
    }

    try
    {
        // 3. 验证文件存在
        if (!std::filesystem::exists(path))
        {
            DAS_CORE_LOG_ERROR("Plugin manifest not found: {}", path.string());
            return tl::make_unexpected(DAS_E_FILE_NOT_FOUND);
        }

        // 4. 读取配置
        std::string entry_point;
        const auto config_result = LoadPluginConfig(path, entry_point);
        if (DAS::IsFailed(config_result))
        {
            return tl::make_unexpected(config_result);
        }

        // 5. 解析入口点
        const auto [module_path, function_name] = ParseEntryPoint(entry_point);

        DAS_CORE_LOG_INFO(
            "Parsed entry point: module={}, function={}",
            module_path,
            function_name);

        // 6. 获取 manifest 目录并添加到 sys.path
        const auto manifest_dir = path.parent_path();

        // 添加 sys.path（不移除，与"解释器永不销毁"设计一致）
        {
            PythonResult{PyObjectPtr::Attach(PyImport_ImportModule("sys"))}
                .then([&manifest_dir](auto sys_module) {
                    PyObjectPtr path_list = PyObjectPtr::Attach(
                        PyObject_GetAttrString(sys_module, "path"));
                    PyObjectPtr dir_str   = PyObjectPtr::Attach(
                        PyUnicode_FromString(manifest_dir.generic_string().c_str()));

                    // 检查是否已存在
                    if (!PySequence_Contains(path_list.Get(), dir_str.Get()))
                    {
                        PyList_Append(path_list.Get(), dir_str.Get());
                    }
                    return PyObjectPtr{}; // 继续链式调用
                })
                .Check();
        }

        // 7. 导入模块
        PyObjectPtr module;
        {
            PythonResult result{PyObjectPtr::Attach(
                PyUnicode_FromString(module_path.c_str()))};

            result.then([&module](auto module_name) {
                module = PyObjectPtr::Attach(PyImport_Import(module_name.Get()));
                return PyObjectPtr{};
            });

            if (!module)
            {
                result.Check(); // 会抛出 PythonException
            }
        }

        // 8. 存储模块引用
        p_plugin_module = module;

        DAS_CORE_LOG_INFO("Successfully imported module: {}", module_path);

        // 9. 获取入口函数
        PyObjectPtr entry_func;
        {
            PythonResult result{PyObjectPtr::Attach(
                PyObject_GetAttrString(p_plugin_module.Get(), function_name.c_str()))};

            result.then([&entry_func, &function_name](auto func) {
                // 检查是否可调用
                if (!PyCallable_Check(func.Get()))
                {
                    DAS_CORE_LOG_ERROR(
                        "Entry point '{}' is not callable",
                        function_name);
                    throw PythonException("Entry point is not callable: " + function_name);
                }
                entry_func = func;
                return PyObjectPtr{};
            }).Check();
        }

        // 10. 调用入口函数（无参数）
        PyObjectPtr result_obj;
        {
            PythonResult result{PyObjectPtr::Attach(
                PyObject_CallObject(entry_func.Get(), nullptr))};

            result.then([&result_obj, &function_name](auto obj) {
                if (!obj.Get() || obj.Get() == Py_None)
                {
                    DAS_CORE_LOG_ERROR("Entry function '{}' returned None or null", function_name);
                    throw PythonException("Entry function returned None or null");
                }
                result_obj = obj;
                return PyObjectPtr{};
            }).Check();
        }

        DAS_CORE_LOG_INFO("Successfully called entry function: {}", function_name);

        // 11. 从返回值提取 IDasBase 指针
        auto plugin_ptr = ExtractIDasBaseFromDasRetBase(result_obj.Get());
        if (!plugin_ptr)
        {
            DAS_CORE_LOG_ERROR("Failed to extract IDasBase from DasRetBase");
            return tl::make_unexpected(DAS_E_SWIG_INTERNAL_ERROR);
        }

        DAS_CORE_LOG_INFO("Successfully extracted plugin pointer from Python");
        return plugin_ptr;
    }
    catch (const PythonException& e)
    {
        DAS_CORE_LOG_ERROR("Python error in LoadPlugin: {}", e.what());
        return tl::make_unexpected(DAS_E_PYTHON_ERROR);
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_ERROR("Exception in LoadPlugin: {}", e.what());
        return tl::make_unexpected(DAS_E_INTERNAL_FATAL_ERROR);
    }
}

// TODO: 未来将重写 PythonHost
/*
auto PythonRuntime::LoadPlugin(const std::filesystem::path& path)
    ->
 * DAS::Utils::Expected<DasPtr<IDasPluginPackage>>
{
    DAS::Utils::OnExit
 * on_exit{
        [] { ::PyEval_ReleaseThread(::PyThreadState_Get()); }};

 * DasPtr<IDasPluginPackage> result{};
    const auto expected_py_module =
 * ImportPluginModule(path);
    if (!expected_py_module)
    {
        return
 * tl::make_unexpected(expected_py_module.error());
    }
    const auto&
 * py_module = expected_py_module.value();
    p_plugin_module = py_module;


 * try
    {
        const auto py_plugin_initializer =

 * GetPluginInitializer(*py_module.Get());

 * PythonResult{PyObjectPtr::Attach(PyTuple_New(0))}
            .then(

 * [&result, &py_plugin_initializer](auto py_null_tuple)
                {
 auto
 * owner = g_plugin_object.GetOwnership();
                    auto
 * lambda_result = PyObjectPtr::Attach(PyObject_Call(

 * py_plugin_initializer.Get(),
                        py_null_tuple,

 * nullptr));
                    if (lambda_result)
                    {

 * result = owner.GetObject();
                    }
                    return
 * lambda_result;
                })
            .Check();

        //
 * Initialize successfully. Store the module to member variable.

 * p_plugin_module = py_module;
        return result;
    }
    catch (const
 * PythonException& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return
 * tl::make_unexpected(DAS_E_PYTHON_ERROR);
    }
}
const auto& py_module = expected_py_module.value();
p_plugin_module =
py_module;

try
{
    const auto py_plugin_initializer =
GetPluginInitializer(*py_module.Get());

PythonResult{PyObjectPtr::Attach(PyTuple_New(0))}
        .then(
 [&result,
&py_plugin_initializer](auto py_null_tuple)
            {
                auto
owner = g_plugin_object.GetOwnership();
                auto lambda_result =
PyObjectPtr::Attach(PyObject_Call(
 py_plugin_initializer.Get(),

py_null_tuple,
                    nullptr));
                if
(lambda_result)
                {
                    result =
owner.GetObject();
                }
                return lambda_result;
 })

.Check();

    // Initialize successfully. Store the module to member variable.

p_plugin_module = py_module;
    return result;
}
catch (const PythonException&
ex)
{
    DAS_CORE_LOG_EXCEPTION(ex);
    return
tl::make_unexpected(DAS_E_PYTHON_ERROR);
}
 */

// TODO: 未来将重写 PythonHost
/*
auto PythonRuntime::ResolveClassName(const std::filesystem::path&
 * relative_path)
    -> DAS::Utils::Expected<std::u8string>
{
    std::u8string
 * result{};

    if (relative_path.begin() == relative_path.end())
    {

 * return tl::make_unexpected(DAS_E_INVALID_PATH);
    }

    const auto it_end
 * = std::end(relative_path);
    auto       it = relative_path.begin();


 * std::u8string part_string{};

    for (auto it_next =
 * std::next(relative_path.begin()); it_next != it_end;
         ++it,
 * ++it_next)
    {
        part_string = it->u8string();
        if
 * (std::u8string_view{part_string}
            ==
 * std::u8string_view{Details::GetPreferredSeparator()})
        {
 return
 * tl::make_unexpected(DAS_E_INVALID_PATH);
        }
        result +=
 * part_string;
        result += u8'.';
    }
    result +=
 * it->stem().u8string();
    return result;
}
*/

// TODO: 未来将重写 PythonHost
/*
auto PythonRuntime::ImportPluginModule(
    const std::filesystem::path&
 * py_plugin_initializer)
    -> DAS::Utils::Expected<PyObjectPtr>
{
    const
 * auto current_path = std::filesystem::current_path();
    if
 * (!Details::IsSubDirectory(
            py_plugin_initializer,

 * std::filesystem::current_path()))
    {
        DAS_CORE_LOG_ERROR(
 "The
 * given path is not  is not a subdirectory of the current working
 * directory.");

        const auto& w_path_string =
 * py_plugin_initializer.wstring();
        const auto& w_current_path_string =
 * current_path.wstring();
        // Use IDasReadOnlySting to convert wchar
 * string to utf8 string.
        DasPtr<IDasReadOnlyString> p_path_string{};

 * DasPtr<IDasReadOnlyString> p_current_path_string{};

 * CreateIDasReadOnlyStringFromWChar(
            w_path_string.c_str(),

 * w_path_string.size(),
            p_path_string.Put());

 * CreateIDasReadOnlyStringFromWChar(
 w_current_path_string.c_str(),

 * w_current_path_string.size(),
            p_current_path_string.Put());

 * DAS_CORE_LOG_ERROR(
            "NOTE: The given path is \"{}\".\nThe current
 * path is \"{}\".",
            p_path_string,
 p_current_path_string);
 return
 * tl::make_unexpected(DAS_E_INVALID_PATH);
    }

    const auto relative_path
 * = std::filesystem::relative(py_plugin_initializer);
    const auto
 * package_path = ResolveClassName(relative_path);
    if (!package_path)
    {

 * return tl::make_unexpected(package_path.error());
    }

    try
    {

 * return PythonResult{

 * Details::PyUnicodeFromU8String(package_path.value().c_str())}
 .then(

 * [](auto py_package_path)
                {
                    return
 * PyObjectPtr::Attach(
 PyImport_Import(py_package_path));
                })

 * .CheckAndGet();
    }
    catch (const PythonException& ex)
    {

 * DAS_CORE_LOG_ERROR("Import python plugin module failed.");


 * DAS_CORE_LOG_EXCEPTION(ex);

        DAS_CORE_LOG_ERROR(
            "NOTE:
 * The python plugin module name is \"{}\".",
            reinterpret_cast<const
 * char*>(package_path.value().c_str()));
        return
 * tl::make_unexpected(DAS_E_PYTHON_ERROR);
    }
}
*/

// TODO: 未来将重写 PythonHost
/*
auto PythonRuntime::GetPluginInitializer(PyObject& py_module) ->
 * PyObjectPtr
{
    return PythonResult{
        PyObjectPtr::Attach(

 * PyObject_GetAttrString(&py_module, DASCOCREATEPLUGIN_NAME))}

    * .CheckAndGet();
}
*/

DAS_NS_PYTHONHOST_END

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_EXPORT_PYTHON
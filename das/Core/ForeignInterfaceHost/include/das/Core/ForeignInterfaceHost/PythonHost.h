#ifndef DAS_CORE_FOREIGNINTERFACEHOST_PYTHONHOST_H
#define DAS_CORE_FOREIGNINTERFACEHOST_PYTHONHOST_H

#ifdef DAS_EXPORT_PYTHON

#include <atomic>

#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>

/**
 * @brief 在Python.h中声明，其中：typedef _object* PyObject;
 *
 */
struct _object;

#define DAS_NS_PYTHONHOST_BEGIN                                                \
    namespace PythonHost                                                       \
    {

#define DAS_NS_PYTHONHOST_END }

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DAS_NS_PYTHONHOST_BEGIN

DAS_API auto CreateForeignLanguageRuntime(
    const ForeignLanguageRuntimeFactoryDesc& desc)
    -> DAS::Utils::Expected<DasPtr<IForeignLanguageRuntime>>;

/**
 * @brief RAII 包装类，用于自动获取和释放 Python GIL
 *
 * 构造函数调用 PyGILState_Ensure() 获取 GIL
 * 析构函数调用 PyGILState_Release() 释放 GIL
 *
 * 参考 JavaHost.cpp 中的 JniEnvGuard 实现模式
 */
class PyGILGuard
{
public:
    PyGILGuard();
    ~PyGILGuard();

    // 禁止拷贝和移动
    PyGILGuard(const PyGILGuard&) = delete;
    PyGILGuard& operator=(const PyGILGuard&) = delete;
    PyGILGuard(PyGILGuard&&) = delete;
    PyGILGuard& operator=(PyGILGuard&&) = delete;

private:
    // 使用 int 存储 PyGILState_STATE，避免 Stable ABI 模式下的前向声明问题
    // PyGILState_STATE 是枚举类型，可以安全转换为 int
    int state_;
};

class PyObjectPtr
{
    struct AttachOnly
    {
    };
    _object* ptr_{nullptr};
    explicit PyObjectPtr(_object* ptr, [[maybe_unused]] AttachOnly) noexcept;

public:
    explicit PyObjectPtr(decltype(nullptr) p = nullptr) noexcept;
    explicit PyObjectPtr(_object* ptr);
    PyObjectPtr(const PyObjectPtr& other);
    PyObjectPtr& operator=(const PyObjectPtr& other);
    PyObjectPtr(PyObjectPtr&& other) noexcept;
    PyObjectPtr& operator=(PyObjectPtr&& other) noexcept;
    ~PyObjectPtr();

    [[nodiscard]]
    static PyObjectPtr Attach(_object* ptr);
    [[nodiscard]]
    _object** Put() noexcept;
    _object*  Get() const noexcept;
    [[nodiscard]]
    _object* Detach() noexcept;
    bool     operator==(const _object* const other) const noexcept;
    bool     operator==(PyObjectPtr other) const noexcept;
    explicit operator bool() const noexcept;
};

class PythonRuntime final : public IForeignLanguageRuntime
{
private:
    std::atomic<uint32_t> ref_count_{1};
    PyObjectPtr            p_plugin_module;

public:
    PythonRuntime();
    ~PythonRuntime();

    uint32_t  AddRef() override;
    uint32_t  Release() override;
    DasResult QueryInterface(const DasGuid& iid, void** pp_object) override;

    auto LoadPlugin(const std::filesystem::path& path)
        -> DAS::Utils::Expected<DasPtr<IDasBase>> override;

    /*
    static auto ImportPluginModule(
        const std::filesystem::path&
     * py_plugin_initializer)
        -> DAS::Utils::Expected<PyObjectPtr>;

     * static auto ResolveClassName(const std::filesystem::path& relative_path)

     * -> DAS::Utils::Expected<std::u8string>;
    auto
     * GetPluginInitializer(_object& py_module) -> PyObjectPtr;
    */
};

/**
 * @brief 管理 Python 解释器生命周期的单例类
 *
 * 提供全局唯一的 Python 解释器管理入口点，支持懒初始化和幂等的 Initialize() 调用。
 * 使用 Meyer's Singleton 模式实现线程安全的单例。
 */
class PythonManager
{
public:
    /**
     * @brief 获取 PythonManager 单例实例
     * @return PythonManager 的唯一实例引用
     */
    static PythonManager& GetInstance();

    /**
     * @brief 初始化 Python 解释器（幂等操作）
     * @return 初始化是否成功
     *
     * 如果解释器已经初始化，此函数不会重复初始化，直接返回 true。
     * 使用 Py_InitializeEx(0) 而非 Py_Initialize() 以避免注册信号处理程序。
     */
    bool Initialize();

    /**
     * @brief 检查 Python 解释器是否已初始化
     * @return 是否已初始化
     */
    bool IsInitialized() const;

private:
    PythonManager() = default;
    ~PythonManager() = default; // 不调用 Py_Finalize，与 JvmManager 模式一致

    PythonManager(const PythonManager&) = delete;
    PythonManager& operator=(const PythonManager&) = delete;
};

DAS_API void RaisePythonInterpreterException();

/**
 * @brief 包装类，同时持有 PyObject* 和 IDasBase*
 *
 * 用于协调 Python GC 和 DasPtr 两套独立的引用计数系统。
 * 构造时同时 Py_INCREF 和 AddRef，析构时同时 Py_DECREF 和 Release。
 * 析构函数必须获取 GIL，否则 Py_DECREF 会崩溃。
 */
class PythonPluginHolder : public IDasBase
{
private:
    std::atomic<uint32_t> ref_count_{1};
    _object*              py_obj_;  // Py_INCREF 持有 (PyObject* = _object*)
    IDasBase*             cpp_ptr_; // 已 AddRef 持有

public:
    PythonPluginHolder(_object* py_obj, IDasBase* cpp_ptr);
    ~PythonPluginHolder();

    // 禁止拷贝和移动
    PythonPluginHolder(const PythonPluginHolder&) = delete;
    PythonPluginHolder& operator=(const PythonPluginHolder&) = delete;
    PythonPluginHolder(PythonPluginHolder&&) = delete;
    PythonPluginHolder& operator=(PythonPluginHolder&&) = delete;

    uint32_t  AddRef() override;
    uint32_t  Release() override;
    DasResult QueryInterface(const DasGuid& iid, void** pp_object) override;
};

DAS_NS_PYTHONHOST_END

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_EXPORT_PYTHON

#endif // DAS_CORE_FOREIGNINTERFACEHOST_PYTHONHOST_H

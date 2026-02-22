#ifndef DAS_CORE_FOREIGNINTERFACEHOST_PYTHONHOST_H
#define DAS_CORE_FOREIGNINTERFACEHOST_PYTHONHOST_H

#ifdef DAS_EXPORT_PYTHON

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

DAS_API auto CreateForeignLanguageRuntime(const ForeignLanguageRuntimeFactoryDesc& desc)
    -> DAS::Utils::Expected<DasPtr<IForeignLanguageRuntime>>;

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
    PyObjectPtr p_plugin_module;

public:
    PythonRuntime();
    ~PythonRuntime();

    uint32_t  AddRef() override { return 1; };
    uint32_t  Release() override { return 1; };
    DasResult QueryInterface(const DasGuid&, void**) override
    {
        return DAS_E_NO_IMPLEMENTATION;
    }

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

DAS_API void RaisePythonInterpreterException();

DAS_NS_PYTHONHOST_END

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_EXPORT_PYTHON

#endif // DAS_CORE_FOREIGNINTERFACEHOST_PYTHONHOST_H

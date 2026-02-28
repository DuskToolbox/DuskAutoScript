#ifdef DAS_EXPORT_JAVA

#include "JavaHost.h"

#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#define DAS_DLOPEN(path) LoadLibraryW(path.wstring().c_str())
#define DLSYM(handle, name) GetProcAddress(reinterpret_cast<HMODULE>(handle), name)
#define DLCLOSE(handle) FreeLibrary(reinterpret_cast<HMODULE>(handle))
#else
#include <dlfcn.h>
#define DAS_DLOPEN(path) dlopen(path.string().c_str(), RTLD_LAZY)
#define DLSYM(handle, name) dlsym(handle, name)
#define DLCLOSE(handle) dlclose(handle)
#endif

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN
DAS_NS_JAVAHOST_BEGIN

// ============================================================================
// JniEnvGuard 实现
// ============================================================================

JniEnvGuard::JniEnvGuard(JavaVM* vm) : vm_(vm), env_(nullptr), attached_(false)
{
    if (!vm_)
        return;

    // 尝试获取当前线程的 JNIEnv
    jint result = vm_->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_21);

    if (result == JNI_EDETACHED)
    {
        // 当前线程未附加到 JVM，需要 Attach
        JavaVMAttachArgs args{};
        args.version = JNI_VERSION_21;
        args.name = const_cast<char*>("DasHostThread");

        result =
            vm_->AttachCurrentThread(reinterpret_cast<void**>(&env_), &args);
        if (result == JNI_OK)
        {
            attached_ = true;
        }
        else
        {
            DAS_CORE_LOG_ERROR(
                "Failed to attach thread to JVM, error: {}",
                result);
        }
    }
    else if (result != JNI_OK)
    {
        DAS_CORE_LOG_ERROR("Failed to get JNIEnv, error: {}", result);
    }
}

JniEnvGuard::~JniEnvGuard()
{
    if (attached_ && vm_)
    {
        vm_->DetachCurrentThread();
    }
}

JniEnvGuard::JniEnvGuard(JniEnvGuard&& other) noexcept
    : vm_(other.vm_), env_(other.env_), attached_(other.attached_)
{
    other.vm_ = nullptr;
    other.env_ = nullptr;
    other.attached_ = false;
}

JniEnvGuard& JniEnvGuard::operator=(JniEnvGuard&& other) noexcept
{
    if (this != &other)
    {
        if (attached_ && vm_)
        {
            vm_->DetachCurrentThread();
        }
        vm_ = other.vm_;
        env_ = other.env_;
        attached_ = other.attached_;
        other.vm_ = nullptr;
        other.env_ = nullptr;
        other.attached_ = false;
    }
    return *this;
}

// ============================================================================
// JObjectGlobalRef 实现
// ============================================================================

JObjectGlobalRef::JObjectGlobalRef(JNIEnv* env, jobject obj)
    : vm_(nullptr), ref_(nullptr)
{
    if (env && obj)
    {
        env->GetJavaVM(&vm_);
        ref_ = env->NewGlobalRef(obj);
    }
}

JObjectGlobalRef::~JObjectGlobalRef()
{
    if (ref_ && vm_)
    {
        auto guard = std::make_unique<JniEnvGuard>(vm_);
        if (guard->is_valid())
        {
            guard->get()->DeleteGlobalRef(ref_);
        }
    }
}

JObjectGlobalRef::JObjectGlobalRef(JObjectGlobalRef&& other) noexcept
    : vm_(other.vm_), ref_(other.ref_)
{
    other.vm_ = nullptr;
    other.ref_ = nullptr;
}

JObjectGlobalRef& JObjectGlobalRef::operator=(JObjectGlobalRef&& other) noexcept
{
    if (this != &other)
    {
        if (ref_ && vm_)
        {
            auto guard = std::make_unique<JniEnvGuard>(vm_);
            if (guard->is_valid())
            {
                guard->get()->DeleteGlobalRef(ref_);
            }
        }
        vm_ = other.vm_;
        ref_ = other.ref_;
        other.vm_ = nullptr;
        other.ref_ = nullptr;
    }
    return *this;
}

// ============================================================================
// JvmManager 实现
// ============================================================================

JvmManager& JvmManager::GetInstance()
{
    static JvmManager instance;
    return instance;
}

JavaVM* JvmManager::GetOrCreateVM(
    const std::filesystem::path&              jvm_dll_path,
    const std::vector<std::string>&           jvm_options,
    const std::vector<std::filesystem::path>& class_path)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (jvm_)
    {
        return jvm_;
    }

    jvm_ = CreateJVM(jvm_dll_path, jvm_options, class_path);
    return jvm_;
}

std::unique_ptr<JniEnvGuard> JvmManager::GetEnv()
{
    if (!jvm_)
    {
        return nullptr;
    }
    return std::make_unique<JniEnvGuard>(jvm_);
}

JavaVM* JvmManager::CreateJVM(
    const std::filesystem::path&              jvm_dll_path,
    const std::vector<std::string>&           jvm_options,
    const std::vector<std::filesystem::path>& class_path)
{
    // 1. 动态加载 jvm.dll
    void* jvm_handle = DAS_DLOPEN(jvm_dll_path);
    if (!jvm_handle)
    {
        DAS_CORE_LOG_ERROR("Failed to load JVM DLL: {}", jvm_dll_path.string());
        return nullptr;
    }

    // 2. 获取 JNI_CreateJavaVM 函数
    using JNI_CreateJavaVM_t = jint (*)(JavaVM**, void**, void*);
    auto p_JNI_CreateJavaVM = reinterpret_cast<JNI_CreateJavaVM_t>(
        DLSYM(jvm_handle, "JNI_CreateJavaVM"));

    if (!p_JNI_CreateJavaVM)
    {
        DAS_CORE_LOG_ERROR("Failed to find JNI_CreateJavaVM function");
        DLCLOSE(jvm_handle);
        return nullptr;
    }

    // 3. 构建 JVM 选项
    std::string               class_path_str = BuildClassPathString(class_path);
    std::vector<JavaVMOption> options;

    // 添加类路径选项
    if (!class_path_str.empty())
    {
        std::string cp_opt = "-Djava.class.path=" + class_path_str;
        options.push_back({const_cast<char*>(cp_opt.c_str()), nullptr});
    }

    // 添加用户选项
    for (const auto& opt : jvm_options)
    {
        options.push_back({const_cast<char*>(opt.c_str()), nullptr});
    }

    // 4. 初始化 JVM
    JavaVMInitArgs vm_args{};
    vm_args.version = JNI_VERSION_21;
    vm_args.nOptions = static_cast<jint>(options.size());
    vm_args.options = options.data();
    vm_args.ignoreUnrecognized = JNI_FALSE;

    JavaVM* jvm = nullptr;
    JNIEnv* env = nullptr;
    jint    result =
        p_JNI_CreateJavaVM(&jvm, reinterpret_cast<void**>(&env), &vm_args);

    if (result != JNI_OK)
    {
        DAS_CORE_LOG_ERROR("Failed to create JVM, error: {}", result);
        DLCLOSE(jvm_handle);
        return nullptr;
    }

    DAS_CORE_LOG_INFO(
        "JVM created successfully with class path: {}",
        class_path_str);

    // 注意：不关闭 jvm_handle，因为 JVM 需要保持加载
    // jvm_handle 在进程生命周期内保持打开

    return jvm;
}

std::filesystem::path JvmManager::FindJvmDllPath()
{
    // 从环境变量查找
    char* java_home = nullptr;
    size_t len = 0;
    if (_dupenv_s(&java_home, &len, "JAVA_HOME") == 0 && java_home)
    {
        std::filesystem::path jvm_path =
            std::filesystem::path(java_home) / "bin" / "server" / "jvm.dll";
        free(java_home);
        if (std::filesystem::exists(jvm_path))
        {
            return jvm_path;
        }
    }

    // 尝试常见路径
    std::vector<std::filesystem::path> common_paths = {
        "C:/Program Files/Microsoft/jdk-21.0.6.7-hotspot/bin/server/jvm.dll",
        "C:/Program Files/Java/jdk-21/bin/server/jvm.dll",
        "C:/Program Files/Java/jdk-17/bin/server/jvm.dll",
        "C:/Program Files/Java/jdk-11/bin/server/jvm.dll",
    };

    for (const auto& path : common_paths)
    {
        if (std::filesystem::exists(path))
        {
            return path;
        }
    }

    return {};
}

std::string JvmManager::BuildClassPathString(
    const std::vector<std::filesystem::path>& class_path)
{
    std::string result;
    for (const auto& path : class_path)
    {
        if (!result.empty())
        {
            result += ";"; // Windows 路径分隔符
        }
        result += path.string();
    }
    return result;
}

// ============================================================================
// JavaRuntime 实现
// ============================================================================

JavaRuntime::JavaRuntime(const JavaRuntimeDesc& desc) : jvm_(nullptr)
{
    // 获取 jvm.dll 路径
    std::filesystem::path jvm_dll_path = desc.jvm_dll_path;
    if (jvm_dll_path.empty())
    {
        jvm_dll_path = JvmManager::FindJvmDllPath();
    }

    if (jvm_dll_path.empty())
    {
        DAS_CORE_LOG_ERROR("Failed to find jvm.dll");
        return;
    }

    // 创建 JVM
    jvm_ = JvmManager::GetInstance().GetOrCreateVM(
        jvm_dll_path,
        desc.jvm_options,
        desc.class_path);

    if (!jvm_)
    {
        DAS_CORE_LOG_ERROR("Failed to create JavaRuntime: JVM creation failed");
    }
}

JavaRuntime::~JavaRuntime()
{
    // JVM 由 JvmManager 单例管理，不在这里销毁
}

auto JavaRuntime::LoadPlugin(const std::filesystem::path& path)
    -> DAS::Utils::Expected<DasPtr<IDasBase>>
{
    if (!jvm_)
    {
        return tl::make_unexpected(DAS_E_OBJECT_NOT_INIT);
    }

    auto env_guard = JvmManager::GetInstance().GetEnv();
    if (!env_guard || !env_guard->is_valid())
    {
        return tl::make_unexpected(DAS_E_OBJECT_NOT_INIT);
    }

    JNIEnv* env = env_guard->get();

    try
    {
        // 1. 读取插件 JSON 配置
        std::string entry_point;
        std::vector<std::filesystem::path> additional_jars;
        auto        result = LoadPluginConfig(path, entry_point, additional_jars);
        if (DAS::IsFailed(result))
        {
            return tl::make_unexpected(result);
        }

        // 2. 解析 entryPoint: "org.das.plugin.JavaTestPlugin.createInstance"
        auto [class_path_str, method_name] = ParseEntryPoint(entry_point);

        // 3. 查找 Java 类
        jclass plugin_class = env->FindClass(class_path_str.c_str());
        if (!plugin_class)
        {
            DAS_CORE_LOG_ERROR("Failed to find Java class: {}", class_path_str);
            if (env->ExceptionCheck())
            {
                env->ExceptionDescribe();
                env->ExceptionClear();
            }
            return tl::make_unexpected(DAS_E_INVALID_FILE);
        }
        // 签名: ()Lorg/das/DasRetBase; - 无参，返回 DasRetBase
        jmethodID method = env->GetStaticMethodID(
            plugin_class,
            method_name.c_str(),
            "()Lorg/das/DasRetBase;");

        if (!method)
        {
            DAS_CORE_LOG_ERROR(
                "Failed to find static method: {} in class: {}",
                method_name,
                class_path_str);
            if (env->ExceptionCheck())
            {
                env->ExceptionDescribe();
                env->ExceptionClear();
            }
            return tl::make_unexpected(DAS_E_SYMBOL_NOT_FOUND);
        }
        // 5. 调用静态方法
        jobject ret_base = env->CallStaticObjectMethod(plugin_class, method);
        if (!ret_base)
        {
            DAS_CORE_LOG_ERROR("Static method {} returned null", method_name);
            if (env->ExceptionCheck())
            {
                env->ExceptionDescribe();
                env->ExceptionClear();
            }
            return tl::make_unexpected(DAS_E_FAIL);
        }
        // 6. 从 DasRetBase 提取 IDasBase
        auto plugin_ptr = ExtractIDasBaseFromDasRetBase(env, ret_base);

        env->DeleteLocalRef(ret_base);
        env->DeleteLocalRef(plugin_class);

        return plugin_ptr;
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_ERROR("Exception in LoadPlugin: {}", e.what());
        return tl::make_unexpected(DAS_E_INTERNAL_FATAL_ERROR);
    }
}

uint32_t JavaRuntime::Release()
{
    const auto count = --ref_count_;
    if (count == 0)
    {
        delete this;
    }
    return count;
}

DasResult JavaRuntime::QueryInterface(const DasGuid& iid, void** pp_object)
{
    if (!pp_object)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    // 只检查 DAS_IID_BASE，与 CppHost 保持一致
    if (iid == DAS_IID_BASE)
    {
        *pp_object = static_cast<IForeignLanguageRuntime*>(this);
        AddRef();
        return DAS_S_OK;
    }

    return DAS_E_NO_INTERFACE;
}

std::pair<std::string, std::string> JavaRuntime::ParseEntryPoint(
    const std::string& entry_point)
{
    // "org.das.plugin.JavaTestPlugin.createInstance"
    // -> ("org/das/plugin/JavaTestPlugin", "createInstance")

    auto last_dot = entry_point.rfind('.');
    if (last_dot == std::string::npos)
    {
        return {"", entry_point};
    }

    std::string class_path = entry_point.substr(0, last_dot);
    std::string method_name = entry_point.substr(last_dot + 1);

    // 将 "." 替换为 "/"
    std::replace(class_path.begin(), class_path.end(), '.', '/');

    return {class_path, method_name};
}

DasPtr<IDasBase> JavaRuntime::ExtractIDasBaseFromDasRetBase(
    JNIEnv* env,
    jobject das_ret_base)
{
    // 1. 获取 DasRetBase.getValue() 方法
    jclass    ret_base_class = env->GetObjectClass(das_ret_base);
    jmethodID get_value_method =
        env->GetMethodID(ret_base_class, "getValue", "()Lorg/das/IDasBase;");

    if (!get_value_method)
    {
        DAS_CORE_LOG_ERROR("Failed to find getValue() method in DasRetBase");
        env->DeleteLocalRef(ret_base_class);
        return nullptr;
    }

    // 2. 调用 getValue() 获取 IDasBase
    jobject idas_base = env->CallObjectMethod(das_ret_base, get_value_method);
    if (!idas_base)
    {
        DAS_CORE_LOG_ERROR("DasRetBase.getValue() returned null");
        env->DeleteLocalRef(ret_base_class);
        return nullptr;
    }

    // 3. 获取 swigCPtr 字段
    jclass   idas_base_class = env->GetObjectClass(idas_base);
    jfieldID swig_cptr_field =
        env->GetFieldID(idas_base_class, "swigCPtr", "J");

    if (!swig_cptr_field)
    {
        DAS_CORE_LOG_ERROR("Failed to find swigCPtr field in IDasBase");
        env->DeleteLocalRef(idas_base_class);
        env->DeleteLocalRef(idas_base);
        env->DeleteLocalRef(ret_base_class);
        return nullptr;
    }

    // 4. 读取 C++ 指针
    jlong c_ptr = env->GetLongField(idas_base, swig_cptr_field);

    // 5. 创建 C++ IDasBase 指针
    // swigCMemOwn=false 表示不拥有内存，Java 端负责释放
    auto* cpp_ptr = reinterpret_cast<IDasBase*>(c_ptr);

    DasPtr<IDasBase> result;
    if (cpp_ptr)
    {
        result.Attach(cpp_ptr);
        // 注意：Attach 会增加引用计数
    }

    env->DeleteLocalRef(idas_base_class);
    env->DeleteLocalRef(idas_base);
    env->DeleteLocalRef(ret_base_class);

    return result;
}

DasResult JavaRuntime::LoadPluginConfig(
    const std::filesystem::path&        json_path,
    std::string&                        out_entry_point,
    std::vector<std::filesystem::path>& out_additional_jars)
{
    (void)out_additional_jars; // 保留参数以供将来扩展
    std::ifstream file(json_path);
    if (!file.is_open())
    {
        DAS_CORE_LOG_ERROR(
            "Failed to open plugin config: {}",
            json_path.string());
        return DAS_E_FILE_NOT_FOUND;
    }
    try
    {
        nlohmann::json config = nlohmann::json::parse(file);

        if (config.contains("entryPoint"))
        {
            out_entry_point = config["entryPoint"].get<std::string>();
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
// 工厂函数
// ============================================================================

auto CreateJavaRuntime(const JavaRuntimeDesc& desc)
    -> DAS::Utils::Expected<DasPtr<IForeignLanguageRuntime>>
{
    try
    {
        auto* runtime = new JavaRuntime(desc);
        if (!runtime->IsInitialized())
        {
            delete runtime;
            return tl::make_unexpected(DAS_E_OBJECT_NOT_INIT);
        }
        return DasPtr<IForeignLanguageRuntime>(runtime);
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_ERROR("Exception in CreateJavaRuntime: {}", e.what());
        return tl::make_unexpected(DAS_E_FAIL);
    }
}

DAS_NS_JAVAHOST_END

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_EXPORT_JAVA

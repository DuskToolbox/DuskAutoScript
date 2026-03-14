#ifdef DAS_EXPORT_JAVA

#include "JavaHost.h"

#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include <boost/dll.hpp>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN
DAS_NS_JAVAHOST_BEGIN

namespace jvm_dll
{
    using JNI_CreateJavaVM_t = decltype(::JNI_CreateJavaVM);
}

// ============================================================================
// JniEnvGuard 实现
// ============================================================================

JniEnvGuard::JniEnvGuard(JavaVM* vm) : vm_(vm), env_(nullptr), attached_(false)
{
    if (!vm_)
        return;

    // 尝试获取当前线程的 JNIEnv
    jint result = vm_->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_2);

    if (result == JNI_EDETACHED)
    {
        // 当前线程未附加到 JVM，需要 Attach
        JavaVMAttachArgs args{};
        args.version = JNI_VERSION_1_2;
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
        jint result = env->GetJavaVM(&vm_);
        if (result != JNI_OK)
        {
            DAS_CORE_LOG_ERROR(
                "Failed to get JavaVM from JNIEnv, error: {}",
                result);
            vm_ = nullptr;
            return;
        }
        ref_ = env->NewGlobalRef(obj);
        if (!ref_)
        {
            DAS_CORE_LOG_ERROR("Failed to create global reference for jobject");
            if (env->ExceptionCheck())
            {
                env->ExceptionDescribe();
                env->ExceptionClear();
            }
        }
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
    // 1. 使用 boost::dll 加载 jvm.dll (RAII 自动管理生命周期)
    try
    {
        jvm_dll_ =
            boost::dll::shared_library(boost::dll::fs::path(jvm_dll_path));
    }
    catch (const boost::system::system_error& e)
    {
        DAS_CORE_LOG_ERROR(
            "Failed to load JVM DLL: {} - {}",
            jvm_dll_path.string(),
            e.what());
        return nullptr;
    }

    // 2. 获取 JNI_CreateJavaVM 函数
    if (!jvm_dll_.has("JNI_CreateJavaVM"))
    {
        DAS_CORE_LOG_ERROR("Failed to find JNI_CreateJavaVM function");
        jvm_dll_ = {}; // 显式卸载
        return nullptr;
    }
    auto p_JNI_CreateJavaVM =
        jvm_dll_.get<jvm_dll::JNI_CreateJavaVM_t>("JNI_CreateJavaVM");
    if (!p_JNI_CreateJavaVM)
    {
        DAS_CORE_LOG_ERROR("Failed to find JNI_CreateJavaVM function");
        jvm_dll_ = {}; // 显式卸载
        return nullptr;
    }

    // 3. 构建 JVM 选项
    const auto class_path_str = BuildClassPathString(class_path);
    // 避免局部对象生命周期问题
    std::string               java_class_path;
    std::vector<JavaVMOption> options;

    // 添加类路径选项
    if (!class_path_str.empty())
    {
        java_class_path =
            DAS_FMT_NS::format("-Djava.class.path={}", class_path_str);
        options.push_back(
            {const_cast<char*>(java_class_path.c_str()), nullptr});
    }

    // 避免局部对象生命周期问题
    std::string java_library_path;
    // 添加 java.library.path 选项，指向 DasCore 所在目录
    // 这样可以找到 DasCoreJavaExport.dll
    {
        // 使用 boost::dll 获取当前模块（DasCore.dll）的路径
        boost::dll::fs::path module_path = boost::dll::this_line_location();
        if (!module_path.empty())
        {
            boost::dll::fs::path module_dir = module_path.parent_path();
            // 使用 generic_string() 获取正斜杠路径，避免 JVM
            // 把反斜杠解析为转义字符
            java_library_path = DAS_FMT_NS::format(
                "-Djava.library.path={}",
                module_dir.generic_string());
            options.push_back(
                {const_cast<char*>(java_library_path.c_str()), nullptr});
            DAS_CORE_LOG_INFO(
                "Setting java.library.path to: {}",
                module_dir.generic_string());
        }
    }

    // 添加用户选项
    for (const auto& opt : jvm_options)
    {
        options.push_back({const_cast<char*>(opt.c_str()), nullptr});
    }

    // 4. 初始化 JVM
    JavaVMInitArgs vm_args{};
    vm_args.version = JNI_VERSION_1_2;
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
        jvm_dll_ = {}; // 显式卸载
        return nullptr;
    }

    DAS_CORE_LOG_INFO(
        "JVM created successfully with class path: {}",
        class_path_str);

    // jvm_dll_ 作为成员变量，由 RAII 自动管理生命周期
    // 在进程结束前保持加载状态

    return jvm;
}

std::filesystem::path JvmManager::FindJvmDllPath()
{
    // 从环境变量 JAVA_HOME 查找
    char*  java_home = nullptr;
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
        result += path.generic_string(); // 使用正斜杠路径，避免 JVM 解析错误
    }
    return result;
}

// ============================================================================
// JavaRuntime 实现
// ============================================================================

JavaRuntime::JavaRuntime(const IDasJavaRuntimeDesc& desc) : jvm_(nullptr)
{
    // 获取 jvm.dll 路径
    std::filesystem::path jvm_dll_path = desc.GetJvmDllPath();
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
        desc.GetJvmOptions(),
        desc.GetClassPath());

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
        std::string                        entry_point;
        std::vector<std::filesystem::path> additional_jars;
        auto result = LoadPluginConfig(path, entry_point, additional_jars);
        if (DAS::IsFailed(result))
        {
            return tl::make_unexpected(result);
        }

        // 2. 解析 entryPoint:
        // "org.das.plugin.JavaTestPlugin.createInstance"
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

        if (!plugin_ptr)
        {
            DAS_CORE_LOG_ERROR(
                "Failed to extract IDasBase from DasRetBase for class: {}",
                class_path_str);
            return tl::make_unexpected(DAS_E_FAIL);
        }

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
    jclass ret_base_class = env->GetObjectClass(das_ret_base);
    if (!ret_base_class)
    {
        DAS_CORE_LOG_ERROR("Failed to get class of DasRetBase object");
        if (env->ExceptionCheck())
        {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
        return nullptr;
    }
    jmethodID get_value_method =
        env->GetMethodID(ret_base_class, "getValue", "()Lorg/das/IDasBase;");

    if (!get_value_method)
    {
        DAS_CORE_LOG_ERROR("Failed to find getValue() method in DasRetBase");
        if (env->ExceptionCheck())
        {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
        env->DeleteLocalRef(ret_base_class);
        return nullptr;
    }

    // 2. 调用 getValue() 获取 IDasBase
    jobject idas_base = env->CallObjectMethod(das_ret_base, get_value_method);
    if (env->ExceptionCheck())
    {
        DAS_CORE_LOG_ERROR(
            "Exception occurred while calling DasRetBase.getValue()");
        env->ExceptionDescribe();
        env->ExceptionClear();
        env->DeleteLocalRef(ret_base_class);
        return nullptr;
    }
    if (!idas_base)
    {
        DAS_CORE_LOG_ERROR("DasRetBase.getValue() returned null");
        env->DeleteLocalRef(ret_base_class);
        return nullptr;
    }

    // 3. 获取 swigCPtr 字段
    jclass idas_base_class = env->GetObjectClass(idas_base);
    if (!idas_base_class)
    {
        DAS_CORE_LOG_ERROR("Failed to get class of IDasBase object");
        if (env->ExceptionCheck())
        {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
        env->DeleteLocalRef(idas_base);
        env->DeleteLocalRef(ret_base_class);
        return nullptr;
    }
    jfieldID swig_cptr_field =
        env->GetFieldID(idas_base_class, "swigCPtr", "J");

    if (!swig_cptr_field)
    {
        DAS_CORE_LOG_ERROR("Failed to find swigCPtr field in IDasBase");
        if (env->ExceptionCheck())
        {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
        env->DeleteLocalRef(idas_base_class);
        env->DeleteLocalRef(idas_base);
        env->DeleteLocalRef(ret_base_class);
        return nullptr;
    }

    // 4. 读取 C++ 指针
    jlong c_ptr = env->GetLongField(idas_base, swig_cptr_field);
    if (env->ExceptionCheck())
    {
        DAS_CORE_LOG_ERROR("Exception occurred while reading swigCPtr field");
        env->ExceptionDescribe();
        env->ExceptionClear();
        env->DeleteLocalRef(idas_base_class);
        env->DeleteLocalRef(idas_base);
        env->DeleteLocalRef(ret_base_class);
        return nullptr;
    }

    // 5. 创建 C++ IDasBase 指针
    // swigCMemOwn=false 表示不拥有内存，Java 端负责释放
    auto* cpp_ptr = std::launder<IDasBase>(std::bit_cast<IDasBase*>(c_ptr));

    DasPtr<IDasBase> result;
    if (cpp_ptr)
    {
        result = decltype(result)::Attach(cpp_ptr);
        // 注意：Attach 不会增加引用计数
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
// 工厂函数
// ============================================================================

auto CreateJavaRuntime(const ForeignLanguageRuntimeFactoryDesc& desc)
    -> DAS::Utils::Expected<DasPtr<IForeignLanguageRuntime>>
{
    // 校验 language
    if (desc.language != ForeignInterfaceLanguage::Java)
    {
        DAS_CORE_LOG_ERROR(
            "CreateJavaRuntime: invalid language, expected Java");
        return tl::make_unexpected(DAS_E_INVALID_ARGUMENT);
    }

    // 从 p_user_data 转换为 IDasJavaRuntimeDesc*
    if (desc.p_user_data == nullptr)
    {
        DAS_CORE_LOG_ERROR("CreateJavaRuntime: p_user_data is null");
        return tl::make_unexpected(DAS_E_INVALID_ARGUMENT);
    }

    const auto* java_desc =
        static_cast<const IDasJavaRuntimeDesc*>(desc.p_user_data);

    try
    {
        const auto runtime = DAS::MakeDasPtr<JavaRuntime>(*java_desc);
        if (!runtime->IsInitialized())
        {
            return tl::make_unexpected(DAS_E_OBJECT_NOT_INIT);
        }
        return runtime;
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_ERROR("Exception in CreateJavaRuntime: {}", e.what());
        return tl::make_unexpected(DAS_E_FAIL);
    }
}

DAS_NS_JAVAHOST_END

// ============================================================================
// IDasJavaRuntimeDesc 工厂函数实现
// ============================================================================

IDasJavaRuntimeDesc* CreateJavaRuntimeDesc()
{
    return new JavaHost::JavaRuntimeDesc();
}

void DestroyJavaRuntimeDesc(IDasJavaRuntimeDesc* desc)
{
    delete static_cast<JavaHost::JavaRuntimeDesc*>(desc);
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_EXPORT_JAVA

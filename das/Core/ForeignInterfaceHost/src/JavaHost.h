#ifndef DAS_CORE_FOREIGNINTERFACEHOST_JAVAHOST_H
#define DAS_CORE_FOREIGNINTERFACEHOST_JAVAHOST_H

#ifdef DAS_EXPORT_JAVA
#include <jni.h>

#include <atomic>
#include <boost/dll.hpp>
#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Utils/Expected.h>
#include <filesystem>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define DAS_NS_JAVAHOST_BEGIN                                                  \
    namespace JavaHost                                                         \
    {
#define DAS_NS_JAVAHOST_END }

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DAS_NS_JAVAHOST_BEGIN

/**
 * JNI 环境守卫 RAII
 *
 * 自动处理 AttachCurrentThread/DetachCurrentThread
 * JNIEnv 不可跨线程传递，每个线程需要通过此类获取
 */
class JniEnvGuard
{
public:
    explicit JniEnvGuard(JavaVM* vm);
    ~JniEnvGuard();

    // 禁止拷贝
    JniEnvGuard(const JniEnvGuard&) = delete;
    JniEnvGuard& operator=(const JniEnvGuard&) = delete;

    // 允许移动
    JniEnvGuard(JniEnvGuard&& other) noexcept;
    JniEnvGuard& operator=(JniEnvGuard&& other) noexcept;

    JNIEnv*  get() const { return env_; }
    bool     is_valid() const { return env_ != nullptr; }
    explicit operator bool() const { return is_valid(); }

private:
    JavaVM* vm_;
    JNIEnv* env_;
    bool    attached_;
};

/**
 * Java 全局引用 RAII
 *
 * 管理 jobject 的全局引用生命周期
 * 防止 GC 回收 Java 对象
 */
class JObjectGlobalRef
{
public:
    JObjectGlobalRef(JNIEnv* env, jobject obj);
    ~JObjectGlobalRef();

    // 禁止拷贝
    JObjectGlobalRef(const JObjectGlobalRef&) = delete;
    JObjectGlobalRef& operator=(const JObjectGlobalRef&) = delete;

    // 允许移动
    JObjectGlobalRef(JObjectGlobalRef&& other) noexcept;
    JObjectGlobalRef& operator=(JObjectGlobalRef&& other) noexcept;

    jobject  get() const { return ref_; }
    explicit operator bool() const { return ref_ != nullptr; }

private:
    JavaVM* vm_;
    jobject ref_;
};

/**
 * JVM 单例管理器
 *
 * 规则:
 * 1. JavaVM* 可跨线程共享（全局缓存）
 * 2. JNIEnv* 不可跨线程传递（每个线程 Attach 获取）
 * 3. 一个进程只能有一个 JVM 实例
 * 4. JVM 永不销毁（进程生命周期）
 */
class JvmManager
{
public:
    static JvmManager& GetInstance();

    /**
     * 获取或创建 JVM
     * 如果 JVM 已存在，直接返回；否则创建新的
     *
     * @param jvm_dll_path jvm.dll 路径
     * @param jvm_options JVM 启动选项
     * @param class_path Java 类路径（JAR 文件列表）
     * @return JavaVM* 或 nullptr（失败时）
     */
    JavaVM* GetOrCreateVM(
        const std::filesystem::path&              jvm_dll_path,
        const std::vector<std::string>&           jvm_options,
        const std::vector<std::filesystem::path>& class_path);

    /**
     * 获取 JNIEnv（当前线程）
     * 自动处理 Attach/Detach
     */
    std::unique_ptr<JniEnvGuard> GetEnv();

    /**
     * 检查 JVM 是否已初始化
     */
    bool IsInitialized() const { return jvm_ != nullptr; }

    /**
     * 查找 jvm.dll 路径
     */
    static std::filesystem::path FindJvmDllPath();

private:
    JvmManager() = default;
    ~JvmManager() = default;

    JvmManager(const JvmManager&) = delete;
    JvmManager& operator=(const JvmManager&) = delete;

    /**
     * 创建 JVM 实例
     */
    JavaVM* CreateJVM(
        const std::filesystem::path&              jvm_dll_path,
        const std::vector<std::string>&           jvm_options,
        const std::vector<std::filesystem::path>& class_path);

    /**
     * 构建类路径字符串
     */
    static std::string BuildClassPathString(
        const std::vector<std::filesystem::path>& class_path);

private:
    JavaVM*                    jvm_ = nullptr;
    boost::dll::shared_library jvm_dll_;
    std::mutex                 mutex_;
};

/**
 * Java 运行时实现
 */
class JavaRuntime final : public IForeignLanguageRuntime
{
public:
    explicit JavaRuntime(const IDasJavaRuntimeDesc& desc);
    ~JavaRuntime();

    // IForeignLanguageRuntime 实现
    auto LoadPlugin(const std::filesystem::path& path)
        -> DAS::Utils::Expected<DasPtr<IDasBase>> override;

    // IDasBase 实现
    uint32_t  AddRef() override { return ++ref_count_; }
    uint32_t  Release() override;
    DasResult QueryInterface(const DasGuid& iid, void** pp_object) override;

    /**
     * 检查 JavaRuntime 是否已初始化
     */
    bool IsInitialized() const { return jvm_ != nullptr; }

private:
    /**
     * 解析 entryPoint 字符串
     * @param entry_point 格式: "org.example.MyPlugin.create"
     * @return pair<java_class_path, method_name>
     *         例如: pair<"org/example/MyPlugin", "create">
     */
    static std::pair<std::string, std::string> ParseEntryPoint(
        const std::string& entry_point);

    /**
     * 从 DasRetBase 提取 IDasBase
     */
    DasPtr<IDasBase> ExtractIDasBaseFromDasRetBase(
        JNIEnv* env,
        jobject das_ret_base);

    /**
     * 加载插件 JSON 配置
     */
    DasResult LoadPluginConfig(
        const std::filesystem::path&        json_path,
        std::string&                        out_entry_point,
        std::vector<std::filesystem::path>& out_additional_jars);

private:
    JavaVM*               jvm_; // 不拥有，由 JvmManager 单例管理
    std::atomic<uint32_t> ref_count_{1};
};

/**
 * Java 运行时描述实现类
 */
class JavaRuntimeDesc final : public IDasJavaRuntimeDesc
{
public:
    // IDasJavaRuntimeDesc 接口实现 - Getters
    [[nodiscard]]
    auto GetJvmDllPath() const -> std::filesystem::path override
    {
        return jvm_dll_path_;
    }
    [[nodiscard]]
    auto GetClassPath() const -> std::vector<std::filesystem::path> override
    {
        return class_path_;
    }
    [[nodiscard]]
    auto GetJvmOptions() const -> std::vector<std::string> override
    {
        return jvm_options_;
    }

    // IDasJavaRuntimeDesc 接口实现 - Setters
    void SetJvmDllPath(const std::filesystem::path& path) override
    {
        jvm_dll_path_ = path;
    }
    void SetClassPath(const std::vector<std::filesystem::path>& paths) override
    {
        class_path_ = paths;
    }
    void SetJvmOptions(const std::vector<std::string>& options) override
    {
        jvm_options_ = options;
    }

private:
    std::filesystem::path              jvm_dll_path_;
    std::vector<std::filesystem::path> class_path_;
    std::vector<std::string>           jvm_options_;
};

/**
 * 工厂函数：创建 Java 运行时
 */
auto CreateJavaRuntime(const ForeignLanguageRuntimeFactoryDesc& desc)
    -> DAS::Utils::Expected<DasPtr<IForeignLanguageRuntime>>;
DAS_NS_JAVAHOST_END

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_EXPORT_JAVA

#endif // DAS_CORE_FOREIGNINTERFACEHOST_JAVAHOST_H

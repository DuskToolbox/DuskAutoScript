#ifndef DAS_CORE_FOREIGNINTERFACEHOST_IFOREIGNLANGUAGERUNTIME_H
#define DAS_CORE_FOREIGNINTERFACEHOST_IFOREIGNLANGUAGERUNTIME_H

#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHostEnum.h>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
#include <das/Utils/Expected.h>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>

#include <filesystem>
#include <memory>
#include <vector>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

struct IDasJavaRuntimeDesc;

/**
 * @brief 创建 Java runtime 配置对象
 * @return IDasJavaRuntimeDesc* 配置指针，失败返回 nullptr
 */
DAS_API IDasJavaRuntimeDesc* CreateJavaRuntimeDesc();

/**
 * @brief 销毁 Java runtime 配置对象
 * @param desc 要销毁的配置指针
 */
DAS_API void DestroyJavaRuntimeDesc(IDasJavaRuntimeDesc* desc);

/**
 * @brief Java runtime 配置接口（纯虚接口）
 */
struct IDasJavaRuntimeDesc
{
    virtual ~IDasJavaRuntimeDesc() = default;

    // Getters
    [[nodiscard]]
    virtual auto GetJvmDllPath() const -> std::filesystem::path = 0;
    [[nodiscard]]
    virtual auto GetClassPath() const -> std::vector<std::filesystem::path> = 0;
    [[nodiscard]]
    virtual auto GetJvmOptions() const -> std::vector<std::string> = 0;

    // Setters
    virtual void SetJvmDllPath(const std::filesystem::path& path) = 0;
    virtual void SetClassPath(
        const std::vector<std::filesystem::path>& paths) = 0;
    virtual void SetJvmOptions(const std::vector<std::string>& options) = 0;
};

/**
 * @brief IDasJavaRuntimeDesc 的删除器
 */
struct JavaRuntimeDescDeleter
{
    void operator()(IDasJavaRuntimeDesc* desc) const
    {
        if (desc)
        {
            DestroyJavaRuntimeDesc(desc);
        }
    }
};

using JavaRuntimeDescPtr =
    std::unique_ptr<IDasJavaRuntimeDesc, JavaRuntimeDescDeleter>;

/**
 * @brief 创建语言runtime必须的内容，一次性给够全部语言runtime创建的信息
 */
struct ForeignLanguageRuntimeFactoryDesc
{
    ForeignInterfaceLanguage language;
    void* p_user_data = nullptr; // 语言特定配置（如 IDasJavaRuntimeDesc*）
};

/**
 * @brief 其它语言运行时接口
 */
DAS_INTERFACE IForeignLanguageRuntime : public IDasBase
{
    virtual auto LoadPlugin(const std::filesystem::path& path)
        -> DAS::Utils::Expected<DasPtr<IDasBase>> = 0;
};

/**
 * @brief 创建其它语言运行时
 * @param desc_base 运行时描述
 * @return DAS_E_NO_IMPLEMENTATION 意味着对应语言的接口未实现
 */
DAS_API auto CreateForeignLanguageRuntime(
    const ForeignLanguageRuntimeFactoryDesc& desc_base)
    -> DAS::Utils::Expected<DasPtr<IForeignLanguageRuntime>>;

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_IFOREIGNLANGUAGERUNTIME_H

#ifndef DAS_API_H
#define DAS_API_H

#include "DasLogger.h"
#include "IDasImage.h"
#include <das/DasString.hpp>
#include <das/IDasBase.h>
#include <limits>

namespace Das::PluginInterface
{
    DAS_INTERFACE IDasPluginPackage;
} // namespace Das::PluginInterface

namespace Das::ExportInterface
{
    struct IDasSourceLocation;
    struct IDasJson;
    struct IDasGuidVector;
    struct IDasPluginManager;
    struct IDasMemory;
    struct IDasImage;
    struct IDasBasicErrorLens;
    struct IDasLogRequester;
    struct IDasReadOnlyGuidVector;
    struct IDasInitializeIDasPluginManagerCallback;
    struct IDasInitializeIDasPluginManagerWaiter;
    struct IDasVariantVector;
    struct IDasStringVector;
} // namespace Das::ExportInterface

DAS_INTERFACE IDasSettingsService;
DAS_INTERFACE IDasPluginManagerService;
struct IDasSchedulerService;

#ifndef SWIG

namespace Das::PluginInterface
{
    struct IDasPluginPackage;
}

struct IDasTypeInfo;

struct DasImageDesc
{
    /**
     * @brief Pointer to the image data pointer.
     *
     */
    char* p_data;
    /**
     * @brief Size of image data in bytes. Can be 0 when both width and height
     * are set and data is decoded.
     *
     */
    size_t data_size;
    /**
     * @brief Supported image format. @see DasImageFormat
     *
     */
    Das::ExportInterface::DasImageFormat data_format;
};

DAS_C_API DasResult ParseDasJsonFromString(
    const char*                      p_u8_string,
    Das::ExportInterface::IDasJson** pp_out_json);

DAS_C_API DasResult
CreateEmptyDasJson(Das::ExportInterface::IDasJson** pp_out_json);

DAS_C_API DasResult CreateIDasVariantVector(
    Das::ExportInterface::IDasVariantVector** pp_out_vector);

DAS_C_API void DasLogInfoU8(const char* p_string);

DAS_C_API void DasLogWarningU8(const char* p_string);

DAS_C_API void DasLogErrorU8(const char* p_string);

DAS_C_API void DasLogDebugU8(const char* p_string);

DAS_C_API void DasLogDebugU8WithSourceLocation(
    const char* p_string,
    DAS::ExportInterface::IDasSourceLocation*);

DAS_C_API void DasLogInfoU8WithSourceLocation(
    const char* p_string,
    DAS::ExportInterface::IDasSourceLocation*);

DAS_C_API void DasLogWarningU8WithSourceLocation(
    const char* p_string,
    DAS::ExportInterface::IDasSourceLocation*);

DAS_C_API void DasLogErrorU8WithSourceLocation(
    const char* p_string,
    DAS::ExportInterface::IDasSourceLocation*);

DAS_C_API DasResult DasGetErrorMessage(
    IDasTypeInfo*        p_error_generator,
    DasResult            error_code,
    IDasReadOnlyString** pp_out_error_message);

DAS_C_API DasResult DasGetPredefinedErrorMessage(
    DasResult            error_code,
    IDasReadOnlyString** pp_out_error_message);

DAS_C_API DasResult DasSetDefaultLocale(IDasReadOnlyString* locale_name);

DAS_C_API DasResult DasGetDefaultLocale(IDasReadOnlyString** locale_name);

DAS_C_API DasResult CreateIDasGuidVector(
    const DasGuid*                         p_data,
    size_t                                 size,
    Das::ExportInterface::IDasGuidVector** pp_out_guid);

DAS_C_API DasResult
CreateIDasStringVector(Das::ExportInterface::IDasStringVector** pp_out_vector);

DAS_C_API DasResult InitializeIDasPluginManager(
    Das::ExportInterface::IDasReadOnlyGuidVector* p_ignore_plugins_guid,
    Das::ExportInterface::IDasInitializeIDasPluginManagerCallback*
        p_on_finished,
    Das::ExportInterface::IDasInitializeIDasPluginManagerWaiter**
        pp_out_waiter);

DAS_C_API DasResult CreateIDasMemory(
    size_t                             size_in_byte,
    Das::ExportInterface::IDasMemory** pp_out_memory);

DAS_C_API DasResult CreateIDasImageFromEncodedData(
    DasImageDesc*                     p_desc,
    Das::ExportInterface::IDasImage** pp_out_image);

DAS_C_API DasResult CreateIDasImageFromDecodedData(
    const DasImageDesc*                  p_desc,
    const DAS::ExportInterface::DasSize* p_size,
    Das::ExportInterface::IDasImage**    pp_out_image);

DAS_C_API DasResult CreateIDasImageFromRgb888(
    Das::ExportInterface::IDasMemory*    p_alias_memory,
    const DAS::ExportInterface::DasSize* p_size,
    Das::ExportInterface::IDasImage**    pp_out_image);

DAS_C_API DasResult DasPluginLoadImageFromResource(
    IDasTypeInfo*                     p_type_info,
    IDasReadOnlyString*               p_relative_path,
    Das::ExportInterface::IDasImage** pp_out_image);

DAS_C_API DasResult CreateIDasBasicErrorLens(
    Das::ExportInterface::IDasBasicErrorLens** pp_out_error_lens);

DAS_C_API DasResult CreateIDasLogRequester(
    uint32_t                                 max_line_count,
    Das::ExportInterface::IDasLogRequester** pp_out_requester);

//=============================================================================
// IPC 超时配置 API
//=============================================================================

/**
 * @brief 设置当前 IPC 调用的超时时间
 *
 * 此设置是线程局部的，仅影响当前线程的下一次 IPC 调用。
 * 调用后超时值会被自动清零（恢复为无限超时）。
 *
 * @param timeout_ms 超时时间（毫秒），0 表示无限超时
 * @return DasResult 成功返回 DAS_S_OK
 */
DAS_C_API DasResult DasSetIpcTimeout(uint32_t timeout_ms);

/**
 * @brief 获取当前 IPC 调用的超时时间
 *
 * @param p_out_timeout_ms 输出参数，接收超时值（毫秒）
 * @return DasResult 成功返回 DAS_S_OK
 */
DAS_C_API DasResult DasGetIpcTimeout(uint32_t* p_out_timeout_ms);

/**
 * @brief 查询主进程全局服务接口
 *
 * 通过 IID 获取主进程中注册的全局服务对象。
 * 插件可使用此接口从 Host 进程访问主进程提供的服务。
 *
 * @param iid 目标接口的 GUID
 * @param pp_out_object [out] 接收接口指针（调用者必须 Release）
 * @return DasResult DAS_S_OK 成功，DAS_E_OBJECT_NOT_INIT 未绑定 IPC 上下文
 */
DAS_C_API DasResult
DasQueryMainProcessInterface(const DasGuid& iid, IDasBase** pp_out_object);

/**
 * @brief 注册一个服务对象到主进程全局服务表
 * @param p_object 服务对象指针（调用后由框架 AddRef，调用者仍持有引用）
 * @param iid 对象实现的接口 IID（作为唯一键，一个 GUID 只能注册一个实例）
 * @return DAS_S_OK 成功
 *         DAS_E_INVALID_POINTER p_object 为 nullptr
 *         DAS_E_OBJECT_NOT_INIT 当前线程无有效 IpcContext（与
 *             DasQueryMainProcessInterface 对齐）
 *         DAS_E_NO_IMPLEMENTATION Host 进程不支持注册
 *         DAS_E_DUPLICATE_ELEMENT 相同 IID 的服务已存在
 */
DAS_C_API DasResult
DasRegisterMainProcessService(IDasBase* p_object, const DasGuid& iid);

/**
 * @brief 从主进程全局服务表注销一个服务对象
 * @param iid 要注销的服务 IID（注册时使用的 IID）
 * @return DAS_S_OK 成功
 *         DAS_E_OBJECT_NOT_INIT 当前线程无有效 IpcContext（与
 *             DasQueryMainProcessInterface 对齐）
 *         DAS_E_NO_IMPLEMENTATION Host 进程不支持注销
 *         DAS_E_IPC_OBJECT_NOT_FOUND 指定 IID 的服务不存在
 */
DAS_C_API DasResult DasUnregisterMainProcessService(const DasGuid& iid);

// Log level constants (match spdlog::level::level_enum values)
#define DAS_LOG_LEVEL_TRACE 0
#define DAS_LOG_LEVEL_DEBUG 1
#define DAS_LOG_LEVEL_INFO 2
#define DAS_LOG_LEVEL_WARN 3
#define DAS_LOG_LEVEL_ERROR 4
#define DAS_LOG_LEVEL_CRITICAL 5
#define DAS_LOG_LEVEL_OFF 6

/**
 * @brief Set the global log level for the DAS logging system.
 * @param level One of DAS_LOG_LEVEL_TRACE/DEBUG/INFO/WARN/ERROR/CRITICAL/OFF
 */
DAS_C_API void DasSetLogLevel(int level);

//=============================================================================
// Settings/PluginManager service factory functions
//=============================================================================

namespace Das::Core::SettingsManager
{
    class SettingsManager;
}
namespace Das::Core::ForeignInterfaceHost
{
    class PluginManager;
}
namespace Das::Core::TaskScheduler
{
    class SchedulerService;
}

/**
 * @brief 创建 IDasSettingsService 实例
 * @param mgr SettingsManager 具体类引用
 * @param pp_out 输出接口指针（调用者必须 Release）
 * @return DAS_S_OK 成功
 */
DAS_C_API DasResult CreateDasSettingsService(
    Das::Core::SettingsManager::SettingsManager& mgr,
    IDasSettingsService**                        pp_out);

/**
 * @brief 创建 IDasPluginManagerService 实例
 * @param mgr PluginManager 具体类引用
 * @param pp_out 输出接口指针（调用者必须 Release）
 * @return DAS_S_OK 成功
 */
DAS_C_API DasResult CreateDasPluginManagerService(
    Das::Core::ForeignInterfaceHost::PluginManager& mgr,
    IDasPluginManagerService**                      pp_out);

/**
 * @brief 创建 IDasSchedulerService 实例
 * @param mgr SchedulerService 具体类引用
 * @param pp_out 输出接口指针（调用者必须 Release）
 * @return DAS_S_OK 成功
 */
DAS_C_API DasResult CreateDasSchedulerService(
    Das::Core::TaskScheduler::SchedulerService& mgr,
    IDasSchedulerService**                      pp_out);

#define DAS_LOG_ERROR(...) DAS_LOG_WITH_SOURCE_LOCATION(Error, __VA_ARGS__)
#define DAS_LOG_WARNING(...) DAS_LOG_WITH_SOURCE_LOCATION(Warning, __VA_ARGS__)
#define DAS_LOG_INFO(...) DAS_LOG_WITH_SOURCE_LOCATION(Info, __VA_ARGS__)
#define DAS_LOG_DEBUG(...) DAS_LOG_WITH_SOURCE_LOCATION(Debug, __VA_ARGS__)

struct DasU8StringOnStack final : IDasReadOnlyString
{
private:
    const char* u8string_;

public:
    DasU8StringOnStack(const char* str) : u8string_{str} {}
    uint32_t DAS_STD_CALL AddRef() override
    {
        return std::numeric_limits<uint32_t>::max();
    }
    uint32_t DAS_STD_CALL Release() override
    {
        return std::numeric_limits<uint32_t>::max();
    }
    DasResult DAS_STD_CALL
    QueryInterface(const DasGuid& iid, void** pp_out_object) override
    {
        if (pp_out_object == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }

        if (iid == DasIidOf<IDasBase>())
        {
            *pp_out_object = static_cast<IDasBase*>(this);
            AddRef();
            return DAS_S_OK;
        }
        else if (iid == DasIidOf<IDasReadOnlyString>())
        {
            *pp_out_object = static_cast<IDasReadOnlyString*>(this);
            AddRef();
            return DAS_S_OK;
        }

        *pp_out_object = nullptr;
        return DAS_E_NO_INTERFACE;
    }
    DasResult GetUtf8(const char** out_string) override
    {
        *out_string = u8string_;
        return DAS_S_OK;
    }
    DasResult GetUtf16(
        const char16_t** out_string,
        size_t*          out_string_size) noexcept override
    {
        (void)out_string;
        (void)out_string_size;
        return DAS_E_NO_IMPLEMENTATION;
    }
    DasResult GetW(const wchar_t**) override { return DAS_E_NO_IMPLEMENTATION; }
    const int32_t* CBegin() override { return nullptr; }
    const int32_t* CEnd() override { return nullptr; }
};

struct DasSourceLocationOnStack final : DAS::ExportInterface::IDasSourceLocation
{
    DasSourceLocationOnStack(
        const char* file,
        int32_t     line,
        const char* function)
        : file_name{file}, line{line}, function_name{function}
    {
    }
    uint32_t DAS_STD_CALL AddRef() override
    {
        return std::numeric_limits<uint32_t>::max();
    }
    uint32_t DAS_STD_CALL Release() override
    {
        return std::numeric_limits<uint32_t>::max();
    }
    DasResult DAS_STD_CALL
    QueryInterface(const DasGuid& iid, void** pp_out_object) override
    {
        if (pp_out_object == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }

        if (iid == DasIidOf<IDasBase>())
        {
            *pp_out_object = static_cast<IDasBase*>(this);
            AddRef();
            return DAS_S_OK;
        }
        else if (iid == DasIidOf<IDasSourceLocation>())
        {
            *pp_out_object = static_cast<IDasSourceLocation*>(this);
            AddRef();
            return DAS_S_OK;
        }

        *pp_out_object = nullptr;
        return DAS_E_NO_INTERFACE;
    }

    DasResult SetFileName(IDasReadOnlyString*) override
    {
        return DAS_E_NO_IMPLEMENTATION;
    }
    DasResult GetFileName(IDasReadOnlyString** pp_out) override
    {
        *pp_out = &file_name;
        return DAS_S_OK;
    }
    DasResult SetFunctionName(IDasReadOnlyString*) override
    {
        return DAS_E_NO_IMPLEMENTATION;
    }
    DasResult GetFunctionName(IDasReadOnlyString** pp_out) override
    {
        *pp_out = &function_name;
        return DAS_S_OK;
    }
    DasResult GetLine(int32_t* p_out) override
    {
        *p_out = line;
        return DAS_S_OK;
    }
    DasResult SetLine(int32_t) override { return DAS_E_NO_IMPLEMENTATION; }

    DasU8StringOnStack file_name;
    int                line;
    DasU8StringOnStack function_name;
};

#define DAS_LOG_WITH_SOURCE_LOCATION(type, ...)                                \
    do                                                                         \
    {                                                                          \
        DasSourceLocationOnStack _das_internal_source_location = {             \
            __FILE__,                                                          \
            __LINE__,                                                          \
            __func__};                                                         \
        DasLog##type##U8WithSourceLocation(                                    \
            __VA_ARGS__,                                                       \
            &_das_internal_source_location);                                   \
    } while (false)

typedef DasResult(DAS_STD_CALL DasCoCreatePluginFunction)(IDasBase**);

constexpr const char* DAS_COCREATE_PLUGIN_NAME = "DasCoCreatePlugin";

#endif // SWIG

#endif // DAS_API_H

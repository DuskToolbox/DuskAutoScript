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
    struct IDasTaskManager;
    struct IDasPluginManager;
    struct IDasMemory;
    struct IDasTaskScheduler;
    struct IDasImage;
    struct IDasBasicErrorLens;
    struct IDasLogRequester;
    struct IDasReadOnlyGuidVector;
    struct IDasInitializeIDasPluginManagerCallback;
    struct IDasInitializeIDasPluginManagerWaiter;
    struct IDasJsonSetting;
}

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

DAS_API void DasLogError(DasReadOnlyString das_string);

DAS_API void DasLogWarning(DasReadOnlyString das_string);

DAS_API void DasLogInfo(DasReadOnlyString das_string);

DAS_C_API void DasLogInfoU8(const char* p_string);

DAS_C_API void DasLogWarningU8(const char* p_string);

DAS_C_API void DasLogErrorU8(const char* p_string);

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

DAS_C_API DasResult CreateIDasTaskManager(
    IDasReadOnlyString*                     p_connection_json,
    Das::ExportInterface::IDasTaskManager** ppIDasTaskManager);

DAS_C_API DasResult InitializeIDasPluginManager(
    Das::ExportInterface::IDasReadOnlyGuidVector* p_ignore_plugins_guid,
    Das::ExportInterface::IDasInitializeIDasPluginManagerCallback*
        p_on_finished,
    Das::ExportInterface::IDasInitializeIDasPluginManagerWaiter**
        pp_out_waiter);

DAS_C_API DasResult GetExistingIDasPluginManager(
    Das::ExportInterface::IDasPluginManager** pp_out_result);

DAS_C_API DasResult CreateIDasMemory(
    size_t                             size_in_byte,
    Das::ExportInterface::IDasMemory** pp_out_memory);

DAS_C_API DasResult GetIDasTaskScheduler(
    Das::ExportInterface::IDasTaskScheduler** pp_out_task_scheduler);

DAS_C_API DasResult SetIDasTaskSchedulerJsonState(
    Das::ExportInterface::IDasJsonSetting* p_scheduler_state);

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

using DasCoCreatePluginFunction = DasResult (*)(
    Das::PluginInterface::IDasPluginPackage** pp_out_plugin_package);

#define DAS_LOG_ERROR(...) DAS_LOG_WITH_SOURCE_LOCATION(Error, __VA_ARGS__)
#define DAS_LOG_WARNING(...) DAS_LOG_WITH_SOURCE_LOCATION(Warning, __VA_ARGS__)
#define DAS_LOG_INFO(...) DAS_LOG_WITH_SOURCE_LOCATION(Info, __VA_ARGS__)

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

typedef DasResult(DAS_STD_CALL *DasCoCreatePluginFunc)(IDasBase**);

#endif // DAS_API_H

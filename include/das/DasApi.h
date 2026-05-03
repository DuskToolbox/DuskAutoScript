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
DAS_INTERFACE IDasCoreServices;

#ifndef SWIG

namespace Das::PluginInterface
{
    struct IDasPluginPackage;
}

struct IDasTypeInfo;

namespace Das::Core::IPC::MainProcess
{
    struct IIpcContext;
}
using Das::Core::IPC::MainProcess::IIpcContext;

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

#include "DasCoreApi.generated.h"

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

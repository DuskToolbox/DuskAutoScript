#ifndef ASR_VARIANT_H
#define ASR_VARIANT_H

#include <AutoStarRail/AsrString.hpp>
#include <AutoStarRail/IAsrBase.h>
#include <AutoStarRail/IAsrTypeInfo.h>

ASR_INTERFACE IAsrComponent;
ASR_INTERFACE IAsrSwigComponent;

struct AsrRetComponent;

typedef enum AsrVariantType
{
    ASR_VARIANT_TYPE_INT = 0,
    ASR_VARIANT_TYPE_FLOAT,
    ASR_VARIANT_TYPE_STRING,
    ASR_VARIANT_TYPE_BOOL,
    ASR_VARIANT_TYPE_BASE,
    ASR_VARIANT_TYPE_COMPONENT,
    ASR_VARIANT_TYPE_FORCE_DWORD = 0x7FFFFFFF
} AsrVariantType;

ASR_DEFINE_RET_TYPE(AsrRetVariantType, AsrVariantType);

// {AEA97E84-4FFC-4E9D-B627-AA8A590AE444}
ASR_DEFINE_GUID(
    ASR_IID_VARIANT_VECTOR,
    IAsrVariantVector,
    0xaea97e84,
    0x4ffc,
    0x4e9d,
    0xb6,
    0x27,
    0xaa,
    0x8a,
    0x59,
    0xa,
    0xe4,
    0x44);
SWIG_IGNORE(IAsrVariantVector)
ASR_INTERFACE IAsrVariantVector : public IAsrBase
{
    ASR_METHOD GetInt(size_t index, int64_t * p_out_int) = 0;
    ASR_METHOD GetFloat(size_t index, float* p_out_float) = 0;
    ASR_METHOD GetString(size_t index, IAsrReadOnlyString * *pp_out_string) = 0;
    ASR_METHOD GetBool(size_t index, bool* p_out_bool) = 0;
    /**
     * @brief
     * 如果访问的值恰好是IAsrBase或IAsrSwigBase，则内部会尝试转换到IAsrComponent
     * @param index 索引
     * @param pp_out_component 返回值
     * @return 错误码
     */
    ASR_METHOD GetComponent(size_t index, IAsrComponent * *pp_out_component) =
        0;
    ASR_METHOD GetBase(size_t index, IAsrBase * *pp_out_base) = 0;

    ASR_METHOD SetInt(size_t index, int64_t in_int) = 0;
    ASR_METHOD SetFloat(size_t index, float in_float) = 0;
    ASR_METHOD SetString(size_t index, IAsrReadOnlyString * in_string) = 0;
    ASR_METHOD SetBool(size_t index, bool in_bool) = 0;
    ASR_METHOD SetComponent(size_t index, IAsrComponent * in_component) = 0;
    ASR_METHOD SetBase(size_t index, IAsrBase * in_base) = 0;

    ASR_METHOD PushBackInt(int64_t in_int) = 0;
    ASR_METHOD PushBackFloat(float in_float) = 0;
    ASR_METHOD PushBackString(IAsrReadOnlyString * in_string) = 0;
    ASR_METHOD PushBackBool(bool in_bool) = 0;
    ASR_METHOD PushBackComponent(IAsrComponent * in_component) = 0;
    ASR_METHOD PushBackBase(IAsrBase * in_base) = 0;

    ASR_METHOD GetType(size_t index, AsrVariantType * p_out_type) = 0;

    ASR_METHOD RemoveAt(size_t index) = 0;

    /**
     * @brief 此函数一定成功，因此返回值即为大小
     * @return 参数数组大小
     */
    ASR_METHOD GetSize() = 0;
};

// {AA167C84-DE92-4893-B39C-21FFF9DBC544}
ASR_DEFINE_GUID(
    ASR_IID_SWIG_VARIANT_VECTOR,
    IAsrSwigVariantVector,
    0xaa167c84,
    0xde92,
    0x4893,
    0xb3,
    0x9c,
    0x21,
    0xff,
    0xf9,
    0xdb,
    0xc5,
    0x44);
ASR_SWIG_EXPORT_ATTRIBUTE(IAsrSwigVariantVector)
ASR_INTERFACE IAsrSwigVariantVector : public IAsrSwigBase
{
    virtual AsrRetInt            GetInt(size_t index) = 0;
    virtual AsrRetFloat          GetFloat(size_t index) = 0;
    virtual AsrRetReadOnlyString GetString(size_t index) = 0;
    virtual AsrRetBool           GetBool(size_t index) = 0;
    virtual AsrRetComponent      GetComponent(size_t index) = 0;
    virtual AsrRetSwigBase       GetBase(size_t index) = 0;

    virtual AsrResult SetInt(size_t index, int64_t in_int) = 0;
    virtual AsrResult SetFloat(size_t index, float in_float) = 0;
    virtual AsrResult SetString(size_t index, AsrReadOnlyString in_string) = 0;
    virtual AsrResult SetBool(size_t index, bool in_bool) = 0;
    virtual AsrResult SetComponent(
        size_t index,
        IAsrSwigComponent * in_component) = 0;
    virtual AsrResult SetBase(size_t index, IAsrSwigBase * in_base) = 0;

    virtual AsrResult PushBackInt(int64_t in_int) = 0;
    virtual AsrResult PushBackFloat(float in_float) = 0;
    virtual AsrResult PushBackString(AsrReadOnlyString in_string) = 0;
    virtual AsrResult PushBackBool(bool in_bool) = 0;
    virtual AsrResult PushBackComponent(IAsrSwigComponent * in_component) = 0;
    virtual AsrResult PushBackBase(IAsrSwigBase * in_base) = 0;

    virtual AsrRetVariantType GetType(size_t index) = 0;

    virtual AsrResult RemoveAt(size_t index) = 0;

    /**
     * @brief 此函数一定成功，因此返回值即为大小
     * @return 参数数组大小
     */
    virtual AsrResult GetSize() = 0;
};

ASR_DEFINE_RET_POINTER(AsrRetVariantVector, IAsrSwigVariantVector);

#include <AutoStarRail/PluginInterface/IAsrComponent.h>

#endif // ASR_VARIANT_H


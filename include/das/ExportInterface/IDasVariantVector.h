#ifndef DAS_VARIANT_H
#define DAS_VARIANT_H

#include <das/DasString.hpp>
#include <das/IDasBase.h>
#include <das/IDasTypeInfo.h>

DAS_INTERFACE IDasComponent;
DAS_INTERFACE IDasSwigComponent;

struct DasRetComponent;

typedef enum DasVariantType
{
    DAS_VARIANT_TYPE_INT = 0,
    DAS_VARIANT_TYPE_FLOAT,
    DAS_VARIANT_TYPE_STRING,
    DAS_VARIANT_TYPE_BOOL,
    DAS_VARIANT_TYPE_BASE,
    DAS_VARIANT_TYPE_COMPONENT,
    DAS_VARIANT_TYPE_FORCE_DWORD = 0x7FFFFFFF
} DasVariantType;

DAS_DEFINE_RET_TYPE(DasRetVariantType, DasVariantType);

// {AEA97E84-4FFC-4E9D-B627-AA8A590AE444}
DAS_DEFINE_GUID(
    DAS_IID_VARIANT_VECTOR,
    IDasVariantVector,
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
SWIG_IGNORE(IDasVariantVector)
DAS_INTERFACE IDasVariantVector : public IDasBase
{
    DAS_METHOD GetInt(size_t index, int64_t * p_out_int) = 0;
    DAS_METHOD GetFloat(size_t index, float* p_out_float) = 0;
    DAS_METHOD GetString(size_t index, IDasReadOnlyString * *pp_out_string) = 0;
    DAS_METHOD GetBool(size_t index, bool* p_out_bool) = 0;
    /**
     * @brief
     * 如果访问的值恰好是IDasBase或IDasSwigBase，则内部会尝试转换到IDasComponent
     * @param index 索引
     * @param pp_out_component 返回值
     * @return 错误码
     */
    DAS_METHOD GetComponent(size_t index, IDasComponent * *pp_out_component) =
        0;
    DAS_METHOD GetBase(size_t index, IDasBase * *pp_out_base) = 0;

    DAS_METHOD SetInt(size_t index, int64_t in_int) = 0;
    DAS_METHOD SetFloat(size_t index, float in_float) = 0;
    DAS_METHOD SetString(size_t index, IDasReadOnlyString * in_string) = 0;
    DAS_METHOD SetBool(size_t index, bool in_bool) = 0;
    DAS_METHOD SetComponent(size_t index, IDasComponent * in_component) = 0;
    DAS_METHOD SetBase(size_t index, IDasBase * in_base) = 0;

    DAS_METHOD PushBackInt(int64_t in_int) = 0;
    DAS_METHOD PushBackFloat(float in_float) = 0;
    DAS_METHOD PushBackString(IDasReadOnlyString * in_string) = 0;
    DAS_METHOD PushBackBool(bool in_bool) = 0;
    DAS_METHOD PushBackComponent(IDasComponent * in_component) = 0;
    DAS_METHOD PushBackBase(IDasBase * in_base) = 0;

    DAS_METHOD GetType(size_t index, DasVariantType * p_out_type) = 0;

    DAS_METHOD RemoveAt(size_t index) = 0;

    /**
     * @brief 此函数一定成功，因此返回值即为大小
     * @return 参数数组大小
     */
    DAS_METHOD GetSize() = 0;
};

// {AA167C84-DE92-4893-B39C-21FFF9DBC544}
DAS_DEFINE_GUID(
    DAS_IID_SWIG_VARIANT_VECTOR,
    IDasSwigVariantVector,
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
DAS_SWIG_EXPORT_ATTRIBUTE(IDasSwigVariantVector)
DAS_INTERFACE IDasSwigVariantVector : public IDasSwigBase
{
    virtual DasRetInt            GetInt(size_t index) = 0;
    virtual DasRetFloat          GetFloat(size_t index) = 0;
    virtual DasRetReadOnlyString GetString(size_t index) = 0;
    virtual DasRetBool           GetBool(size_t index) = 0;
    virtual DasRetComponent      GetComponent(size_t index) = 0;
    virtual DasRetSwigBase       GetBase(size_t index) = 0;

    virtual DasResult SetInt(size_t index, int64_t in_int) = 0;
    virtual DasResult SetFloat(size_t index, float in_float) = 0;
    virtual DasResult SetString(size_t index, DasReadOnlyString in_string) = 0;
    virtual DasResult SetBool(size_t index, bool in_bool) = 0;
    virtual DasResult SetComponent(
        size_t index,
        IDasSwigComponent * in_component) = 0;
    virtual DasResult SetBase(size_t index, IDasSwigBase * in_base) = 0;

    virtual DasResult PushBackInt(int64_t in_int) = 0;
    virtual DasResult PushBackFloat(float in_float) = 0;
    virtual DasResult PushBackString(DasReadOnlyString in_string) = 0;
    virtual DasResult PushBackBool(bool in_bool) = 0;
    virtual DasResult PushBackComponent(IDasSwigComponent * in_component) = 0;
    virtual DasResult PushBackBase(IDasSwigBase * in_base) = 0;

    virtual DasRetVariantType GetType(size_t index) = 0;

    virtual DasResult RemoveAt(size_t index) = 0;

    /**
     * @brief 此函数一定成功，因此返回值即为大小
     * @return 参数数组大小
     */
    virtual DasResult GetSize() = 0;
};

DAS_DEFINE_RET_POINTER(DasRetVariantVector, IDasSwigVariantVector);

#include <das/PluginInterface/IDasComponent.h>

#endif // DAS_VARIANT_H


#ifndef DAS_BASE_H
#define DAS_BASE_H

#include <cstdint>
#include <cstring>
#include <das/DasConfig.h>
#include <das/DasGuidHolder.h>

#include <das/DasTypes.hpp>

// clang-format off
#ifdef SWIG
#define SWIG_IGNORE(x) %ignore x;
#define SWIG_ENABLE_DIRECTOR(x) %feature("director") x;
#define SWIG_POINTER_CLASS(x, y) %pointer_class(x, y);
#define SWIG_NEW_OBJECT(x) %newobject x;
#define SWIG_DEL_OBJECT(x) %delobject x::Release;
#define SWIG_UNREF_OBJECT(x) %feature("unref") x "if($this) { $this->Release(); }"
#define SWIG_ENABLE_SHARED_PTR(x) %shared_ptr(x);
#define SWIG_NO_DEFAULT_CTOR(x) %nodefaultctor x;
#define DAS_DEFINE_GUID(name, type, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    static const DasGuid name =                                                \
        {l, w1, w2, {b1, b2, b3, b4, b5, b6, b7, b8}};
#define DAS_DEFINE_CLASS_GUID(name, type, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    static const DasGuid name =                                                      \
        {l, w1, w2, {b1, b2, b3, b4, b5, b6, b7, b8}};
#define DAS_DEFINE_GUID_IN_NAMESPACE(iid_name, ns, type, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    namespace ns { struct type; }\
    static const DasGuid iid_name =                                                \
        {l, w1, w2, {b1, b2, b3, b4, b5, b6, b7, b8}};
#define SWIG_PRIVATE private:
#define SWIG_PUBLIC public:
#else
#define SWIG_IGNORE(x)
#define SWIG_ENABLE_DIRECTOR(x)
#define SWIG_POINTER_CLASS(x, y)
#define SWIG_NEW_OBJECT(x)
#define SWIG_DEL_OBJECT(x)
#define SWIG_UNREF_OBJECT(x)
#define SWIG_ENABLE_SHARED_PTR(x)
#define SWIG_NO_DEFAULT_CTOR(x)
#define DAS_DEFINE_GUID(name, type, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    static const DasGuid name =                                                \
        {l, w1, w2, {b1, b2, b3, b4, b5, b6, b7, b8}};                         \
    DAS_DEFINE_GUID_HOLDER(type, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8);
#define DAS_DEFINE_CLASS_GUID(type, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    DAS_DEFINE_CLASS_GUID_HOLDER(type, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8);
#define DAS_DEFINE_CLASS_IN_NAMESPACE(ns, type, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    namespace ns { class type; }\
    DAS_DEFINE_CLASS_GUID_HOLDER_IN_NAMESPACE(ns ,type, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8);
#define DAS_DEFINE_STRUCT_IN_NAMESPACE(ns, type, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    namespace ns { struct type; }\
    DAS_DEFINE_GUID_HOLDER_IN_NAMESPACE(ns ,type, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8);
#define DAS_DEFINE_GUID_IN_NAMESPACE(iid_name, ns, type, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    namespace ns { struct type; }\
    DAS_DEFINE_GUID_HOLDER_IN_NAMESPACE_WITH_IID(iid_name, ns, type, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8);
#define SWIG_PRIVATE
#define SWIG_PUBLIC
#endif
// clang-format on

template <class T>
void _das_internal_DelayAddRef(T* pointer)
{
    pointer->AddRef();
}

template <class T>
void _das_internal_DelayRelease(T* pointer) noexcept
{
    pointer->Release();
}

#define DAS_DEFINE_RET_TYPE(type_name, type)                                   \
    struct type_name                                                           \
    {                                                                          \
        SWIG_PRIVATE                                                           \
        DasResult error_code{DAS_E_UNDEFINED_RETURN_VALUE};                    \
        type      value{};                                                     \
                                                                               \
    public:                                                                    \
        DasResult GetErrorCode() noexcept { return error_code; }               \
        void      SetErrorCode(DasResult in_error_code) noexcept               \
        {                                                                      \
            this->error_code = in_error_code;                                  \
        }                                                                      \
        type GetValue() { return value; }                                      \
        void SetValue(const type& input_value) { value = input_value; }        \
    }

/**
 * @brief 注意：GetValue将取得指针所有权
 *
 */
#define DAS_DEFINE_RET_POINTER(type_name, pointer_type)                        \
    struct type_name                                                           \
    {                                                                          \
        SWIG_PRIVATE                                                           \
        DasResult                 error_code{DAS_E_UNDEFINED_RETURN_VALUE};    \
        DAS::DasPtr<pointer_type> value{};                                     \
                                                                               \
    public:                                                                    \
        DasResult GetErrorCode() noexcept { return error_code; }               \
        void      SetErrorCode(DasResult in_error_code) noexcept               \
        {                                                                      \
            this->error_code = in_error_code;                                  \
        }                                                                      \
        pointer_type* GetValue() noexcept                                      \
        {                                                                      \
            auto* const result = value.Get();                                  \
            _das_internal_DelayAddRef(result);                                 \
            return result;                                                     \
        }                                                                      \
        void SetValue(pointer_type* input_value) { value = input_value; }      \
    }

#define DAS_SWIG_DIRECTOR_ATTRIBUTE(x)                                         \
    SWIG_ENABLE_DIRECTOR(x)                                                    \
    SWIG_UNREF_OBJECT(x)

#define DAS_SWIG_EXPORT_ATTRIBUTE(x) SWIG_UNREF_OBJECT(x)

// 错误码宏已迁移到 <das/DasTypes.hpp>

#ifdef DAS_WINDOWS
// MSVC
#ifdef _MSC_VER
#define DAS_STD_CALL __stdcall
#else
// GCC AND CLANG
#define DAS_STD_CALL __attribute__((__stdcall__))
#endif // _MSC_VER
#endif // DAS_WINDOWS

#ifndef DAS_STD_CALL
#define DAS_STD_CALL
#endif // DAS_STD_CALL

#define DAS_INTERFACE struct

#define DAS_METHOD virtual DasResult DAS_STD_CALL
#define DAS_METHOD_(x) virtual x DAS_STD_CALL
#define DAS_BOOL_METHOD virtual DasBool DAS_STD_CALL
#define DAS_IMPL DasResult DAS_STD_CALL
#define DAS_BOOL_IMPL DasBool DAS_STD_CALL

DAS_DEFINE_RET_TYPE(DasRetGuid, DasGuid);

typedef int32_t DasBool;

#define DAS_TRUE 1
#define DAS_FALSE 0

/**
 * @brief input format should be "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
 *
 * @param p_guid_string
 * @return DAS_S_OK if success.
 */
DAS_API DasRetGuid DasMakeDasGuid(const char* p_guid_string);

/**
 * @brief input format should be "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
 *
 * @param p_guid_string
 * @return DAS_S_OK if success.
 */
SWIG_IGNORE(DasMakeDasGuid)
DAS_C_API DasResult
DasMakeDasGuid(const char* p_guid_string, DasGuid* p_out_guid);

#ifndef SWIG

#ifdef __cplusplus
inline bool operator==(const DasGuid& lhs, const DasGuid& rhs) noexcept
{
    const auto result = ::memcmp(&lhs, &rhs, sizeof(lhs));
    return result == 0;
}

DAS_NS_BEGIN

inline bool      IsOk(const DasResult result) { return result >= 0; }
inline bool      IsFailed(const DasResult result) { return result < 0; }
inline DasResult GetErrorCodeFrom(const DasResult result) { return result; }

template <class T>
concept is_das_ret_type = requires { T::error_code; };

template <is_das_ret_type T>
bool IsOk(const T& t)
{
    return t.error_code >= 0;
}

template <is_das_ret_type T>
bool IsFailed(const T& t)
{
    return t.error_code < 0;
}

template <is_das_ret_type T>
DasResult GetErrorCodeFrom(const T& t)
{
    return t.error_code;
}

DAS_NS_END

#endif // __cplusplus

DAS_C_API DasResult InitializeDasCore();

#endif // SWIG

DAS_DEFINE_GUID(
    DAS_IID_BASE,
    IDasBase,
    0x00000000,
    0x0000,
    0x0000,
    0xc0,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x46)
DAS_SWIG_EXPORT_ATTRIBUTE(IDasBase)
DAS_INTERFACE IDasBase
{
    SWIG_PRIVATE
    virtual uint32_t AddRef() = 0;
    virtual uint32_t Release() = 0;
    SWIG_PUBLIC
    DAS_METHOD       QueryInterface(const DasGuid& iid, void** pp_object) = 0;
};

// {9CA2095E-3F1E-44C0-BB14-515446666892}
DAS_DEFINE_GUID(
    DAS_IID_WEAK_REFERENCE,
    IDasWeakReference,
    0x9ca2095e,
    0x3f1e,
    0x44c0,
    0xbb,
    0x14,
    0x51,
    0x54,
    0x46,
    0x66,
    0x68,
    0x92);
DAS_SWIG_EXPORT_ATTRIBUTE(IDasWeakReference)
DAS_INTERFACE IDasWeakReference : public IDasBase
{
    /**
     * @brief 获取对象的强引用
     *
     * @param pp_out_object 对象强引用指针，已AddRef
     * @return DasResult DAS_S_OK 表示成功，
     * DAS_E_STRONG_REFERENCE_NOT_AVAILABLE 表示内部对象已不可用
     */
    DAS_METHOD Resolve(IDasBase * *pp_out_object) = 0;
};

// {1A39C88A-CC59-4999-A828-2686F466DA05}
DAS_DEFINE_GUID(
    DAS_IID_WEAK_REFERENCE_SOURCE,
    IDasWeakReferenceSource,
    0x1a39c88a,
    0xcc59,
    0x4999,
    0xa8,
    0x28,
    0x26,
    0x86,
    0xf4,
    0x66,
    0xda,
    0x5);
DAS_SWIG_EXPORT_ATTRIBUTE(IDasWeakReferenceSource)
DAS_INTERFACE IDasWeakReferenceSource : public IDasBase
{
    DAS_METHOD GetWeakReference(IDasWeakReference * *pp_out_weak) = 0;
};

inline bool IsDasGuidEqual(
    const DasGuid& lhs,
    const DasGuid& rhs) noexcept
{
    return lhs == rhs;
}

DAS_DEFINE_RET_TYPE(DasRetBool, bool);

DAS_DEFINE_RET_TYPE(DasRetInt, int64_t);

DAS_DEFINE_RET_TYPE(DasRetUInt, uint64_t);

DAS_DEFINE_RET_TYPE(DasRetFloat, float);

#define DAS_THROW_IF_FAILED(result)                                            \
    {                                                                          \
        if (::Das::IsFailed(result))                                           \
        {                                                                      \
            DAS_THROW_EC(result);                                              \
        }                                                                      \
    }

#endif // DAS_BASE_H

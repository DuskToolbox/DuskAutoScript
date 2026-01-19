#ifndef DAS_BASE_H
#define DAS_BASE_H

#include <cstddef>
#include <cstdint>
#include <das/DasConfig.h>
#include <das/DasGuidHolder.h>
#include <string>
#include <type_traits>

using DasResult = int32_t;

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

// 使用宏定义后SWIG会认为这些值必然是常量
#define DAS_S_OK 0
#define DAS_S_FALSE 1
#define DAS_E_RESERVED -1073741830
#define DAS_E_NO_INTERFACE -1073741831
#define DAS_E_UNDEFINED_RETURN_VALUE -1073741832
#define DAS_E_INVALID_STRING -1073741833
#define DAS_E_INVALID_STRING_SIZE -1073741834
#define DAS_E_NO_IMPLEMENTATION -1073741835
#define DAS_E_UNSUPPORTED_SYSTEM -1073741836
#define DAS_E_INVALID_JSON -1073741837
#define DAS_E_TYPE_ERROR -1073741838
#define DAS_E_INVALID_FILE -1073741839
#define DAS_E_INVALID_URL -1073741840
/**
 * @brief 返回此值可以表示枚举结束
 *
 */
#define DAS_E_OUT_OF_RANGE -1073741841
#define DAS_E_DUPLICATE_ELEMENT -1073741842
#define DAS_E_FILE_NOT_FOUND -1073741843
#define DAS_E_MAYBE_OVERFLOW -1073741844
#define DAS_E_OUT_OF_MEMORY -1073741845
#define DAS_E_INVALID_PATH -1073741846
#define DAS_E_INVALID_POINTER -1073741847
#define DAS_E_SWIG_INTERNAL_ERROR -1073741848
#define DAS_E_PYTHON_ERROR -1073741849
#define DAS_E_JAVA_ERROR -1073741850
#define DAS_E_CSHARP_ERROR -1073741851
#define DAS_E_INTERNAL_FATAL_ERROR -1073741852
#define DAS_E_INVALID_ENUM -1073741853
#define DAS_E_INVALID_SIZE -1073741854
#define DAS_E_OPENCV_ERROR -1073741855
#define DAS_E_ONNX_RUNTIME_ERROR -1073741856
#define DAS_E_TIMEOUT -1073741857
#define DAS_E_PERMISSION_DENIED -1073741859
#define DAS_E_SYMBOL_NOT_FOUND -1073741860
#define DAS_E_DANGLING_REFERENCE -1073741861
#define DAS_E_OBJECT_NOT_INIT -1073741862
#define DAS_E_UNEXPECTED_THREAD_DETECTED -1073741863
#define DAS_E_STRONG_REFERENCE_NOT_AVAILABLE -1073741864
#define DAS_E_TASK_WORKING -1073741865
#define DAS_E_OBJECT_ALREADY_INIT -1073741866

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

/**
 * @brief NOTE: Be careful about the lifetime of this structure.\n
 *       If you want to keep it, you MUST make a copy of it.
 */
typedef struct _das_GUID
{
#ifdef SWIG
private:
#endif // SWIG
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
} DasGuid;

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
DAS_INTERFACE IDasBase
{
    virtual uint32_t AddRef() = 0;
    virtual uint32_t Release() = 0;
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
DAS_INTERFACE IDasWeakReferenceSource : public IDasBase
{
    DAS_METHOD GetWeakReference(IDasWeakReference * *pp_out_weak) = 0;
};

#endif // SWIG

DAS_API inline bool IsDasGuidEqual(
    const DasGuid& lhs,
    const DasGuid& rhs) noexcept
{
    return lhs == rhs;
}

DAS_DEFINE_RET_TYPE(DasRetBool, bool);

DAS_DEFINE_RET_TYPE(DasRetInt, int64_t);

DAS_DEFINE_RET_TYPE(DasRetUInt, uint64_t);

DAS_DEFINE_RET_TYPE(DasRetFloat, float);

// ============================================
// 异常处理宏和函数声明（用于代码生成器）
// ============================================

#ifdef __cplusplus
DAS_NS_BEGIN

// 前置声明 IDasException 接口
namespace ExportInterface
{
    DAS_INTERFACE IDasException;
} // namespace ExportInterface

/**
 * @brief 源代码位置信息结构
 */
struct DasExceptionSourceInfo
{
    const char* file;
    int         line;
    const char* function;
};

/**
 * @brief 抛出 IDasException* 的函数声明
 *
 * 这个函数在 DasCore.dll 中实现，会抛出 IDasException* 指针。
 * 调用方使用完毕后需要调用 Release 释放资源。
 *
 * @param error_code 错误码
 * @param p_source_info 源代码位置信息
 */
extern "C" DAS_API void ThrowDasExceptionPtr(
    DasResult               error_code,
    DasExceptionSourceInfo* p_source_info);

/**
 * @brief 创建 IDasException 的 C 函数声明
 *
 * @param error_code 错误码
 * @param message 错误消息
 * @return IDasException* 异常接口指针，调用方使用后需调用 Release
 */
extern "C" DAS_API ExportInterface::IDasException* DasCreateException(
    DasResult   error_code,
    const char* message);

/**
 * @brief 从 IDasException* 获取消息的辅助函数声明
 *
 * @param p_exception 异常指针
 * @return std::string 异常消息
 */
DAS_API std::string DasGetExceptionMessage(
    ExportInterface::IDasException* p_exception);

/**
 * @brief 从 IDasException* 获取错误码的辅助函数声明
 *
 * @param p_exception 异常指针
 * @return DasResult 错误码
 */
DAS_API DasResult
DasGetExceptionErrorCode(ExportInterface::IDasException* p_exception);

DAS_NS_END
#endif // __cplusplus

// 宏定义
#define DAS_THROW_IF_FAILED(result)                                            \
    {                                                                          \
        if (::Das::IsFailed(result))                                           \
        {                                                                      \
            ::Das::DasExceptionSourceInfo __das_internal_source_location{      \
                __FILE__,                                                      \
                __LINE__,                                                      \
                DAS_FUNCTION};                                                 \
            ::Das::ThrowDasExceptionPtr(                                       \
                result,                                                        \
                &__das_internal_source_location);                              \
        }                                                                      \
    }

#endif // DAS_BASE_H

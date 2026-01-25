#ifndef DAS_TYPES_HPP
#define DAS_TYPES_HPP

#include <cstddef>
#include <cstdint>
#include <das/DasExport.h>

// ============================================================================
// 基础类型定义（所有头文件共用）
// ============================================================================

using DasResult = int32_t;

// ============================================================================
// GUID 结构定义
// ============================================================================

typedef struct _das_GUID
{
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
} DasGuid;

// ============================================================================
// 错误码宏
// ============================================================================

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
#define DAS_E_FAIL -1073741867
#define DAS_E_INVALID_ARGUMENT -1073741868
#define DAS_E_CAPTURE_FAILED -1073741869
#define DAS_E_NOT_FOUND -1073741870

// ============================================================================
// 接口前置声明（用于打破循环依赖）
// ============================================================================

namespace Das
{
    template <class T>
    class DasPtr;
}

struct IDasBase;
struct IDasWeakReference;
struct IDasWeakReferenceSource;
struct IDasReadOnlyString;
struct IDasString;
struct IDasException;

#endif // DAS_TYPES_HPP

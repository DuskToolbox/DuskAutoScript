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
#ifndef SWIG
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
#endif // SWIG
} DasGuid;

// ============================================================================
// 错误码宏
// ============================================================================

#define DAS_S_OK 0
#define DAS_S_FALSE 1

#define DAS_E_RESERVED -1073750000
#define DAS_E_NO_INTERFACE -1073750001
#define DAS_E_UNDEFINED_RETURN_VALUE -1073750002
#define DAS_E_INVALID_STRING -1073750003
#define DAS_E_INVALID_STRING_SIZE -1073750004
#define DAS_E_NO_IMPLEMENTATION -1073750005
#define DAS_E_UNSUPPORTED_SYSTEM -1073750006
#define DAS_E_INVALID_JSON -1073750007
#define DAS_E_TYPE_ERROR -1073750008
#define DAS_E_INVALID_FILE -1073750009
#define DAS_E_INVALID_URL -1073750010
#define DAS_E_OUT_OF_RANGE -1073750011
#define DAS_E_DUPLICATE_ELEMENT -1073750012
#define DAS_E_FILE_NOT_FOUND -1073750013
#define DAS_E_MAYBE_OVERFLOW -1073750014
#define DAS_E_OUT_OF_MEMORY -1073750015
#define DAS_E_INVALID_PATH -1073750016
#define DAS_E_INVALID_POINTER -1073750017
#define DAS_E_SWIG_INTERNAL_ERROR -1073750018
#define DAS_E_PYTHON_ERROR -1073750019
#define DAS_E_JAVA_ERROR -1073750020
#define DAS_E_CSHARP_ERROR -1073750021
#define DAS_E_INTERNAL_FATAL_ERROR -1073750022
#define DAS_E_INVALID_ENUM -1073750023
#define DAS_E_INVALID_SIZE -1073750024
#define DAS_E_OPENCV_ERROR -1073750025
#define DAS_E_ONNX_RUNTIME_ERROR -1073750026
#define DAS_E_TIMEOUT -1073750027
#define DAS_E_PERMISSION_DENIED -1073750029
#define DAS_E_SYMBOL_NOT_FOUND -1073750030
#define DAS_E_DANGLING_REFERENCE -1073750031
#define DAS_E_OBJECT_NOT_INIT -1073750032
#define DAS_E_UNEXPECTED_THREAD_DETECTED -1073750033
#define DAS_E_STRONG_REFERENCE_NOT_AVAILABLE -1073750034
#define DAS_E_TASK_WORKING -1073750035
#define DAS_E_OBJECT_ALREADY_INIT -1073750036
#define DAS_E_FAIL -1073750037
#define DAS_E_INVALID_ARGUMENT -1073750038
#define DAS_E_CAPTURE_FAILED -1073750039
#define DAS_E_NOT_FOUND -1073750040

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

#endif // DAS_TYPES_HPP

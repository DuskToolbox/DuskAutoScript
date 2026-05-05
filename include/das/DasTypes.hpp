#ifndef DAS_TYPES_HPP
#define DAS_TYPES_HPP

#include <cstdint>

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

#include "DasResult.generated.h"

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

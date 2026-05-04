#include <das/DasConfig.h>
#include <das/DasString.hpp>
#include <das/IDasBase.h>

#include "IDasVariantVector.h"

// SWIG 友好接口：包含 [export] 函数（自动转为 DasRet 返回类型）
// DasRetXxx 结构体由 header generator 从 [export] 函数的 [out] 参数自动生成
// 不包含 c_abi 双指针函数，避免 SWIG 生成不正确的 Java 绑定
#include "DasCoreApiSwig.generated.h"

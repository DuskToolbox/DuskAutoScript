// clang-format off

// ============================================================================
// ① 基础头文件
// ============================================================================
%include <das/DasConfig.h>
%include <das/DasExport.h>
%include <das/DasTypes.hpp>
// DasResult 错误码常量（DAS_S_OK 等），通过 DasTypes.hpp 的 #include 无法被 SWIG 解析
%include <DasResult.generated.h>

// ============================================================================
// ② %ignore（必须在 IDasBase 定义之前）
// ============================================================================

// 隐藏原始的 QueryInterface 方法（必须带参数签名，避免误杀 %extend 添加的同名方法）
%ignore IDasBase::QueryInterface(const DasGuid&, void**);

// ============================================================================
// ③ 类定义
// ============================================================================
%include <das/IDasBase.h>
%include <das/DasString.hpp>;
%include <das/DasException.hpp>;

%nodefaultctor DasException;

// !!! 包含CMake从IDL自动生成的SWIG接口汇总文件 !!!
// 该文件包含所有从IDL生成的SWIG接口（如DasCV, IDasCapture, IDasPluginManager等）
%include <das/_autogen/idl/swig/swig_all.i>

// ============================================================================
// ⑤ DasRetBase 的 %rename/%ignore（必须在 struct 定义之前）
// ============================================================================
%include "java/DasRetBase.i"

// ============================================================================
// ⑥ DasRetBase 定义
// ============================================================================
%include "common/DasRetBase.i"

// ============================================================================
// ⑦ %extend IDasBase（必须在 IDasBase 和 DasRetBase 定义之后）
// ============================================================================

// 添加返回 DasRetBase 的 QueryInterface 包装方法
%extend IDasBase {
    DasRetBase QueryInterface(const DasGuid& iid) {
        DasRetBase result;
        result.error_code = $self->QueryInterface(iid, reinterpret_cast<void**>(&result.value));
        return result;
    }
}

// ============================================================================
// ⑧ 自动生成的 %extend 批量（必须在类定义之后）
// ============================================================================
%include <DasTypeMapsExtend.i>

%{
#include <das/DasInternalRetPluginPackage.h>
%}
%include <das/DasInternalRetPluginPackage.h>

%include <das/DasSwigApi.h>;
%include <DasCoreApiSwig.generated.h>
// 源自 DasApi.h

DAS_C_API void DasLogInfoU8(const char* p_string);

DAS_C_API void DasLogWarningU8(const char* p_string);

DAS_C_API void DasLogErrorU8(const char* p_string);

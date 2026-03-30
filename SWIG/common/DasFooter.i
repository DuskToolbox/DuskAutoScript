%include <das/DasString.hpp>;
%include <das/DasException.hpp>;

%nodefaultctor DasException;

// !!! 包含CMake从IDL自动生成的SWIG接口汇总文件 !!!
// 该文件包含所有从IDL生成的SWIG接口（如DasCV, IDasCapture, IDasPluginManager等）
%include <das/_autogen/idl/swig/swig_all.i>

// ============================================================================
// 包含所有%extend和DasRetXxx定义（必须在类定义之后）
// 这些%extend指令用于添加返回DasRetXxx的包装方法
// ============================================================================
%include <DasTypeMapsExtend.i>

%{
#include <das/DasInternalRetPluginPackage.h>
%}
%include <das/DasInternalRetPluginPackage.h>

%include <das/DasSwigApi.h>;
// 源自 DasApi.h

DAS_C_API void DasLogInfoU8(const char* p_string);

DAS_C_API void DasLogWarningU8(const char* p_string);

DAS_C_API void DasLogErrorU8(const char* p_string);
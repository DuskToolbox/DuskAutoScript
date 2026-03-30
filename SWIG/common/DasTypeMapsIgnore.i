// ============================================================================
// 包含所有%ignore定义（必须在类定义之前）
// 这些%ignore指令用于隐藏带[out]参数的原始方法
// ============================================================================
%include <DasTypeMapsIgnore.i>

%include <das/DasConfig.h>
%include <das/DasExport.h>
%include <das/DasTypes.hpp>
%include <das/IDasBase.h>
%include <das/IDasAsyncOperation.h>

// 忽略 GetResults 方法，因为其指针参数类型在 SWIG 中需要特殊 typemap
// 绑定语言用户应通过 SetCompleted 回调获取结果
%ignore IDasAsyncLoadPluginOperation::GetResults;
%ignore IDasAsyncHandshakeOperation::GetResults;

%include <das/IDasAsyncLoadPluginOperation.h>
%include <das/IDasAsyncHandshakeOperation.h>
%include <das/IDasAsyncLoadPluginOperation.h>
%include <das/IDasAsyncHandshakeOperation.h>
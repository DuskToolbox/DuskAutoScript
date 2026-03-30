// clang-format off
// ═══════════════════════════════════════════════════════════
// ExportAll.i — 编排入口
// 严格按原始行号顺序 include 子文件
// ═══════════════════════════════════════════════════════════

// --- common: %module + SWIG 标准库 ---
%include "common/DasModule.i"

// --- common: C++ 头文件 ---
%include "common/DasHeaders.i"

// --- java: DasRetBase pragma/rename/ignore/javacode ---
%include "java/DasRetBase.i"

// --- common: DasRetBase struct ---
%include "common/DasRetBase.i"

// --- python: IDasBase director:except ---
%include "python/IDasBase.i"

// --- common: IDasBase extend + ignore ---
%include "common/IDasBaseShared.i"

// --- java: JNI RAII + 日志辅助 + IDasBase javacode ---
%include "java/JniHelpers.i"

// --- csharp: IDasBase cscode ---
%include "csharp/IDasBase.i"

// --- common: DasTypeMapsIgnore + headers + ignore ---
%include "common/DasTypeMapsIgnore.i"

// --- java: DasReadOnlyString support ---
%include "java/DasReadOnlyString.i"

// --- common: DasException 注释 ---
%include "common/DasExceptionComment.i"

// --- java: DasException typemaps ---
%include "java/DasException.i"

// --- csharp: DasException csbase ---
%include "csharp/DasException.i"

// --- common: DasReadOnlyString ignore 注释 ---
%include "common/DasReadOnlyStringIgnoreComment.i"

// --- java: DasReadOnlyString ignore ---
%include "java/DasReadOnlyStringIgnore.i"

// --- csharp: IDasReadOnlyString* typemaps ---
%include "csharp/IDasReadOnlyString.i"

// --- python: IDasReadOnlyString* typemaps ---
%include "python/IDasReadOnlyString.i"

// --- common: DasString/DasException/DasTypeMapsExtend/DasSwigApi ---
%include "common/DasFooter.i"

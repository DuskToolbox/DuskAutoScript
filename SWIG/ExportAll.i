// clang-format off
// ═══════════════════════════════════════════════════════════
// ExportAll.i — 编排入口
// 严格按原始行号顺序 include 子文件，SWIG 展开后与原文件等价
// ═══════════════════════════════════════════════════════════

// --- L1-10: %module + SWIG 标准库 includes ---
%include "common/DasModule.i"

// --- L12-66: C++ 头文件 includes ---
%include "common/DasHeaders.i"

// --- L68-100: DasRetBase Java pragma/rename/ignore/javacode ---
%include "java/DasRetBase.i"

// --- L102-125: DasRetBase struct 定义 ---
%include "common/DasRetBase.i"

// --- L127-136: IDasBase Python director:except ---
%include "python/IDasBase.i"

// --- L138-148: IDasBase shared extend + ignore ---
%include "common/IDasBaseShared.i"

// --- L150-660: JNI RAII + 日志辅助 + IDasBase Java javacode ---
%include "java/JniHelpers.i"

// --- L662-754: IDasBase C# cscode ---
%include "csharp/IDasBase.i"

// --- L756-776: DasTypeMapsIgnore + headers + ignore ---
%include "common/DasTypeMapsIgnore.i"

// --- L777-1019: DasReadOnlyString Java support ---
%include "java/DasReadOnlyString.i"

// --- L1021-1028: DasException 共享注释 ---
%include "common/DasExceptionComment.i"

// --- L1029-1111: DasException Java typemaps ---
%include "java/DasException.i"

// --- L1113-1118: DasException C# csbase ---
%include "csharp/DasException.i"

// --- L1120-1123: DasReadOnlyString ignore 注释 ---
%include "common/DasReadOnlyStringIgnoreComment.i"

// --- L1124-1127: DasReadOnlyString Java ignore ---
%include "java/DasReadOnlyStringIgnore.i"

// --- L1129-1166: IDasReadOnlyString* C# typemaps ---
%include "csharp/IDasReadOnlyString.i"

// --- L1168-1231: IDasReadOnlyString* Python typemaps ---
%include "python/IDasReadOnlyString.i"

// --- L1233-1260: DasString/DasException/DasTypeMapsExtend/DasSwigApi ---
%include "common/DasFooter.i"
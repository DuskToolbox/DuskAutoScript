#ifdef SWIGCSHARP
// ============================================================================
// IDasReadOnlyString* C# Typemap -> DasReadOnlyString
//
// 目的：当返回结构/参数出现 IDasReadOnlyString* 时，避免生成不透明的
//      SWIGTYPE_p_IDasReadOnlyString，让 C# API 直接使用 DasReadOnlyString。
// 关键点：
//   - C++ %typemap(out) 把 IDasReadOnlyString* 包装成 new DasReadOnlyString($1)
//   - C# %typemap(csout) 用 IntPtr.Zero 判空，并以 (cPtr, true) 接管释放
// ============================================================================
// 1) 对外暴露给用户的 C# 类型
%typemap(cstype) IDasReadOnlyString * "DasReadOnlyString"
// 2) P/Invoke 中间类型：返回用 IntPtr，入参用 HandleRef（匹配 getCPtr()）
%typemap(imtype) IDasReadOnlyString * "global::System.IntPtr"
// 3) C# -> native 入参转换：DasReadOnlyString.getCPtr(null) 会给 Zero HandleRef
%typemap(csin) IDasReadOnlyString * "DasReadOnlyString.getCPtr($csinput)"
// 4) native -> C# 返回值转换：判空 + 接管所有权（因为 native out 中 new 了包装对象）
%typemap(csout) IDasReadOnlyString * %{
  {
    global::System.IntPtr cPtr = $imcall;
    return (cPtr == global::System.IntPtr.Zero) ? null : new DasReadOnlyString(cPtr, true);
  }
%}
// 5) (配套) C++ in：$input 实际上传的是 DasReadOnlyString*（被当成 IDasReadOnlyString* 透传）
//    取出其底层接口指针 IDasReadOnlyString*
%typemap(in) IDasReadOnlyString * %{
    {
        DasReadOnlyString* p_tmp = reinterpret_cast<DasReadOnlyString*>($input);
        $1 = p_tmp ? p_tmp->Get() : nullptr;
    }
%}
// 6) (配套) C++ out：把 IDasReadOnlyString* 包装成一个新的 DasReadOnlyString*
//    再把这个"包装对象指针"透传回 C#（C# 侧用 (cPtr,true) 释放它）
%typemap(out) IDasReadOnlyString * %{
    if ($1) {
        DasReadOnlyString* tmp = new DasReadOnlyString($1);
        $result = reinterpret_cast<IDasReadOnlyString*>(tmp);
    } else {
        $result = nullptr;
    }
%}
// 7) Director C/C++ -> C#：先把 IDasReadOnlyString* 包成 DasReadOnlyString* 再传给 C# 代理类
%typemap(directorin) IDasReadOnlyString * %{
    if (!$1) {
        $input = nullptr;
    } else {
        DasReadOnlyString* tmp = new DasReadOnlyString($1);
        $input = reinterpret_cast<void*>(tmp);
    }
%}
// 8) Director C# -> C/C++：C# 返回 DasReadOnlyString，对应的 IntPtr 需要解回底层接口指针
%typemap(directorout) IDasReadOnlyString * %{
    if (!$input) {
        $result = nullptr;
    } else {
        DasReadOnlyString* p_tmp = reinterpret_cast<DasReadOnlyString*>($input);
        $result = p_tmp ? p_tmp->Get() : nullptr;
    }
%}
// 9) Director C# 入参显示为 DasReadOnlyString，而不是 SWIGTYPE_p_IDasReadOnlyString
%typemap(csdirectorin) IDasReadOnlyString * "($iminput == global::System.IntPtr.Zero) ? null : new DasReadOnlyString($iminput, true)"
// 10) Director C# 返回值从 DasReadOnlyString 提取出包装对象指针
%typemap(csdirectorout) IDasReadOnlyString * "($cscall == null) ? global::System.IntPtr.Zero : DasReadOnlyString.getCPtr($cscall).Handle"
#endif // SWIGCSHARP

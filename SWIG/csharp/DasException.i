#ifdef SWIGCSHARP
// ============================================================================
// DasException C# 异常继承
// ============================================================================
%typemap(csbase) DasException "System.Exception"
#endif // SWIGCSHARP
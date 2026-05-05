%begin %{
#ifndef SWIG
#include <utility>
#include <atomic>
#include <das/IDasBase.h>
#endif // SWIG
#ifndef DAS_RET_BASE
#define DAS_RET_BASE
// ============================================================================
// DasRetBase - IDasBase 的返回包装类
// 用于封装 QueryInterface 的返回值
// ============================================================================
struct DasRetBase {
    DasResult error_code;
    IDasBase* value;

    DasRetBase() : error_code(DAS_E_UNDEFINED_RETURN_VALUE), value(nullptr) {}

    DasResult GetErrorCode() const { return error_code; }
    void SetErrorCode(DasResult code) { error_code = code; }

    IDasBase* GetValue() const { return value; }
    void SetValue(IDasBase* v) { value = v; }

    bool IsOk() const { return DAS::IsOk(error_code); }
};
#endif // DAS_RET_BASE

%}

// 让 SWIG 类型系统认识 DasRetBase（%begin 不够，需要 %inline）
%inline %{
#ifndef DAS_RET_BASE
#define DAS_RET_BASE
struct DasRetBase {
    DasResult error_code;
    IDasBase* value;

    DasRetBase() : error_code(DAS_E_UNDEFINED_RETURN_VALUE), value(nullptr) {}

    DasResult GetErrorCode() const { return error_code; }
    void SetErrorCode(DasResult code) { error_code = code; }

    IDasBase* GetValue() const { return value; }
    void SetValue(IDasBase* v) { value = v; }

    bool IsOk() const { return DAS::IsOk(error_code); }
};
#endif // DAS_RET_BASE

%}
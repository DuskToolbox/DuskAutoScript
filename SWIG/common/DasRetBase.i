%inline %{
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
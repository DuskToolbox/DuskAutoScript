#ifdef SWIGPYTHON
/* IDasReadOnlyString*  <->  Python DasReadOnlyString */
/* Python 侧类型显示为 DasReadOnlyString（而不是 SWIGTYPE_p_xxx） */
%typemap(pytype) IDasReadOnlyString * "DasReadOnlyString"
/* ---------- 输入：Python -> C++ (IDasReadOnlyString*) ---------- */
/* 允许传 None；否则必须是 DasReadOnlyString*（包装对象） */
%typemap(typecheck) IDasReadOnlyString * {
    if ($input == Py_None) {
        $1 = 1;
    } else {
        void *ptr = nullptr;
        int res = SWIG_ConvertPtr($input, &ptr, SWIGTYPE_p_DasReadOnlyString, 0);
        $1 = SWIG_IsOK(res) && ptr;
    }
}
%typemap(in) IDasReadOnlyString * %{
    if ($input == Py_None) {
        $1 = nullptr;
    } else {
        DasReadOnlyString *tmp = nullptr;
        int res = SWIG_ConvertPtr($input, (void **)&tmp, SWIGTYPE_p_DasReadOnlyString, 0);
        if (!SWIG_IsOK(res) || !tmp) {
            SWIG_exception_fail(SWIG_TypeError,
                "Expected DasReadOnlyString (or None) for IDasReadOnlyString*");
        }
        $1 = tmp->Get();
    }
%}
/* director 输入（仅当该参数用于 director 虚函数签名时需要；加上通常无害） */
%typemap(directorin) IDasReadOnlyString * %{
    if (!$1) {
        $input = Py_None;
        Py_INCREF($input);
    } else {
        DasReadOnlyString *tmp = new DasReadOnlyString($1);
        $input = SWIG_NewPointerObj((void *)tmp, SWIGTYPE_p_DasReadOnlyString, SWIG_POINTER_OWN);
    }
%}
/* ---------- 输出：C++ -> Python ---------- */
/* 关键：把 IDasReadOnlyString* 包一层 new DasReadOnlyString($1)，Python 看到 DasReadOnlyString */
%typemap(out) IDasReadOnlyString * %{
    if (!$1) {
        Py_INCREF(Py_None);
        $result = Py_None;
    } else {
        DasReadOnlyString *tmp = new DasReadOnlyString($1);
        $result = SWIG_NewPointerObj((void *)tmp, SWIGTYPE_p_DasReadOnlyString, SWIG_POINTER_OWN);
    }
%}
/* director 输出（仅当返回值/出参用于 director 虚函数签名时需要） */
%typemap(directorout) IDasReadOnlyString * %{
    if ($input == Py_None) {
        $result = nullptr;
    } else {
        DasReadOnlyString *tmp = nullptr;
        int res = SWIG_ConvertPtr($input, (void **)&tmp, SWIGTYPE_p_DasReadOnlyString, 0);
        if (!SWIG_IsOK(res) || !tmp) {
            SWIG_exception_fail(SWIG_TypeError,
                "Expected DasReadOnlyString (or None) for IDasReadOnlyString* (director)");
        }
        $result = tmp->Get();
    }
%}
#endif // SWIGPYTHON

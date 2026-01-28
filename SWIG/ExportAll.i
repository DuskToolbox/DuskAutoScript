// clang-format off

%module(directors="1") DuskAutoScript

%include <stdint.i>
%include <typemaps.i>
%include <cpointer.i>
%include <std_string.i>
%include <std_vector.i>
%include <wchar.i>

%{
#include <das/DasExport.h>
#include <das/IDasBase.h>
#include <das/DasString.hpp>
#include <das/DasException.hpp>

#include <das/DasApi.h>

#ifdef SWIGPYTHON
#include <das/Core/ForeignInterfaceHost/PythonHost.h>

#ifdef DEBUG

// 修复 dll 文件带 _d 后缀时，PyInit 函数名称不正确的问题。
// TODO: 后续在CMake中配置更名，而不再靠这里修复
#ifdef __cplusplus
extern "C"
#endif
SWIGEXPORT
#if PY_VERSION_HEX >= 0x03000000
PyObject*
#else
void
#endif
SWIG_init(void);

#ifdef __cplusplus
extern "C"
#endif
SWIGEXPORT
#if PY_VERSION_HEX >= 0x03000000
PyObject*
#else
void
#endif
PyInit__DasCorePythonExport(void) {
#if PY_VERSION_HEX >= 0x03000000
    return ::SWIG_init();
#else
    ::SWIG_init();
#endif
}

#endif // DEBUG

#endif // SWIGPYTHON

// ============================================================================
// DasException Support for Python
// ============================================================================

// Extend DasException to add error_code attribute for Python access
%extend DasException {
    int GetErrorCode() const {
        return $self->GetErrorCode();
    }
    const char* GetMessage() const {
        return $self->what();
    }
}

// Exception handling for DasResult return values
%exception {
    try {
        $action
    }
    catch (const DasException& ex) {
        SWIG_exception_fail(SWIG_RuntimeError, ex.what());
    }
}

#ifdef SWIGJAVA
%typemap(jni) char16_t* "jstring"
%typemap(jtype) char16_t* "String"
%typemap(jstype) char16_t* "String"
%typemap(javadirectorin) char16_t* "$jniinput"
%typemap(javadirectorout) char16_t* "$javacall"

%typemap(jni) void DasReadOnlyString::GetUtf16 "jstring"
%typemap(jtype) void DasReadOnlyString::GetUtf16 "String"
%typemap(jstype) void DasReadOnlyString::GetUtf16 "String"
%typemap(javaout) void DasReadOnlyString::GetUtf16 {
    return $jnicall;
}

%typemap(in, numinputs=0) (const char16_t** out_string, size_t* out_string_size) %{
    char16_t* p_u16string;
    $1 = &p_u16string;

    size_t u16string_size;
    $2 = &u16string_size;
%}

%typemap(argout) (const char16_t** out_string, size_t* out_string_size) {
    if($1 && $2)
    {
        jsize j_length = (jsize)u16string_size;
        $result = jenv->NewString((jchar*)p_u16string, j_length);
    }
    else
    {
        jclass null_pointer_exception = jenv->FindClass("java/lang/NullPointerException");
        jenv->ThrowNew(null_pointer_exception, "Input pointer is null");
    }
}

%typemap(javain) (const char16_t* p_u16string, size_t length) "p_u16string"

// ============================================================================
// DasException Support for Java
// ============================================================================

// Rename DasException methods for Java naming convention
%rename(ErrorCode) DasException::GetErrorCode;
%rename(Message) DasException::what;

// Exception handling for DasException
%javaexception("DasException") DasException {
    $action
    try {
        return $result;
    }
    catch (const DasException& ex) {
        jclass exc = jenv->FindClass("DasException");
        jmethodID ctor = jenv->GetMethodID(exc, "<init>", "(ILjava/lang/String;)V");
        jstring msg = jenv->NewStringUTF(ex.what());
        jobject jexc = jenv->NewObject(exc, ctor, ex.GetErrorCode(), msg);
        jenv->Throw(jexc);
        return $null;
    }
}

%typemap(javabody) DasException %{
    private int errorCode;
    private String message;

    public DasException(int errorCode, String message) {
        this.errorCode = errorCode;
        this.message = message;
    }

    public int getErrorCode() {
        return errorCode;
    }

    public String getMessage() {
        return message;
    }
%}

%typemap(in, numinputs=1) (const char16_t* p_u16string, size_t length) %{
    class _das_InternalJavaString{
        const jchar* p_jstring_;
        JNIEnv* jenv_;
        jstring java_string_;
    public:
        _das_InternalJavaString(JNIEnv* jenv, jstring java_string)
            : p_jstring_{jenv->GetStringChars(java_string, nullptr)},
              jenv_{jenv}, java_string_{java_string}
        {
        }
        ~_das_InternalJavaString() { jenv_->ReleaseStringChars(java_string_, p_jstring_); }
        const jchar* Get() noexcept { return p_jstring_; }
    } jstring_wrapper{jenv, jarg1};
    const jsize string_length = jenv->GetStringLength(jarg1);
    static_assert(sizeof(jchar) == sizeof(char16_t), "Size of jchar is NOT equal to size of char16_t.");
    jchar* p_non_const_jstring = const_cast<jchar*>(jstring_wrapper.Get());
    $1 = reinterpret_cast<decltype($1)>(p_non_const_jstring);
    $2 = string_length;
%}

#endif

#ifdef SWIGPYTHON

%feature("director:except") {
    if ($error != NULL) {
        DAS_LOG_ERROR("SWIG Python exception found!");
        DAS::Core::ForeignInterfaceHost::PythonHost::RaisePythonInterpreterException();
    }
}

#endif // SWIGPYTHON

%include <das/DasExport.h>
%include <das/IDasBase.h>
%include <das/DasString.hpp>
%include <das/_autogen/idl/abi/IDasTypeInfo.h>

// !!! 包含CMake从IDL自动生成的SWIG接口汇总文件 !!!
// 该文件包含所有从IDL生成的SWIG接口（如DasCV, IDasCapture, IDasPluginManager等）
%include <das/_autogen/idl/swig/swig_all.i>

// 以下接口文件没有对应的IDL定义，需要手动包含（按字母顺序排列）
%include <das/_autogen/idl/abi/DasLogger.h>

%include <das/_autogen/idl/abi/IDasMemory.h>

// ============================================================================
// DasException Support for C#
// ============================================================================

%rename(ErrorCode) DasException::GetErrorCode;
%rename(Message) DasException::what;

%typemap(cscode) DasException %{
    private int errorCode;
    private string message;

    public DasException(int errorCode, string message) {
        this.errorCode = errorCode;
        this.message = message;
    }

    public int ErrorCode {
        get { return errorCode; }
    }

    public string Message {
        get { return message; }
    }
%}

%typemap(csout) DasException %{
    SWIG_CSharpSetPendingException(SWIG_CSharpApplicationException, "DasException occurred");
    throw $modulePINVOKE.SWIGPendingException.Retrieve();
%}

%exception DasException {
    try {
        $action
    }
    catch (const DasException& ex) {
        SWIG_CSharpSetPendingException(SWIG_CSharpApplicationException, ex.what());
    }
}


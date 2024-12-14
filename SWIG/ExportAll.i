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
#include <das/IDasTypeInfo.h>

#include <das/PluginInterface/IDasCapture.h>
#include <das/PluginInterface/IDasErrorLens.h>
#include <das/PluginInterface/IDasInput.h>
#include <das/PluginInterface/IDasPluginPackage.h>
#include <das/PluginInterface/IDasTask.h>

#include <das/ExportInterface/DasCV.h>
#include <das/ExportInterface/DasJson.h>
#include <das/ExportInterface/DasLogger.h>

#include <das/ExportInterface/IDasGuidVector.h>
#include <das/ExportInterface/IDasBasicErrorLens.h>

#include <das/ExportInterface/IDasImage.h>
#include <das/ExportInterface/IDasCaptureManager.h>

#include <das/ExportInterface/IDasOcr.h>
#include <das/ExportInterface/IDasPluginManager.h>
#include <das/ExportInterface/IDasSettings.h>
#include <das/ExportInterface/IDasTaskManager.h>

#ifdef SWIGPYTHON

#include <das/Core/ForeignInterfaceHost/PythonHost.h>

#ifdef DEBUG

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

%}

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
%include <das/IDasTypeInfo.h>

// 以下文件按照字母顺序排列！ The following files are in alphabetical order!
// 例外：由于依赖关系的原因，IDasImage.h必须在IDasCaptureManager.h前
// IDasGuidVector.h必须在IDasBasicErrorLens.h前

%include <das/PluginInterface/IDasCapture.h>
%include <das/PluginInterface/IDasErrorLens.h>
%include <das/PluginInterface/IDasInput.h>
%include <das/PluginInterface/IDasPluginPackage.h>
%include <das/PluginInterface/IDasTask.h>

%include <das/ExportInterface/DasCV.h>
%include <das/ExportInterface/DasJson.h>
%include <das/ExportInterface/DasLogger.h>

%include <das/ExportInterface/IDasGuidVector.h>
%include <das/ExportInterface/IDasBasicErrorLens.h>

%include <das/ExportInterface/IDasImage.h>
%include <das/ExportInterface/IDasCaptureManager.h>

%include <das/ExportInterface/IDasOcr.h>
%include <das/ExportInterface/IDasPluginManager.h>
%include <das/ExportInterface/IDasSettings.h>
%include <das/ExportInterface/IDasTaskManager.h>

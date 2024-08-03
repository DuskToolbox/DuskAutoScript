// clang-format off

%module(directors="1") AutoStarRail

%include <stdint.i>
%include <typemaps.i>
%include <cpointer.i>
%include <std_string.i>
%include <std_vector.i>
%include <wchar.i>

%{
#include <AutoStarRail/AsrExport.h>
#include <AutoStarRail/IAsrBase.h>
#include <AutoStarRail/AsrString.hpp>
#include <AutoStarRail/IAsrTypeInfo.h>

#include <AutoStarRail/PluginInterface/IAsrCapture.h>
#include <AutoStarRail/PluginInterface/IAsrErrorLens.h>
#include <AutoStarRail/PluginInterface/IAsrInput.h>
#include <AutoStarRail/PluginInterface/IAsrPlugin.h>
#include <AutoStarRail/PluginInterface/IAsrTask.h>

#include <AutoStarRail/ExportInterface/AsrCV.h>
#include <AutoStarRail/ExportInterface/AsrJson.h>
#include <AutoStarRail/ExportInterface/AsrLogger.h>

#include <AutoStarRail/ExportInterface/IAsrGuidVector.h>
#include <AutoStarRail/ExportInterface/IAsrBasicErrorLens.h>

#include <AutoStarRail/ExportInterface/IAsrImage.h>
#include <AutoStarRail/ExportInterface/IAsrCaptureManager.h>

#include <AutoStarRail/ExportInterface/IAsrOcr.h>
#include <AutoStarRail/ExportInterface/IAsrPluginManager.h>
#include <AutoStarRail/ExportInterface/IAsrSettings.h>
#include <AutoStarRail/ExportInterface/IAsrTaskManager.h>

#ifdef SWIGPYTHON

#include <AutoStarRail/Core/ForeignInterfaceHost/PythonHost.h>

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
PyInit__AsrCorePythonExport(void) {
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

%typemap(jni) void AsrReadOnlyString::GetUtf16 "jstring"
%typemap(jtype) void AsrReadOnlyString::GetUtf16 "String"
%typemap(jstype) void AsrReadOnlyString::GetUtf16 "String"
%typemap(javaout) void AsrReadOnlyString::GetUtf16 {
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
    class _asr_InternalJavaString{
        const jchar* p_jstring_;
        JNIEnv* jenv_;
        jstring java_string_;
    public:
        _asr_InternalJavaString(JNIEnv* jenv, jstring java_string)
            : p_jstring_{jenv->GetStringChars(java_string, nullptr)},
              jenv_{jenv}, java_string_{java_string}
        {
        }
        ~_asr_InternalJavaString() { jenv_->ReleaseStringChars(java_string_, p_jstring_); }
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
        ASR_LOG_ERROR("SWIG Python exception found!");
        ASR::Core::ForeignInterfaceHost::PythonHost::RaisePythonInterpreterException();
    }
}

#endif // SWIGPYTHON

%include <AutoStarRail/AsrExport.h>
%include <AutoStarRail/IAsrBase.h>
%include <AutoStarRail/AsrString.hpp>
%include <AutoStarRail/IAsrTypeInfo.h>

// 以下文件按照字母顺序排列！ The following files are in alphabetical order!
// 例外：由于依赖关系的原因，IAsrImage.h必须在IAsrCaptureManager.h前
// IAsrGuidVector.h必须在IAsrBasicErrorLens.h前

%include <AutoStarRail/PluginInterface/IAsrCapture.h>
%include <AutoStarRail/PluginInterface/IAsrErrorLens.h>
%include <AutoStarRail/PluginInterface/IAsrInput.h>
%include <AutoStarRail/PluginInterface/IAsrPlugin.h>
%include <AutoStarRail/PluginInterface/IAsrTask.h>

%include <AutoStarRail/ExportInterface/AsrCV.h>
%include <AutoStarRail/ExportInterface/AsrJson.h>
%include <AutoStarRail/ExportInterface/AsrLogger.h>

%include <AutoStarRail/ExportInterface/IAsrGuidVector.h>
%include <AutoStarRail/ExportInterface/IAsrBasicErrorLens.h>

%include <AutoStarRail/ExportInterface/IAsrImage.h>
%include <AutoStarRail/ExportInterface/IAsrCaptureManager.h>

%include <AutoStarRail/ExportInterface/IAsrOcr.h>
%include <AutoStarRail/ExportInterface/IAsrPluginManager.h>
%include <AutoStarRail/ExportInterface/IAsrSettings.h>
%include <AutoStarRail/ExportInterface/IAsrTaskManager.h>

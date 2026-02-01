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
#include <das/DasTypes.hpp>
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

%}

#ifdef SWIGJAVA
%typemap(jni) char16_t* "jstring"
%typemap(jtype) char16_t* "String"
%typemap(jstype) char16_t* "String"
%typemap(javadirectorin) char16_t* "$jniinput"
%typemap(javadirectorout) char16_t* "$javacall"

// ============================================================================
// IDasReadOnlyString* 参数映射到 DasReadOnlyString
// 由于 IDasReadOnlyString 被 SWIG_IGNORE，需要手动定义 typemap
// ============================================================================
%typemap(jni) IDasReadOnlyString* "jlong"
%typemap(jtype) IDasReadOnlyString* "long"
%typemap(jstype) IDasReadOnlyString* "DasReadOnlyString"
%typemap(javain) IDasReadOnlyString* "DasReadOnlyString.getCPtr($javainput)"
%typemap(javaout) IDasReadOnlyString* {
    long cPtr = $jnicall;
    return (cPtr == 0) ? null : new DasReadOnlyString(cPtr, $owner);
}
%typemap(in) IDasReadOnlyString* %{
    $1 = *(IDasReadOnlyString **)&$input;
%}
%typemap(out) IDasReadOnlyString* %{
    *(IDasReadOnlyString **)&$result = $1;
%}
// Director typemaps for IDasReadOnlyString*
%typemap(directorin, descriptor="J") IDasReadOnlyString* %{
    *(IDasReadOnlyString **)&$input = $1;
%}
%typemap(directorout) IDasReadOnlyString* %{
    $result = *(IDasReadOnlyString **)&$input;
%}
%typemap(javadirectorin) IDasReadOnlyString* "new DasReadOnlyString($jniinput, false)"
%typemap(javadirectorout) IDasReadOnlyString* "DasReadOnlyString.getCPtr($javacall)"

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

// ============================================================================
// IDasBase Java Support - as() 泛型方法
// 提供类型安全的接口转换，通过反射和缓存机制实现
// ============================================================================
#ifdef SWIGJAVA

// ============================================================================
// DasRetBase - IDasBase 的返回包装类
// 用于封装 QueryInterface 的返回值
// ============================================================================
%inline %{
#ifndef DAS_RET_BASE
#define DAS_RET_BASE
struct DasRetBase {
    DasResult error_code;
    IDasBase* value;

    DasRetBase() : error_code(DAS_E_UNDEFINED_RETURN_VALUE), value(nullptr) {}

    DasResult GetErrorCode() const { return error_code; }

    IDasBase* GetValue() const { return value; }

    bool IsOk() const { return DAS::IsOk(error_code); }
};
#endif // DAS_RET_BASE
%}

// 为 DasRetBase 添加 Java 便捷方法
%typemap(javacode) DasRetBase %{
    /**
     * 获取值，如果操作失败则抛出异常
     * @return 结果值
     * @throws DasException 当操作失败时
     */
    public IDasBase getValueOrThrow() throws DasException {
        if (!IsOk()) {
            throw new DasException(GetErrorCode(), "DasRetBase operation failed");
        }
        return GetValue();
    }
%}

// 隐藏原始的 QueryInterface 方法
%ignore IDasBase::QueryInterface;

// 添加返回 DasRetBase 的 QueryInterface 包装方法
%extend IDasBase {
    DasRetBase QueryInterface(const DasGuid& iid) {
        DasRetBase result;
        result.error_code = $self->QueryInterface(iid, reinterpret_cast<void**>(&result.value));
        return result;
    }
}

%typemap(javacode) IDasBase %{
    // ========================================================================
    // 反射缓存 - 用于优化 as() 方法的性能
    // ========================================================================
    private static final java.util.concurrent.ConcurrentHashMap<Class<?>, java.lang.reflect.Constructor<?>> ctorCache = 
        new java.util.concurrent.ConcurrentHashMap<>();
    private static final java.util.concurrent.ConcurrentHashMap<Class<?>, java.lang.reflect.Method> iidCache = 
        new java.util.concurrent.ConcurrentHashMap<>();

    /**
     * 类型安全的接口转换
     * <p>
     * 内部调用 QueryInterface 验证类型，确保转换安全。
     * 首次调用时会进行反射查找，后续调用使用缓存，性能接近直接调用。
     * </p>
     * <p>
     * <b>引用计数说明：</b><br>
     * QueryInterface 会增加引用计数，返回的新对象拥有独立的引用。
     * 原对象不受影响，仍然有效。
     * </p>
     * 
     * @param <T> 目标接口类型，必须继承自 IDasBase
     * @param targetClass 目标接口的 Class 对象
     * @return 转换后的接口对象
     * @throws DasException 如果转换失败（类型不兼容）
     * @throws IllegalStateException 如果当前对象不拥有内存所有权
     * @throws IllegalArgumentException 如果目标类不是有效的接口类型
     */
    @SuppressWarnings("unchecked")
    public final <T extends IDasBase> T as(Class<T> targetClass) throws DasException {
        if (!swigCMemOwn) {
            throw new IllegalStateException(
                "Cannot convert: this object does not own memory.");
        }
        
        try {
            // 获取目标类型的 IID（使用缓存）
            java.lang.reflect.Method iidMethod = iidCache.computeIfAbsent(targetClass, cls -> {
                try {
                    return cls.getMethod("IID");
                } catch (NoSuchMethodException e) {
                    throw new RuntimeException("Target class " + cls.getName() + " does not have IID() method", e);
                }
            });
            DasGuid targetIid = (DasGuid) iidMethod.invoke(null);
            
            // 调用 QueryInterface 验证类型并获取新的引用
            // 注意：QueryInterface 会增加引用计数，返回的指针有独立的引用
            DasRetBase ret = QueryInterface(targetIid);
            if (DuskAutoScript.IsFailed(ret.GetErrorCode())) {
                throw new DasException(ret.GetErrorCode(), 
                    "QueryInterface failed for " + targetClass.getName());
            }
            
            // 使用工厂方法创建目标类型实例（使用缓存）
            java.lang.reflect.Method factoryMethod = targetClass.getMethod("createFromPtr", long.class, boolean.class);
            long newPtr = IDasBase.getCPtr(ret.GetValue());
            return (T) factoryMethod.invoke(null, newPtr, true);
        } catch (DasException e) {
            throw e;
        } catch (Exception e) {
            throw new RuntimeException("Failed to convert to " + targetClass.getName(), e);
        }
    }

    /**
     * 检查是否可以转换为目标类型
     * <p>
     * 此方法通过 QueryInterface 验证类型兼容性，但不创建新对象。
     * 可用于在执行实际转换前进行检查。
     * </p>
     * 
     * @param targetClass 目标接口的 Class 对象
     * @return true 如果可以转换，false 如果不兼容
     */
    public final boolean canCastTo(Class<? extends IDasBase> targetClass) {
        if (!swigCMemOwn) {
            return false;
        }
        
        try {
            java.lang.reflect.Method iidMethod = iidCache.computeIfAbsent(targetClass, cls -> {
                try {
                    return cls.getMethod("IID");
                } catch (NoSuchMethodException e) {
                    throw new RuntimeException(e);
                }
            });
            DasGuid targetIid = (DasGuid) iidMethod.invoke(null);
            
            DasRetBase ret = QueryInterface(targetIid);
            if (DuskAutoScript.IsOk(ret.GetErrorCode())) {
                // QueryInterface 成功，需要释放返回的引用
                IDasBase tempObj = ret.GetValue();
                if (tempObj != null) {
                    tempObj.delete();
                }
                return true;
            }
            return false;
        } catch (Exception e) {
            return false;
        }
    }
%}
#endif // SWIGJAVA

%include <das/DasExport.h>
%include <das/DasTypes.hpp>
%include <das/IDasBase.h>
%include <das/DasString.hpp>
%include <das/DasException.hpp>

%nodefaultctor DasException;

// !!! 包含CMake从IDL自动生成的SWIG接口汇总文件 !!!
// 该文件包含所有从IDL生成的SWIG接口（如DasCV, IDasCapture, IDasPluginManager等）
%include <das/_autogen/idl/swig/swig_all.i>



#ifdef SWIGJAVA

// ============================================================================
// DasException Support for Java
// ============================================================================

// Rename DasException methods for Java naming convention
%rename(ErrorCode) DasException::GetErrorCode;
%rename(Message) DasException::what;

// Exception handling for DasException
%javaexception("DasException") DasException {
    // $action
    // try {
    //     return $result;
    // }
    // catch (const DasException& ex) {
    //     jclass exc = jenv->FindClass("DasException");
    //     jmethodID ctor = jenv->GetMethodID(exc, "<init>", "(ILjava/lang/String;)V");
    //     jstring msg = jenv->NewStringUTF(ex.what());
    //     jobject jexc = jenv->NewObject(exc, ctor, ex.GetErrorCode(), msg);
    //     jenv->Throw(jexc);
    //     return $null;
    // }
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

#endif // SWIGJAVA


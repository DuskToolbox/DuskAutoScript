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
// #ifdef __cplusplus
// extern "C"
// #endif
// SWIGEXPORT
// #if PY_VERSION_HEX >= 0x03000000
// PyObject*
// #else
// void
// #endif
// SWIG_init(void);

// #ifdef __cplusplus
// extern "C"
// #endif
// SWIGEXPORT
// #if PY_VERSION_HEX >= 0x03000000
// PyObject*
// #else
// void
// #endif
// PyInit__DasCorePythonExport(void) {
// #if PY_VERSION_HEX >= 0x03000000
//     return ::SWIG_init();
// #else
//     ::SWIG_init();
// #endif
// }

#endif // DEBUG

#endif // SWIGPYTHON

%}

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

// Java 命名规范：将 PascalCase 方法 rename 为小驼峰
// 注意：%rename 和 %ignore 必须放在 struct 定义之前才能生效
%rename("getErrorCode") DasRetBase::GetErrorCode;
%rename("setErrorCode") DasRetBase::SetErrorCode;
%rename("getValue") DasRetBase::GetValue;
%rename("setValue") DasRetBase::SetValue;
%rename("isOk") DasRetBase::IsOk;
// 隐藏 public 字段的自动 getter/setter，避免重复方法
%ignore DasRetBase::error_code;
%ignore DasRetBase::value;

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

// 为 DasRetBase 添加 Java 便捷方法
%typemap(javacode) DasRetBase %{
    /**
     * 获取值，如果操作失败则抛出异常
     * @return 结果值
     * @throws DasException 当操作失败时
     */
    public IDasBase getValueOrThrow() throws DasException {
        if (!isOk()) {
            throw new DasException(getErrorCode(), "DasRetBase operation failed");
        }
        return getValue();
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
            if (DuskAutoScript.IsFailed(ret.getErrorCode())) {
                throw new DasException(ret.getErrorCode(),
                    "QueryInterface failed for " + targetClass.getName());
            }

            // 使用工厂方法创建目标类型实例（使用缓存）
            java.lang.reflect.Method factoryMethod = targetClass.getMethod("createFromPtr", long.class, boolean.class);
            long newPtr = IDasBase.getCPtr(ret.getValue());
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
            if (DuskAutoScript.IsOk(ret.getErrorCode())) {
                // QueryInterface 成功，需要释放返回的引用
                IDasBase tempObj = ret.getValue();
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

// ============================================================================
// 包含所有%ignore定义（必须在类定义之前）
// 这些%ignore指令用于隐藏带[out]参数的原始方法
// ============================================================================
%include <DasTypeMapsIgnore.i>

%include <das/DasExport.h>
%include <das/DasTypes.hpp>
%include <das/IDasBase.h>

#ifdef SWIGJAVA
// ============================================================================
// DasReadOnlyString Java Support - 从 Java String 创建
// ============================================================================
%typemap(javacode) DasReadOnlyString %{
    /**
     * 从 Java String 创建 DasReadOnlyString
     * <p>
     * Java String 内部使用 UTF-16 编码，此方法将其转换为 DasReadOnlyString。
     * </p>
     * @param str Java 字符串
     * @return 新的 DasReadOnlyString 实例
     */
    public static DasReadOnlyString fromString(String str) {
        if (str == null) {
            return new DasReadOnlyString();
        }
        // Java String 是 UTF-16 编码，获取 char 数组
        char[] chars = str.toCharArray();
        // 转换为 SWIGTYPE_p_char16_t（Java char 就是 16 位，与 char16_t 兼容）
        return new DasReadOnlyString(
            DuskAutoScriptJNI.new_DasReadOnlyString__SWIG_2_helper(str),
            true
        );
    }
    
    /**
     * 将 DasReadOnlyString 转换为 Java String
     * @return Java 字符串，如果转换失败则返回空字符串
     */
    public String toJavaString() {
        return DuskAutoScriptJNI.DasReadOnlyString_toJavaString_helper(swigCPtr, this);
    }
%}

// JNI 辅助方法 - 从 Java String 创建 DasReadOnlyString
%native(new_DasReadOnlyString__SWIG_2_helper) jlong new_DasReadOnlyString__SWIG_2_helper(jstring str);
%{
SWIGEXPORT jlong JNICALL Java_org_das_DuskAutoScriptJNI_new_1DasReadOnlyString_1_1SWIG_12_1helper(
    JNIEnv *jenv, jclass jcls, jstring jstr) {
    DasReadOnlyString *result = nullptr;
    if (jstr) {
        const jchar *chars = jenv->GetStringChars(jstr, nullptr);
        jsize len = jenv->GetStringLength(jstr);
        if (chars) {
            result = new DasReadOnlyString(reinterpret_cast<const char16_t*>(chars), static_cast<size_t>(len));
            jenv->ReleaseStringChars(jstr, chars);
        }
    }
    if (!result) {
        result = new DasReadOnlyString();
    }
    return (jlong)result;
}
%}

// JNI 辅助方法 - 将 DasReadOnlyString 转换为 Java String
%native(DasReadOnlyString_toJavaString_helper) jstring DasReadOnlyString_toJavaString_helper(jlong ptr, jobject obj);
%{
SWIGEXPORT jstring JNICALL Java_org_das_DuskAutoScriptJNI_DasReadOnlyString_1toJavaString_1helper(
    JNIEnv *jenv, jclass jcls, jlong jptr, jobject jobj) {
    DasReadOnlyString *self = reinterpret_cast<DasReadOnlyString*>(jptr);
    if (!self) {
        return jenv->NewStringUTF("");
    }
    const char16_t* utf16_str = nullptr;
    size_t utf16_len = 0;
    self->GetUtf16(&utf16_str, &utf16_len);
    if (utf16_str && utf16_len > 0) {
        return jenv->NewString(reinterpret_cast<const jchar*>(utf16_str), static_cast<jsize>(utf16_len));
    }
    return jenv->NewStringUTF("");
}
%}

// ============================================================================
// IDasReadOnlyString* Java Typemap -> DasReadOnlyString
//
// 背景：IDasReadOnlyString 被 SWIG_IGNORE，因此不会生成 Java 包装类。
// 当某些自动生成的返回结构体字段类型为 IDasReadOnlyString* 时，SWIG 默认只能用
// SWIGTYPE_p_IDasReadOnlyString，导致 Java API 不友好。
// 这里通过 typemap 将 IDasReadOnlyString* 映射为 Java 的 DasReadOnlyString。
// ============================================================================

// 1) jni: JNI C/C++ 包装层使用的 JNI 类型（这里用 jlong 传递指针）
%typemap(jni) IDasReadOnlyString * "jlong"

// 2) jtype: 生成的 *JNI.java 中对应的 Java 类型（与 jni 对应，这里是 long）
%typemap(jtype) IDasReadOnlyString * "long"

// 3) jstype: 对最终用户暴露的 Java 类型（这里希望看到 DasReadOnlyString）
%typemap(jstype) IDasReadOnlyString * "DasReadOnlyString"

// 4) javain: Java -> JNI 的参数转换
//    用户传入 DasReadOnlyString，JNI 侧拿到的是它内部持有的 C++ 指针（DasReadOnlyString*）
%typemap(javain) IDasReadOnlyString * "$javainput == null ? 0 : DasReadOnlyString.getCPtr($javainput)"

// 5) javaout: JNI -> Java 的返回值转换
//    约定：%typemap(out) 会返回一个"新分配的 DasReadOnlyString*"，因此这里用 (cPtr,true) 让 Java 拥有并 delete()
%typemap(javaout) IDasReadOnlyString * {
    long cPtr = $jnicall;
    return (cPtr == 0) ? null : new DasReadOnlyString(cPtr, true);
}

// 6) (配套) in: JNI 输入到 C++ 真实类型的转换
//    $input 是 jlong（实际是 DasReadOnlyString*），这里取出其底层接口指针 IDasReadOnlyString*
%typemap(in) IDasReadOnlyString * %{
    DasReadOnlyString* tmp = reinterpret_cast<DasReadOnlyString*>($input);
    $1 = tmp ? tmp->Get() : nullptr;
%}

// 7) (配套) out: C++ 返回到 JNI 的转换
//    C++ 层返回的是 IDasReadOnlyString*；为了让 Java 侧始终拿到 DasReadOnlyString 代理对象，
//    这里创建一个新的 DasReadOnlyString 包装它，然后把 DasReadOnlyString* 作为 jlong 返回。
%typemap(out) IDasReadOnlyString * %{
    if ($1) {
        DasReadOnlyString* tmp = new DasReadOnlyString($1);
        $result = (jlong)tmp;
    } else {
        $result = 0;
    }
%}

#endif // SWIGJAVA

// ============================================================================
// DasException Support for Java
// ============================================================================

// Rename DasException methods for Java naming convention
%rename(ErrorCode) DasException::GetErrorCode;
%rename(Message) DasException::what;

#ifdef SWIGJAVA
// Make DasException extend RuntimeException
%typemap(javabase) DasException "RuntimeException"

%typemap(javabody) DasException %{
    private transient long swigCPtr;
    protected transient boolean swigCMemOwn;

    protected $javaclassname(long cPtr, boolean cMemoryOwn) {
        super(getMessageFromPtr(cPtr));
        swigCMemOwn = cMemoryOwn;
        swigCPtr = cPtr;
    }

    protected static long getCPtr($javaclassname obj) {
        return (obj == null) ? 0 : obj.swigCPtr;
    }

    protected static long swigRelease($javaclassname obj) {
        long ptr = 0;
        if (obj != null) {
            if (!obj.swigCMemOwn)
                throw new RuntimeException("Cannot release ownership as memory is not owned");
            ptr = obj.swigCPtr;
            obj.swigCMemOwn = false;
            obj.delete();
        }
        return ptr;
    }

    private static String getMessageFromPtr(long cPtr) {
        if (cPtr == 0) return "";
        return DasExportJNI.DasException_Message(cPtr, null);
    }
%}

%typemap(javacode) DasException %{
    /**
     * 创建一个 DasException
     * @param errorCode 错误码
     * @param sourceFile 源文件名
     * @param sourceLine 源文件行号
     * @param sourceFunction 源函数名
     * @return 新创建的 DasException
     */
    public static DasException create(int errorCode, String sourceFile, int sourceLine, String sourceFunction) {
        DasExceptionSourceInfoSwig sourceInfo = new DasExceptionSourceInfoSwig();
        sourceInfo.setFile(sourceFile);
        sourceInfo.setLine(sourceLine);
        sourceInfo.setFunction(sourceFunction);
        IDasExceptionString exStr = DasExport.CreateDasExceptionStringSwig(errorCode, sourceInfo);
        return new DasException(errorCode, exStr);
    }

    /**
     * 创建一个带类型信息的 DasException
     * @param errorCode 错误码
     * @param sourceFile 源文件名
     * @param sourceLine 源文件行号
     * @param sourceFunction 源函数名
     * @param typeInfo 类型信息
     * @return 新创建的 DasException
     */
    public static DasException createWithTypeInfo(int errorCode, String sourceFile, int sourceLine, String sourceFunction, IDasTypeInfo typeInfo) {
        DasExceptionSourceInfoSwig sourceInfo = new DasExceptionSourceInfoSwig();
        sourceInfo.setFile(sourceFile);
        sourceInfo.setLine(sourceLine);
        sourceInfo.setFunction(sourceFunction);
        IDasExceptionString exStr = DasExport.CreateDasExceptionStringWithTypeInfoSwig(errorCode, sourceInfo, typeInfo);
        return new DasException(errorCode, exStr);
    }

    /**
     * 用于 Ez 便捷方法抛出异常
     * @param errorCode 错误码
     * @param methodName 方法名
     * @return 新创建的 DasException
     */
    public static DasException fromErrorCode(int errorCode, String methodName) {
        return create(errorCode, "Java", 0, methodName);
    }
%}
#endif // SWIGJAVA

// ============================================================================
// 隐藏 DasReadOnlyString 中暴露 SWIGTYPE 的方法
// char16_t 构造函数和 GetUtf16 已由 fromString/toJavaString 替代
// ============================================================================
#ifdef SWIGJAVA
%ignore DasReadOnlyString::DasReadOnlyString(const char16_t*, size_t);
%ignore DasReadOnlyString::GetUtf16;
#endif // SWIGJAVA

%include <das/DasString.hpp>;
%include <das/DasException.hpp>;

%nodefaultctor DasException;

// !!! 包含CMake从IDL自动生成的SWIG接口汇总文件 !!!
// 该文件包含所有从IDL生成的SWIG接口（如DasCV, IDasCapture, IDasPluginManager等）
%include <das/_autogen/idl/swig/swig_all.i>

// ============================================================================
// 包含所有%extend和DasRetXxx定义（必须在类定义之后）
// 这些%extend指令用于添加返回DasRetXxx的包装方法
// ============================================================================
%include <DasTypeMapsExtend.i>


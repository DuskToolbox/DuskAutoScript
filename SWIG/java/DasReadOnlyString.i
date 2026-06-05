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
        // 转换为 DasReadOnlyString （Java char 就是 16 位，与 char16_t 兼容）
        return new DasReadOnlyString(
            DuskAutoScript.new_DasReadOnlyString__SWIG_2_helper(str),
            true
        );
    }

    /**
     * 将 DasReadOnlyString 转换为 Java String
     * @return Java 字符串，如果转换失败则返回空字符串
     */
    public String toJavaString() {
        return DuskAutoScript.DasReadOnlyString_toJavaString_helper(swigCPtr, this);
    }
%}

// JNI 辅助方法 - 从 Java String 创建 DasReadOnlyString
%native(new_DasReadOnlyString__SWIG_2_helper) jlong new_DasReadOnlyString__SWIG_2_helper(jstring str);
%{
extern "C" {
SWIGEXPORT jlong JNICALL Java_org_das_DuskAutoScriptJNI_new_1DasReadOnlyString_1_1SWIG_12_1helper(
    JNIEnv *jenv, jclass jcls, jstring jstr) {
    if (!jstr) {
        return (jlong)new DasReadOnlyString();
    }

    // Use RAII guard to manage string chars
    JniStringCharsGuard u16str_guard(jenv, jstr);
    if (!u16str_guard.IsValid()) {
        // GetStringChars failed, JVM has already set an exception
        return (jlong)new DasReadOnlyString();
    }

    jsize len = jenv->GetStringLength(jstr);
    const char16_t* p_u16string = reinterpret_cast<const char16_t*>(u16str_guard.Get());
    size_t length = static_cast<size_t>(len);

    try {
        DasReadOnlyString *result = new DasReadOnlyString(p_u16string, length);
        return (jlong)result;
    } catch (const std::bad_alloc&) {
        // Memory allocation failed, use ThrowNew to simplify
        jenv->ThrowNew(jenv->FindClass("java/lang/OutOfMemoryError"),
                       "Failed to allocate memory for DasReadOnlyString");
        return 0;
    } catch (const DasException& e) {
        // 清除可能残留的JNI异常
        if (jenv->ExceptionCheck()) {
            DasLogPendingJniException(jenv, "[JNI] Detected pending exception before rethrowing DasException.");
        }

        // 复制DasException对象 - 使用unique_ptr防止内存泄漏
        std::unique_ptr<DasException> pNewException(new (std::nothrow) DasException(e.GetErrorCode(), e.what()));
        if (!pNewException) {
            // 无法分配内存，使用ThrowNew简化
            jenv->ThrowNew(jenv->FindClass("java/lang/OutOfMemoryError"),
                           "Failed to allocate memory for DasException wrapper");
            return 0;
        }

        // 调用Java的DasException(long cPtr, boolean cMemoryOwn)构造函数
        JniLocalRefGuard dasExClass_guard(jenv, jenv->FindClass("org/das/DasException"));
        jclass dasExClass = (jclass)dasExClass_guard.Get();
        if (!dasExClass || jenv->ExceptionCheck()) {
            jenv->ExceptionClear();
            // unique_ptr会自动释放pNewException
            return 0;
        }

        jmethodID exConstructor = jenv->GetMethodID(dasExClass, "<init>", "(JZ)V");
        if (!exConstructor || jenv->ExceptionCheck()) {
            jenv->ExceptionClear();
            // unique_ptr会自动释放pNewException
            return 0;
        }

        JniLocalRefGuard exObj_guard(jenv, jenv->NewObject(dasExClass, exConstructor,
            reinterpret_cast<jlong>(pNewException.get()), JNI_TRUE));
        jobject exObj = exObj_guard.Get();

        if (!exObj || jenv->ExceptionCheck()) {
            jenv->ExceptionClear();
            // unique_ptr会自动释放pNewException
            return 0;
        }

        // 成功创建Java异常对象，释放unique_ptr的所有权
        pNewException.release();
        jenv->Throw((jthrowable)exObj);

        return 0;
    } catch (...) {
        // Unknown exception
        jenv->ThrowNew(jenv->FindClass("java/lang/RuntimeException"),
                       "Unknown exception occurred in new_DasReadOnlyString__SWIG_2_helper");
        return 0;
    }
}
}
%}

// JNI 辅助方法 - 将 DasReadOnlyString 转换为 Java String
%native(DasReadOnlyString_toJavaString_helper) jstring DasReadOnlyString_toJavaString_helper(jlong ptr, jobject obj);
%{
extern "C" {
SWIGEXPORT jstring JNICALL Java_org_das_DuskAutoScriptJNI_DasReadOnlyString_1toJavaString_1helper(
    JNIEnv *jenv, jclass jcls, jlong jptr, jobject jobj) {
    DasReadOnlyString *self = reinterpret_cast<DasReadOnlyString*>(jptr);
    if (!self) {
        return jenv->NewStringUTF("");
    }

    try {
        const char16_t* utf16_str = nullptr;
        size_t utf16_len = 0;
        self->BorrowUtf16(&utf16_str, &utf16_len);
        if (utf16_str && utf16_len > 0) {
            return jenv->NewString(reinterpret_cast<const jchar*>(utf16_str), static_cast<jsize>(utf16_len));
        }
        return jenv->NewStringUTF("");
    } catch (const DasException& e) {
        // 清除可能残留的JNI异常
        if (jenv->ExceptionCheck()) {
            DasLogPendingJniException(jenv, "[JNI] Detected pending exception before rethrowing DasException.");
        }

        // 复制DasException对象 - 使用unique_ptr防止内存泄漏
        std::unique_ptr<DasException> pNewException(new (std::nothrow) DasException(e.GetErrorCode(), e.what()));
        if (!pNewException) {
            // 无法分配内存，仍然抛出一个简单的异常
            JniLocalRefGuard oomExClass_guard(jenv, jenv->FindClass("java/lang/OutOfMemoryError"));
            if (oomExClass_guard.Get() && !jenv->ExceptionCheck()) {
                jmethodID oomConstructor = jenv->GetMethodID((jclass)oomExClass_guard.Get(), "<init>", "()V");
                if (oomConstructor && !jenv->ExceptionCheck()) {
                    JniLocalRefGuard oomExObj_guard(jenv, jenv->NewObject((jclass)oomExClass_guard.Get(), oomConstructor));
                    if (oomExObj_guard.Get() && !jenv->ExceptionCheck()) {
                        jenv->Throw((jthrowable)oomExObj_guard.Get());
                    }
                }
            }
            return jenv->NewStringUTF("");
        }

        // 调用Java的DasException构造函数
        JniLocalRefGuard dasExClass_guard(jenv, jenv->FindClass("org/das/DasException"));
        jclass dasExClass = (jclass)dasExClass_guard.Get();
        if (!dasExClass || jenv->ExceptionCheck()) {
            jenv->ExceptionClear();
            // unique_ptr会自动释放pNewException
            return jenv->NewStringUTF("");
        }

        jmethodID exConstructor = jenv->GetMethodID(dasExClass, "<init>", "(JZ)V");
        if (!exConstructor || jenv->ExceptionCheck()) {
            jenv->ExceptionClear();
            // unique_ptr会自动释放pNewException
            return jenv->NewStringUTF("");
        }

        JniLocalRefGuard exObj_guard(jenv, jenv->NewObject(dasExClass, exConstructor,
            reinterpret_cast<jlong>(pNewException.get()), JNI_TRUE));
        jobject exObj = exObj_guard.Get();

        if (!exObj || jenv->ExceptionCheck()) {
            jenv->ExceptionClear();
            // unique_ptr会自动释放pNewException
            return jenv->NewStringUTF("");
        }

        // 成功创建Java异常对象，释放unique_ptr的所有权
        pNewException.release();
        jenv->Throw((jthrowable)exObj);

        return jenv->NewStringUTF("");
    }
}
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
%typemap(jni) IDasReadOnlyString *, ::IDasReadOnlyString * "jlong"

// 2) jtype: 生成的 *JNI.java 中对应的 Java 类型（与 jni 对应，这里是 long）
%typemap(jtype) IDasReadOnlyString *, ::IDasReadOnlyString * "long"

// 3) jstype: 对最终用户暴露的 Java 类型（这里希望看到 DasReadOnlyString）
%typemap(jstype) IDasReadOnlyString *, ::IDasReadOnlyString * "DasReadOnlyString"

// 4) javain: Java -> JNI 的参数转换
//    用户传入 DasReadOnlyString，JNI 侧拿到的是它内部持有的 C++ 指针（DasReadOnlyString*）
%typemap(javain) IDasReadOnlyString *, ::IDasReadOnlyString * "$javainput == null ? 0 : DasReadOnlyString.getCPtr($javainput)"

// 5) javaout: JNI -> Java 的返回值转换
//    约定：%typemap(out) 会返回一个"新分配的 DasReadOnlyString*"，因此这里用 (cPtr,true) 让 Java 拥有并 delete()
%typemap(javaout) IDasReadOnlyString *, ::IDasReadOnlyString * {
    long cPtr = $jnicall;
    return (cPtr == 0) ? null : new DasReadOnlyString(cPtr, true);
}

// 6) (配套) in: JNI 输入到 C++ 真实类型的转换
//    $input 是 jlong（实际是 DasReadOnlyString*），这里取出其底层接口指针 IDasReadOnlyString*
%typemap(in) IDasReadOnlyString *, ::IDasReadOnlyString * %{
    {
        DasReadOnlyString* p_tmp = reinterpret_cast<DasReadOnlyString*>($input);
        $1 = p_tmp ? p_tmp->Get() : nullptr;
    }
%}

// 7) (配套) out: C++ 返回到 JNI 的转换
//    C++ 层返回的是 IDasReadOnlyString*；为了让 Java 侧始终拿到 DasReadOnlyString 代理对象，
//    这里创建一个新的 DasReadOnlyString 包装它，然后把 DasReadOnlyString* 作为 jlong 返回。
%typemap(out) IDasReadOnlyString *, ::IDasReadOnlyString * %{
    if ($1) {
        DasReadOnlyString* tmp = new DasReadOnlyString($1);
        $result = (jlong)tmp;
    } else {
        $result = 0;
    }
%}

// ============================================================================
// Director support: C++ -> Java callbacks with IDasReadOnlyString* params
//
// When a C++ base class calls a virtual method with IDasReadOnlyString* param,
// the director needs to convert it to a Java DasReadOnlyString object.
// Without these typemaps, SWIG generates duplicate director method declarations
// (one with ::IDasReadOnlyString* from headers, one with IDasReadOnlyString*
// from typemap resolution), causing C++ "class member cannot be redeclared" errors.
//
// 注意：同时为 IDasReadOnlyString* 和 IDasReadOnlyString** 定义 director typemaps，
// 因为 ABI 头文件中使用 ::IDasReadOnlyString* 和 ::IDasReadOnlyString** 两种形式。
// ============================================================================

// directorin: C++ calls Java virtual method — wrap IDasReadOnlyString* as DasReadOnlyString*
%typemap(directorin, descriptor="Lorg/das/DasReadOnlyString;") IDasReadOnlyString *, ::IDasReadOnlyString * %{
    if ($1) {
        DasReadOnlyString *wrapper = new DasReadOnlyString($1);
        $input = (jlong)wrapper;
    } else {
        $input = 0;
    }
%}

// javadirectorin: JNI long -> Java DasReadOnlyString in director callback
%typemap(javadirectorin) IDasReadOnlyString *, ::IDasReadOnlyString * "($jniinput == 0) ? null : new DasReadOnlyString($jniinput, true)"

// directorout: Java returns/produces DasReadOnlyString -> extract IDasReadOnlyString* for C++
%typemap(directorout) IDasReadOnlyString *, ::IDasReadOnlyString * %{
    if ($input) {
        DasReadOnlyString *wrapper = reinterpret_cast<DasReadOnlyString*>($input);
        $result = wrapper ? wrapper->Get() : nullptr;
    } else {
        $result = nullptr;
    }
%}

// javadirectorout: Java DasReadOnlyString -> JNI long for director return
%typemap(javadirectorout) IDasReadOnlyString *, ::IDasReadOnlyString * "DasReadOnlyString.getCPtr($javacall)"

// IDasReadOnlyString** director typemaps (for [out] parameters)
// [out] IDasReadOnlyString** params: pass as 0 to Java, handled via DasRetXxx return pattern
%typemap(directorin, descriptor="") IDasReadOnlyString **, ::IDasReadOnlyString ** ""
%typemap(javadirectorin) IDasReadOnlyString **, ::IDasReadOnlyString ** "0"
%typemap(directorout) IDasReadOnlyString **, ::IDasReadOnlyString ** ""
%typemap(javadirectorout) IDasReadOnlyString **, ::IDasReadOnlyString ** ""

// IDasReadOnlyString** jni/jtype typemaps — also needed for :: qualified form
// Without these, SWIG generates separate director entries for ::IDasReadOnlyString**
%typemap(jni) IDasReadOnlyString **, ::IDasReadOnlyString ** "jlong"
%typemap(jtype) IDasReadOnlyString **, ::IDasReadOnlyString ** "long"
%typemap(jstype) IDasReadOnlyString **, ::IDasReadOnlyString ** "long"
%typemap(javain) IDasReadOnlyString **, ::IDasReadOnlyString ** "$javainput"

#endif // SWIGJAVA

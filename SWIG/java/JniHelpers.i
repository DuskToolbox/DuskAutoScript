#ifdef SWIGJAVA

%{

// RAII wrapper for JNI local references
// Automatically deletes the local reference when it goes out of scope
struct JniLocalRefGuard {
    JNIEnv* env_;
    jobject ref_;

    JniLocalRefGuard(JNIEnv* env, jobject ref) : env_(env), ref_(ref) {}
    ~JniLocalRefGuard()
    {
        if (env_ && ref_)
        {
            env_->DeleteLocalRef(ref_);
        }
    }

    // Reset the reference and clear it
    void Reset()
    {
        if (env_ && ref_)
        {
            env_->DeleteLocalRef(ref_);
            ref_ = nullptr;
        }
    }

    // Get the underlying reference
    jobject Get() const { return ref_; }

    // Disable copy
    JniLocalRefGuard(const JniLocalRefGuard&) = delete;
    JniLocalRefGuard& operator=(const JniLocalRefGuard&) = delete;

    // Disable move (explicitly delete to avoid misuse)
    JniLocalRefGuard(JniLocalRefGuard&&) = delete;
    JniLocalRefGuard& operator=(JniLocalRefGuard&&) = delete;
};

// RAII wrapper for JNI string critical sections (GetStringChars/ReleaseStringChars)
// Automatically releases the string chars when it goes out of scope
struct JniStringCharsGuard {
    JNIEnv* env_;
    jstring jstr_;
    const jchar* chars_;

    JniStringCharsGuard(JNIEnv* env, jstring jstr)
        : env_(env), jstr_(jstr), chars_(nullptr)
    {
        if (env_ && jstr_)
        {
            chars_ = env_->GetStringChars(jstr_, nullptr);
        }
    }

    ~JniStringCharsGuard()
    {
        if (env_ && jstr_ && chars_)
        {
            env_->ReleaseStringChars(jstr_, chars_);
        }
    }

    // Get the underlying chars pointer
    const jchar* Get() const { return chars_; }

    // Check if the chars pointer is valid
    bool IsValid() const { return chars_ != nullptr; }

    // Disable copy and move
    JniStringCharsGuard(const JniStringCharsGuard&) = delete;
    JniStringCharsGuard& operator=(const JniStringCharsGuard&) = delete;
    JniStringCharsGuard(JniStringCharsGuard&&) = delete;
    JniStringCharsGuard& operator=(JniStringCharsGuard&&) = delete;
};

// Log JNI pending exception with stack trace (Reference: Oracle JNI best practices)
static void DasLogPendingJniException(JNIEnv* jenv, const char* context_u8)
{
    if (!jenv)
    {
        DasLogErrorU8("[JNI] JNIEnv is null in DasLogPendingJniException");
        return;
    }

    // Step 1: Immediately check and clear the pending exception (Oracle best practice)
    jthrowable exception = jenv->ExceptionOccurred();
    if (!exception)
    {
        // No pending exception
        return;
    }
    jenv->ExceptionClear(); // Clear immediately to allow further JNI calls

    // Use RAII guard for the exception object
    JniLocalRefGuard exception_guard(jenv, exception);

    // Step 2: Get Throwable class
    JniLocalRefGuard throwableClass_guard(jenv, jenv->FindClass("java/lang/Throwable"));
    if (jenv->ExceptionCheck() || !throwableClass_guard.Get())
    {
        jenv->ExceptionClear();
        DasLogErrorU8("[JNI] Failed to find java/lang/Throwable class");
        return;
    }
    jclass throwableClass = (jclass)throwableClass_guard.Get();

    // Step 3: Get toString method
    jmethodID toStringMethod = jenv->GetMethodID(throwableClass, "toString", "()Ljava/lang/String;");
    if (jenv->ExceptionCheck() || !toStringMethod)
    {
        jenv->ExceptionClear();
        DasLogErrorU8("[JNI] Failed to get toString method");
        return;
    }

    // Step 4: Call toString to get exception description
    JniLocalRefGuard toStringResult_guard(jenv, jenv->CallObjectMethod(exception, toStringMethod));
    if (jenv->ExceptionCheck() || !toStringResult_guard.Get())
    {
        jenv->ExceptionClear();
        DasLogErrorU8("[JNI] Failed to call toString on exception");
        return;
    }
    jstring jtoStringResult = (jstring)toStringResult_guard.Get();

    // Helper to log with context prefix
    auto logWithContext = [context_u8](const char* message) {
        if (context_u8 && *context_u8)
        {
            std::string full_message = std::string("[JNI] ") + context_u8 + ": " + message;
            DasLogErrorU8(full_message.c_str());
        }
        else
        {
            DasLogErrorU8(message);
        }
    };

    // Step 5: Get full stack trace using StringWriter + PrintWriter (Oracle best practice)
    // Find StringWriter class
    JniLocalRefGuard stringWriterClass_guard(jenv, jenv->FindClass("java/io/StringWriter"));
    if (jenv->ExceptionCheck() || !stringWriterClass_guard.Get())
    {
        jenv->ExceptionClear();
        // Log basic info and return
        const char* toString_u8 = jenv->GetStringUTFChars(jtoStringResult, nullptr);
        if (toString_u8)
        {
            logWithContext(toString_u8);
            jenv->ReleaseStringUTFChars(jtoStringResult, toString_u8);
        }
        else
        {
            if (jenv->ExceptionCheck())
            {
                jenv->ExceptionClear();
            }
            logWithContext("Failed to get string from exception (GetStringUTFChars failed)");
        }
        return;
    }
    jclass stringWriterClass = (jclass)stringWriterClass_guard.Get();

    // Get StringWriter constructor
    jmethodID stringWriterConstructor = jenv->GetMethodID(stringWriterClass, "<init>", "()V");
    if (jenv->ExceptionCheck() || !stringWriterConstructor)
    {
        jenv->ExceptionClear();
        const char* toString_u8 = jenv->GetStringUTFChars(jtoStringResult, nullptr);
        if (toString_u8)
        {
            logWithContext(toString_u8);
            jenv->ReleaseStringUTFChars(jtoStringResult, toString_u8);
        }
        else
        {
            if (jenv->ExceptionCheck())
            {
                jenv->ExceptionClear();
            }
            logWithContext("Failed to get string from exception (GetStringUTFChars failed)");
        }
        return;
    }

    // Create StringWriter instance
    JniLocalRefGuard stringWriter_guard(jenv, jenv->NewObject(stringWriterClass, stringWriterConstructor));
    if (jenv->ExceptionCheck() || !stringWriter_guard.Get())
    {
        jenv->ExceptionClear();
        const char* toString_u8 = jenv->GetStringUTFChars(jtoStringResult, nullptr);
        if (toString_u8)
        {
            logWithContext(toString_u8);
            jenv->ReleaseStringUTFChars(jtoStringResult, toString_u8);
        }
        else
        {
            if (jenv->ExceptionCheck())
            {
                jenv->ExceptionClear();
            }
            logWithContext("Failed to get string from exception (GetStringUTFChars failed)");
        }
        return;
    }
    jobject stringWriter = stringWriter_guard.Get();

    // Find PrintWriter class
    JniLocalRefGuard printWriterClass_guard(jenv, jenv->FindClass("java/io/PrintWriter"));
    if (jenv->ExceptionCheck() || !printWriterClass_guard.Get())
    {
        jenv->ExceptionClear();
        const char* toString_u8 = jenv->GetStringUTFChars(jtoStringResult, nullptr);
        if (toString_u8)
        {
            logWithContext(toString_u8);
            jenv->ReleaseStringUTFChars(jtoStringResult, toString_u8);
        }
        else
        {
            if (jenv->ExceptionCheck())
            {
                jenv->ExceptionClear();
            }
            logWithContext("Failed to get string from exception (GetStringUTFChars failed)");
        }
        return;
    }
    jclass printWriterClass = (jclass)printWriterClass_guard.Get();

    // Get PrintWriter constructor: (Ljava/io/Writer;)V
    jmethodID printWriterConstructor = jenv->GetMethodID(printWriterClass, "<init>", "(Ljava/io/Writer;)V");
    if (jenv->ExceptionCheck() || !printWriterConstructor)
    {
        jenv->ExceptionClear();
        const char* toString_u8 = jenv->GetStringUTFChars(jtoStringResult, nullptr);
        if (toString_u8)
        {
            logWithContext(toString_u8);
            jenv->ReleaseStringUTFChars(jtoStringResult, toString_u8);
        }
        else
        {
            if (jenv->ExceptionCheck())
            {
                jenv->ExceptionClear();
            }
            logWithContext("Failed to get string from exception (GetStringUTFChars failed)");
        }
        return;
    }

    // Create PrintWriter instance with StringWriter
    JniLocalRefGuard printWriter_guard(jenv, jenv->NewObject(printWriterClass, printWriterConstructor, stringWriter));
    if (jenv->ExceptionCheck() || !printWriter_guard.Get())
    {
        jenv->ExceptionClear();
        const char* toString_u8 = jenv->GetStringUTFChars(jtoStringResult, nullptr);
        if (toString_u8)
        {
            logWithContext(toString_u8);
            jenv->ReleaseStringUTFChars(jtoStringResult, toString_u8);
        }
        else
        {
            if (jenv->ExceptionCheck())
            {
                jenv->ExceptionClear();
            }
            logWithContext("Failed to get string from exception (GetStringUTFChars failed)");
        }
        return;
    }
    jobject printWriter = printWriter_guard.Get();

    // Get printStackTrace method: (Ljava/io/PrintWriter;)V
    jmethodID printStackTraceMethod = jenv->GetMethodID(throwableClass, "printStackTrace", "(Ljava/io/PrintWriter;)V");
    if (jenv->ExceptionCheck() || !printStackTraceMethod)
    {
        jenv->ExceptionClear();
        const char* toString_u8 = jenv->GetStringUTFChars(jtoStringResult, nullptr);
        if (toString_u8)
        {
            logWithContext(toString_u8);
            jenv->ReleaseStringUTFChars(jtoStringResult, toString_u8);
        }
        else
        {
            if (jenv->ExceptionCheck())
            {
                jenv->ExceptionClear();
            }
            logWithContext("Failed to get string from exception (GetStringUTFChars failed)");
        }
        return;
    }

    // Call printStackTrace to write to PrintWriter
    jenv->CallVoidMethod(exception, printStackTraceMethod, printWriter);
    if (jenv->ExceptionCheck())
    {
        jenv->ExceptionClear();
        const char* toString_u8 = jenv->GetStringUTFChars(jtoStringResult, nullptr);
        if (toString_u8)
        {
            logWithContext(toString_u8);
            jenv->ReleaseStringUTFChars(jtoStringResult, toString_u8);
        }
        else
        {
            if (jenv->ExceptionCheck())
            {
                jenv->ExceptionClear();
            }
            logWithContext("Failed to get string from exception (GetStringUTFChars failed)");
        }
        return;
    }

    // Get StringWriter.toString() method
    jmethodID stringWriterToString = jenv->GetMethodID(stringWriterClass, "toString", "()Ljava/lang/String;");
    if (jenv->ExceptionCheck() || !stringWriterToString)
    {
        jenv->ExceptionClear();
        const char* toString_u8 = jenv->GetStringUTFChars(jtoStringResult, nullptr);
        if (toString_u8)
        {
            logWithContext(toString_u8);
            jenv->ReleaseStringUTFChars(jtoStringResult, toString_u8);
        }
        else
        {
            if (jenv->ExceptionCheck())
            {
                jenv->ExceptionClear();
            }
            logWithContext("Failed to get string from exception (GetStringUTFChars failed)");
        }
        return;
    }

    // Call toString on StringWriter to get the full stack trace
    JniLocalRefGuard stackTraceString_guard(jenv, jenv->CallObjectMethod(stringWriter, stringWriterToString));
    if (jenv->ExceptionCheck() || !stackTraceString_guard.Get())
    {
        jenv->ExceptionClear();
        const char* toString_u8 = jenv->GetStringUTFChars(jtoStringResult, nullptr);
        if (toString_u8)
        {
            logWithContext(toString_u8);
            jenv->ReleaseStringUTFChars(jtoStringResult, toString_u8);
        }
        else
        {
            if (jenv->ExceptionCheck())
            {
                jenv->ExceptionClear();
            }
            logWithContext("Failed to get string from exception (GetStringUTFChars failed)");
        }
        return;
    }
    jstring jstackTrace = (jstring)stackTraceString_guard.Get();

    // Step 6: Log the full exception with stack trace
    const char* stackTrace_u8 = jenv->GetStringUTFChars(jstackTrace, nullptr);
    if (stackTrace_u8)
    {
        logWithContext(stackTrace_u8);
        jenv->ReleaseStringUTFChars(jstackTrace, stackTrace_u8);
    }
    else
    {
        if (jenv->ExceptionCheck())
        {
            jenv->ExceptionClear();
        }
        const char* toString_u8 = jenv->GetStringUTFChars(jtoStringResult, nullptr);
        if (toString_u8)
        {
            logWithContext(toString_u8);
            jenv->ReleaseStringUTFChars(jtoStringResult, toString_u8);
        }
        else
        {
            if (jenv->ExceptionCheck())
            {
                jenv->ExceptionClear();
            }
            logWithContext("Failed to get string from exception (GetStringUTFChars failed)");
        }
    }
}

%}

// ============================================================================
// IDasBase Java Support - as() 泛型方法
// 提供类型安全的接口转换，通过反射和缓存机制实现
// ============================================================================
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
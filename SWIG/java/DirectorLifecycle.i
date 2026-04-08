// DirectorLifecycle.i — Java bridge lifecycle support
// Provides DasSwigPreventManagedObject and DasSwigReleaseManagedObject
// for Java director objects.
//
// These functions are called from the bridge core (das_swig_generator.py)
// when a director object crosses the native boundary.
//
// Dependencies: JniHelpers.i (for JniLocalRefGuard, DasLogPendingJniException)

#ifdef SWIGJAVA
%{

// Java bridge lifecycle implementation
// Calls __das_bridge_prevent() / __das_bridge_release() on the Java object

static int DasJavaCallBridgeLifecycleMethod(JNIEnv* jenv, jobject self, const char* method_name)
{
    if (!jenv || !self || !method_name)
    {
        return -1;
    }
    JniLocalRefGuard cls_guard(jenv, jenv->GetObjectClass(self));
    jclass cls = static_cast<jclass>(cls_guard.Get());
    if (!cls)
    {
        DasLogPendingJniException(jenv, "GetObjectClass bridge lifecycle");
        return -1;
    }
    jmethodID mid = jenv->GetMethodID(cls, method_name, "()I");
    if (!mid)
    {
        DasLogPendingJniException(jenv, method_name);
        return -1;
    }
    int result = static_cast<int>(jenv->CallIntMethod(self, mid));
    DasLogPendingJniException(jenv, method_name);
    return result;
}

static int DasSwigPreventManagedObject(DasSwigRuntimeContext* p_context)
{
    if (!p_context || p_context->kind != DasSwigRuntimeKind::Java)
    {
        return -1;
    }
    // Note: We need to attach the current thread to get JNIEnv
    // The JavaVM* is stored in the runtime context
    JavaVM* java_vm = static_cast<JavaVM*>(p_context->java_vm);
    if (!java_vm)
    {
        return -1;
    }
    JNIEnv* jenv = nullptr;
    bool attached = false;
    int get_env_result = java_vm->GetEnv(reinterpret_cast<void**>(&jenv), JNI_VERSION_1_8);
    if (get_env_result == JNI_EDETACHED)
    {
        if (java_vm->AttachCurrentThread(reinterpret_cast<void**>(&jenv), nullptr) != JNI_OK)
        {
            return -1;
        }
        attached = true;
    }
    else if (get_env_result != JNI_OK)
    {
        return -1;
    }

    jobject java_self = static_cast<jobject>(p_context->java_self);
    int result = DasJavaCallBridgeLifecycleMethod(jenv, java_self, "__das_bridge_prevent");

    if (attached)
    {
        java_vm->DetachCurrentThread();
    }
    return result;
}

static int DasSwigReleaseManagedObject(DasSwigRuntimeContext* p_context)
{
    if (!p_context || p_context->kind != DasSwigRuntimeKind::Java)
    {
        return -1;
    }
    JavaVM* java_vm = static_cast<JavaVM*>(p_context->java_vm);
    if (!java_vm)
    {
        return -1;
    }
    JNIEnv* jenv = nullptr;
    bool attached = false;
    int get_env_result = java_vm->GetEnv(reinterpret_cast<void**>(&jenv), JNI_VERSION_1_8);
    if (get_env_result == JNI_EDETACHED)
    {
        if (java_vm->AttachCurrentThread(reinterpret_cast<void**>(&jenv), nullptr) != JNI_OK)
        {
            return -1;
        }
        attached = true;
    }
    else if (get_env_result != JNI_OK)
    {
        return -1;
    }

    jobject java_self = static_cast<jobject>(p_context->java_self);
    int result = DasJavaCallBridgeLifecycleMethod(jenv, java_self, "__das_bridge_release");

    if (attached)
    {
        java_vm->DetachCurrentThread();
    }
    return result;
}

%}
#endif // SWIGJAVA

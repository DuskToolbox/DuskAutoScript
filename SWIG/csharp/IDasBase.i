#ifdef SWIGCSHARP
// ============================================================================
// IDasBase C# 辅助方法
// ReleaseOwnership/IsOwnershipOwner: 用于 CastFrom 安全地转移内存所有权
// As<T>/CanCastTo<T>: 类型安全的接口转换
// ============================================================================
%typemap(cscode) IDasBase %{
    /// <summary>
    /// 检查当前对象是否拥有内存所有权
    /// </summary>
    public bool IsOwnershipOwner() {
        return swigCMemOwn;
    }

    /// <summary>
    /// 释放内存所有权，允许其他对象接管
    /// </summary>
    /// <exception cref="System.InvalidOperationException">当不拥有内存时抛出</exception>
    public void ReleaseOwnership() {
        if (!swigCMemOwn) {
            throw new System.InvalidOperationException("Cannot release ownership: memory is not owned.");
        }
        swigCMemOwn = false;
    }

    // 反射缓存，避免重复查找
    private static readonly System.Collections.Concurrent.ConcurrentDictionary<System.Type, System.Reflection.MethodInfo> _iidCache
        = new System.Collections.Concurrent.ConcurrentDictionary<System.Type, System.Reflection.MethodInfo>();
    private static readonly System.Collections.Concurrent.ConcurrentDictionary<System.Type, System.Reflection.MethodInfo> _factoryCache
        = new System.Collections.Concurrent.ConcurrentDictionary<System.Type, System.Reflection.MethodInfo>();

    /// <summary>
    /// 类型安全的接口转换
    /// </summary>
    /// <typeparam name="T">目标接口类型</typeparam>
    /// <returns>转换后的接口实例</returns>
    /// <exception cref="DasException">当转换失败时抛出</exception>
    public T As<T>() where T : IDasBase {
        if (!swigCMemOwn) {
            DasExceptionSourceInfoSwig sourceInfo = new DasExceptionSourceInfoSwig();
            sourceInfo.File = "IDasBase.cs";
            sourceInfo.Line = 0;
            sourceInfo.Function = "As";
            IDasExceptionString exStr = DuskAutoScript.CreateDasExceptionStringSwig((int)DasResult.DAS_E_INVALID_OPERATION, sourceInfo);
            throw new DasException((int)DasResult.DAS_E_INVALID_OPERATION, exStr);
        }
        System.Type targetType = typeof(T);
        System.Reflection.MethodInfo iidMethod = _iidCache.GetOrAdd(targetType, t =>
            t.GetMethod("IID", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static));
        if (iidMethod == null) {
            throw new System.ArgumentException("Target type does not have IID() method");
        }
        DasGuid targetIid = (DasGuid)iidMethod.Invoke(null, null);
        DasRetBase ret = QueryInterface(targetIid);
        if (!ret.IsOk()) {
            DasExceptionSourceInfoSwig sourceInfo2 = new DasExceptionSourceInfoSwig();
            sourceInfo2.File = "IDasBase.cs";
            sourceInfo2.Line = 0;
            sourceInfo2.Function = "As<" + targetType.Name + ">";
            IDasExceptionString exStr2 = DuskAutoScript.CreateDasExceptionStringSwig(ret.GetErrorCode(), sourceInfo2);
            throw new DasException(ret.GetErrorCode(), exStr2);
        }
        System.Reflection.MethodInfo factoryMethod = _factoryCache.GetOrAdd(targetType, t =>
            t.GetMethod("CreateFromPtr", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static));
        if (factoryMethod == null) {
            throw new System.ArgumentException("Target type does not have CreateFromPtr() method");
        }
        System.IntPtr newPtr = IDasBase.getCPtr(ret.GetValue()).Handle;
        return (T)factoryMethod.Invoke(null, new object[] { newPtr, true });
    }

    /// <summary>
    /// 检查是否可以转换为目标类型
    /// </summary>
    /// <typeparam name="T">目标接口类型</typeparam>
    /// <returns>true 如果可以转换，false 如果不兼容</returns>
    public bool CanCastTo<T>() where T : IDasBase {
        if (!swigCMemOwn) { return false; }
        System.Type targetType = typeof(T);
        System.Reflection.MethodInfo iidMethod = _iidCache.GetOrAdd(targetType, t =>
            t.GetMethod("IID", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static));
        if (iidMethod == null) { return false; }
        DasGuid targetIid = (DasGuid)iidMethod.Invoke(null, null);
        DasRetBase ret = QueryInterface(targetIid);
        if (ret.IsOk()) {
            IDasBase tempObj = ret.GetValue();
            if (tempObj != null) { tempObj.Dispose(); }
            return true;
        }
        return false;
    }
%}
#endif // SWIGCSHARP
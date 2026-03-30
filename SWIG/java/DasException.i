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
        return DuskAutoScriptJNI.DasException_what(cPtr, null);
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
        IDasExceptionString exStr = DuskAutoScript.CreateDasExceptionStringSwig(errorCode, sourceInfo);
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
        IDasExceptionString exStr = DuskAutoScript.CreateDasExceptionStringWithTypeInfoSwig(errorCode, sourceInfo, typeInfo);
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
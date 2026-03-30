#ifdef SWIGJAVA
%pragma(java) jniclasscode=%{
    static {
        System.loadLibrary("DasCoreJavaExport");
    }
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
#endif // SWIGJAVA
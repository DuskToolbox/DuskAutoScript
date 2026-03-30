// 添加返回 DasRetBase 的 QueryInterface 包装方法
%extend IDasBase {
    DasRetBase QueryInterface(const DasGuid& iid) {
        DasRetBase result;
        result.error_code = $self->QueryInterface(iid, reinterpret_cast<void**>(&result.value));
        return result;
    }
}

// 隐藏原始的 QueryInterface 方法
%ignore IDasBase::QueryInterface;
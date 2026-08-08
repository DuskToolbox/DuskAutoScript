#include <das/Core/i18n/IDasI18nImpl.h>

#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/Utils/CommonUtils.hpp>

#include <filesystem>
#include <new>

// C ABI 工厂：成功移交所有权（*pp_out 填充并已 AddRef），失败不填充。
// FromDirectory/FromFile 返回只读 IDasI18n 视图，CreateIDasMutableI18n 返回
// 可变 IDasMutableI18n。Impl 单类继承派生接口基类（DasMutableI18nImplBase），
// 其 QueryInterface 同时处理 IDasI18n（父）与 IDasMutableI18n（自身），因此
// 三工厂共用同一实现：static_cast 上行到目标接口即可。

namespace
{
    // 从 IDasReadOnlyString 取 UTF-8 C 字符串；失败返回 nullptr。
    const char* ReadUtf8(IDasReadOnlyString* p)
    {
        if (p == nullptr)
        {
            return nullptr;
        }
        const char* out = nullptr;
        if (!DAS::IsOk(p->GetUtf8(&out)))
        {
            return nullptr;
        }
        return out;
    }
} // namespace

DasResult CreateIDasI18nFromDirectory(
    IDasReadOnlyString*              directory_path_utf8,
    Das::ExportInterface::IDasI18n** pp_out_i18n)
{
    DAS_UTILS_CHECK_POINTER(directory_path_utf8);
    DAS_UTILS_CHECK_POINTER(pp_out_i18n);

    *pp_out_i18n = nullptr;
    const char* dir_utf8 = ReadUtf8(directory_path_utf8);
    if (dir_utf8 == nullptr)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    DAS::Core::i18n::IDasI18nImpl* impl = nullptr;
    try
    {
        impl = new DAS::Core::i18n::IDasI18nImpl{};
        impl->AddRef();
        const auto load_result =
            impl->LoadDirectory(std::filesystem::path(dir_utf8));
        if (DAS::IsFailed(load_result))
        {
            impl->Release();
            return load_result;
        }
        *pp_out_i18n = static_cast<Das::ExportInterface::IDasI18n*>(impl);
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        if (impl)
        {
            impl->Release();
        }
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult CreateIDasI18nFromFile(
    IDasReadOnlyString*              json_file_path_utf8,
    Das::ExportInterface::IDasI18n** pp_out_i18n)
{
    DAS_UTILS_CHECK_POINTER(json_file_path_utf8);
    DAS_UTILS_CHECK_POINTER(pp_out_i18n);

    *pp_out_i18n = nullptr;
    const char* file_utf8 = ReadUtf8(json_file_path_utf8);
    if (file_utf8 == nullptr)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    DAS::Core::i18n::IDasI18nImpl* impl = nullptr;
    try
    {
        impl = new DAS::Core::i18n::IDasI18nImpl{};
        impl->AddRef();
        // 经 IDasMutableI18n::MergeFromFile 复用单文件合并逻辑：单文件工厂等价
        // 于「空表 + 合并该文件」。
        Das::DasPtr<IDasReadOnlyString> path_str;
        ::CreateIDasReadOnlyStringFromUtf8(file_utf8, path_str.Put());
        const auto load_result =
            static_cast<Das::ExportInterface::IDasMutableI18n*>(impl)
                ->MergeFromFile(path_str.Get());
        if (DAS::IsFailed(load_result))
        {
            impl->Release();
            return load_result;
        }
        *pp_out_i18n = static_cast<Das::ExportInterface::IDasI18n*>(impl);
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        if (impl)
        {
            impl->Release();
        }
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult CreateIDasMutableI18n(
    Das::ExportInterface::IDasMutableI18n** pp_out_i18n)
{
    DAS_UTILS_CHECK_POINTER(pp_out_i18n);

    *pp_out_i18n = nullptr;
    try
    {
        auto* impl = new DAS::Core::i18n::IDasI18nImpl{};
        impl->AddRef();
        *pp_out_i18n = impl;
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

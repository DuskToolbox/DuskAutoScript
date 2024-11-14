#ifndef DAS_CORE_FOREIGNINTERFACEHOST_CPPSWIGINTEROP_H
#define DAS_CORE_FOREIGNINTERFACEHOST_CPPSWIGINTEROP_H

#include <das/DasPtr.hpp>
#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/ForeignInterfaceHost/IsCastAvailableImpl.hpp>
#include <das/Core/Logger/Logger.h>
#include <das/ExportInterface/IDasGuidVector.h>
#include <das/ExportInterface/IDasImage.h>
#include <das/IDasBase.h>
#include <das/PluginInterface/IDasCapture.h>
#include <das/PluginInterface/IDasComponent.h>
#include <das/PluginInterface/IDasErrorLens.h>
#include <das/PluginInterface/IDasInput.h>
#include <das/PluginInterface/IDasPlugin.h>
#include <das/PluginInterface/IDasTask.h>
#include <das/Utils/Expected.h>
#include <das/Utils/QueryInterface.hpp>
#include <cstdint>

DAS_NS_BEGIN

using CommonTypeInfoPtr =
    std::variant<DasPtr<IDasTypeInfo>, DasPtr<IDasSwigTypeInfo>>;

DAS_NS_END

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

template <class T>
concept is_asr_swig_interface = std::is_base_of_v<IDasSwigBase, T>;

template <class T>
concept is_asr_interface = std::is_base_of_v<IDasBase, T>;

template <class SwigT>
class SwigToCpp;

auto ConvertCppIidToSwigIid(const DasGuid& cpp_iid)
    -> DAS::Utils::Expected<DasGuid>;

// TODO: 实现这个函数
auto ConvertSwigIidToCppIid(const DasGuid& swig_iid)
    -> DAS::Utils::Expected<DasGuid>;

bool IsCppIid(const DasGuid& cpp_iid);

bool IsSwigIid(const DasGuid& swig_iid);

/**
 * @brief 使用SWIG接口iid和对象指针创建对应的C++对象包装
 * @param swig_iid swig 接口类型的iid
 * @param p_swig_object swig 对象指针
 * @param pp_out_cpp_object 输出的 cpp 对象
 * @return 操作结果
 */
DasResult CreateCppToSwigObject(
    const DasGuid& swig_iid,
    void*          p_swig_object,
    void**         pp_out_cpp_object);

auto CreateSwigToCppObject(const DasGuid& iid, void* p_cpp_object)
    -> DasRetSwigBase;

template <is_asr_interface T>
auto ConvertCppIidToSwigIid()
{
    return ConvertCppIidToSwigIid(DasIidOf<T>());
}

template <is_asr_swig_interface T>
auto ConvertSwigIidToCppIid() -> DAS::Utils::Expected<DasGuid>
{
    return ConvertSwigIidToCppIid(DasIidOf<T>());
}

struct FunctionArgumentsSeparator
{
};

/**
 * @brief
 * 这个函数适用于仅一个输出参数的情况，注意：在指定了输出参数后，再指定输入参数。
 * @tparam OutputArg
 * @tparam InputArgs
 * @param p_swig_object
 * @param output_arg
 * @param input_args
 * @return
 */
template <
    class T4Function,
    T4Function            FunctionPointer,
    is_asr_swig_interface SwigT,
    class OutputArg,
    class... InputArgs>
[[nodiscard]]
auto CallSwigMethod(
    SwigT*      p_swig_object,
    OutputArg&& output_arg,
    InputArgs&&... input_args)
{
    const auto result = (p_swig_object->*FunctionPointer)(
        std::forward<InputArgs>(input_args)...);
    *std::forward<OutputArg>(output_arg) =
        static_cast<std::remove_reference_t<decltype(*output_arg)>>(
            result.value);
    return result.error_code;
}

#define DAS_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD(                        \
    p_swig_object,                                                             \
    function_pointer,                                                          \
    ...)                                                                       \
    CallSwigMethod<decltype(function_pointer), function_pointer>(              \
        p_swig_object,                                                         \
        __VA_ARGS__)

/**
 * @brief This class can be seen as a smart pointer.
 * @tparam SwigT
 * @tparam T
 */
template <is_asr_swig_interface SwigT, is_asr_interface T>
class SwigToCppBase : public T
{
protected:
    DAS::DasPtr<SwigT> p_impl_;

public:
    template <class Other>
    explicit SwigToCppBase(DAS::DasPtr<Other> p_impl) : p_impl_{p_impl}
    {
    }

    template <class Other>
    explicit SwigToCppBase(SwigToCpp<Other> other) : p_impl_{other.p_impl_}
    {
    }

    template <class Other, class = std::enable_if<is_asr_swig_interface<Other>>>
    explicit SwigToCppBase(Other* p_other) : p_impl_{p_other}
    {
    }

    SwigToCppBase() = default;

    int64_t AddRef() final
    {
        try
        {
            return p_impl_->AddRef();
        }
        catch (std::exception& ex)
        {
            DAS_CORE_LOG_ERROR(ex.what());
            throw;
        }
    }

    int64_t Release() final
    {
        try
        {
            return p_impl_->Release();
        }
        catch (std::exception& ex)
        {
            DAS_CORE_LOG_ERROR(ex.what());
            throw;
        }
    }

    /**
     * @brief 只会接收CPP版本的IID
     * @param iid
     * @param pp_out_object
     * @return
     */
    DasResult QueryInterface(const DasGuid& iid, void** pp_out_object) final
    {
        DAS_UTILS_CHECK_POINTER(pp_out_object)
        // 先看能不能直接转换
        const auto get_default_query_interface_result =
            DAS::Utils::QueryInterface<T>(this, iid, pp_out_object);
        if (IsOk(get_default_query_interface_result)
            || get_default_query_interface_result != DAS_E_NO_INTERFACE)
        {
            return get_default_query_interface_result;
        }
        // 再看内部实现能不能直接转换
        if (const auto get_swig_query_interface_result =
                p_impl_->QueryInterface(iid);
            IsOk(get_swig_query_interface_result.error_code)
            || get_swig_query_interface_result.error_code != DAS_E_NO_INTERFACE)
        {
            *pp_out_object = get_swig_query_interface_result.value;
            return get_swig_query_interface_result.error_code;
        }
        // 最后看是不是要转换到子类
        if (const auto swig_iid = ConvertCppIidToSwigIid(iid); swig_iid)
        {
            DasRetSwigBase result;
            try
            {
                result = p_impl_->QueryInterface(swig_iid.value());
            }
            catch (std::exception& ex)
            {
                DAS_CORE_LOG_EXCEPTION(ex);
                return DAS_E_SWIG_INTERNAL_ERROR;
            }
            if (IsOk(result.error_code))
            {
                if (pp_out_object == nullptr)
                {
                    return DAS_E_INVALID_POINTER;
                }
                // 注意：此时虚表指针应该指向Swig生成的导演类的虚表
                return CreateCppToSwigObject(
                    swig_iid.value(),
                    result.value,
                    pp_out_object);
            }
            DasPtr<IDasReadOnlyString> predefined_error_explanation{};
            ::DasGetPredefinedErrorMessage(
                result.error_code,
                predefined_error_explanation.Put());
            DAS_CORE_LOG_ERROR(
                "Error happened in class IDasSwigBase. Error code: "
                "{}. Explanation: {}.",
                result.error_code,
                predefined_error_explanation);
            return DAS_E_NO_INTERFACE;
        }
        return DAS_E_NO_INTERFACE;
    };

    [[nodiscard]]
    auto Get() const noexcept
    {
        return p_impl_;
    }

    [[nodiscard]]
    T* operator->() const noexcept
    {
        return static_cast<T*>(this);
    }
    [[nodiscard]]
    T& operator*() const noexcept
    {
        return static_cast<T&>(*this);
    }
};

#define DAS_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD_IMPL_AND_HANDLE_EXCEPTION( \
    p_swig_object,                                                                \
    function_pointer,                                                             \
    ...)                                                                          \
    try                                                                           \
    {                                                                             \
        return DAS_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD(                    \
            p_swig_object,                                                        \
            function_pointer,                                                     \
            __VA_ARGS__);                                                         \
    }                                                                             \
    catch (const std::exception& ex)                                              \
    {                                                                             \
        DAS_CORE_LOG_ERROR(ex.what());                                            \
        return DAS_E_SWIG_INTERNAL_ERROR;                                         \
    }

template <is_asr_swig_interface SwigT, is_asr_interface T>
class SwigToCppTypeInfo : public SwigToCppBase<SwigT, T>
{
    static_assert(
        std::is_base_of_v<IDasSwigTypeInfo, SwigT>,
        "SwigT is not inherit from IDasSwigTypeInfo!");

    using Base = SwigToCppBase<SwigT, T>;

public:
    using Base::Base;
    /**
     * @brief 返回SWIG对象定义所有IID
     * @param pp_out_vector
     * @return
     */
    DasResult GetGuid(DasGuid* p_out_guid) override
    {
        DAS_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD_IMPL_AND_HANDLE_EXCEPTION(
            Base::p_impl_.Get(),
            &IDasSwigTypeInfo::GetGuid,
            p_out_guid);
    }

    DasResult GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
    {
        DAS_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD_IMPL_AND_HANDLE_EXCEPTION(
            Base::p_impl_.Get(),
            &IDasSwigTypeInfo::GetRuntimeClassName,
            pp_out_name)
    }
};

template <>
class SwigToCpp<IDasSwigBase> final
    : public SwigToCppBase<IDasSwigBase, IDasBase>
{
public:
    DAS_USING_BASE_CTOR(SwigToCppBase);
};

template <>
class SwigToCpp<IDasSwigTypeInfo> final
    : public SwigToCppTypeInfo<IDasSwigTypeInfo, IDasTypeInfo>
{
public:
    DAS_USING_BASE_CTOR(SwigToCppTypeInfo);
};

template <>
class SwigToCpp<IDasSwigErrorLens> final
    : public SwigToCppBase<IDasSwigErrorLens, IDasErrorLens>
{
public:
    DAS_USING_BASE_CTOR(SwigToCppBase);

    DasResult GetSupportedIids(IDasReadOnlyGuidVector** pp_out_iids) override;
    DasResult GetErrorMessage(
        IDasReadOnlyString*  locale_name,
        DasResult            error_code,
        IDasReadOnlyString** pp_out_string) override;
};

template <>
class SwigToCpp<IDasSwigTask> final
    : public SwigToCppTypeInfo<IDasSwigTask, IDasTask>
{
public:
    DAS_USING_BASE_CTOR(SwigToCppTypeInfo);

    DasResult OnRequestExit() override;
    DasResult Do(
        IDasReadOnlyString* p_environment_json,
        IDasReadOnlyString* p_task_settings_json) override;
    DasResult GetNextExecutionTime(DasDate* p_out_date) override;
    DasResult GetName(IDasReadOnlyString** pp_out_name) override;
    DasResult GetDescription(IDasReadOnlyString** pp_out_settings) override;
    DasResult GetLabel(IDasReadOnlyString** pp_out_label) override;
};

template <>
class SwigToCpp<IDasSwigGuidVector> final
    : public SwigToCppBase<IDasSwigGuidVector, IDasGuidVector>
{
public:
    DAS_USING_BASE_CTOR(SwigToCppBase);

    DAS_IMPL Size(size_t* p_out_size) override;
    DAS_IMPL At(size_t index, DasGuid* p_out_iid) override;
    DAS_IMPL Find(const DasGuid& iid) override;
    DAS_IMPL PushBack(const DasGuid& iid) override;
    DAS_IMPL ToConst(IDasReadOnlyGuidVector** pp_out_object) override;
};

template <>
class SwigToCpp<IDasSwigReadOnlyGuidVector> final
    : public SwigToCppBase<IDasSwigReadOnlyGuidVector, IDasReadOnlyGuidVector>
{
public:
    DAS_USING_BASE_CTOR(SwigToCppBase);

    DAS_IMPL Size(size_t* p_out_size) override;
    DAS_IMPL At(size_t index, DasGuid* p_out_iid) override;
    DAS_IMPL Find(const DasGuid& iid) override;
};

template <is_asr_swig_interface SwigT, is_asr_interface T>
class SwigToCppInput : public SwigToCppTypeInfo<SwigT, T>
{
    static_assert(
        std::is_base_of_v<IDasSwigInput, SwigT>,
        "SwigT is not inherit from SwigToCppInput!");

    using Base = SwigToCppTypeInfo<SwigT, T>;

public:
    using Base::Base;

    DAS_IMPL Click(int32_t x, int32_t y) override;
};

template <>
class SwigToCpp<IDasSwigInput> final
    : public SwigToCppInput<IDasSwigInput, IDasInput>
{
public:
    DAS_USING_BASE_CTOR(SwigToCppInput);
};

template <>
class SwigToCpp<IDasSwigTouch> final
    : public SwigToCppInput<IDasSwigTouch, IDasTouch>
{
public:
    DAS_USING_BASE_CTOR(SwigToCppInput);

    DAS_IMPL Swipe(DasPoint from, DasPoint to, int32_t duration_ms) override;
};

template <>
class SwigToCpp<IDasSwigInputFactory> final
    : public SwigToCppTypeInfo<IDasSwigInputFactory, IDasInputFactory>
{
public:
    DAS_USING_BASE_CTOR(SwigToCppTypeInfo);

    DasResult CreateInstance(
        IDasReadOnlyString* p_json_config,
        IDasInput**         pp_out_input) override;
};

template <>
class SwigToCpp<IDasSwigComponent> final
    : public SwigToCppTypeInfo<IDasSwigComponent, IDasComponent>
{
public:
    DAS_USING_BASE_CTOR(SwigToCppTypeInfo);

    DasResult Dispatch(
        IDasReadOnlyString* p_function_name,
        IDasVariantVector*  p_arguments,
        IDasVariantVector** pp_out_result) override;
};

DasResult CommonPluginEnumFeature(
    const CommonPluginPtr& p_this,
    size_t                 index,
    DasPluginFeature*      p_out_feature);

template <class T>
class CppToSwig;

template <is_asr_swig_interface SwigT, is_asr_interface T>
class CppToSwigBase : public SwigT
{
public:
    using SwigType = SwigT;

protected:
    DAS::DasPtr<T> p_impl_;

public:
    template <class Other>
    CppToSwigBase(DAS::DasPtr<Other> p_impl) : p_impl_{p_impl}
    {
    }

    template <class Other>
    explicit CppToSwigBase(CppToSwig<Other> other) : p_impl_{other.p_impl_}
    {
    }

    template <class Other, class = std::enable_if<is_asr_interface<Other>>>
    explicit CppToSwigBase(Other* p_other) : p_impl_{p_other}
    {
    }

    int64_t AddRef() final
    {
        try
        {
            return p_impl_->AddRef();
        }
        catch (std::exception& ex)
        {
            DAS_CORE_LOG_ERROR(ex.what());
            throw;
        }
    }

    int64_t Release() final
    {
        try
        {
            return p_impl_->Release();
        }
        catch (std::exception& ex)
        {
            DAS_CORE_LOG_ERROR(ex.what());
            throw;
        }
    }

    DasRetSwigBase QueryInterface(const DasGuid& swig_iid) final
    {
        DasRetSwigBase result{};
        void*          p_out_object{};
        const auto     pp_out_object = &p_out_object;

        if (const auto get_default_query_interface_result =
                DAS::Utils::QueryInterface<SwigT>(this, swig_iid);
            IsOk(get_default_query_interface_result.error_code)
            || get_default_query_interface_result.error_code
                   != DAS_E_NO_INTERFACE)
        {
            return get_default_query_interface_result;
        }

        if (const auto get_cpp_query_interface_result =
                p_impl_->QueryInterface(swig_iid, pp_out_object);
            IsOk(get_cpp_query_interface_result)
            || get_cpp_query_interface_result != DAS_E_NO_INTERFACE)
        {
            result = {get_cpp_query_interface_result, p_out_object};
            return result;
        }

        if (const auto expected_iid = ConvertCppIidToSwigIid(swig_iid);
            expected_iid)
        {
            try
            {
                result.error_code = p_impl_->QueryInterface(
                    expected_iid.value(),
                    pp_out_object);
            }
            catch (std::exception& ex)
            {
                DAS_CORE_LOG_EXCEPTION(ex);
                result.error_code = DAS_E_INTERNAL_FATAL_ERROR;
                return result;
            }

            if (IsOk(result.error_code))
            {
                result =
                    CreateSwigToCppObject(expected_iid.value(), p_out_object);
                return result;
            }

            DasPtr<IDasReadOnlyString> predefined_error_explanation{};
            ::DasGetPredefinedErrorMessage(
                result.error_code,
                predefined_error_explanation.Put());
            DAS_CORE_LOG_ERROR(
                "Error happened in class IDasSwigBase. Error code: "
                "{}. Explanation: {}.",
                result.error_code,
                predefined_error_explanation);
        }
        result.error_code = DAS_E_NO_INTERFACE;
        return result;
    }
};

// NOTE: template<auto FunctionPointer>
// 或许可用，但是上面的都是那么写的，就不重构了
template <
    class DasRetT,
    class CppRetT,
    class T4Function,
    T4Function       FunctionPointer,
    is_asr_interface T,
    class... InputArgs>
[[nodiscard]]
DasRetT CallCppMethod(T* p_cpp_object, InputArgs&&... input_args)
{
    DasRetT         result{};
    DasPtr<CppRetT> p_result;

    result.error_code = (p_cpp_object->*FunctionPointer)(
        std::forward<InputArgs>(input_args)...,
        p_result.Put());

    if (!DAS::IsOk(result.error_code))
    {
        return result;
    }

    using ValueType = decltype(result.value);
    // 注意：如出现对象会在SWIG和C++之间反复转换的情况，可能还要处理实现类同时提供C++和SWIG接口的情况
    if constexpr (std::is_pointer_v<ValueType>)
    {
        result.value = p_result.Get();
    }
    else
    {
        result.value = {std::move(p_result)};
    }

    return result;
}

template <is_asr_swig_interface SwigT, is_asr_interface T>
class CppToSwigTypeInfo : public CppToSwigBase<SwigT, T>
{
    static_assert(
        std::is_base_of_v<IDasTypeInfo, T>,
        "T is not inherit from IDasTypeInfo!");

    using Base = CppToSwigBase<SwigT, T>;

public:
    using Base::Base;

    auto GetRuntimeClassName() -> DasRetReadOnlyString final
    {
        return CallCppMethod<
            DasRetReadOnlyString,
            IDasReadOnlyString,
            DAS_DV_V(&IDasTypeInfo::GetRuntimeClassName)>(Base::p_impl_.Get());
    }
    auto GetGuid() -> DasRetGuid final
    {
        DasRetGuid swig_result;
        swig_result.error_code = Base::p_impl_->GetGuid(&swig_result.value);

        return swig_result;
    }
};

template <>
class CppToSwig<IDasBase> final : public CppToSwigBase<IDasSwigBase, IDasBase>
{
public:
    DAS_USING_BASE_CTOR(CppToSwigBase);
};

template <>
class CppToSwig<IDasTypeInfo> final
    : public CppToSwigTypeInfo<IDasSwigTypeInfo, IDasTypeInfo>
{
public:
    DAS_USING_BASE_CTOR(CppToSwigTypeInfo);
};

template <>
class CppToSwig<IDasCapture> final
    : public CppToSwigTypeInfo<IDasSwigCapture, IDasCapture>
{
public:
    DAS_USING_BASE_CTOR(CppToSwigTypeInfo);

    DasRetImage Capture() override;
};

template <>
class CppToSwig<IDasGuidVector> final
    : public CppToSwigBase<IDasSwigGuidVector, IDasGuidVector>
{
public:
    DAS_USING_BASE_CTOR(CppToSwigBase);

    DasRetUInt               Size() override;
    DasRetGuid               At(size_t index) override;
    DasResult                Find(const DasGuid& guid) override;
    DasResult                PushBack(const DasGuid& guid) override;
    DasRetReadOnlyGuidVector ToConst() override;
};

template <>
class CppToSwig<IDasReadOnlyGuidVector> final
    : public CppToSwigBase<IDasSwigReadOnlyGuidVector, IDasReadOnlyGuidVector>
{
public:
    DAS_USING_BASE_CTOR(CppToSwigBase);

    DasRetUInt Size() override;
    DasRetGuid At(size_t index) override;
    DasResult  Find(const DasGuid& guid) override;
};

template <is_asr_swig_interface SwigT, is_asr_interface T>
class CppToSwigInput : public CppToSwigTypeInfo<SwigT, T>
{
    static_assert(
        std::is_base_of_v<IDasInput, T>,
        "T is not inherit from IDasInput!");

    using Base = CppToSwigTypeInfo<SwigT, T>;

public:
    using Base::Base;

    DAS_IMPL Click(const int32_t x, const int32_t y) override;
};

template <>
class CppToSwig<IDasInput> final
    : public CppToSwigInput<IDasSwigInput, IDasInput>
{
public:
    DAS_USING_BASE_CTOR(CppToSwigInput);
};

template <>
class CppToSwig<IDasTouch> final
    : public CppToSwigInput<IDasSwigTouch, IDasTouch>
{
public:
    DAS_USING_BASE_CTOR(CppToSwigInput);

    DAS_IMPL Swipe(DasPoint from, DasPoint to, const int32_t duration_ms)
        override;
};

template <>
class CppToSwig<IDasInputFactory> final
    : public CppToSwigTypeInfo<IDasSwigInputFactory, IDasInputFactory>
{
public:
    DAS_USING_BASE_CTOR(CppToSwigTypeInfo);

    DasRetInput CreateInstance(DasReadOnlyString json_config) override;
};

template <>
class CppToSwig<IDasComponent> final
    : public CppToSwigTypeInfo<IDasSwigComponent, IDasComponent>
{
public:
    DAS_USING_BASE_CTOR(CppToSwigTypeInfo);

    /**
     * @brief 不支持自定义的 IDasVariant 或 IDasSwigVariant
     * @param p_arguments 参数
     * @return 操作的返回值
     */
    DasRetVariantVector Dispatch(
        DasReadOnlyString      function_name,
        IDasSwigVariantVector* p_arguments) override;
};

template <is_asr_interface ToCpp, is_asr_swig_interface FromSwig>
auto MakeInterop(FromSwig* p_from) -> Utils::Expected<DasPtr<ToCpp>>
{
    if (const auto qi_result = p_from->QueryInterface(DasIidOf<ToCpp>());
        IsOk(qi_result.error_code))
    {
        return DasPtr{static_cast<ToCpp*>(qi_result.GetVoidNoAddRef())};
    }

    try
    {
        return MakeDasPtr<ToCpp, SwigToCpp<FromSwig>>(p_from);
    }
    catch (const std::bad_alloc&)
    {
        DAS_CORE_LOG_ERROR("Out of memory!");
        return tl::make_unexpected(DAS_E_OUT_OF_MEMORY);
    }
}

template <is_asr_interface ToCpp, is_asr_swig_interface FromSwig>
auto MakeInterop(DasPtr<FromSwig> p_from) -> Utils::Expected<DasPtr<ToCpp>>
{
    return MakeInterop<ToCpp>(p_from.Get());
}

template <is_asr_swig_interface ToSwig, is_asr_interface FromCpp>
auto MakeInterop(FromCpp* p_from) -> Utils::Expected<DasPtr<ToSwig>>
{
    void*  p_out_object{};
    void** pp_out_object = &p_out_object;
    if (const auto qi_result =
            p_from->QueryInterface(DasIidOf<ToSwig>(), pp_out_object);
        IsOk(qi_result))
    {
        return {static_cast<ToSwig*>(p_out_object)};
    }

    try
    {
        return MakeDasPtr<ToSwig, CppToSwig<FromCpp>>(p_from);
    }
    catch (const std::bad_alloc&)
    {
        DAS_CORE_LOG_ERROR("Out of memory!");
        return tl::make_unexpected(DAS_E_OUT_OF_MEMORY);
    }
}

template <is_asr_swig_interface ToSwig, is_asr_interface FromCpp>
auto MakeInterop(DasPtr<FromCpp> p_from) -> Utils::Expected<DasPtr<ToSwig>>
{
    return MakeInterop<ToSwig>(p_from.Get());
}

template <class RetType, is_asr_swig_interface SwigT>
auto ToDasRetType(
    const Utils::Expected<DasPtr<SwigT>>& expected_result,
    RetType&                              ref_out_result)
{
    if (expected_result)
    {
        const auto& value = expected_result.value();
        ref_out_result = RetType{DAS_S_OK, value.Get()};
    }
    else
    {
        ref_out_result = {expected_result.error(), nullptr};
    }
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_CPPSWIGINTEROP_H

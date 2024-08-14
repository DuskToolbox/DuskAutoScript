#ifndef ASR_CORE_FOREIGNINTERFACEHOST_CPPSWIGINTEROP_H
#define ASR_CORE_FOREIGNINTERFACEHOST_CPPSWIGINTEROP_H

#include <AutoStarRail/AsrPtr.hpp>
#include <AutoStarRail/Core/ForeignInterfaceHost/AsrStringImpl.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/Config.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/IsCastAvailableImpl.hpp>
#include <AutoStarRail/Core/Logger/Logger.h>
#include <AutoStarRail/ExportInterface/IAsrGuidVector.h>
#include <AutoStarRail/ExportInterface/IAsrImage.h>
#include <AutoStarRail/IAsrBase.h>
#include <AutoStarRail/PluginInterface/IAsrCapture.h>
#include <AutoStarRail/PluginInterface/IAsrComponent.h>
#include <AutoStarRail/PluginInterface/IAsrErrorLens.h>
#include <AutoStarRail/PluginInterface/IAsrInput.h>
#include <AutoStarRail/PluginInterface/IAsrPlugin.h>
#include <AutoStarRail/PluginInterface/IAsrTask.h>
#include <AutoStarRail/Utils/Expected.h>
#include <AutoStarRail/Utils/QueryInterface.hpp>
#include <cstdint>

ASR_NS_BEGIN

using CommonTypeInfoPtr =
    std::variant<AsrPtr<IAsrTypeInfo>, AsrPtr<IAsrSwigTypeInfo>>;

ASR_NS_END

ASR_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

template <class T>
concept is_asr_swig_interface = std::is_base_of_v<IAsrSwigBase, T>;

template <class T>
concept is_asr_interface = std::is_base_of_v<IAsrBase, T>;

template <class SwigT>
class SwigToCpp;

auto ConvertCppIidToSwigIid(const AsrGuid& cpp_iid)
    -> ASR::Utils::Expected<AsrGuid>;

// TODO: 实现这个函数
auto ConvertSwigIidToCppIid(const AsrGuid& swig_iid)
    -> ASR::Utils::Expected<AsrGuid>;

bool IsCppIid(const AsrGuid& cpp_iid);

bool IsSwigIid(const AsrGuid& swig_iid);

/**
 * @brief 使用SWIG接口iid和对象指针创建对应的C++对象包装
 * @param swig_iid swig 接口类型的iid
 * @param p_swig_object swig 对象指针
 * @param pp_out_cpp_object 输出的 cpp 对象
 * @return 操作结果
 */
AsrResult CreateCppToSwigObject(
    const AsrGuid& swig_iid,
    void*          p_swig_object,
    void**         pp_out_cpp_object);

auto CreateSwigToCppObject(const AsrGuid& iid, void* p_cpp_object)
    -> AsrRetSwigBase;

template <is_asr_interface T>
auto ConvertCppIidToSwigIid()
{
    return ConvertCppIidToSwigIid(AsrIidOf<T>());
}

template <is_asr_swig_interface T>
auto ConvertSwigIidToCppIid() -> ASR::Utils::Expected<AsrGuid>
{
    return ConvertSwigIidToCppIid(AsrIidOf<T>());
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

#define ASR_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD(                        \
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
    ASR::AsrPtr<SwigT> p_impl_;

public:
    template <class Other>
    explicit SwigToCppBase(ASR::AsrPtr<Other> p_impl) : p_impl_{p_impl}
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
            ASR_CORE_LOG_ERROR(ex.what());
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
            ASR_CORE_LOG_ERROR(ex.what());
            throw;
        }
    }

    /**
     * @brief 只会接收CPP版本的IID
     * @param iid
     * @param pp_out_object
     * @return
     */
    AsrResult QueryInterface(const AsrGuid& iid, void** pp_out_object) final
    {
        ASR_UTILS_CHECK_POINTER(pp_out_object)
        // 先看能不能直接转换
        const auto get_default_query_interface_result =
            ASR::Utils::QueryInterface<T>(this, iid, pp_out_object);
        if (IsOk(get_default_query_interface_result)
            || get_default_query_interface_result != ASR_E_NO_INTERFACE)
        {
            return get_default_query_interface_result;
        }
        // 再看内部实现能不能直接转换
        if (const auto get_swig_query_interface_result =
                p_impl_->QueryInterface(iid);
            IsOk(get_swig_query_interface_result.error_code)
            || get_swig_query_interface_result.error_code != ASR_E_NO_INTERFACE)
        {
            *pp_out_object = get_swig_query_interface_result.value;
            return get_swig_query_interface_result.error_code;
        }
        // 最后看是不是要转换到子类
        if (const auto swig_iid = ConvertCppIidToSwigIid(iid); swig_iid)
        {
            AsrRetSwigBase result;
            try
            {
                result = p_impl_->QueryInterface(swig_iid.value());
            }
            catch (std::exception& ex)
            {
                ASR_CORE_LOG_EXCEPTION(ex);
                return ASR_E_SWIG_INTERNAL_ERROR;
            }
            if (IsOk(result.error_code))
            {
                if (pp_out_object == nullptr)
                {
                    return ASR_E_INVALID_POINTER;
                }
                // 注意：此时虚表指针应该指向Swig生成的导演类的虚表
                return CreateCppToSwigObject(
                    swig_iid.value(),
                    result.value,
                    pp_out_object);
            }
            AsrPtr<IAsrReadOnlyString> predefined_error_explanation{};
            ::AsrGetPredefinedErrorMessage(
                result.error_code,
                predefined_error_explanation.Put());
            ASR_CORE_LOG_ERROR(
                "Error happened in class IAsrSwigBase. Error code: "
                "{}. Explanation: {}.",
                result.error_code,
                predefined_error_explanation);
            return ASR_E_NO_INTERFACE;
        }
        return ASR_E_NO_INTERFACE;
    };

    [[nodiscard]]
    auto Get() const noexcept
    {
        return p_impl_;
    }

    [[nodiscard]]
    T*
    operator->() const noexcept
    {
        return static_cast<T*>(this);
    }
    [[nodiscard]]
    T&
    operator*() const noexcept
    {
        return static_cast<T&>(*this);
    }
};

#define ASR_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD_IMPL_AND_HANDLE_EXCEPTION( \
    p_swig_object,                                                                \
    function_pointer,                                                             \
    ...)                                                                          \
    try                                                                           \
    {                                                                             \
        return ASR_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD(                    \
            p_swig_object,                                                        \
            function_pointer,                                                     \
            __VA_ARGS__);                                                         \
    }                                                                             \
    catch (const std::exception& ex)                                              \
    {                                                                             \
        ASR_CORE_LOG_ERROR(ex.what());                                            \
        return ASR_E_SWIG_INTERNAL_ERROR;                                         \
    }

template <is_asr_swig_interface SwigT, is_asr_interface T>
class SwigToCppTypeInfo : public SwigToCppBase<SwigT, T>
{
    static_assert(
        std::is_base_of_v<IAsrSwigTypeInfo, SwigT>,
        "SwigT is not inherit from IAsrSwigTypeInfo!");

    using Base = SwigToCppBase<SwigT, T>;

public:
    using Base::Base;
    /**
     * @brief 返回SWIG对象定义所有IID
     * @param pp_out_vector
     * @return
     */
    AsrResult GetGuid(AsrGuid* p_out_guid) override
    {
        ASR_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD_IMPL_AND_HANDLE_EXCEPTION(
            Base::p_impl_.Get(),
            &IAsrSwigTypeInfo::GetGuid,
            p_out_guid);
    }

    AsrResult GetRuntimeClassName(IAsrReadOnlyString** pp_out_name) override
    {
        ASR_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD_IMPL_AND_HANDLE_EXCEPTION(
            Base::p_impl_.Get(),
            &IAsrSwigTypeInfo::GetRuntimeClassName,
            pp_out_name)
    }
};

template <>
class SwigToCpp<IAsrSwigBase> final
    : public SwigToCppBase<IAsrSwigBase, IAsrBase>
{
public:
    ASR_USING_BASE_CTOR(SwigToCppBase);
};

template <>
class SwigToCpp<IAsrSwigTypeInfo> final
    : public SwigToCppTypeInfo<IAsrSwigTypeInfo, IAsrTypeInfo>
{
public:
    ASR_USING_BASE_CTOR(SwigToCppTypeInfo);
};

template <>
class SwigToCpp<IAsrSwigErrorLens> final
    : public SwigToCppBase<IAsrSwigErrorLens, IAsrErrorLens>
{
public:
    ASR_USING_BASE_CTOR(SwigToCppBase);

    AsrResult GetSupportedIids(IAsrReadOnlyGuidVector** pp_out_iids) override;
    AsrResult GetErrorMessage(
        IAsrReadOnlyString*  locale_name,
        AsrResult            error_code,
        IAsrReadOnlyString** pp_out_string) override;
};

template <>
class SwigToCpp<IAsrSwigTask> final
    : public SwigToCppTypeInfo<IAsrSwigTask, IAsrTask>
{
public:
    ASR_USING_BASE_CTOR(SwigToCppTypeInfo);

    AsrResult OnRequestExit() override;
    AsrResult Do(IAsrReadOnlyString* p_task_settings_json) override;
    AsrResult GetNextExecutionTime(AsrDate* p_out_date) override;
    AsrResult GetName(IAsrReadOnlyString** pp_out_name) override;
    AsrResult GetDescription(IAsrReadOnlyString** pp_out_settings) override;
    AsrResult GetLabel(IAsrReadOnlyString** pp_out_label) override;
    AsrResult GetType(AsrTaskType* p_out_type) override;
};

template <>
class SwigToCpp<IAsrSwigGuidVector> final
    : public SwigToCppBase<IAsrSwigGuidVector, IAsrGuidVector>
{
public:
    ASR_USING_BASE_CTOR(SwigToCppBase);

    ASR_IMPL Size(size_t* p_out_size) override;
    ASR_IMPL At(size_t index, AsrGuid* p_out_iid) override;
    ASR_IMPL Find(const AsrGuid& iid) override;
    ASR_IMPL PushBack(const AsrGuid& iid) override;
    ASR_IMPL ToConst(IAsrReadOnlyGuidVector** pp_out_object) override;
};

template <>
class SwigToCpp<IAsrSwigReadOnlyGuidVector> final
    : public SwigToCppBase<IAsrSwigReadOnlyGuidVector, IAsrReadOnlyGuidVector>
{
public:
    ASR_USING_BASE_CTOR(SwigToCppBase);

    ASR_IMPL Size(size_t* p_out_size) override;
    ASR_IMPL At(size_t index, AsrGuid* p_out_iid) override;
    ASR_IMPL Find(const AsrGuid& iid) override;
};

template <is_asr_swig_interface SwigT, is_asr_interface T>
class SwigToCppInput : public SwigToCppTypeInfo<SwigT, T>
{
    static_assert(
        std::is_base_of_v<IAsrSwigInput, SwigT>,
        "SwigT is not inherit from SwigToCppInput!");

    using Base = SwigToCppTypeInfo<SwigT, T>;

public:
    using Base::Base;

    ASR_IMPL Click(int32_t x, int32_t y) override;
};

template <>
class SwigToCpp<IAsrSwigInput> final
    : public SwigToCppInput<IAsrSwigInput, IAsrInput>
{
public:
    ASR_USING_BASE_CTOR(SwigToCppInput);
};

template <>
class SwigToCpp<IAsrSwigTouch> final
    : public SwigToCppInput<IAsrSwigTouch, IAsrTouch>
{
public:
    ASR_USING_BASE_CTOR(SwigToCppInput);

    ASR_IMPL Swipe(AsrPoint from, AsrPoint to, int32_t duration_ms) override;
};

template <>
class SwigToCpp<IAsrSwigInputFactory> final
    : public SwigToCppTypeInfo<IAsrSwigInputFactory, IAsrInputFactory>
{
public:
    ASR_USING_BASE_CTOR(SwigToCppTypeInfo);

    AsrResult CreateInstance(
        IAsrReadOnlyString* p_json_config,
        IAsrInput**         pp_out_input) override;
};

template <>
class SwigToCpp<IAsrSwigComponent> final
    : public SwigToCppTypeInfo<IAsrSwigComponent, IAsrComponent>
{
public:
    ASR_USING_BASE_CTOR(SwigToCppTypeInfo);

    AsrResult Dispatch(
        IAsrReadOnlyString* p_function_name,
        IAsrVariantVector*  p_arguments,
        IAsrVariantVector** pp_out_result) override;
};

AsrResult CommonPluginEnumFeature(
    const CommonPluginPtr& p_this,
    size_t                 index,
    AsrPluginFeature*      p_out_feature);

template <class T>
class CppToSwig;

template <is_asr_swig_interface SwigT, is_asr_interface T>
class CppToSwigBase : public SwigT
{
public:
    using SwigType = SwigT;

protected:
    ASR::AsrPtr<T> p_impl_;

public:
    template <class Other>
    CppToSwigBase(ASR::AsrPtr<Other> p_impl) : p_impl_{p_impl}
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
            ASR_CORE_LOG_ERROR(ex.what());
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
            ASR_CORE_LOG_ERROR(ex.what());
            throw;
        }
    }

    AsrRetSwigBase QueryInterface(const AsrGuid& swig_iid) final
    {
        AsrRetSwigBase result{};
        void*          p_out_object{};
        const auto     pp_out_object = &p_out_object;

        if (const auto get_default_query_interface_result =
                ASR::Utils::QueryInterface<SwigT>(this, swig_iid);
            IsOk(get_default_query_interface_result.error_code)
            || get_default_query_interface_result.error_code
                   != ASR_E_NO_INTERFACE)
        {
            return get_default_query_interface_result;
        }

        if (const auto get_cpp_query_interface_result =
                p_impl_->QueryInterface(swig_iid, pp_out_object);
            IsOk(get_cpp_query_interface_result)
            || get_cpp_query_interface_result != ASR_E_NO_INTERFACE)
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
                ASR_CORE_LOG_EXCEPTION(ex);
                result.error_code = ASR_E_INTERNAL_FATAL_ERROR;
                return result;
            }

            if (IsOk(result.error_code))
            {
                result =
                    CreateSwigToCppObject(expected_iid.value(), p_out_object);
                return result;
            }

            AsrPtr<IAsrReadOnlyString> predefined_error_explanation{};
            ::AsrGetPredefinedErrorMessage(
                result.error_code,
                predefined_error_explanation.Put());
            ASR_CORE_LOG_ERROR(
                "Error happened in class IAsrSwigBase. Error code: "
                "{}. Explanation: {}.",
                result.error_code,
                predefined_error_explanation);
        }
        result.error_code = ASR_E_NO_INTERFACE;
        return result;
    }
};

// NOTE: template<auto FunctionPointer>
// 或许可用，但是上面的都是那么写的，就不重构了
template <
    class AsrRetT,
    class CppRetT,
    class T4Function,
    T4Function       FunctionPointer,
    is_asr_interface T,
    class... InputArgs>
[[nodiscard]]
AsrRetT CallCppMethod(T* p_cpp_object, InputArgs&&... input_args)
{
    AsrRetT         result{};
    AsrPtr<CppRetT> p_result;

    result.error_code = (p_cpp_object->*FunctionPointer)(
        std::forward<InputArgs>(input_args)...,
        p_result.Put());

    if (!ASR::IsOk(result.error_code))
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
        std::is_base_of_v<IAsrTypeInfo, T>,
        "T is not inherit from IAsrTypeInfo!");

    using Base = CppToSwigBase<SwigT, T>;

public:
    using Base::Base;

    auto GetRuntimeClassName() -> AsrRetReadOnlyString final
    {
        return CallCppMethod<
            AsrRetReadOnlyString,
            IAsrReadOnlyString,
            ASR_DV_V(&IAsrTypeInfo::GetRuntimeClassName)>(Base::p_impl_.Get());
    }
    auto GetGuid() -> AsrRetGuid final
    {
        AsrRetGuid swig_result;
        swig_result.error_code = Base::p_impl_->GetGuid(&swig_result.value);

        return swig_result;
    }
};

template <>
class CppToSwig<IAsrBase> final : public CppToSwigBase<IAsrSwigBase, IAsrBase>
{
public:
    ASR_USING_BASE_CTOR(CppToSwigBase);
};

template <>
class CppToSwig<IAsrTypeInfo> final
    : public CppToSwigTypeInfo<IAsrSwigTypeInfo, IAsrTypeInfo>
{
public:
    ASR_USING_BASE_CTOR(CppToSwigTypeInfo);
};

template <>
class CppToSwig<IAsrCapture> final
    : public CppToSwigTypeInfo<IAsrSwigCapture, IAsrCapture>
{
public:
    ASR_USING_BASE_CTOR(CppToSwigTypeInfo);

    AsrRetImage Capture() override;
};

template <>
class CppToSwig<IAsrGuidVector> final
    : public CppToSwigBase<IAsrSwigGuidVector, IAsrGuidVector>
{
public:
    ASR_USING_BASE_CTOR(CppToSwigBase);

    AsrRetUInt               Size() override;
    AsrRetGuid               At(size_t index) override;
    AsrResult                Find(const AsrGuid& guid) override;
    AsrResult                PushBack(const AsrGuid& guid) override;
    AsrRetReadOnlyGuidVector ToConst() override;
};

template <>
class CppToSwig<IAsrReadOnlyGuidVector> final
    : public CppToSwigBase<IAsrSwigReadOnlyGuidVector, IAsrReadOnlyGuidVector>
{
public:
    ASR_USING_BASE_CTOR(CppToSwigBase);

    AsrRetUInt Size() override;
    AsrRetGuid At(size_t index) override;
    AsrResult  Find(const AsrGuid& guid) override;
};

template <is_asr_swig_interface SwigT, is_asr_interface T>
class CppToSwigInput : public CppToSwigTypeInfo<SwigT, T>
{
    static_assert(
        std::is_base_of_v<IAsrInput, T>,
        "T is not inherit from IAsrInput!");

    using Base = CppToSwigTypeInfo<SwigT, T>;

public:
    using Base::Base;

    ASR_IMPL Click(const int32_t x, const int32_t y) override;
};

template <>
class CppToSwig<IAsrInput> final
    : public CppToSwigInput<IAsrSwigInput, IAsrInput>
{
public:
    ASR_USING_BASE_CTOR(CppToSwigInput);
};

template <>
class CppToSwig<IAsrTouch> final
    : public CppToSwigInput<IAsrSwigTouch, IAsrTouch>
{
public:
    ASR_USING_BASE_CTOR(CppToSwigInput);

    ASR_IMPL Swipe(AsrPoint from, AsrPoint to, const int32_t duration_ms)
        override;
};

template <>
class CppToSwig<IAsrInputFactory> final
    : public CppToSwigTypeInfo<IAsrSwigInputFactory, IAsrInputFactory>
{
public:
    ASR_USING_BASE_CTOR(CppToSwigTypeInfo);

    AsrRetInput CreateInstance(AsrReadOnlyString json_config) override;
};

template <>
class CppToSwig<IAsrComponent> final
    : public CppToSwigTypeInfo<IAsrSwigComponent, IAsrComponent>
{
public:
    ASR_USING_BASE_CTOR(CppToSwigTypeInfo);

    /**
     * @brief 不支持自定义的 IAsrVariant 或 IAsrSwigVariant
     * @param p_arguments 参数
     * @return 操作的返回值
     */
    AsrRetVariantVector Dispatch(
        AsrReadOnlyString function_name,
        IAsrSwigVariantVector * p_arguments) override;
};

template <is_asr_interface ToCpp, is_asr_swig_interface FromSwig>
auto MakeInterop(FromSwig* p_from) -> Utils::Expected<AsrPtr<ToCpp>>
{
    if (const auto qi_result = p_from->QueryInterface(AsrIidOf<ToCpp>());
        IsOk(qi_result.error_code))
    {
        return AsrPtr{static_cast<ToCpp*>(qi_result.GetVoidNoAddRef())};
    }

    try
    {
        return MakeAsrPtr<ToCpp, SwigToCpp<FromSwig>>(p_from);
    }
    catch (const std::bad_alloc&)
    {
        ASR_CORE_LOG_ERROR("Out of memory!");
        return tl::make_unexpected(ASR_E_OUT_OF_MEMORY);
    }
}

template <is_asr_interface ToCpp, is_asr_swig_interface FromSwig>
auto MakeInterop(AsrPtr<FromSwig> p_from) -> Utils::Expected<AsrPtr<ToCpp>>
{
    return MakeInterop<ToCpp>(p_from.Get());
}

template <is_asr_swig_interface ToSwig, is_asr_interface FromCpp>
auto MakeInterop(FromCpp* p_from) -> Utils::Expected<AsrPtr<ToSwig>>
{
    void*  p_out_object{};
    void** pp_out_object = &p_out_object;
    if (const auto qi_result =
            p_from->QueryInterface(AsrIidOf<ToSwig>(), pp_out_object);
        IsOk(qi_result))
    {
        return {static_cast<ToSwig*>(p_out_object)};
    }

    try
    {
        return MakeAsrPtr<ToSwig, CppToSwig<FromCpp>>(p_from);
    }
    catch (const std::bad_alloc&)
    {
        ASR_CORE_LOG_ERROR("Out of memory!");
        return tl::make_unexpected(ASR_E_OUT_OF_MEMORY);
    }
}

template <is_asr_swig_interface ToSwig, is_asr_interface FromCpp>
auto MakeInterop(AsrPtr<FromCpp> p_from) -> Utils::Expected<AsrPtr<ToSwig>>
{
    return MakeInterop<ToSwig>(p_from.Get());
}

template <class RetType, is_asr_swig_interface SwigT>
auto ToAsrRetType(
    const Utils::Expected<AsrPtr<SwigT>>& expected_result,
    RetType&                              ref_out_result)
{
    if (expected_result)
    {
        const auto& value = expected_result.value();
        ref_out_result = RetType{ASR_S_OK, value.Get()};
    }
    else
    {
        ref_out_result = {expected_result.error(), nullptr};
    }
}

ASR_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // ASR_CORE_FOREIGNINTERFACEHOST_CPPSWIGINTEROP_H

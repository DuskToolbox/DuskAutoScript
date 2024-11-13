#include <AutoStarRail/AsrPtr.hpp>
#include <AutoStarRail/Core/ForeignInterfaceHost/CppSwigInterop.h>
#include <AutoStarRail/ExportInterface/IAsrPluginManager.h>
#include <AutoStarRail/IAsrBase.h>
#include <AutoStarRail/PluginInterface/IAsrPlugin.h>
#include <DAS/_autogen/CppSwigBiMap.h>
ASR_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

auto ConvertCppIidToSwigIid(const AsrGuid& cpp_iid)
    -> ASR::Utils::Expected<AsrGuid>
{
    const auto& g_cpp_swig_map = Das::_autogen::g_cpp_swig_map;
    auto        it = g_cpp_swig_map.left.find(cpp_iid);
    if (it == g_cpp_swig_map.left.end())
    {
        return tl::make_unexpected(ASR_E_NO_INTERFACE);
    }
    return it->second;
}

bool IsCppIid(const AsrGuid& cpp_iid)
{
    const auto& g_cpp_swig_map = Das::_autogen::g_cpp_swig_map;
    const auto  it = g_cpp_swig_map.left.find(cpp_iid);
    return it != g_cpp_swig_map.left.end();
}

bool IsSwigIid(const AsrGuid& swig_iid)
{
    const auto& g_cpp_swig_map = Das::_autogen::g_cpp_swig_map;
    const auto  it = g_cpp_swig_map.right.find(swig_iid);
    return it != g_cpp_swig_map.right.end();
}

ASR_NS_ANONYMOUS_DETAILS_BEGIN

template <class SwigT>
auto CreateCppToSwigObjectImpl(void* p_swig_object, void** pp_out_cpp_object)
    -> AsrResult
{
    try
    {
        auto* const p_cpp_object =
            new SwigToCpp<SwigT>(static_cast<SwigT*>(p_swig_object));
        p_cpp_object->AddRef();
        *pp_out_cpp_object = p_cpp_object;
        return ASR_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return ASR_E_OUT_OF_MEMORY;
    }
}

ASR_NS_ANONYMOUS_DETAILS_END

#define ASR_CORE_FOREIGNINTERFACEHOST_CREATE_CPP_TO_SWIG_OBJECT(SwigType)      \
    {AsrIidOf<SwigType>(),                                                     \
     [](void* p_swig_object, void** pp_out_cpp_object)                         \
     {                                                                         \
         return Details::CreateCppToSwigObjectImpl<SwigType>(                  \
             p_swig_object,                                                    \
             pp_out_cpp_object);                                               \
     }}

// TODO: 添加所有PluginInterface中的导出类型
const static std::unordered_map<
    AsrGuid,
    AsrResult (*)(void* p_swig_object, void** pp_out_cpp_object)>
    g_cpp_to_swig_factory{
        ASR_CORE_FOREIGNINTERFACEHOST_CREATE_CPP_TO_SWIG_OBJECT(IAsrSwigBase),
        ASR_CORE_FOREIGNINTERFACEHOST_CREATE_CPP_TO_SWIG_OBJECT(
            IAsrSwigTypeInfo),
        ASR_CORE_FOREIGNINTERFACEHOST_CREATE_CPP_TO_SWIG_OBJECT(
            IAsrSwigErrorLens)};

AsrResult CreateCppToSwigObject(
    const AsrGuid& swig_iid,
    void*          p_swig_object,
    void**         pp_out_cpp_object)
{
    const auto it = g_cpp_to_swig_factory.find(swig_iid);
    if (it != g_cpp_to_swig_factory.end())
    {
        return it->second(p_swig_object, pp_out_cpp_object);
    }

    return ASR_E_NO_INTERFACE;
}

ASR_NS_ANONYMOUS_DETAILS_BEGIN

/**
 * @brief
 * 注意：外部保证传入的指针一定是已经转换到T的指针。如果指针是QueryInterface的返回值，则代表无问题。
 * @tparam T
 * @param p_cpp_object
 * @return
 */
template <class T>
auto CreateSwigToCppObjectImpl(void* p_cpp_object) -> AsrRetSwigBase
{
    AsrRetSwigBase result{};
    try
    {
        using SwigType = CppToSwig<T>::SwigType;
        auto* const p_swig_object =
            new CppToSwig<T>(static_cast<T*>(p_cpp_object));
        result.error_code = ASR_S_OK;
        // explicit 导致要decltype来显式写出类型，似乎没有必要explicit了
        result.value = decltype(result.value){
            static_cast<void*>(static_cast<SwigType*>(p_swig_object))};

        return result;
    }
    catch (const std::bad_alloc&)
    {
        result.error_code = ASR_E_OUT_OF_MEMORY;
        return result;
    }
}

ASR_NS_ANONYMOUS_DETAILS_END

#define ASR_CORE_FOREIGNINTERFACEHOST_CREATE_SWIG_TO_CPP_OBJECT(Type)          \
    {AsrIidOf<Type>(), [](void* p_cpp_object) {                                \
         return Details::CreateSwigToCppObjectImpl<Type>(p_cpp_object);        \
     }}

// TODO: 添加所有PluginInterface中的导出类型
const static std::unordered_map<AsrGuid, AsrRetSwigBase (*)(void* p_cpp_object)>
    g_swig_to_cpp_factory{};

auto CreateSwigToCppObject(const AsrGuid& iid, void* p_cpp_object)
    -> AsrRetSwigBase
{
    AsrRetSwigBase result;

    const auto it = g_swig_to_cpp_factory.find(iid);
    if (it != g_swig_to_cpp_factory.end())
    {
        return it->second(p_cpp_object);
    }

    result.error_code = ASR_E_NO_INTERFACE;
    return result;
}

// -------------------- implementation of SwigToCpp class --------------------

AsrResult SwigToCpp<IAsrSwigErrorLens>::GetSupportedIids(
    IAsrReadOnlyGuidVector** pp_out_iids)
{
    try
    {
        AsrPtr<IAsrGuidVector> p_guid_vector{};
        const auto             swig_result = p_impl_->GetSupportedIids();
        if (ASR::IsFailed(swig_result.error_code))
        {
            return swig_result.error_code;
        }

        auto p_result =
            new SwigToCpp<IAsrSwigReadOnlyGuidVector>(swig_result.value);
        p_result->AddRef();
        swig_result.value->Release();
        *pp_out_iids = p_result;

        return ASR_S_OK;
    }
    catch (const std::exception& ex)
    {
        ASR_CORE_LOG_ERROR(ex.what());
        return ASR_E_SWIG_INTERNAL_ERROR;
    }
}

AsrResult SwigToCpp<IAsrSwigErrorLens>::GetErrorMessage(
    IAsrReadOnlyString*  locale_name,
    AsrResult            error_code,
    IAsrReadOnlyString** pp_out_string)
{
    ASR_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD_IMPL_AND_HANDLE_EXCEPTION(
        p_impl_.Get(),
        &IAsrSwigErrorLens::GetErrorMessage,
        pp_out_string,
        locale_name,
        error_code);
}

AsrResult SwigToCpp<IAsrSwigTask>::OnRequestExit()
{
    return p_impl_->OnRequestExit();
}

AsrResult SwigToCpp<IAsrSwigTask>::Do(
    IAsrReadOnlyString* p_environment_json,
    IAsrReadOnlyString* p_task_settings_json)
{
    try
    {
        const auto result =
            p_impl_->Do({p_environment_json}, {p_task_settings_json});
        return result;
    }
    catch (const std::exception& ex)
    {
        ASR_CORE_LOG_ERROR(ex.what());
        return ASR_E_SWIG_INTERNAL_ERROR;
    }
}

AsrResult SwigToCpp<IAsrSwigTask>::GetNextExecutionTime(AsrDate* p_out_date)
{
    ASR_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD_IMPL_AND_HANDLE_EXCEPTION(
        p_impl_.Get(),
        &IAsrSwigTask::GetNextExecutionTime,
        p_out_date);
}

AsrResult SwigToCpp<IAsrSwigTask>::GetName(IAsrReadOnlyString** pp_out_name)
{
    ASR_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD_IMPL_AND_HANDLE_EXCEPTION(
        p_impl_.Get(),
        &IAsrSwigTask::GetName,
        pp_out_name);
}

AsrResult SwigToCpp<IAsrSwigTask>::GetDescription(
    IAsrReadOnlyString** pp_out_settings)
{
    ASR_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD_IMPL_AND_HANDLE_EXCEPTION(
        p_impl_.Get(),
        &IAsrSwigTask::GetDescription,
        pp_out_settings);
}

AsrResult SwigToCpp<IAsrSwigTask>::GetLabel(IAsrReadOnlyString** pp_out_label)
{
    ASR_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD_IMPL_AND_HANDLE_EXCEPTION(
        p_impl_.Get(),
        &IAsrSwigTask::GetLabel,
        pp_out_label);
}

ASR_IMPL SwigToCpp<IAsrSwigGuidVector>::Size(size_t* p_out_size)
{
    ASR_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD_IMPL_AND_HANDLE_EXCEPTION(
        p_impl_.Get(),
        &IAsrSwigGuidVector::Size,
        p_out_size);
}

ASR_IMPL SwigToCpp<IAsrSwigGuidVector>::At(size_t index, AsrGuid* p_out_iid)
{
    ASR_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD_IMPL_AND_HANDLE_EXCEPTION(
        p_impl_.Get(),
        &IAsrSwigGuidVector::At,
        p_out_iid,
        index);
}

ASR_IMPL SwigToCpp<IAsrSwigGuidVector>::Find(const AsrGuid& iid)
{
    try
    {
        return p_impl_->Find(iid);
    }
    catch (const std::exception& ex)
    {
        ASR_CORE_LOG_ERROR(ex.what());
        return ASR_E_SWIG_INTERNAL_ERROR;
    }
}

ASR_IMPL SwigToCpp<IAsrSwigGuidVector>::PushBack(const AsrGuid& iid)
{
    try
    {
        return p_impl_->PushBack(iid);
    }
    catch (const std::exception& ex)
    {
        ASR_CORE_LOG_ERROR(ex.what());
        return ASR_E_SWIG_INTERNAL_ERROR;
    }
}

AsrResult SwigToCpp<IAsrSwigGuidVector>::ToConst(
    IAsrReadOnlyGuidVector** pp_out_object)
{
    auto swig_result = p_impl_->ToConst();
    if (IsFailed(swig_result.error_code))
    {
        return swig_result.error_code;
    }

    AsrPtr<IAsrReadOnlyGuidVector> p_result{};

    const auto expected_result =
        MakeInterop<IAsrReadOnlyGuidVector>(swig_result.value);
    if (!expected_result)
    {
        return expected_result.error();
    }

    ASR_UTILS_CHECK_POINTER(pp_out_object)
    const auto value = expected_result.value().Get();
    *pp_out_object = value;
    value->AddRef();
    return ASR_S_OK;
}

ASR_IMPL SwigToCpp<IAsrSwigReadOnlyGuidVector>::Size(size_t* p_out_size)
{
    ASR_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD_IMPL_AND_HANDLE_EXCEPTION(
        p_impl_.Get(),
        &IAsrSwigReadOnlyGuidVector::Size,
        p_out_size);
}

ASR_IMPL SwigToCpp<IAsrSwigReadOnlyGuidVector>::At(
    size_t   index,
    AsrGuid* p_out_iid)
{
    ASR_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD_IMPL_AND_HANDLE_EXCEPTION(
        p_impl_.Get(),
        &IAsrSwigReadOnlyGuidVector::At,
        p_out_iid,
        index);
}

ASR_IMPL SwigToCpp<IAsrSwigReadOnlyGuidVector>::Find(const AsrGuid& iid)
{
    try
    {
        return p_impl_->Find(iid);
    }
    catch (const std::exception& ex)
    {
        ASR_CORE_LOG_ERROR(ex.what());
        return ASR_E_SWIG_INTERNAL_ERROR;
    }
}

template <is_asr_swig_interface SwigT, is_asr_interface T>
AsrResult SwigToCppInput<SwigT, T>::Click(int32_t x, int32_t y)
{
    return Base::p_impl_->Click(x, y);
}

AsrResult SwigToCpp<IAsrSwigTouch>::Swipe(
    AsrPoint from,
    AsrPoint to,
    int32_t  duration_ms)
{
    return p_impl_->Swipe(from, to, duration_ms);
}

AsrResult SwigToCpp<IAsrSwigInputFactory>::CreateInstance(
    IAsrReadOnlyString* p_json_config,
    IAsrInput**         pp_out_input)
{
    ASR_UTILS_CHECK_POINTER(p_json_config)
    ASR_UTILS_CHECK_POINTER(pp_out_input)

    const auto swig_result = p_impl_->CreateInstance({p_json_config});
    const auto expected_result = MakeInterop<IAsrInput>(swig_result.value);
    if (expected_result)
    {
        const auto& value = expected_result.value();
        *pp_out_input = value.Get();
        value->AddRef();
        return ASR_S_OK;
    }
    return expected_result.error();
}

AsrResult SwigToCpp<IAsrSwigComponent>::Dispatch(
    IAsrReadOnlyString* p_function_name,
    IAsrVariantVector*  p_arguments,
    IAsrVariantVector** pp_out_result)
{
    AsrPtr<IAsrSwigVariantVector> p_swig_in;
    if (const auto qi_in_result = p_arguments->QueryInterface(
            AsrIidOf<IAsrSwigVariantVector>(),
            p_swig_in.PutVoid());
        IsFailed(qi_in_result))
    {
        ASR_CORE_LOG_ERROR(
            "Unsupported IAsrVariantVector implementation. Error code = {}. Pointer = {}.",
            qi_in_result,
            Utils::VoidP(p_arguments));
        return qi_in_result;
    }

    try
    {
        const auto value =
            p_impl_->Dispatch({p_function_name}, p_swig_in.Get());
        if (IsOk(value.error_code))
        {
            const auto qi_p_result =
                value.value->QueryInterface(AsrIidOf<IAsrVariantVector>());
            if (IsFailed(qi_p_result.error_code))
            {
                ASR_CORE_LOG_ERROR(
                    "Unsupported IAsrSwigVariantVector implementation when reading result. Pointer = {}.",
                    Utils::VoidP(value.value.Get()));
                return qi_p_result.error_code;
            }
            auto& p_out_result = *pp_out_result;
            p_out_result = static_cast<IAsrVariantVector*>(qi_p_result.value);
            p_out_result->AddRef();
        }
        return value.error_code;
    }
    catch (const std::exception& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_SWIG_INTERNAL_ERROR;
    }
}

// TODO: IAsrSwigCaptureFactory CreateInstance

AsrResult CommonPluginEnumFeature(
    const CommonPluginPtr& p_this,
    size_t                 index,
    AsrPluginFeature*      p_out_feature)
{
    if (p_out_feature == nullptr)
    {
        return ASR_E_INVALID_POINTER;
    }

    return std::visit(
        ASR::Utils::overload_set{
            [index, p_out_feature](AsrPtr<IAsrPlugin> p_plugin) -> AsrResult
            { return p_plugin->EnumFeature(index, p_out_feature); },
            [index,
             p_out_feature](AsrPtr<IAsrSwigPlugin> p_swig_plugin) -> AsrResult
            {
                const auto result = p_swig_plugin->EnumFeature(index);
                if (ASR::IsOk(result.error_code))
                {
                    *p_out_feature = result.value;
                }
                return result.error_code;
            }},
        p_this);
}

AsrRetImage CppToSwig<IAsrCapture>::Capture()
{
    return CallCppMethod<
        AsrRetImage,
        IAsrImage,
        ASR_DV_V(&IAsrCapture::Capture)>(p_impl_.Get());
}

AsrRetUInt CppToSwig<IAsrGuidVector>::Size()
{
    AsrRetUInt swig_result;
    swig_result.error_code = p_impl_->Size(&swig_result.value);

    return swig_result;
}

AsrRetGuid CppToSwig<IAsrGuidVector>::At(size_t index)
{
    AsrRetGuid swig_result;
    swig_result.error_code = p_impl_->At(index, &swig_result.value);

    return swig_result;
}

AsrResult CppToSwig<IAsrGuidVector>::Find(const AsrGuid& guid)
{
    return p_impl_->Find(guid);
}

AsrResult CppToSwig<IAsrGuidVector>::PushBack(const AsrGuid& guid)
{
    return p_impl_->PushBack(guid);
}

AsrRetReadOnlyGuidVector CppToSwig<IAsrGuidVector>::ToConst()
{
    AsrPtr<IAsrReadOnlyGuidVector> p_const_result{};
    if (const auto tc_result = p_impl_->ToConst(p_const_result.Put());
        IsFailed(tc_result))
    {
        return {tc_result};
    }

    AsrPtr<IAsrSwigReadOnlyGuidVector> p_result{};
    try
    {
        p_result = MakeAsrPtr<
            IAsrSwigReadOnlyGuidVector,
            CppToSwig<IAsrReadOnlyGuidVector>>(p_const_result);
    }
    catch (const std::bad_alloc&)
    {
        return {ASR_E_OUT_OF_MEMORY};
    }

    return {ASR_S_OK, p_result.Get()};
}

AsrRetUInt CppToSwig<IAsrReadOnlyGuidVector>::Size()
{
    AsrRetUInt swig_result;
    swig_result.error_code = p_impl_->Size(&swig_result.value);

    return swig_result;
}

AsrRetGuid CppToSwig<IAsrReadOnlyGuidVector>::At(size_t index)
{
    AsrRetGuid swig_result;
    swig_result.error_code = p_impl_->At(index, &swig_result.value);

    return swig_result;
}

AsrResult CppToSwig<IAsrReadOnlyGuidVector>::Find(const AsrGuid& guid)
{
    return p_impl_->Find(guid);
}

template <is_asr_swig_interface SwigT, is_asr_interface T>
AsrResult CppToSwigInput<SwigT, T>::Click(const int32_t x, const int32_t y)
{
    return Base::p_impl_->Click(x, y);
}

AsrResult CppToSwig<IAsrTouch>::Swipe(
    AsrPoint      from,
    AsrPoint      to,
    const int32_t duration_ms)
{
    return p_impl_->Swipe(from, to, duration_ms);
}

AsrRetInput CppToSwig<IAsrInputFactory>::CreateInstance(
    AsrReadOnlyString json_config)
{
    try
    {
        AsrRetInput       swig_result{};
        AsrPtr<IAsrInput> p_cpp_result{};
        swig_result.error_code =
            p_impl_->CreateInstance(json_config.Get(), p_cpp_result.Put());
        const auto expected_result =
            MakeInterop<IAsrSwigInput>(p_cpp_result.Get());
        ToAsrRetType(expected_result, swig_result);
        return swig_result;
    }
    catch (const std::exception& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return {ASR_E_SWIG_INTERNAL_ERROR};
    }
}

AsrRetVariantVector CppToSwig<IAsrComponent>::Dispatch(
    AsrReadOnlyString      function_name,
    IAsrSwigVariantVector* p_arguments)
{
    const auto qi_result = p_arguments->QueryInterface(ASR_IID_VARIANT_VECTOR);
    if (IsFailed(qi_result.error_code))
    {
        ASR_CORE_LOG_ERROR(
            "Unsupported IAsrSwigVariantVector implementation. Pointer = {}.",
            Utils::VoidP(p_arguments));
        return {qi_result.error_code};
    }
    try
    {
        AsrRetVariantVector       result;
        AsrPtr<IAsrVariantVector> p_result;
        result.error_code = p_impl_->Dispatch(
            function_name.Get(),
            static_cast<IAsrVariantVector*>(qi_result.value),
            p_result.Put());
        AsrPtr<IAsrSwigVariantVector> p_swig_result;
        if (const auto qi_p_result = p_result.As(p_swig_result);
            IsFailed(qi_p_result))
        {
            ASR_CORE_LOG_ERROR(
                "Unsupported IAsrVariantVector implementation when reading result. Pointer = {}.",
                Utils::VoidP(p_result.Get()));
            return {qi_p_result};
        }
        result.value = p_swig_result;
        return result;
    }
    catch (const std::exception& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return {ASR_E_SWIG_INTERNAL_ERROR};
    }
}

ASR_CORE_FOREIGNINTERFACEHOST_NS_END

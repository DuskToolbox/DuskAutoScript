#include <DAS/_autogen/CppSwigBiMap.h>
#include <das/Core/ForeignInterfaceHost/CppSwigInterop.h>
#include <das/DasPtr.hpp>
#include <das/ExportInterface/IDasPluginManager.h>
#include <das/IDasBase.h>
#include <das/PluginInterface/IDasPluginPackage.h>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

auto ConvertCppIidToSwigIid(const DasGuid& cpp_iid)
    -> DAS::Utils::Expected<DasGuid>
{
    const auto& g_cpp_swig_map = Das::_autogen::g_cpp_swig_map;
    auto        it = g_cpp_swig_map.left.find(cpp_iid);
    if (it == g_cpp_swig_map.left.end())
    {
        return tl::make_unexpected(DAS_E_NO_INTERFACE);
    }
    return it->second;
}

bool IsCppIid(const DasGuid& cpp_iid)
{
    const auto& g_cpp_swig_map = Das::_autogen::g_cpp_swig_map;
    const auto  it = g_cpp_swig_map.left.find(cpp_iid);
    return it != g_cpp_swig_map.left.end();
}

bool IsSwigIid(const DasGuid& swig_iid)
{
    const auto& g_cpp_swig_map = Das::_autogen::g_cpp_swig_map;
    const auto  it = g_cpp_swig_map.right.find(swig_iid);
    return it != g_cpp_swig_map.right.end();
}

DAS_NS_ANONYMOUS_DETAILS_BEGIN

template <class SwigT>
auto CreateCppToSwigObjectImpl(void* p_swig_object, void** pp_out_cpp_object)
    -> DasResult
{
    try
    {
        auto* const p_cpp_object =
            new SwigToCpp<SwigT>(static_cast<SwigT*>(p_swig_object));
        p_cpp_object->AddRef();
        *pp_out_cpp_object = p_cpp_object;
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DAS_NS_ANONYMOUS_DETAILS_END

#define DAS_CORE_FOREIGNINTERFACEHOST_CREATE_CPP_TO_SWIG_OBJECT(SwigType)      \
    {DasIidOf<SwigType>(),                                                     \
     [](void* p_swig_object, void** pp_out_cpp_object)                         \
     {                                                                         \
         return Details::CreateCppToSwigObjectImpl<SwigType>(                  \
             p_swig_object,                                                    \
             pp_out_cpp_object);                                               \
     }}

// TODO: 添加所有PluginInterface中的导出类型
const static std::unordered_map<
    DasGuid,
    DasResult (*)(void* p_swig_object, void** pp_out_cpp_object)>
    g_cpp_to_swig_factory{
        DAS_CORE_FOREIGNINTERFACEHOST_CREATE_CPP_TO_SWIG_OBJECT(IDasSwigBase),
        DAS_CORE_FOREIGNINTERFACEHOST_CREATE_CPP_TO_SWIG_OBJECT(
            IDasSwigTypeInfo),
        DAS_CORE_FOREIGNINTERFACEHOST_CREATE_CPP_TO_SWIG_OBJECT(
            IDasSwigErrorLens)};

DasResult CreateCppToSwigObject(
    const DasGuid& swig_iid,
    void*          p_swig_object,
    void**         pp_out_cpp_object)
{
    const auto it = g_cpp_to_swig_factory.find(swig_iid);
    if (it != g_cpp_to_swig_factory.end())
    {
        return it->second(p_swig_object, pp_out_cpp_object);
    }

    return DAS_E_NO_INTERFACE;
}

DAS_NS_ANONYMOUS_DETAILS_BEGIN

/**
 * @brief
 * 注意：外部保证传入的指针一定是已经转换到T的指针。如果指针是QueryInterface的返回值，则代表无问题。
 * @tparam T
 * @param p_cpp_object
 * @return
 */
template <class T>
auto CreateSwigToCppObjectImpl(void* p_cpp_object) -> DasRetSwigBase
{
    DasRetSwigBase result{};
    try
    {
        using SwigType = CppToSwig<T>::SwigType;
        auto* const p_swig_object =
            new CppToSwig<T>(static_cast<T*>(p_cpp_object));
        result.error_code = DAS_S_OK;
        // explicit 导致要decltype来显式写出类型，似乎没有必要explicit了
        result.value = decltype(result.value){
            static_cast<void*>(static_cast<SwigType*>(p_swig_object))};

        return result;
    }
    catch (const std::bad_alloc&)
    {
        result.error_code = DAS_E_OUT_OF_MEMORY;
        return result;
    }
}

DAS_NS_ANONYMOUS_DETAILS_END

#define DAS_CORE_FOREIGNINTERFACEHOST_CREATE_SWIG_TO_CPP_OBJECT(Type)          \
    {DasIidOf<Type>(),                                                         \
     [](void* p_cpp_object)                                                    \
     { return Details::CreateSwigToCppObjectImpl<Type>(p_cpp_object); }}

// TODO: 添加所有PluginInterface中的导出类型
const static std::unordered_map<DasGuid, DasRetSwigBase (*)(void* p_cpp_object)>
    g_swig_to_cpp_factory{};

auto CreateSwigToCppObject(const DasGuid& iid, void* p_cpp_object)
    -> DasRetSwigBase
{
    DasRetSwigBase result;

    const auto it = g_swig_to_cpp_factory.find(iid);
    if (it != g_swig_to_cpp_factory.end())
    {
        return it->second(p_cpp_object);
    }

    result.error_code = DAS_E_NO_INTERFACE;
    return result;
}

// -------------------- implementation of SwigToCpp class --------------------

DasResult SwigToCpp<IDasSwigErrorLens>::GetSupportedIids(
    IDasReadOnlyGuidVector** pp_out_iids)
{
    try
    {
        DasPtr<IDasGuidVector> p_guid_vector{};
        const auto             swig_result = p_impl_->GetSupportedIids();
        if (DAS::IsFailed(swig_result.error_code))
        {
            return swig_result.error_code;
        }

        auto p_result =
            new SwigToCpp<IDasSwigReadOnlyGuidVector>(swig_result.value);
        p_result->AddRef();
        swig_result.value->Release();
        *pp_out_iids = p_result;

        return DAS_S_OK;
    }
    catch (const std::exception& ex)
    {
        DAS_CORE_LOG_ERROR(ex.what());
        return DAS_E_SWIG_INTERNAL_ERROR;
    }
}

DasResult SwigToCpp<IDasSwigErrorLens>::GetErrorMessage(
    IDasReadOnlyString*  locale_name,
    DasResult            error_code,
    IDasReadOnlyString** pp_out_string)
{
    DAS_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD_IMPL_AND_HANDLE_EXCEPTION(
        p_impl_.Get(),
        &IDasSwigErrorLens::GetErrorMessage,
        pp_out_string,
        locale_name,
        error_code);
}

DasResult SwigToCpp<IDasSwigTask>::Do(
    IDasStopToken*      p_stop_token,
    IDasReadOnlyString* p_environment_json,
    IDasReadOnlyString* p_task_settings_json)
{
    DasPtr<IDasSwigStopToken> p_swig_stop_token{};
    if (const auto qi_result = p_stop_token->QueryInterface(
            DasIidOf<IDasSwigStopToken>(),
            p_swig_stop_token.PutVoid());
        IsFailed(qi_result))
    {
        DAS_CORE_LOG_ERROR(
            "Can not get IDasSwigStopToken from a IDasStopToken Object. Error code = {}.",
            qi_result);
        return qi_result;
    }
    try
    {
        const auto result = p_impl_->Do(
            p_swig_stop_token.Get(),
            {p_environment_json},
            {p_task_settings_json});
        return result;
    }
    catch (const std::exception& ex)
    {
        DAS_CORE_LOG_ERROR(ex.what());
        return DAS_E_SWIG_INTERNAL_ERROR;
    }
}

DasResult SwigToCpp<IDasSwigTask>::GetNextExecutionTime(DasDate* p_out_date)
{
    DAS_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD_IMPL_AND_HANDLE_EXCEPTION(
        p_impl_.Get(),
        &IDasSwigTask::GetNextExecutionTime,
        p_out_date);
}

DasResult SwigToCpp<IDasSwigTask>::GetName(IDasReadOnlyString** pp_out_name)
{
    DAS_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD_IMPL_AND_HANDLE_EXCEPTION(
        p_impl_.Get(),
        &IDasSwigTask::GetName,
        pp_out_name);
}

DasResult SwigToCpp<IDasSwigTask>::GetDescription(
    IDasReadOnlyString** pp_out_settings)
{
    DAS_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD_IMPL_AND_HANDLE_EXCEPTION(
        p_impl_.Get(),
        &IDasSwigTask::GetDescription,
        pp_out_settings);
}

DasResult SwigToCpp<IDasSwigTask>::GetGameName(IDasReadOnlyString** pp_out_label)
{
    DAS_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD_IMPL_AND_HANDLE_EXCEPTION(
        p_impl_.Get(),
        &IDasSwigTask::GetGameName,
        pp_out_label);
}

DAS_IMPL SwigToCpp<IDasSwigGuidVector>::Size(size_t* p_out_size)
{
    DAS_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD_IMPL_AND_HANDLE_EXCEPTION(
        p_impl_.Get(),
        &IDasSwigGuidVector::Size,
        p_out_size);
}

DAS_IMPL SwigToCpp<IDasSwigGuidVector>::At(size_t index, DasGuid* p_out_iid)
{
    DAS_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD_IMPL_AND_HANDLE_EXCEPTION(
        p_impl_.Get(),
        &IDasSwigGuidVector::At,
        p_out_iid,
        index);
}

DAS_IMPL SwigToCpp<IDasSwigGuidVector>::Find(const DasGuid& iid)
{
    try
    {
        return p_impl_->Find(iid);
    }
    catch (const std::exception& ex)
    {
        DAS_CORE_LOG_ERROR(ex.what());
        return DAS_E_SWIG_INTERNAL_ERROR;
    }
}

DAS_IMPL SwigToCpp<IDasSwigGuidVector>::PushBack(const DasGuid& iid)
{
    try
    {
        return p_impl_->PushBack(iid);
    }
    catch (const std::exception& ex)
    {
        DAS_CORE_LOG_ERROR(ex.what());
        return DAS_E_SWIG_INTERNAL_ERROR;
    }
}

DasResult SwigToCpp<IDasSwigGuidVector>::ToConst(
    IDasReadOnlyGuidVector** pp_out_object)
{
    auto swig_result = p_impl_->ToConst();
    if (IsFailed(swig_result.error_code))
    {
        return swig_result.error_code;
    }

    DasPtr<IDasReadOnlyGuidVector> p_result{};

    const auto expected_result =
        MakeInterop<IDasReadOnlyGuidVector>(swig_result.value);
    if (!expected_result)
    {
        return expected_result.error();
    }

    DAS_UTILS_CHECK_POINTER(pp_out_object)
    const auto value = expected_result.value().Get();
    *pp_out_object = value;
    value->AddRef();
    return DAS_S_OK;
}

DAS_IMPL SwigToCpp<IDasSwigReadOnlyGuidVector>::Size(size_t* p_out_size)
{
    DAS_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD_IMPL_AND_HANDLE_EXCEPTION(
        p_impl_.Get(),
        &IDasSwigReadOnlyGuidVector::Size,
        p_out_size);
}

DAS_IMPL SwigToCpp<IDasSwigReadOnlyGuidVector>::At(
    size_t   index,
    DasGuid* p_out_iid)
{
    DAS_CORE_FOREIGNINTERFACEHOST_CALL_SWIG_METHOD_IMPL_AND_HANDLE_EXCEPTION(
        p_impl_.Get(),
        &IDasSwigReadOnlyGuidVector::At,
        p_out_iid,
        index);
}

DAS_IMPL SwigToCpp<IDasSwigReadOnlyGuidVector>::Find(const DasGuid& iid)
{
    try
    {
        return p_impl_->Find(iid);
    }
    catch (const std::exception& ex)
    {
        DAS_CORE_LOG_ERROR(ex.what());
        return DAS_E_SWIG_INTERNAL_ERROR;
    }
}

template <is_das_swig_interface SwigT, is_das_interface T>
DasResult SwigToCppInput<SwigT, T>::Click(int32_t x, int32_t y)
{
    return Base::p_impl_->Click(x, y);
}

DasResult SwigToCpp<IDasSwigTouch>::Swipe(
    DasPoint from,
    DasPoint to,
    int32_t  duration_ms)
{
    return p_impl_->Swipe(from, to, duration_ms);
}

DasResult SwigToCpp<IDasSwigInputFactory>::CreateInstance(
    IDasReadOnlyString* p_json_config,
    IDasInput**         pp_out_input)
{
    DAS_UTILS_CHECK_POINTER(p_json_config)
    DAS_UTILS_CHECK_POINTER(pp_out_input)

    const auto swig_result = p_impl_->CreateInstance({p_json_config});
    const auto expected_result = MakeInterop<IDasInput>(swig_result.value);
    if (expected_result)
    {
        const auto& value = expected_result.value();
        *pp_out_input = value.Get();
        value->AddRef();
        return DAS_S_OK;
    }
    return expected_result.error();
}

DasResult SwigToCpp<IDasSwigComponent>::Dispatch(
    IDasReadOnlyString* p_function_name,
    IDasVariantVector*  p_arguments,
    IDasVariantVector** pp_out_result)
{
    DasPtr<IDasSwigVariantVector> p_swig_in;
    if (const auto qi_in_result = p_arguments->QueryInterface(
            DasIidOf<IDasSwigVariantVector>(),
            p_swig_in.PutVoid());
        IsFailed(qi_in_result))
    {
        DAS_CORE_LOG_ERROR(
            "Unsupported IDasVariantVector implementation. Error code = {}. Pointer = {}.",
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
                value.value->QueryInterface(DasIidOf<IDasVariantVector>());
            if (IsFailed(qi_p_result.error_code))
            {
                DAS_CORE_LOG_ERROR(
                    "Unsupported IDasSwigVariantVector implementation when reading result. Pointer = {}.",
                    Utils::VoidP(value.value.Get()));
                return qi_p_result.error_code;
            }
            auto& p_out_result = *pp_out_result;
            p_out_result = static_cast<IDasVariantVector*>(qi_p_result.value);
            p_out_result->AddRef();
        }
        return value.error_code;
    }
    catch (const std::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_SWIG_INTERNAL_ERROR;
    }
}

// TODO: IDasSwigCaptureFactory CreateInstance

DasResult CommonPluginEnumFeature(
    const CommonPluginPtr& p_this,
    size_t                 index,
    DasPluginFeature*      p_out_feature)
{
    if (p_out_feature == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    return std::visit(
        DAS::Utils::overload_set{
            [index,
             p_out_feature](DasPtr<IDasPluginPackage> p_plugin) -> DasResult
            { return p_plugin->EnumFeature(index, p_out_feature); },
            [index, p_out_feature](
                DasPtr<IDasSwigPluginPackage> p_swig_plugin) -> DasResult
            {
                const auto result = p_swig_plugin->EnumFeature(index);
                if (DAS::IsOk(result.error_code))
                {
                    *p_out_feature = result.value;
                }
                return result.error_code;
            }},
        p_this);
}

DasRetImage CppToSwig<IDasCapture>::Capture()
{
    return CallCppMethod<
        DasRetImage,
        IDasImage,
        DAS_DV_V(&IDasCapture::Capture)>(p_impl_.Get());
}

DasRetUInt CppToSwig<IDasGuidVector>::Size()
{
    DasRetUInt swig_result;
    swig_result.error_code = p_impl_->Size(&swig_result.value);

    return swig_result;
}

DasRetGuid CppToSwig<IDasGuidVector>::At(size_t index)
{
    DasRetGuid swig_result;
    swig_result.error_code = p_impl_->At(index, &swig_result.value);

    return swig_result;
}

DasResult CppToSwig<IDasGuidVector>::Find(const DasGuid& guid)
{
    return p_impl_->Find(guid);
}

DasResult CppToSwig<IDasGuidVector>::PushBack(const DasGuid& guid)
{
    return p_impl_->PushBack(guid);
}

DasRetReadOnlyGuidVector CppToSwig<IDasGuidVector>::ToConst()
{
    DasPtr<IDasReadOnlyGuidVector> p_const_result{};
    if (const auto tc_result = p_impl_->ToConst(p_const_result.Put());
        IsFailed(tc_result))
    {
        return {tc_result};
    }

    DasPtr<IDasSwigReadOnlyGuidVector> p_result{};
    try
    {
        p_result = MakeDasPtr<
            IDasSwigReadOnlyGuidVector,
            CppToSwig<IDasReadOnlyGuidVector>>(p_const_result);
    }
    catch (const std::bad_alloc&)
    {
        return {DAS_E_OUT_OF_MEMORY};
    }

    return {DAS_S_OK, p_result.Get()};
}

DasRetUInt CppToSwig<IDasReadOnlyGuidVector>::Size()
{
    DasRetUInt swig_result;
    swig_result.error_code = p_impl_->Size(&swig_result.value);

    return swig_result;
}

DasRetGuid CppToSwig<IDasReadOnlyGuidVector>::At(size_t index)
{
    DasRetGuid swig_result;
    swig_result.error_code = p_impl_->At(index, &swig_result.value);

    return swig_result;
}

DasResult CppToSwig<IDasReadOnlyGuidVector>::Find(const DasGuid& guid)
{
    return p_impl_->Find(guid);
}

template <is_das_swig_interface SwigT, is_das_interface T>
DasResult CppToSwigInput<SwigT, T>::Click(const int32_t x, const int32_t y)
{
    return Base::p_impl_->Click(x, y);
}

DasResult CppToSwig<IDasTouch>::Swipe(
    DasPoint      from,
    DasPoint      to,
    const int32_t duration_ms)
{
    return p_impl_->Swipe(from, to, duration_ms);
}

DasRetInput CppToSwig<IDasInputFactory>::CreateInstance(
    DasReadOnlyString json_config)
{
    try
    {
        DasRetInput       swig_result{};
        DasPtr<IDasInput> p_cpp_result{};
        swig_result.error_code =
            p_impl_->CreateInstance(json_config.Get(), p_cpp_result.Put());
        const auto expected_result =
            MakeInterop<IDasSwigInput>(p_cpp_result.Get());
        ToDasRetType(expected_result, swig_result);
        return swig_result;
    }
    catch (const std::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return {DAS_E_SWIG_INTERNAL_ERROR};
    }
}

DasRetVariantVector CppToSwig<IDasComponent>::Dispatch(
    DasReadOnlyString      function_name,
    IDasSwigVariantVector* p_arguments)
{
    const auto qi_result = p_arguments->QueryInterface(DAS_IID_VARIANT_VECTOR);
    if (IsFailed(qi_result.error_code))
    {
        DAS_CORE_LOG_ERROR(
            "Unsupported IDasSwigVariantVector implementation. Pointer = {}.",
            Utils::VoidP(p_arguments));
        return {qi_result.error_code};
    }
    try
    {
        DasRetVariantVector       result;
        DasPtr<IDasVariantVector> p_result;
        result.error_code = p_impl_->Dispatch(
            function_name.Get(),
            static_cast<IDasVariantVector*>(qi_result.value),
            p_result.Put());
        DasPtr<IDasSwigVariantVector> p_swig_result;
        if (const auto qi_p_result = p_result.As(p_swig_result);
            IsFailed(qi_p_result))
        {
            DAS_CORE_LOG_ERROR(
                "Unsupported IDasVariantVector implementation when reading result. Pointer = {}.",
                Utils::VoidP(p_result.Get()));
            return {qi_p_result};
        }
        result.value = p_swig_result;
        return result;
    }
    catch (const std::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return {DAS_E_SWIG_INTERNAL_ERROR};
    }
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

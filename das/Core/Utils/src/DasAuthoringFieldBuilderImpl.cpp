#include "ProtocolBuilderState.h"

#include <das/Core/Logger/Logger.h>
#include <das/Core/Utils/DasAuthoringFieldBuilderImpl.h>
#include <das/_autogen/idl/header/IDasAuthoringFieldBuilder.generated.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>

#include <exception>
#include <new>

DAS_CORE_UTILS_NS_BEGIN

DasAuthoringFieldBuilderImpl::DasAuthoringFieldBuilderImpl()
{
    const auto init_result = Details::InitBuilderStateAsEmptyObject(state_);
    if (DAS::IsFailed(init_result))
    {
        throw std::runtime_error{"init authoring-field builder state failed"};
    }
}

DasResult DasAuthoringFieldBuilderImpl::SetName(IDasReadOnlyString* p_in_name)
{
    DAS_UTILS_CHECK_POINTER(p_in_name);

    const auto key_string = DasReadOnlyString::FromUtf8("name", nullptr);
    return state_->SetStringByName(key_string.Get(), p_in_name);
}

DasResult DasAuthoringFieldBuilderImpl::SetLabel(IDasReadOnlyString* p_in_label)
{
    DAS_UTILS_CHECK_POINTER(p_in_label);

    const auto key_string = DasReadOnlyString::FromUtf8("label", nullptr);
    return state_->SetStringByName(key_string.Get(), p_in_label);
}

DasResult DasAuthoringFieldBuilderImpl::SetValueType(int32_t in_value_type)
{
    const auto key_string = DasReadOnlyString::FromUtf8("valueType", nullptr);
    return state_->SetIntByName(key_string.Get(), in_value_type);
}

DasResult DasAuthoringFieldBuilderImpl::SetInputType(int32_t in_input_type)
{
    const auto key_string = DasReadOnlyString::FromUtf8("inputType", nullptr);
    return state_->SetIntByName(key_string.Get(), in_input_type);
}

DasResult DasAuthoringFieldBuilderImpl::SetValue(
    Das::ExportInterface::IDasJson* p_in_value)
{
    DAS_UTILS_CHECK_POINTER(p_in_value);

    const auto key_string = DasReadOnlyString::FromUtf8("value", nullptr);
    return state_->SetObjectByName(key_string.Get(), p_in_value);
}

DasResult DasAuthoringFieldBuilderImpl::SetDescription(
    IDasReadOnlyString* p_in_description)
{
    DAS_UTILS_CHECK_POINTER(p_in_description);

    const auto key_string = DasReadOnlyString::FromUtf8("description", nullptr);
    return state_->SetStringByName(key_string.Get(), p_in_description);
}

DasResult DasAuthoringFieldBuilderImpl::SetRequired(bool in_required)
{
    const auto key_string = DasReadOnlyString::FromUtf8("required", nullptr);
    return state_->SetBoolByName(key_string.Get(), in_required);
}

DasResult DasAuthoringFieldBuilderImpl::SetReadonly(bool in_readonly)
{
    const auto key_string = DasReadOnlyString::FromUtf8("readonly", nullptr);
    return state_->SetBoolByName(key_string.Get(), in_readonly);
}

DasResult DasAuthoringFieldBuilderImpl::ToJson(
    Das::ExportInterface::IDasJson** pp_out_json)
{
    return Details::ExportBuilderStateDeepCopy(state_.Get(), pp_out_json);
}

DasResult DasAuthoringFieldBuilderImpl::Clear() { return state_->Clear(); }

DAS_CORE_UTILS_NS_END

DasResult CreateIDasAuthoringFieldBuilder(
    Das::ExportInterface::IDasAuthoringFieldBuilder** pp_out_builder)
{
    DAS_UTILS_CHECK_POINTER(pp_out_builder);

    try
    {
        auto* result = new DAS::Core::Utils::DasAuthoringFieldBuilderImpl{};
        result->AddRef();
        *pp_out_builder = result;
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
    catch (const std::exception&)
    {
        return DAS_E_FAIL;
    }
}

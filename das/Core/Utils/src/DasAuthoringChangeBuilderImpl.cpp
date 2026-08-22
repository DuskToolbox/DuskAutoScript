#include "ProtocolBuilderState.h"

#include <das/Core/Logger/Logger.h>
#include <das/Core/Utils/DasAuthoringChangeBuilderImpl.h>
#include <das/_autogen/idl/header/IDasAuthoringChangeBuilder.generated.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>

#include <exception>
#include <new>

DAS_CORE_UTILS_NS_BEGIN

DasAuthoringChangeBuilderImpl::DasAuthoringChangeBuilderImpl()
{
    const auto init_result = Details::InitBuilderStateAsEmptyObject(state_);
    if (DAS::IsFailed(init_result))
    {
        throw std::runtime_error{"init authoring-change builder state failed"};
    }
}

DasResult DasAuthoringChangeBuilderImpl::SetKind(IDasReadOnlyString* p_in_kind)
{
    DAS_UTILS_CHECK_POINTER(p_in_kind);

    const auto key_string = DasReadOnlyString::FromUtf8("kind", nullptr);
    return state_->SetStringByName(key_string.Get(), p_in_kind);
}

DasResult DasAuthoringChangeBuilderImpl::SetPayload(
    Das::ExportInterface::IDasJson* p_in_payload)
{
    DAS_UTILS_CHECK_POINTER(p_in_payload);

    const auto key_string = DasReadOnlyString::FromUtf8("payload", nullptr);
    return state_->SetObjectByName(key_string.Get(), p_in_payload);
}

DasResult DasAuthoringChangeBuilderImpl::MakeSetValueChange(
    IDasReadOnlyString*             p_in_path,
    Das::ExportInterface::IDasJson* p_in_value)
{
    DAS_UTILS_CHECK_POINTER(p_in_path);
    DAS_UTILS_CHECK_POINTER(p_in_value);

    Das::DasPtr<Das::ExportInterface::IDasJson> payload;
    const auto payload_result = ParseDasJsonFromString("{}", payload.Put());
    if (DAS::IsFailed(payload_result))
    {
        return payload_result;
    }

    const auto path_key = DasReadOnlyString::FromUtf8("path", nullptr);
    const auto set_path_result =
        payload->SetStringByName(path_key.Get(), p_in_path);
    if (DAS::IsFailed(set_path_result))
    {
        return set_path_result;
    }

    const auto value_key = DasReadOnlyString::FromUtf8("value", nullptr);
    const auto set_value_result =
        payload->SetObjectByName(value_key.Get(), p_in_value);
    if (DAS::IsFailed(set_value_result))
    {
        return set_value_result;
    }

    const auto kind_key = DasReadOnlyString::FromUtf8("kind", nullptr);
    const auto kind_value = DasReadOnlyString::FromUtf8("setValue", nullptr);
    const auto set_kind_result =
        state_->SetStringByName(kind_key.Get(), kind_value.Get());
    if (DAS::IsFailed(set_kind_result))
    {
        return set_kind_result;
    }

    const auto payload_key = DasReadOnlyString::FromUtf8("payload", nullptr);
    return state_->SetObjectByName(payload_key.Get(), payload.Get());
}

DasResult DasAuthoringChangeBuilderImpl::ToJson(
    Das::ExportInterface::IDasJson** pp_out_json)
{
    return Details::ExportBuilderStateDeepCopy(state_.Get(), pp_out_json);
}

DasResult DasAuthoringChangeBuilderImpl::Clear() { return state_->Clear(); }

DAS_CORE_UTILS_NS_END

DasResult CreateIDasAuthoringChangeBuilder(
    Das::ExportInterface::IDasAuthoringChangeBuilder** pp_out_builder)
{
    DAS_UTILS_CHECK_POINTER(pp_out_builder);

    try
    {
        auto* result = new DAS::Core::Utils::DasAuthoringChangeBuilderImpl{};
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

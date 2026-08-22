#include "ProtocolBuilderState.h"

#include <das/Core/Logger/Logger.h>
#include <das/Core/Utils/DasSettingsChangeBuilderImpl.h>
#include <das/_autogen/idl/header/IDasSettingsChangeBuilder.generated.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>

#include <exception>
#include <new>

DAS_CORE_UTILS_NS_BEGIN

DasSettingsChangeBuilderImpl::DasSettingsChangeBuilderImpl()
{
    const auto init_result = Details::InitBuilderStateAsEmptyObject(state_);
    if (DAS::IsFailed(init_result))
    {
        throw std::runtime_error{"init settings-change builder state failed"};
    }
}

DasResult DasSettingsChangeBuilderImpl::SetSettings(
    Das::ExportInterface::IDasJson* p_in_settings)
{
    DAS_UTILS_CHECK_POINTER(p_in_settings);

    const auto key_string = DasReadOnlyString::FromUtf8("settings", nullptr);
    return state_->SetObjectByName(key_string.Get(), p_in_settings);
}

DasResult DasSettingsChangeBuilderImpl::SetPayload(
    Das::ExportInterface::IDasJson* p_in_payload)
{
    DAS_UTILS_CHECK_POINTER(p_in_payload);

    const auto key_string = DasReadOnlyString::FromUtf8("payload", nullptr);
    return state_->SetObjectByName(key_string.Get(), p_in_payload);
}

DasResult DasSettingsChangeBuilderImpl::SetAcceptedSettings(
    Das::ExportInterface::IDasJson* p_in_accepted)
{
    DAS_UTILS_CHECK_POINTER(p_in_accepted);

    const auto key_string =
        DasReadOnlyString::FromUtf8("acceptedSettings", nullptr);
    return state_->SetObjectByName(key_string.Get(), p_in_accepted);
}

DasResult DasSettingsChangeBuilderImpl::ToJson(
    Das::ExportInterface::IDasJson** pp_out_json)
{
    return Details::ExportBuilderStateDeepCopy(state_.Get(), pp_out_json);
}

DasResult DasSettingsChangeBuilderImpl::Clear() { return state_->Clear(); }

DAS_CORE_UTILS_NS_END

DasResult CreateIDasSettingsChangeBuilder(
    Das::ExportInterface::IDasSettingsChangeBuilder** pp_out_builder)
{
    DAS_UTILS_CHECK_POINTER(pp_out_builder);

    try
    {
        auto* result = new DAS::Core::Utils::DasSettingsChangeBuilderImpl{};
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

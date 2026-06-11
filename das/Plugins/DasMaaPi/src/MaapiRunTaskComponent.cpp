#define DAS_BUILD_SHARED

#include "MaapiRunTaskComponent.h"

#include "PluginUtils.h"

#include <das/DasString.hpp>
#include <das/Plugins/DasMaaPi/MaaPiExecutionEngine.h>
#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Plugins/DasMaaPi/MaapiDto.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/abi/IDasStringVector.h>

#include <new>
#include <string_view>

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    namespace
    {
        DasResult ReturnJson(
            yyjson::value                     value,
            ExportInterface::IDasJson** const pp_out_result_json)
        {
            auto wrapped = WrapJson(std::move(value));
            if (!wrapped)
            {
                return DAS_E_INVALID_JSON;
            }

            *pp_out_result_json = wrapped.Get();
            (*pp_out_result_json)->AddRef();
            return DAS_S_OK;
        }
    } // namespace

    DasResult MaapiRunTaskComponent::GetGuid(DasGuid* p_out_guid)
    {
        if (p_out_guid == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *p_out_guid = DasIidOf<MaapiRunTaskComponent>();
        return DAS_S_OK;
    }

    DasResult MaapiRunTaskComponent::GetRuntimeClassName(
        IDasReadOnlyString** pp_out_name)
    {
        if (pp_out_name == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        return CreateIDasReadOnlyStringFromUtf8(
            "Das.MaaPi.RunTaskComponent",
            pp_out_name);
    }

    DasResult MaapiRunTaskComponent::ApplySettingsChange(
        ExportInterface::IDasJson*  p_request_json,
        ExportInterface::IDasJson** pp_out_result_json)
    {
        if (pp_out_result_json == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_result_json = nullptr;

        if (p_request_json != nullptr)
        {
            auto parsed = ReadJson(p_request_json);
            if (parsed)
            {
                // GraphRuntime::ApplyNodeSettings wraps the compiled_settings
                // inside {"settings": <value>, "payload": <value>}.
                // Extract the inner settings object when the wrapper is
                // present.
                auto obj = parsed->as_object();
                if (obj && obj->contains(std::string_view("settings")))
                {
                    auto inner = (*obj)[std::string_view("settings")];
                    if (!inner.is_null())
                    {
                        settings_ = Das::Utils::CloneYyjsonValue(inner);
                        settings_applied_ = true;
                    }
                }
                else
                {
                    settings_ = Das::Utils::CloneYyjsonValue(*parsed);
                    settings_applied_ = true;
                }
            }
        }

        auto result = Das::Utils::MakeYyjsonObject();
        (*result.as_object())[std::string_view("acceptedSettings")] =
            Das::Utils::MakeYyjsonObject();
        return ReturnJson(std::move(result), pp_out_result_json);
    }

    DasResult MaapiRunTaskComponent::Do(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map)
    {
        if (pp_out_port_map == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_port_map = nullptr;

        DasPtr<ExportInterface::IDasPortMap> output_map;
        DasResult hr = CreateIDasPortMap(output_map.Put());
        if (DAS::IsFailed(hr))
        {
            return hr;
        }

        if (!settings_applied_)
        {
            return DAS_E_OBJECT_NOT_INIT;
        }

        auto obj = settings_.as_object();
        if (!obj)
        {
            return DAS_E_OBJECT_NOT_INIT;
        }

        EngineInput input;
        input.pi_path = (*obj)["piPath"].as_string().value_or("");
        input.task_name = (*obj)["taskName"].as_string().value_or("");

        auto options_val = (*obj)["options"];
        if (options_val.is_object() || options_val.is_array())
        {
            input.options = Das::Utils::CloneYyjsonValue(options_val);
        }
        else
        {
            input.options = Das::Utils::MakeYyjsonObject();
        }

        auto port_map_val = (*obj)["portMap"];
        if (port_map_val.is_object())
        {
            auto port_map_obj = port_map_val.as_object();
            for (auto&& [key, value] : *port_map_obj)
            {
                auto pi_param = value.as_string();
                if (pi_param)
                {
                    input.port_map[std::string(key)] = std::string(*pi_param);
                }
            }
        }

        if (p_input_port_map)
        {
            DasPtr<ExportInterface::IDasStringVector> keys;
            hr = p_input_port_map->GetKeys(keys.Put());
            if (DAS::IsOk(hr) && keys)
            {
                uint64_t count = 0;
                keys->Size(&count);
                for (uint64_t i = 0; i < count; ++i)
                {
                    DasPtr<IDasReadOnlyString> p_key;
                    if (DAS::IsFailed(keys->At(i, p_key.Put())) || !p_key)
                    {
                        continue;
                    }

                    std::string key_str;
                    {
                        const char* raw = nullptr;
                        p_key->GetUtf8(&raw);
                        if (raw)
                        {
                            key_str = raw;
                        }
                    }

                    auto pm_it = input.port_map.find(key_str);
                    if (pm_it == input.port_map.end())
                    {
                        continue;
                    }

                    ExportInterface::DasVariantType type =
                        ExportInterface::DAS_VARIANT_TYPE_NULL;
                    hr = p_input_port_map->GetType(p_key.Get(), &type);

                    if (DAS::IsFailed(hr))
                    {
                        continue;
                    }

                    if (type == ExportInterface::DAS_VARIANT_TYPE_STRING)
                    {
                        DasPtr<IDasReadOnlyString> p_val;
                        if (DAS::IsOk(p_input_port_map->GetString(
                                DasReadOnlyString(key_str.c_str()).Get(),
                                p_val.Put()))
                            && p_val)
                        {
                            DasReadOnlyString val_reader{p_val.Get()};
                            auto arr = Das::Utils::MakeYyjsonArray();
                            arr.as_array()->emplace_back(
                                yyjson::value(
                                    std::string{val_reader.GetUtf8()}));
                            input.inputs[key_str] = std::move(arr);
                        }
                    }
                    else if (type == ExportInterface::DAS_VARIANT_TYPE_INT)
                    {
                        int64_t val = 0;
                        if (DAS::IsOk(p_input_port_map->GetInt(
                                DasReadOnlyString(key_str.c_str()).Get(),
                                &val)))
                        {
                            input.inputs[key_str] = yyjson::value(val);
                        }
                    }
                    else if (type == ExportInterface::DAS_VARIANT_TYPE_BOOL)
                    {
                        bool val = false;
                        if (DAS::IsOk(p_input_port_map->GetBool(
                                DasReadOnlyString(key_str.c_str()).Get(),
                                &val)))
                        {
                            input.inputs[key_str] = yyjson::value(val);
                        }
                    }
                    else if (type == ExportInterface::DAS_VARIANT_TYPE_FLOAT)
                    {
                        double val = 0.0;
                        if (DAS::IsOk(p_input_port_map->GetFloat(
                                DasReadOnlyString(key_str.c_str()).Get(),
                                &val)))
                        {
                            input.inputs[key_str] = yyjson::value(val);
                        }
                    }
                }
            }
        }

        MaaPiExecutionEngine engine;
        EngineOutput         output =
            engine.Execute(input, MaaApiBoundaryForRuntime(), stop_token);

        output_map->SetBool(
            DasReadOnlyString("stopped").Get(),
            output.das_result == DAS_E_TIMEOUT);

        if (!output.diagnostics.empty())
        {
            auto diag_arr = Das::Utils::MakeYyjsonArray();
            for (const auto& d : output.diagnostics)
            {
                auto obj = Das::Utils::MakeYyjsonObject();
                auto o = obj.as_object();
                (*o)[std::string_view("severity")] = std::make_pair(
                    std::string_view(d.severity),
                    yyjson::copy_string);
                (*o)[std::string_view("code")] = std::make_pair(
                    std::string_view(d.code),
                    yyjson::copy_string);
                (*o)[std::string_view("message")] = std::make_pair(
                    std::string_view(d.message),
                    yyjson::copy_string);
                diag_arr.as_array()->emplace_back(std::move(obj));
            }
            auto serialized = Das::Utils::SerializeYyjsonValue(diag_arr);
            if (serialized)
            {
                output_map->SetString(
                    DasReadOnlyString("diagnostics").Get(),
                    DasReadOnlyString(serialized->c_str()).Get());
            }
        }
        else
        {
            output_map->SetString(
                DasReadOnlyString("diagnostics").Get(),
                DasReadOnlyString("[]").Get());
        }

        for (const auto& [port_id, value] : output.outputs)
        {
            auto serialized = Das::Utils::SerializeYyjsonValue(value);
            if (serialized)
            {
                output_map->SetString(
                    DasReadOnlyString(port_id.c_str()).Get(),
                    DasReadOnlyString(serialized->c_str()).Get());
            }
        }

        *pp_out_port_map = output_map.Get();
        output_map.Get()->AddRef();

        if (DAS::IsFailed(output.das_result))
        {
            return output.das_result;
        }

        return DAS_S_OK;
    }

    DasResult MaapiRunTaskComponentFactory::GetGuid(DasGuid* p_out_guid)
    {
        if (p_out_guid == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *p_out_guid = DasIidOf<MaapiRunTaskComponentFactory>();
        return DAS_S_OK;
    }

    DasResult MaapiRunTaskComponentFactory::GetRuntimeClassName(
        IDasReadOnlyString** pp_out_name)
    {
        if (pp_out_name == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        return CreateIDasReadOnlyStringFromUtf8(
            "Das.MaaPi.RunTaskComponentFactory",
            pp_out_name);
    }

    DasResult MaapiRunTaskComponentFactory::CreateComponent(
        const DasGuid&                       component_guid,
        PluginInterface::IDasTaskComponent** pp_out_component)
    {
        if (pp_out_component == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_component = nullptr;
        if (component_guid != DasIidOf<MaapiRunTaskComponent>())
        {
            return DAS_E_NOT_FOUND;
        }

        try
        {
            auto* component = new MaapiRunTaskComponent();
            component->AddRef();
            *pp_out_component = component;
            return DAS_S_OK;
        }
        catch (const std::bad_alloc&)
        {
            return DAS_E_OUT_OF_MEMORY;
        }
    }

    DasResult MaapiRunTaskComponentFactory::SetTaskComponentHost(
        PluginInterface::IDasTaskComponentHost* p_host)
    {
        host_ = DasPtr<PluginInterface::IDasTaskComponentHost>(p_host);
        return DAS_S_OK;
    }
} // namespace Plugins::DasMaaPi
DAS_NS_END

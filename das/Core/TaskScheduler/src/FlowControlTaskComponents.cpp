#include <das/Core/TaskScheduler/FlowControlTaskComponents.h>
#include <das/Core/TaskScheduler/TaskComponentRuntime.h>

#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/Utils/DasJsonImpl.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasComponent.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponent.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponentFactory.Implements.hpp>

#include <optional>
#include <string>

namespace Das::Core::TaskScheduler
{
    namespace
    {
        yyjson::value CloneJson(const yyjson::value& value)
        {
            auto serialized = value.write(yyjson::WriteFlag::NoFlag);
            auto parsed = Das::Utils::ParseYyjsonFromString(
                std::string_view(serialized.data(), serialized.size()));
            return parsed ? std::move(*parsed) : yyjson::value{};
        }

        DasPtr<Das::ExportInterface::IDasJson> WrapJson(yyjson::value value)
        {
            return Das::MakeDasPtr<Das::Core::Utils::IDasJsonImpl>(
                std::move(value));
        }

        std::optional<DasGuid> GuidFromString(std::string_view value)
        {
            try
            {
                return Das::Core::ForeignInterfaceHost::MakeDasGuid(
                    std::string{value});
            }
            catch (const std::exception&)
            {
                return std::nullopt;
            }
        }

        const FlowControl::ComponentSpec* FindSpec(const DasGuid& guid)
        {
            for (const auto& spec : FlowControl::kOfficialComponents)
            {
                auto spec_guid = GuidFromString(spec.guid);
                if (spec_guid && *spec_guid == guid)
                {
                    return &spec;
                }
            }
            return nullptr;
        }

        yyjson::value MakePort(
            std::string_view id,
            std::string_view label,
            std::string_view type)
        {
            auto port = Das::Utils::MakeYyjsonObject();
            auto obj = *port.as_object();
            obj[std::string_view("id")] = id;
            obj[std::string_view("label")] = label;
            obj[std::string_view("type")] = type;
            return port;
        }

        yyjson::value MakeDefinition(const FlowControl::ComponentSpec& spec)
        {
            auto definition = Das::Utils::MakeYyjsonObject();
            auto obj = *definition.as_object();
            obj[std::string_view("stableName")] = spec.stable_name;
            obj[std::string_view("componentGuid")] = spec.guid;
            obj[std::string_view("kind")] = spec.kind;
            obj[std::string_view("label")] = spec.label;
            obj[std::string_view("traits")] = Das::Utils::MakeYyjsonObject();
            (*obj[std::string_view("traits")]
                  .as_object())[std::string_view("flowControl")] = true;

            auto inputs = Das::Utils::MakeYyjsonArray();
            auto outputs = Das::Utils::MakeYyjsonArray();
            auto in_arr = *inputs.as_array();
            auto out_arr = *outputs.as_array();

            if (spec.kind == "branch")
            {
                in_arr.emplace_back(MakePort("condition", "Condition", "bool"));
                out_arr.emplace_back(MakePort("true", "True", "signal"));
                out_arr.emplace_back(MakePort("false", "False", "signal"));
            }
            else if (spec.kind == "goto")
            {
                in_arr.emplace_back(MakePort("target", "Target", "string"));
                out_arr.emplace_back(MakePort("next", "Next", "signal"));
            }
            else
            {
                in_arr.emplace_back(MakePort("in", "In", "signal"));
                out_arr.emplace_back(MakePort("next", "Next", "signal"));
            }

            obj[std::string_view("inputs")] = std::move(inputs);
            obj[std::string_view("outputs")] = std::move(outputs);
            obj[std::string_view("config")] = Das::Utils::MakeYyjsonObject();
            obj[std::string_view("diagnostics")] =
                Das::Utils::MakeYyjsonArray();
            return definition;
        }

        class FlowControlTaskComponent final
            : public Das::PluginInterface::DasTaskComponentImplBase<
                  FlowControlTaskComponent>
        {
        public:
            explicit FlowControlTaskComponent(FlowControl::ComponentSpec spec)
                : spec_(spec), settings_(Das::Utils::MakeYyjsonObject())
            {
            }

            DasResult GetDefinition(
                Das::ExportInterface::IDasJson** pp_out_definition_json)
                override
            {
                if (pp_out_definition_json == nullptr)
                {
                    return DAS_E_INVALID_POINTER;
                }
                auto wrapped = WrapJson(MakeDefinition(spec_));
                *pp_out_definition_json = wrapped.Get();
                (*pp_out_definition_json)->AddRef();
                return DAS_S_OK;
            }

            DasResult ApplySettingsChange(
                Das::ExportInterface::IDasJson*  p_request_json,
                Das::ExportInterface::IDasJson** pp_out_result_json) override
            {
                if (pp_out_result_json == nullptr)
                {
                    return DAS_E_INVALID_POINTER;
                }

                yyjson::value accepted = CloneJson(settings_);
                if (p_request_json != nullptr)
                {
                    DasPtr<IDasReadOnlyString> request_string;
                    if (DAS::IsOk(
                            p_request_json->ToString(0, request_string.Put()))
                        && request_string)
                    {
                        const char* request_u8 = nullptr;
                        request_string->GetUtf8(&request_u8);
                        auto parsed = Das::Utils::ParseYyjsonFromString(
                            request_u8 != nullptr ? request_u8 : "");
                        if (parsed && parsed->is_object())
                        {
                            auto request_obj = *parsed->as_object();
                            auto kind = request_obj[std::string_view("kind")]
                                            .as_string();
                            if (kind && *kind == "setValue")
                            {
                                auto payload =
                                    request_obj[std::string_view("payload")]
                                        .as_object();
                                if (payload)
                                {
                                    auto key =
                                        (*payload)[std::string_view("path")]
                                            .as_string();
                                    if (key
                                        && payload->contains(
                                            std::string_view("value")))
                                    {
                                        (*accepted
                                              .as_object())[std::string_view(
                                            *key)] =
                                            CloneJson(
                                                (*payload)[std::string_view(
                                                    "value")]);
                                    }
                                }
                            }
                        }
                    }
                }

                settings_ = CloneJson(accepted);
                auto result = Das::Utils::MakeYyjsonObject();
                auto obj = *result.as_object();
                obj[std::string_view("acceptedSettings")] = std::move(accepted);
                auto wrapped = WrapJson(std::move(result));
                *pp_out_result_json = wrapped.Get();
                (*pp_out_result_json)->AddRef();
                return DAS_S_OK;
            }

            DasResult Do(
                Das::PluginInterface::IDasStopToken* stop_token,
                Das::ExportInterface::IDasJson*,
                Das::ExportInterface::IDasJson*,
                Das::ExportInterface::IDasJson*  p_input_json,
                Das::ExportInterface::IDasJson** pp_out_result_json) override
            {
                if (pp_out_result_json == nullptr)
                {
                    return DAS_E_INVALID_POINTER;
                }

                bool stop_requested = false;
                if (stop_token != nullptr
                    && DAS::IsOk(stop_token->StopRequested(&stop_requested))
                    && stop_requested)
                {
                    auto wrapped = WrapJson(MakeTaskComponentResult(
                        "cancelled",
                        Das::Utils::MakeYyjsonObject(),
                        Das::Utils::MakeYyjsonArray()));
                    *pp_out_result_json = wrapped.Get();
                    (*pp_out_result_json)->AddRef();
                    return DAS_S_OK;
                }

                bool condition = false;
                if (p_input_json != nullptr)
                {
                    DasPtr<IDasReadOnlyString> input_string;
                    if (DAS::IsOk(p_input_json->ToString(0, input_string.Put()))
                        && input_string)
                    {
                        const char* input_u8 = nullptr;
                        input_string->GetUtf8(&input_u8);
                        auto input = Das::Utils::ParseYyjsonFromString(
                            input_u8 != nullptr ? input_u8 : "");
                        if (input && input->is_object())
                        {
                            auto obj = *input->as_object();
                            auto value =
                                obj[std::string_view("condition")].as_bool();
                            condition = value.value_or(false);
                        }
                    }
                }

                auto outputs = Das::Utils::MakeYyjsonObject();
                auto signals = Das::Utils::MakeYyjsonArray();
                auto signals_arr = *signals.as_array();
                if (spec_.kind == "branch")
                {
                    (*outputs.as_object())[std::string_view("selected")] =
                        condition ? "true" : "false";
                    signals_arr.emplace_back(condition ? "true" : "false");
                }
                else
                {
                    signals_arr.emplace_back("next");
                }

                auto wrapped = WrapJson(MakeTaskComponentResult(
                    "completed",
                    std::move(outputs),
                    std::move(signals)));
                *pp_out_result_json = wrapped.Get();
                (*pp_out_result_json)->AddRef();
                return DAS_S_OK;
            }

        private:
            FlowControl::ComponentSpec spec_;
            yyjson::value              settings_;
        };

        class FlowControlTaskComponentFactory final
            : public Das::PluginInterface::DasTaskComponentFactoryImplBase<
                  FlowControlTaskComponentFactory>
        {
        public:
            DasResult GetCatalog(
                Das::ExportInterface::IDasJson** pp_out_catalog_json) override
            {
                if (pp_out_catalog_json == nullptr)
                {
                    return DAS_E_INVALID_POINTER;
                }

                auto catalog = Das::Utils::MakeYyjsonObject();
                auto components = Das::Utils::MakeYyjsonArray();
                auto components_arr = *components.as_array();
                for (const auto& spec : FlowControl::kOfficialComponents)
                {
                    auto item = Das::Utils::MakeYyjsonObject();
                    auto obj = *item.as_object();
                    obj[std::string_view("stableName")] = spec.stable_name;
                    obj[std::string_view("componentGuid")] = spec.guid;
                    obj[std::string_view("label")] = spec.label;
                    obj[std::string_view("kind")] = spec.kind;
                    components_arr.emplace_back(std::move(item));
                }
                (*catalog.as_object())[std::string_view("components")] =
                    std::move(components);

                auto wrapped = WrapJson(std::move(catalog));
                *pp_out_catalog_json = wrapped.Get();
                (*pp_out_catalog_json)->AddRef();
                return DAS_S_OK;
            }

            DasResult CreateComponent(
                const DasGuid&                            component_guid,
                Das::PluginInterface::IDasTaskComponent** pp_out_component)
                override
            {
                if (pp_out_component == nullptr)
                {
                    return DAS_E_INVALID_POINTER;
                }
                *pp_out_component = nullptr;

                auto* spec = FindSpec(component_guid);
                if (spec == nullptr)
                {
                    return DAS_E_NOT_FOUND;
                }

                auto* component = new FlowControlTaskComponent(*spec);
                component->AddRef();
                *pp_out_component = component;
                return DAS_S_OK;
            }
        };
    } // namespace

    DasPtr<Das::PluginInterface::IDasTaskComponentFactory>
    CreateOfficialFlowControlTaskComponentFactory()
    {
        auto* factory = new FlowControlTaskComponentFactory();
        factory->AddRef();
        return DasPtr<Das::PluginInterface::IDasTaskComponentFactory>::Attach(
            factory);
    }

} // namespace Das::Core::TaskScheduler

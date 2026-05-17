#include <das/Core/Debug/DebugDecorators.h>

#include <das/Core/Debug/DebugEvent.h>
#include <das/Core/Debug/DebugImageAnnotator.h>
#include <das/Core/Debug/DebugRuntime.h>
#include <das/DasPtr.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasInput.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTouch.Implements.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

extern "C++"
{
    template <>
    struct DasIidHolder<
        Das::Core::Debug::DebugDecorator<Das::PluginInterface::IDasInput>>
    {
        static constexpr DasGuid iid = {
            0x21fc6642,
            0x2045,
            0x4e20,
            {0xb6, 0xf3, 0xb8, 0x61, 0x99, 0x40, 0x70, 0x36}};
    };

    template <>
    constexpr const DasGuid& DasIidOf<
        Das::Core::Debug::DebugDecorator<Das::PluginInterface::IDasInput>>()
    {
        return DasIidHolder<Das::Core::Debug::DebugDecorator<
            Das::PluginInterface::IDasInput>>::iid;
    }

    template <>
    constexpr const DasGuid& DasIidOf<
        Das::Core::Debug::DebugDecorator<Das::PluginInterface::IDasInput>*>()
    {
        return DasIidHolder<Das::Core::Debug::DebugDecorator<
            Das::PluginInterface::IDasInput>>::iid;
    }
}

DAS_CORE_DEBUG_NS_BEGIN
namespace
{
    auto ElapsedMs(std::chrono::steady_clock::time_point start) -> double
    {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - start)
            .count();
    }

    auto SafeName(const char* p_name) -> std::string
    {
        return p_name && *p_name ? std::string{p_name} : std::string{"input"};
    }

    auto SerializeClickParams(
        const std::string& input_name,
        int32_t            x,
        int32_t            y) -> std::string
    {
        auto obj = DAS::Utils::MakeYyjsonObject();
        (*obj.as_object())[std::string_view("input_name")] = input_name;
        (*obj.as_object())[std::string_view("x")] = static_cast<int64_t>(x);
        (*obj.as_object())[std::string_view("y")] = static_cast<int64_t>(y);
        auto serialized = DAS::Utils::SerializeYyjsonValue(obj);
        return serialized.value_or("{}");
    }

    auto SerializeSwipeParams(
        const std::string&             input_name,
        Das::PluginInterface::DasPoint from,
        Das::PluginInterface::DasPoint to,
        int32_t                        duration_ms) -> std::string
    {
        auto obj = DAS::Utils::MakeYyjsonObject();
        auto from_obj = DAS::Utils::MakeYyjsonObject();
        auto to_obj = DAS::Utils::MakeYyjsonObject();

        (*from_obj.as_object())[std::string_view("x")] =
            static_cast<int64_t>(from.x);
        (*from_obj.as_object())[std::string_view("y")] =
            static_cast<int64_t>(from.y);
        (*to_obj.as_object())[std::string_view("x")] =
            static_cast<int64_t>(to.x);
        (*to_obj.as_object())[std::string_view("y")] =
            static_cast<int64_t>(to.y);

        (*obj.as_object())[std::string_view("input_name")] = input_name;
        (*obj.as_object())[std::string_view("from")] = std::move(from_obj);
        (*obj.as_object())[std::string_view("to")] = std::move(to_obj);
        (*obj.as_object())[std::string_view("duration_ms")] =
            static_cast<int64_t>(duration_ms);

        auto serialized = DAS::Utils::SerializeYyjsonValue(obj);
        return serialized.value_or("{}");
    }

    auto SerializeInputResult(
        DasResult                    result,
        const DebugImageWriteResult& image_result) -> std::string
    {
        auto obj = DAS::Utils::MakeYyjsonObject();
        (*obj.as_object())[std::string_view("result_code")] =
            static_cast<int64_t>(result);
        (*obj.as_object())[std::string_view("image_status")] =
            image_result.image_status;
        (*obj.as_object())[std::string_view("original_image_filename")] =
            image_result.original_image_filename;
        (*obj.as_object())[std::string_view("image_filename")] =
            image_result.image_filename;
        auto serialized = DAS::Utils::SerializeYyjsonValue(obj);
        return serialized.value_or("{}");
    }

    auto AnnotateClick(const std::string& input_name, int32_t x, int32_t y)
        -> DebugImageWriteResult
    {
        auto snapshot = DebugRuntime::GetLatestImage();
        if (!snapshot || !snapshot->available)
        {
            return {};
        }

        DebugImageAnnotations annotations{};
        annotations.points.push_back(
            DebugDrawPoint{x, y, 6, DebugAnnotationColor::Red, "click"});
        return SaveOriginalAndAnnotated(
            "input_click_" + input_name,
            std::move(snapshot),
            annotations);
    }

    auto AnnotateSwipe(
        const std::string&             input_name,
        Das::PluginInterface::DasPoint from,
        Das::PluginInterface::DasPoint to) -> DebugImageWriteResult
    {
        auto snapshot = DebugRuntime::GetLatestImage();
        if (!snapshot || !snapshot->available)
        {
            return {};
        }

        DebugImageAnnotations annotations{};
        annotations.lines.push_back(
            DebugDrawLine{
                from.x,
                from.y,
                to.x,
                to.y,
                2,
                DebugAnnotationColor::Blue,
                "swipe"});
        return SaveOriginalAndAnnotated(
            "input_swipe_" + input_name,
            std::move(snapshot),
            annotations);
    }

} // namespace

template <>
class DebugDecorator<Das::PluginInterface::IDasInput> final
    : public Das::PluginInterface::DasInputImplBase<
          DebugDecorator<Das::PluginInterface::IDasInput>>
{
public:
    DebugDecorator(
        Das::PluginInterface::IDasInput* p_inner,
        const char*                      p_input_name)
        : inner_(Das::DasPtr<Das::PluginInterface::IDasInput>::Attach(p_inner)),
          input_name_(SafeName(p_input_name))
    {
    }

    DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
    {
        if (!inner_)
        {
            return DAS_E_INVALID_POINTER;
        }
        return inner_->GetGuid(p_out_guid);
    }

    DasResult DAS_STD_CALL
    GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
    {
        if (!inner_)
        {
            return DAS_E_INVALID_POINTER;
        }
        return inner_->GetRuntimeClassName(pp_out_name);
    }

    DasResult DAS_STD_CALL Click(int32_t x, int32_t y) override
    {
        if (!inner_)
        {
            return DAS_E_INVALID_POINTER;
        }

        const auto start = std::chrono::steady_clock::now();
        const auto result = inner_->Click(x, y);
        const auto elapsed_ms = ElapsedMs(start);
        const auto image_result = AnnotateClick(input_name_, x, y);

        auto event = MakeDebugEvent(
            "input_click",
            SerializeClickParams(input_name_, x, y),
            SerializeInputResult(result, image_result));
        event.elapsed_ms = elapsed_ms;
        event.image_filename = image_result.image_filename;
        static_cast<void>(DebugRuntime::SubmitEvent(event));

        return result;
    }

private:
    Das::DasPtr<Das::PluginInterface::IDasInput> inner_;
    std::string                                  input_name_;
};

template <>
class DebugDecorator<Das::PluginInterface::IDasTouch> final
    : public Das::PluginInterface::DasTouchImplBase<
          DebugDecorator<Das::PluginInterface::IDasTouch>>
{
public:
    DebugDecorator(
        Das::PluginInterface::IDasInput* p_inner_input,
        Das::PluginInterface::IDasTouch* p_inner_touch,
        const char*                      p_input_name)
        : inner_input_(
              Das::DasPtr<Das::PluginInterface::IDasInput>::Attach(
                  p_inner_input)),
          inner_touch_(
              Das::DasPtr<Das::PluginInterface::IDasTouch>::Attach(
                  p_inner_touch)),
          input_name_(SafeName(p_input_name))
    {
    }

    DasResult DAS_STD_CALL
    QueryInterface(const DasGuid& iid, void** pp_out_object) override
    {
        if (!pp_out_object)
        {
            return DAS_E_INVALID_POINTER;
        }

        if (iid == DasIidOf<IDasBase>())
        {
            *pp_out_object = static_cast<IDasBase*>(static_cast<IDasTypeInfo*>(
                static_cast<Das::PluginInterface::IDasInput*>(this)));
        }
        else if (iid == DasIidOf<IDasTypeInfo>())
        {
            *pp_out_object = static_cast<IDasTypeInfo*>(
                static_cast<Das::PluginInterface::IDasInput*>(this));
        }
        else if (iid == DasIidOf<Das::PluginInterface::IDasInput>())
        {
            *pp_out_object =
                static_cast<Das::PluginInterface::IDasInput*>(this);
        }
        else if (iid == DasIidOf<Das::PluginInterface::IDasTouch>())
        {
            *pp_out_object =
                static_cast<Das::PluginInterface::IDasTouch*>(this);
        }
        else
        {
            *pp_out_object = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        AddRef();
        return DAS_S_OK;
    }

    DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
    {
        if (!inner_input_)
        {
            return DAS_E_INVALID_POINTER;
        }
        return inner_input_->GetGuid(p_out_guid);
    }

    DasResult DAS_STD_CALL
    GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
    {
        if (!inner_input_)
        {
            return DAS_E_INVALID_POINTER;
        }
        return inner_input_->GetRuntimeClassName(pp_out_name);
    }

    DasResult DAS_STD_CALL Click(int32_t x, int32_t y) override
    {
        if (!inner_input_)
        {
            return DAS_E_INVALID_POINTER;
        }

        const auto start = std::chrono::steady_clock::now();
        const auto result = inner_input_->Click(x, y);
        const auto elapsed_ms = ElapsedMs(start);
        const auto image_result = AnnotateClick(input_name_, x, y);

        auto event = MakeDebugEvent(
            "input_click",
            SerializeClickParams(input_name_, x, y),
            SerializeInputResult(result, image_result));
        event.elapsed_ms = elapsed_ms;
        event.image_filename = image_result.image_filename;
        static_cast<void>(DebugRuntime::SubmitEvent(event));

        return result;
    }

    DasResult DAS_STD_CALL Swipe(
        Das::PluginInterface::DasPoint from,
        Das::PluginInterface::DasPoint to,
        int32_t                        duration_ms) override
    {
        if (!inner_touch_)
        {
            return DAS_E_INVALID_POINTER;
        }

        const auto start = std::chrono::steady_clock::now();
        const auto result = inner_touch_->Swipe(from, to, duration_ms);
        const auto elapsed_ms = ElapsedMs(start);
        const auto image_result = AnnotateSwipe(input_name_, from, to);

        auto event = MakeDebugEvent(
            "input_swipe",
            SerializeSwipeParams(input_name_, from, to, duration_ms),
            SerializeInputResult(result, image_result));
        event.elapsed_ms = elapsed_ms;
        event.image_filename = image_result.image_filename;
        static_cast<void>(DebugRuntime::SubmitEvent(event));

        return result;
    }

private:
    Das::DasPtr<Das::PluginInterface::IDasInput> inner_input_;
    Das::DasPtr<Das::PluginInterface::IDasTouch> inner_touch_;
    std::string                                  input_name_;
};

Das::PluginInterface::IDasInput* MaybeDecorateInput(
    Das::PluginInterface::IDasInput* p_raw,
    const char*                      p_input_name)
{
    if (!p_raw || !DebugRuntime::IsEnabled())
    {
        return p_raw;
    }

    Das::PluginInterface::IDasTouch* p_touch = nullptr;
    if (p_raw->QueryInterface(
            DasIidOf<Das::PluginInterface::IDasTouch>(),
            reinterpret_cast<void**>(&p_touch))
            >= 0
        && p_touch)
    {
        return static_cast<Das::PluginInterface::IDasInput*>(
            DebugDecorator<Das::PluginInterface::IDasTouch>::MakeRaw(
                p_raw,
                p_touch,
                p_input_name));
    }

    return DebugDecorator<Das::PluginInterface::IDasInput>::MakeRaw(
        p_raw,
        p_input_name);
}

DAS_CORE_DEBUG_NS_END

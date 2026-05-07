#include <das/Core/Debug/DebugDecorators.h>

#include <das/Core/Debug/DebugEvent.h>
#include <das/Core/Debug/DebugImageAnnotator.h>
#include <das/Core/Debug/DebugRuntime.h>
#include <das/DasPtr.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasCapture.Implements.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

extern "C++"
{
    template <>
    struct DasIidHolder<
        Das::Core::Debug::DebugDecorator<Das::PluginInterface::IDasCapture>>
    {
        static constexpr DasGuid iid = {
            0xf8d43a5d,
            0x6af2,
            0x4c06,
            {0xa1, 0x0e, 0xa7, 0xb1, 0xa7, 0xb8, 0x2d, 0x41}};
    };

    template <>
    constexpr const DasGuid& DasIidOf<
        Das::Core::Debug::DebugDecorator<Das::PluginInterface::IDasCapture>>()
    {
        return DasIidHolder<
            Das::Core::Debug::DebugDecorator<
                Das::PluginInterface::IDasCapture>>::iid;
    }

    template <>
    constexpr const DasGuid& DasIidOf<
        Das::Core::Debug::DebugDecorator<Das::PluginInterface::IDasCapture>*>()
    {
        return DasIidHolder<
            Das::Core::Debug::DebugDecorator<
                Das::PluginInterface::IDasCapture>>::iid;
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

    auto SerializeCaptureParams(const std::string& capture_name) -> std::string
    {
        auto obj = DAS::Utils::MakeYyjsonObject();
        (*obj.as_object())[std::string_view("capture_name")] = capture_name;
        auto serialized = DAS::Utils::SerializeYyjsonValue(obj);
        return serialized.value_or("{}");
    }

    auto SerializeCaptureResult(
        DasResult                        result,
        const DebugImageWriteResult&     image_result) -> std::string
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

    auto SafeName(const char* p_name) -> std::string
    {
        return p_name && *p_name ? std::string{p_name} : std::string{"capture"};
    }

} // namespace

template <>
class DebugDecorator<Das::PluginInterface::IDasCapture> final
    : public Das::PluginInterface::DasCaptureImplBase<
          DebugDecorator<Das::PluginInterface::IDasCapture>>
{
public:
    DebugDecorator(
        Das::PluginInterface::IDasCapture* p_inner,
        const char*                        p_capture_name)
        : inner_(Das::DasPtr<Das::PluginInterface::IDasCapture>::Attach(
              p_inner)),
          capture_name_(SafeName(p_capture_name))
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

    DasResult DAS_STD_CALL
    Capture(Das::ExportInterface::IDasImage** pp_out_image) override
    {
        if (!inner_)
        {
            return DAS_E_INVALID_POINTER;
        }

        const auto start = std::chrono::steady_clock::now();
        const auto result = inner_->Capture(pp_out_image);
        const auto elapsed_ms = ElapsedMs(start);

        DebugImageWriteResult image_result{};
        if (result >= 0 && pp_out_image && *pp_out_image)
        {
            auto snapshot = CaptureImageSnapshot(*pp_out_image);
            DebugRuntime::SetLatestImage(snapshot);
            image_result = SaveOriginalAndAnnotated(
                "capture_" + capture_name_,
                snapshot,
                DebugImageAnnotations{});
        }

        auto event = MakeDebugEvent(
            "capture",
            SerializeCaptureParams(capture_name_),
            SerializeCaptureResult(result, image_result));
        event.elapsed_ms = elapsed_ms;
        event.image_filename = image_result.image_filename;
        static_cast<void>(DebugRuntime::SubmitEvent(event));

        return result;
    }

private:
    Das::DasPtr<Das::PluginInterface::IDasCapture> inner_;
    std::string                                    capture_name_;
};

Das::PluginInterface::IDasCapture* MaybeDecorateCapture(
    Das::PluginInterface::IDasCapture* p_raw,
    const char*                        p_capture_name)
{
    if (!p_raw || !DebugRuntime::IsEnabled())
    {
        return p_raw;
    }

    return DebugDecorator<Das::PluginInterface::IDasCapture>::MakeRaw(
        p_raw,
        p_capture_name);
}

DAS_CORE_DEBUG_NS_END

#ifndef DAS_PLUGINS_DASADBCAPTURE_ADBCAPTUREIMPL_H
#define DAS_PLUGINS_DASADBCAPTURE_ADBCAPTUREIMPL_H

#include <das/DasConfig.h>
#include <das/IDasBase.h>
#include <das/PluginInterface/IDasCapture.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/Expected.h>
#include <filesystem>
#include <cstdint>

// {C2300184-A311-4880-8966-53F57519F32A}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das,
    AdbCapture,
    0xc2300184,
    0xa311,
    0x4880,
    0x89,
    0x66,
    0x53,
    0xf5,
    0x75,
    0x19,
    0xf3,
    0x2a)

DAS_NS_BEGIN

class AdbCapture final : public IDasCapture
{
private:
    DAS::Utils::RefCounter<AdbCapture> ref_counter_{};
    std::string                        capture_png_command_;
    std::string                        capture_raw_by_nc_command_;
    std::string                        capture_gzip_raw_command_;
    std::string                        get_screen_size_command_;

    enum class Type
    {
        Png,
        RawByNc,
        RawWithGZip,
        Raw
    };

    DasResult (AdbCapture::*current_capture_method)() = {nullptr};
    Type      type_{Type::RawWithGZip};

public:
    struct Size
    {
        int32_t width;
        int32_t height;
    };

private:
    Size adb_device_screen_size_{0, 0};

    DAS::Utils::Expected<Size> GetDeviceSize() const;

    DasResult CaptureRawWithGZip();
    DasResult CaptureRaw();
    DasResult CapturePng();
    DasResult CaptureRawByNc();
    auto AutoDetectType() -> DAS::Utils::Expected<DasResult (AdbCapture::*)()>;

public:
    AdbCapture(
        const std::filesystem::path& adb_path,
        std::string_view             adb_device_serial);
    ~AdbCapture();
    // IDasBase
    int64_t  AddRef() override;
    int64_t  Release() override;
    DAS_IMPL QueryInterface(const DasGuid& iid, void** pp_out_object) override;
    // IDasTypeInfo
    DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
    DAS_IMPL GetRuntimeClassName(
        IDasReadOnlyString** pp_out_class_name) override;
    // IDasCapture
    DAS_IMPL Capture(IDasImage** pp_out_image) override;
};

DAS_NS_END

#endif

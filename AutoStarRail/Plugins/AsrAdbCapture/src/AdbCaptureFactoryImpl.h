#ifndef ASR_PLUGINS_ASRADBCAPTURE_ADBCAPTUREFACTORYIMPL_H
#define ASR_PLUGINS_ASRADBCAPTURE_ADBCAPTUREFACTORYIMPL_H

#include <AutoStarRail/PluginInterface/IAsrCapture.h>
#include <AutoStarRail/Utils/CommonUtils.hpp>

// {23290FC8-CD40-4C4E-9F58-20EC404F1F3C}
ASR_DEFINE_CLASS_IN_NAMESPACE(
    Asr,
    AdbCaptureFactoryImpl,
    0x23290fc8,
    0xcd40,
    0x4c4e,
    0x9f,
    0x58,
    0x20,
    0xec,
    0x40,
    0x4f,
    0x1f,
    0x3c)

ASR_NS_BEGIN

class AdbCaptureFactoryImpl final : public IAsrCaptureFactory
{
private:
    ASR::Utils::RefCounter<AdbCaptureFactoryImpl> ref_counter_{};

public:
    AdbCaptureFactoryImpl();
    ~AdbCaptureFactoryImpl();
    // IAsrBase
    int64_t  AddRef() override;
    int64_t  Release() override;
    ASR_IMPL QueryInterface(const AsrGuid& iid, void** pp_object) override;
    // IAsrTypeInfo
    ASR_IMPL GetGuid(AsrGuid* p_out_guid) override;
    ASR_IMPL GetRuntimeClassName(
        IAsrReadOnlyString** pp_out_class_name) override;
    // IAsrCaptureFactory
    /**
     * @brief Require url property
     *
     * @param p_environment_json_config
     * @param p_plugin_config
     * @param pp_object
     * @return ASR_METHOD
     */
    ASR_IMPL CreateInstance(
        IAsrReadOnlyString* p_environment_json_config,
        IAsrReadOnlyString* p_plugin_config,
        IAsrCapture**       pp_object) override;
};

ASR_NS_END

#endif // ASR_PLUGINS_ASRADBCAPTURE_ADBCAPTUREFACTORYIMPL_H

#ifndef DAS_PLUGINS_DASADBCAPTURE_ADBCAPTUREFACTORYIMPL_H
#define DAS_PLUGINS_DASADBCAPTURE_ADBCAPTUREFACTORYIMPL_H

#include <das/PluginInterface/IDasCapture.h>
#include <das/Utils/CommonUtils.hpp>

// {23290FC8-CD40-4C4E-9F58-20EC404F1F3C}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das,
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

DAS_NS_BEGIN

class AdbCaptureFactoryImpl final : public IDasCaptureFactory
{
private:
    DAS::Utils::RefCounter<AdbCaptureFactoryImpl> ref_counter_{};

public:
    AdbCaptureFactoryImpl();
    ~AdbCaptureFactoryImpl();
    // IDasBase
    int64_t  AddRef() override;
    int64_t  Release() override;
    DAS_IMPL QueryInterface(const DasGuid& iid, void** pp_object) override;
    // IDasTypeInfo
    DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
    DAS_IMPL GetRuntimeClassName(
        IDasReadOnlyString** pp_out_class_name) override;
    // IDasCaptureFactory
    /**
     * @brief Require url property
     *
     * @param p_environment_json_config
     * @param p_plugin_config
     * @param pp_object
     * @return DAS_METHOD
     */
    DAS_IMPL CreateInstance(
        IDasReadOnlyString* p_environment_json_config,
        IDasReadOnlyString* p_plugin_config,
        IDasCapture**       pp_object) override;
};

DAS_NS_END

#endif // DAS_PLUGINS_DASADBCAPTURE_ADBCAPTUREFACTORYIMPL_H

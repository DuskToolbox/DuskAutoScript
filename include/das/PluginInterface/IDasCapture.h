#ifndef DAS_ICAPTURE_H
#define DAS_ICAPTURE_H

#include <das/DasString.hpp>
#include <das/ExportInterface/IDasImage.h>
#include <das/IDasTypeInfo.h>

DAS_INTERFACE IDasImage;

// {69A9BDB0-4657-45B6-8ECB-E4A8F0428E95}
DAS_DEFINE_GUID(
    DAS_IID_CAPTURE,
    IDasCapture,
    0x69a9bdb0,
    0x4657,
    0x45b6,
    0x8e,
    0xcb,
    0xe4,
    0xa8,
    0xf0,
    0x42,
    0x8e,
    0x95)
SWIG_IGNORE(IDasCapture)
DAS_INTERFACE IDasCapture : public IDasTypeInfo
{
    DAS_METHOD Capture(IDasImage * *pp_out_image) = 0;
};

// {35264072-8F42-46B5-99EA-3A83E0227CF9}
DAS_DEFINE_GUID(
    DAS_IID_CAPTURE_FACTORY,
    IDasCaptureFactory,
    0x35264072,
    0x8f42,
    0x46b5,
    0x99,
    0xea,
    0x3a,
    0x83,
    0xe0,
    0x22,
    0x7c,
    0xf9)
SWIG_IGNORE(IDasCaptureFactory)
DAS_INTERFACE IDasCaptureFactory : public IDasTypeInfo
{
    /**
     * @brief Create an instance
     *
     * @param p_environment_json_config
     * @param p_plugin_config
     * @param p_json_config
     * @param pp_out_object
     * @return DAS_METHOD
     */
    DAS_METHOD CreateInstance(
        IDasReadOnlyString * p_environment_json_config,
        IDasReadOnlyString * p_plugin_config,
        IDasCapture * *pp_out_object) = 0;
};

DAS_INTERFACE IDasSwigCapture;

DAS_DEFINE_RET_POINTER(DasRetCapture, IDasSwigCapture);

// {FC326FB1-9669-4D41-8003-27709071DA10}
DAS_DEFINE_GUID(
    DAS_IID_SWIG_CAPTURE,
    IDasSwigCapture,
    0xfc326fb1,
    0x9669,
    0x4d41,
    0x80,
    0x3,
    0x27,
    0x70,
    0x90,
    0x71,
    0xda,
    0x10);
DAS_SWIG_EXPORT_ATTRIBUTE(IDasSwigCapture)
DAS_INTERFACE IDasSwigCapture : public IDasSwigTypeInfo
{
    virtual DasRetImage Capture() = 0;
};

#endif // DAS_ICAPTURE_H

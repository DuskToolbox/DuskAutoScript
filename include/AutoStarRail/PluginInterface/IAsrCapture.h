#ifndef ASR_ICAPTURE_H
#define ASR_ICAPTURE_H

#include <AutoStarRail/AsrString.hpp>
#include <AutoStarRail/ExportInterface/IAsrImage.h>
#include <AutoStarRail/IAsrTypeInfo.h>

ASR_INTERFACE IAsrImage;

// {69A9BDB0-4657-45B6-8ECB-E4A8F0428E95}
ASR_DEFINE_GUID(
    ASR_IID_CAPTURE,
    IAsrCapture,
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
SWIG_IGNORE(IAsrCapture)
ASR_INTERFACE IAsrCapture : public IAsrTypeInfo
{
    ASR_METHOD Capture(IAsrImage * *pp_out_image) = 0;
};

// {35264072-8F42-46B5-99EA-3A83E0227CF9}
ASR_DEFINE_GUID(
    ASR_IID_CAPTURE_FACTORY,
    IAsrCaptureFactory,
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
SWIG_IGNORE(IAsrCaptureFactory)
ASR_INTERFACE IAsrCaptureFactory : public IAsrTypeInfo
{
    /**
     * @brief Create an instance
     *
     * @param p_environment_json_config
     * @param p_plugin_config
     * @param p_json_config
     * @param pp_out_object
     * @return ASR_METHOD
     */
    ASR_METHOD CreateInstance(
        IAsrReadOnlyString * p_environment_json_config,
        IAsrReadOnlyString * p_plugin_config,
        IAsrCapture * *pp_out_object) = 0;
};

ASR_INTERFACE IAsrSwigCapture;

ASR_DEFINE_RET_POINTER(AsrRetCapture, IAsrSwigCapture);

// {FC326FB1-9669-4D41-8003-27709071DA10}
ASR_DEFINE_GUID(
    ASR_IID_SWIG_CAPTURE,
    IAsrSwigCapture,
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
ASR_SWIG_EXPORT_ATTRIBUTE(IAsrSwigCapture)
ASR_INTERFACE IAsrSwigCapture : public IAsrSwigTypeInfo
{
    virtual AsrRetImage Capture() = 0;
};

#endif // ASR_ICAPTURE_H

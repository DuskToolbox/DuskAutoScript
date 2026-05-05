#ifndef DAS_CORE_OCVWRAPPER_IIMAGEBACKEND_H
#define DAS_CORE_OCVWRAPPER_IIMAGEBACKEND_H

#include "Config.h"

#include <das/DasGuidHolder.h>
#include <das/IDasBase.h>
#include <das/_autogen/idl/abi/IDasImage.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasImage.Implements.hpp>

#include <optional>

DAS_DISABLE_WARNING_BEGIN

DAS_IGNORE_OPENCV_WARNING
#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>

DAS_DISABLE_WARNING_END

// {B3C4D5E6-0001-4000-8000-000000000002}
DAS_DEFINE_GUID_IN_NAMESPACE(
    DAS_IID_IMAGE_BACKEND,
    Das::Core::OcvWrapper,
    IImageBackend,
    0xB3C4D5E6,
    0x0001,
    0x4000,
    0x80,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x02);

DAS_CORE_OCVWRAPPER_NS_BEGIN

/**
 * @brief Internal interface for accessing image backend data.
 *
 * Provides unified access to CPU/GPU image data through QueryInterface.
 * Inherits from IDasImage so all implementations also satisfy the public ABI.
 */
class IImageBackend : public ExportInterface::IDasImage
{
public:
    /// @brief Get CPU-side image data (auto-download from GPU if needed)
    virtual cv::Mat& GetCpuMat() = 0;

    /// @brief Check if CPU data is currently resident
    virtual bool HasCpuMat() const = 0;

    /// @brief Check if GPU data is currently resident
    virtual bool HasGpuMat() const = 0;

    /// @brief Get runtime pixel format metadata
    virtual ExportInterface::DasImagePixelFormat GetPixelFormatValue()
        const = 0;

    /// @brief Set runtime pixel format metadata
    virtual void SetPixelFormat(
        ExportInterface::DasImagePixelFormat format) = 0;

#ifdef DAS_WITH_CUDA
    /// @brief Get GPU-side image data (auto-upload from CPU if needed)
    virtual cv::cuda::GpuMat& GetGpuMat() = 0;
#endif
};

DAS_CORE_OCVWRAPPER_NS_END

#endif // DAS_CORE_OCVWRAPPER_IIMAGEBACKEND_H

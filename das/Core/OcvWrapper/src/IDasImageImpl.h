#ifndef DAS_CORE_OCVWRAPPER_IDASIMAGEIMPL_H
#define DAS_CORE_OCVWRAPPER_IDASIMAGEIMPL_H

#include "Config.h"

#include <DAS/_autogen/idl/wrapper/Das.ExportInterface.IDasImage.Implements.hpp>
#include <DAS/_autogen/idl/wrapper/Das.ExportInterface.IDasImage.hpp>
#include <DAS/_autogen/idl/wrapper/Das.ExportInterface.IDasMemory.hpp>

DAS_DISABLE_WARNING_BEGIN

DAS_IGNORE_OPENCV_WARNING
#include <opencv2/core/mat.hpp>

DAS_DISABLE_WARNING_END

// {911CF30B-352D-4979-9C9C-DF7AF97362DF}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Core::OcvWrapper,
    IDasImageImpl,
    0x911cf30b,
    0x352d,
    0x4979,
    0x9c,
    0x9c,
    0xdf,
    0x7a,
    0xf9,
    0x73,
    0x62,
    0xdf);

DAS_CORE_OCVWRAPPER_NS_BEGIN

class IDasImageImpl final
    : public ExportInterface::DasImageImplBase<IDasImageImpl>
{
    DAS::ExportInterface::DasMemory p_memory_;
    cv::Mat                         mat_;
    uint32_t                        ref_counter_{};

public:
    /**
     * @brief 参数校验由外部完成！
     * @param height
     * @param width
     * @param type
     * @param p_data
     * @param p_das_data
     */
    IDasImageImpl(
        int                               height,
        int                               width,
        int                               type,
        void*                             p_data,
        Das::ExportInterface::IDasMemory* p_das_data);

    IDasImageImpl(cv::Mat mat);

    DAS_IMPL GetSize(ExportInterface::DasSize* p_out_size) override;
    DAS_IMPL GetChannelCount(int32_t* p_out_channel_count) override;
    DAS_IMPL Clip(
        const Das::ExportInterface::DasRect* p_rect,
        Das::ExportInterface::IDasImage**    pp_out_image) override;
    DAS_IMPL GetDataSize(uint64_t* p_out_size) override;
    DAS_IMPL GetData(unsigned char** pp_out_data) override;

    auto GetImpl() -> cv::Mat;
};

DAS_CORE_OCVWRAPPER_NS_END

#endif // DAS_CORE_OCVWRAPPER_IDASIMAGEIMPL_H

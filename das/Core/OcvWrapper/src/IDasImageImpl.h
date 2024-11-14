#ifndef DAS_CORE_OCVWRAPPER_IDASIMAGEIMPL_H
#define DAS_CORE_OCVWRAPPER_IDASIMAGEIMPL_H

#include "Config.h"

#include <das/ExportInterface/IDasImage.h>
#include <das/Utils/CommonUtils.hpp>

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

class IDasImageImpl final : public IDasImage
{
    DasPtr<IDasMemory> p_memory_;
    cv::Mat            mat_;

    DAS_UTILS_IDASBASE_AUTO_IMPL(IDasImageImpl);

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
        int         height,
        int         width,
        int         type,
        void*       p_data,
        IDasMemory* p_das_data);

    IDasImageImpl(cv::Mat mat);

    DAS_IMPL QueryInterface(const DasGuid& iid, void** pp_out_object) override;
    DAS_IMPL GetSize(DasSize* p_out_size) override;
    DAS_IMPL GetChannelCount(int* p_out_channel_count) override;
    DAS_IMPL Clip(const DasRect* p_rect, IDasImage** pp_out_image) override;
    DAS_IMPL GetDataSize(size_t* p_out_size) override;
    DAS_IMPL CopyTo(unsigned char* p_out_memory) override;

    auto GetImpl() -> cv::Mat;
};

DAS_CORE_OCVWRAPPER_NS_END

#endif // DAS_CORE_OCVWRAPPER_IDASIMAGEIMPL_H

#ifndef DAS_IMAGE_H
#define DAS_IMAGE_H

#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/ExportInterface/IDasMemory.h>

typedef enum DasImageFormat
{
    DAS_IMAGE_FORMAT_PNG = 0,
    DAS_IMAGE_FORMAT_RGBA_8888 = 1,
    DAS_IMAGE_FORMAT_RGBX_8888 = 2,
    DAS_IMAGE_FORMAT_RGB_888 = 3,
    DAS_IMAGE_FORMAT_JPG = 4,
    DAS_IMAGE_FORMAT_FORCE_DWORD = 0x7FFFFFFF
} DasImageFormat;

struct DasSize
{
    int32_t width;
    int32_t height;
};

/**
 * @brief just like cv::Rect
 *
 */
struct DasRect
{
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
};

#ifndef SWIG

// {6C98E79F-2342-4B98-AC8A-2B29EA53F951}
DAS_DEFINE_GUID(
    DAS_IID_IMAGE,
    IDasImage,
    0x6c98e79f,
    0x2342,
    0x4b98,
    0xac,
    0x8a,
    0x2b,
    0x29,
    0xea,
    0x53,
    0xf9,
    0x51);
DAS_INTERFACE IDasImage : public IDasBase
{
    DAS_METHOD GetSize(DasSize * p_out_size) = 0;
    DAS_METHOD GetChannelCount(int* p_out_channel_count) = 0;
    DAS_METHOD Clip(const DasRect* p_rect, IDasImage** p_out_image) = 0;
    DAS_METHOD GetDataSize(size_t * p_out_size) = 0;
    DAS_METHOD CopyTo(unsigned char* p_out_memory) = 0;
};

struct DasImageDesc
{
    /**
     * @brief Pointer to the image data pointer.
     *
     */
    char* p_data;
    /**
     * @brief Size of image data in bytes. Can be 0 when both width and height
     * are set and data is decoded.
     *
     */
    size_t data_size;
    /**
     * @brief Supported image format. @see DasImageFormat
     *
     */
    DasImageFormat data_format;
};

/**
 * @brief DAS Core will copy the image data.
 *
 * @param p_desc
 * @param pp_out_image
 * @return DAS_C_API
 */
DAS_C_API DasResult
CreateIDasImageFromEncodedData(DasImageDesc* p_desc, IDasImage** pp_out_image);

DAS_C_API DasResult CreateIDasImageFromDecodedData(
    const DasImageDesc* p_desc,
    const DasSize*      p_size,
    IDasImage**         pp_out_image);

DAS_C_API DasResult CreateIDasImageFromRgb888(
    DAS_INTERFACE IDasMemory* p_alias_memory,
    const DasSize*            p_size,
    IDasImage**               pp_out_image);

DAS_C_API DasResult DasPluginLoadImageFromResource(
    DAS_INTERFACE IDasTypeInfo* p_type_info,
    IDasReadOnlyString*         p_relative_path,
    IDasImage**                 pp_out_image);

#endif // SWIG

class DasSwigImage
{
    DAS::DasPtr<IDasImage> p_image_;

public:
    DAS_API DasSwigImage();
#ifndef SWIG
    DasSwigImage(Das::DasPtr<IDasImage> p_image);
    IDasImage* Get() const;
#endif // SWIG
};

DAS_DEFINE_RET_TYPE(DasRetImage, DasSwigImage);

DAS_DEFINE_RET_TYPE(DasRetRect, DasRect);

DAS_API DasRetImage DasPluginLoadImageFromResource(
    DAS_INTERFACE IDasSwigTypeInfo* p_type_info,
    DasReadOnlyString               relative_path);

#endif // DAS_IMAGE_H

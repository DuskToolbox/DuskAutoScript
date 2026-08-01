#ifndef DAS_CORE_OCVWRAPPER_IMAGESAVER_H
#define DAS_CORE_OCVWRAPPER_IMAGESAVER_H

#include "Config.h"

#include <das/_autogen/idl/abi/IDasBinaryBuffer.h>
#include <das/_autogen/idl/abi/IDasImage.h>

#include <filesystem>

DAS_CORE_OCVWRAPPER_NS_BEGIN

// 内部图片编码落盘助手（D-88-04a）：仅供 Core 内部使用，非导出、不进
// DasCoreApi.idl、不注册 C ABI。Recorder 在 Record 时把插件传入的
// IDasImage 经 OpenCV JPG/PNG 压缩编码写入会话目录 img/{seq}.jpg。
// 全程走 IDasImage 公共 ABI（GetSize/GetPixelFormat/GetBinaryBuffer），
// 不 QueryInterface 到 IImageBackend：IPC proxy 的 IDasImage 不支持 QI，
// 跨进程编码只能依赖公共 ABI。
class ImageSaver
{
public:
    // 把 p_image 编码为 format 字节流，输出到 IDasBinaryBuffer。
    // JPG 的 quality 即压缩质量（默认 85）；PNG 忽略 quality。
    static DasResult Encode(
        ExportInterface::IDasImage*         p_image,
        ExportInterface::DasImageFormat     format,
        int32_t                             quality,
        ExportInterface::IDasBinaryBuffer** pp_out_encoded);

    // Encode 后以调用方给定的完整 path 二进制写盘（父目录由调用方确保；
    // 会话目录/seq/清理是 Recorder 的职责，SaveToFile 不碰）。
    static DasResult SaveToFile(
        ExportInterface::IDasImage*     p_image,
        ExportInterface::DasImageFormat format,
        int32_t                         quality,
        const std::filesystem::path&    path);
};

DAS_CORE_OCVWRAPPER_NS_END

#endif // DAS_CORE_OCVWRAPPER_IMAGESAVER_H

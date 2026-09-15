#ifndef DAS_PLUGINS_DASADBTOUCH_ADBTOUCH_H
#define DAS_PLUGINS_DASADBTOUCH_ADBTOUCH_H

#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/abi/IDasInput.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

DAS_NS_BEGIN

class AdbTouch final : public Das::PluginInterface::IDasTouch
{
    // adb 可执行文件与公共参数（-s 设备序列）：exe 走 path 通道，参数走
    // bp2 窄 UTF-8 逐参数通道（设备端 shell 命令段单参数成组）。
    std::filesystem::path    adb_exe_path_;
    std::vector<std::string> adb_arguments_;

public:
    AdbTouch(std::string_view adb_path, std::string_view adb_serial);
    // IDasBase
    DAS_UTILS_IDASBASE_AUTO_IMPL(AdbTouch);
    DAS_IMPL QueryInterface(const DasGuid& iid, void** pp_out_object) override;
    // IDasTypeInfo
    DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
    DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;
    // IDasInput
    DAS_IMPL Click(int32_t x, int32_t y) override;
    // IDasTouch
    DAS_IMPL Swipe(
        Das::PluginInterface::DasPoint from,
        Das::PluginInterface::DasPoint to,
        int32_t                        duration_ms) override;
};

DAS_NS_END

#endif // DAS_PLUGINS_DASADBTOUCH_ADBTOUCH_H

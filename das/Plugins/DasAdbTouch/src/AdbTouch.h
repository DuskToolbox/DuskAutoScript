#ifndef DAS_PLUGINS_DASADBTOUCH_ADBTOUCH_H
#define DAS_PLUGINS_DASADBTOUCH_ADBTOUCH_H

#include <DAS/_autogen/idl/abi/IDasInput.h>
#include <das/Utils/CommonUtils.hpp>
#include <string>
#include <string_view>

DAS_NS_BEGIN

class AdbTouch final : public IDasTouch
{
    // should be f"{adb_path} -s {adb_serial} "
    std::string adb_cmd_;

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
    DAS_IMPL Swipe(DasPoint from, DasPoint to, int32_t duration_ms) override;
};

DAS_NS_END

#endif // DAS_PLUGINS_DASADBTOUCH_ADBTOUCH_H

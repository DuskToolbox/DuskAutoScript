#ifndef DAS_CORE_UTILS_PROTOCOLBUILDERSTATE_H
#define DAS_CORE_UTILS_PROTOCOLBUILDERSTATE_H

#include <das/Core/Logger/Logger.h>
#include <das/Core/Utils/Config.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/abi/DasJson.h>

#include <string>

DAS_CORE_UTILS_NS_BEGIN

namespace Details
{

    // 把 builder 的内部状态初始化为空 JSON 对象。IDasJson 默认构造出来是
    // null 值，Set*ByName 会拒绝写入，必须以 "{}" 解析产物作为可写对象起点。
    inline DasResult InitBuilderStateAsEmptyObject(
        Das::DasPtr<Das::ExportInterface::IDasJson>& state)
    {
        return ParseDasJsonFromString("{}", state.Put());
    }

    // 导出内部状态的深拷贝：紧凑序列化后重新解析，导出物与 builder 内部
    // 状态互不别名，调用方修改导出物不影响后续 Set*/ToJson 的产出。
    inline DasResult ExportBuilderStateDeepCopy(
        Das::ExportInterface::IDasJson*  p_state,
        Das::ExportInterface::IDasJson** pp_out_json)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_json);

        Das::DasPtr<IDasReadOnlyString> serialized;
        const auto serialize_result = p_state->ToString(-1, serialized.Put());
        if (DAS::IsFailed(serialize_result))
        {
            return serialize_result;
        }

        const char* utf8_text = nullptr;
        if (DAS::IsFailed(serialized->GetUtf8(&utf8_text))
            || utf8_text == nullptr)
        {
            return DAS_E_FAIL;
        }

        return ParseDasJsonFromString(utf8_text, pp_out_json);
    }

} // namespace Details

DAS_CORE_UTILS_NS_END

#endif // DAS_CORE_UTILS_PROTOCOLBUILDERSTATE_H

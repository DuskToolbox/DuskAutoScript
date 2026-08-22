#ifndef DAS_CORE_UTILS_DASSETTINGSCHANGEBUILDERIMPL_H
#define DAS_CORE_UTILS_DASSETTINGSCHANGEBUILDERIMPL_H

#include <das/Core/Utils/Config.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasSettingsChangeBuilder.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasSettingsChangeBuilder.Implements.hpp>

DAS_CORE_UTILS_NS_BEGIN

// IDasSettingsChangeBuilder 实现：内部持一个空对象起点的 IDasJson 状态，
// Set* 把传入值写入固定协议键（settings / payload / acceptedSettings）。
// 字段名只出现在本实现的方法体里，调用方只见方法名。ToJson 导出深拷贝。
class DasSettingsChangeBuilderImpl final
    : public Das::ExportInterface::DasSettingsChangeBuilderImplBase<
          DasSettingsChangeBuilderImpl>
{
public:
    DasSettingsChangeBuilderImpl();

    DAS_IMPL
    SetSettings(Das::ExportInterface::IDasJson* p_in_settings) override;
    DAS_IMPL
    SetPayload(Das::ExportInterface::IDasJson* p_in_payload) override;
    DAS_IMPL SetAcceptedSettings(
        Das::ExportInterface::IDasJson* p_in_accepted) override;
    DAS_IMPL
    ToJson(Das::ExportInterface::IDasJson** pp_out_json) override;
    DAS_IMPL Clear() override;

private:
    Das::DasPtr<Das::ExportInterface::IDasJson> state_;
};

DAS_CORE_UTILS_NS_END

#endif // DAS_CORE_UTILS_DASSETTINGSCHANGEBUILDERIMPL_H

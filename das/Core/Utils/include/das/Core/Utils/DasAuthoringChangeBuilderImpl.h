#ifndef DAS_CORE_UTILS_DASAUTHORINGCHANGEBUILDERIMPL_H
#define DAS_CORE_UTILS_DASAUTHORINGCHANGEBUILDERIMPL_H

#include <das/Core/Utils/Config.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasAuthoringChangeBuilder.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasAuthoringChangeBuilder.Implements.hpp>

DAS_CORE_UTILS_NS_BEGIN

// IDasAuthoringChangeBuilder 实现：authoring 变更信封 {kind, payload} 的
// 固定键写入与 setValue 特化组装。ToJson 导出深拷贝。
class DasAuthoringChangeBuilderImpl final
    : public Das::ExportInterface::DasAuthoringChangeBuilderImplBase<
          DasAuthoringChangeBuilderImpl>
{
public:
    DasAuthoringChangeBuilderImpl();

    DAS_IMPL SetKind(IDasReadOnlyString* p_in_kind) override;
    DAS_IMPL
    SetPayload(Das::ExportInterface::IDasJson* p_in_payload) override;
    DAS_IMPL MakeSetValueChange(
        IDasReadOnlyString*             p_in_path,
        Das::ExportInterface::IDasJson* p_in_value) override;
    DAS_IMPL
    ToJson(Das::ExportInterface::IDasJson** pp_out_json) override;
    DAS_IMPL Clear() override;

private:
    Das::DasPtr<Das::ExportInterface::IDasJson> state_;
};

DAS_CORE_UTILS_NS_END

#endif // DAS_CORE_UTILS_DASAUTHORINGCHANGEBUILDERIMPL_H

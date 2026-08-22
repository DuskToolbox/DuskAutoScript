#ifndef DAS_CORE_UTILS_DASAUTHORINGFIELDBUILDERIMPL_H
#define DAS_CORE_UTILS_DASAUTHORINGFIELDBUILDERIMPL_H

#include <das/Core/Utils/Config.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasAuthoringFieldBuilder.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasAuthoringFieldBuilder.Implements.hpp>

DAS_CORE_UTILS_NS_BEGIN

// IDasAuthoringFieldBuilder 实现：authoring doc FieldDescriptor 固定字段
// （name / label / valueType / inputType / value / description / required /
// readonly）的写入。ToJson 导出深拷贝，未设置的字段不出现在导出物中。
class DasAuthoringFieldBuilderImpl final
    : public Das::ExportInterface::DasAuthoringFieldBuilderImplBase<
          DasAuthoringFieldBuilderImpl>
{
public:
    DasAuthoringFieldBuilderImpl();

    DAS_IMPL SetName(IDasReadOnlyString* p_in_name) override;
    DAS_IMPL SetLabel(IDasReadOnlyString* p_in_label) override;
    DAS_IMPL SetValueType(int32_t in_value_type) override;
    DAS_IMPL SetInputType(int32_t in_input_type) override;
    DAS_IMPL
    SetValue(Das::ExportInterface::IDasJson* p_in_value) override;
    DAS_IMPL SetDescription(IDasReadOnlyString* p_in_description) override;
    DAS_IMPL SetRequired(bool in_required) override;
    DAS_IMPL SetReadonly(bool in_readonly) override;
    DAS_IMPL
    ToJson(Das::ExportInterface::IDasJson** pp_out_json) override;
    DAS_IMPL Clear() override;

private:
    Das::DasPtr<Das::ExportInterface::IDasJson> state_;
};

DAS_CORE_UTILS_NS_END

#endif // DAS_CORE_UTILS_DASAUTHORINGFIELDBUILDERIMPL_H

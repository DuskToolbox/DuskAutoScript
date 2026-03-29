#ifndef DAS_CORE_UTILS_DASVARIANTVECTORIMPL_H
#define DAS_CORE_UTILS_DASVARIANTVECTORIMPL_H

#include <das/Core/Utils/Config.h>
#include <das/DasBase.hpp>
#include <das/DasString.hpp>
#include <das/_autogen/idl/abi/IDasVariantVector.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasVariantVector.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasComponent.hpp>
#include <variant>
#include <vector>

DAS_CORE_UTILS_NS_BEGIN

class DasVariantVectorImpl final
    : public Das::ExportInterface::DasVariantVectorImplBase<
          DasVariantVectorImpl>
{
public:
    using Variant = std::variant<
        int64_t,
        float,
        bool,
        DasReadOnlyString,
        DasBase,
        Das::PluginInterface::DasComponent>;

    DasVariantVectorImpl() = default;

    DAS_IMPL GetInt(uint64_t index, int64_t* p_out_int) override;
    DAS_IMPL GetFloat(uint64_t index, float* p_out_float) override;
    DAS_IMPL
    GetString(uint64_t index, IDasReadOnlyString** pp_out_string) override;
    DAS_IMPL GetBool(uint64_t index, bool* p_out_bool) override;
    DAS_IMPL
    GetComponent(
        uint64_t                              index,
        Das::PluginInterface::IDasComponent** pp_out_component) override;
    DAS_IMPL GetBase(uint64_t index, IDasBase** pp_out_base) override;

    DAS_IMPL SetInt(uint64_t index, int64_t in_int) override;
    DAS_IMPL SetFloat(uint64_t index, float in_float) override;
    DAS_IMPL
    SetString(uint64_t index, IDasReadOnlyString* in_string) override;
    DAS_IMPL SetBool(uint64_t index, bool in_bool) override;
    DAS_IMPL
    SetComponent(
        uint64_t                             index,
        Das::PluginInterface::IDasComponent* in_component) override;
    DAS_IMPL SetBase(uint64_t index, IDasBase* in_base) override;

    DAS_IMPL PushBackInt(int64_t in_int) override;
    DAS_IMPL PushBackFloat(float in_float) override;
    DAS_IMPL PushBackString(IDasReadOnlyString* in_string) override;
    DAS_IMPL PushBackBool(bool in_bool) override;
    DAS_IMPL
    PushBackComponent(
        Das::PluginInterface::IDasComponent* in_component) override;
    DAS_IMPL PushBackBase(IDasBase* in_base) override;

    DAS_IMPL
    GetType(uint64_t index, Das::ExportInterface::DasVariantType* p_out_type)
        override;
    DAS_IMPL RemoveAt(uint64_t index) override;
    DAS_IMPL GetSize() override;

private:
    std::vector<Variant> variants_{};
};

DAS_CORE_UTILS_NS_END


#endif // DAS_CORE_UTILS_DASVARIANTVECTORIMPL_H

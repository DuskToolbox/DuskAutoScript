#include <das/Core/GraphRuntime/EntryRefExpansion.h>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

DasResult ExpandEntryRefNodes(
    Dto::CompiledGraphPlanDto& plan,
    const EntryRefResolver&    resolver,
    std::vector<std::string>*  out_diagnostics)
{
    std::ignore = plan;
    std::ignore = resolver;
    std::ignore = out_diagnostics;
    return DAS_E_NO_IMPLEMENTATION;
}

DAS_CORE_GRAPHRUNTIME_NS_END

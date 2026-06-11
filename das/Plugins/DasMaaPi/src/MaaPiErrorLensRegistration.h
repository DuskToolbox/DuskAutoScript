#pragma once

#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasErrorLens.h>

DAS_NS_BEGIN

namespace Plugins::DasMaaPi
{

    /// Creates a BasicErrorLens with all MaaPi -10000 series error codes
    /// registered (en + zh-cn locales).
    /// Returns a DasPtr<IDasBasicErrorLens> on success, or nullptr on failure.
    DasPtr<PluginInterface::IDasBasicErrorLens>
    CreateRegisteredMaapiErrorLens();

} // namespace Plugins::DasMaaPi

DAS_NS_END

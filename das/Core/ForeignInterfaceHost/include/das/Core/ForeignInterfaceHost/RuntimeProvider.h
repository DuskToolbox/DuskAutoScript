#ifndef DAS_CORE_FOREIGNINTERFACEHOST_RUNTIMEPROVIDER_H
#define DAS_CORE_FOREIGNINTERFACEHOST_RUNTIMEPROVIDER_H

#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHostEnum.h>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
#include <das/Utils/Expected.h>

#include <cstdint>
#include <filesystem>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

struct RuntimeLoadRequest
{
    std::filesystem::path manifest_path;
    std::filesystem::path runtime_path;
    DasGuid               plugin_guid{};
    ForeignInterfaceLanguage language{};
    LoadMode                 load_mode{LoadMode::InProcess};
    uint16_t                 main_process_owner_session_id = 0;
};

struct RuntimeLoadResult
{
    DAS::DasPtr<IDasBase> object;
    uint16_t              owner_session_id = 0;
};

class IRuntimeProvider
{
public:
    virtual ~IRuntimeProvider() = default;

    virtual auto LoadPlugin(const RuntimeLoadRequest& request)
        -> DAS::Utils::Expected<RuntimeLoadResult> = 0;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_RUNTIMEPROVIDER_H

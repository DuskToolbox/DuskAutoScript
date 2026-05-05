#ifndef DAS_CORE_FOREIGNINTERFACEHOST_LUAHOST_H
#define DAS_CORE_FOREIGNINTERFACEHOST_LUAHOST_H

#ifdef DAS_EXPORT_LUA

#include <boost/dll.hpp>
#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/Expected.h>

#include <filesystem>
#include <string>

struct lua_State;

#define DAS_NS_LUAHOST_BEGIN                                                   \
    namespace LuaHost                                                          \
    {
#define DAS_NS_LUAHOST_END }

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DAS_NS_LUAHOST_BEGIN

class LuaRuntime final : public IForeignLanguageRuntime
{
public:
    LuaRuntime();
    ~LuaRuntime();

    // IForeignLanguageRuntime
    auto LoadPlugin(const std::filesystem::path& path)
        -> DAS::Utils::Expected<DasPtr<IDasBase>> override;

    // IDasBase
    uint32_t  AddRef() final;
    uint32_t  Release() final;
    DasResult QueryInterface(const DasGuid& iid, void** pp_out_object) override;

private:
    /// Parse entryPoint "Module.Func" into pair<Module, Func>
    static auto ParseEntryPoint(const std::string& entry_point)
        -> std::pair<std::string, std::string>;

private:
    DAS::Utils::RefCounter<LuaRuntime> ref_counter_{};
    lua_State*                         lua_state_{nullptr};
    boost::dll::shared_library         export_lib_;
};

auto CreateForeignLanguageRuntime(const ForeignLanguageRuntimeFactoryDesc& desc)
    -> DAS::Utils::Expected<DasPtr<IForeignLanguageRuntime>>;

DAS_NS_LUAHOST_END

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_EXPORT_LUA

#endif // DAS_CORE_FOREIGNINTERFACEHOST_LUAHOST_H

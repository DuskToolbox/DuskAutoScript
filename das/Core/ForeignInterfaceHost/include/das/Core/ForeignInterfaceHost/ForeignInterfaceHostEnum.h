#ifndef DAS_CORE_FOREIGNINTERFACEHOST_FOREIGNINTERFACEHOSTENUM_H
#define DAS_CORE_FOREIGNINTERFACEHOST_FOREIGNINTERFACEHOSTENUM_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <nlohmann/json.hpp>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

enum class ForeignInterfaceLanguage
{
    Python,
    CSharp,
    Java,
    Cpp,
    Lua
};

#define DAS_CORE_FOREIGNINTERFACEHOST_DEFAULT_ENUM_SERIALIZE(x) {ForeignInterfaceLanguage::x, #x}

NLOHMANN_JSON_SERIALIZE_ENUM(
    ForeignInterfaceLanguage,
    {DAS_CORE_FOREIGNINTERFACEHOST_DEFAULT_ENUM_SERIALIZE(Python),
     DAS_CORE_FOREIGNINTERFACEHOST_DEFAULT_ENUM_SERIALIZE(CSharp),
     DAS_CORE_FOREIGNINTERFACEHOST_DEFAULT_ENUM_SERIALIZE(Java),
     DAS_CORE_FOREIGNINTERFACEHOST_DEFAULT_ENUM_SERIALIZE(Cpp),
     DAS_CORE_FOREIGNINTERFACEHOST_DEFAULT_ENUM_SERIALIZE(Lua)});

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_FOREIGNINTERFACEHOSTENUM_H
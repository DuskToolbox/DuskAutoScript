#ifndef DAS_CORE_IPC_METHOD_METADATA_H
#define DAS_CORE_IPC_METHOD_METADATA_H

#include <cstdint>
#include <string_view>

#include <das/Core/IPC/Config.h>
#include <das/DasTypes.hpp>

DAS_CORE_IPC_NS_BEGIN
struct MethodMetadata
{
    uint16_t    method_id;
    const char* method_name;
    uint32_t    method_hash;
};

// Compute interface_id from DasGuid binary data (FNV-1a hash)
// constexpr — no static initialization order dependency.
constexpr uint32_t ComputeInterfaceId(const DasGuid& guid) noexcept
{
    constexpr uint32_t FNV_PRIME = 0x01000193;
    constexpr uint32_t FNV_OFFSET_BASIS = 0x811c9dc5;

    uint32_t hash_value = FNV_OFFSET_BASIS;

    // Hash data1 (4 bytes, little-endian assumed on Windows)
    hash_value ^= static_cast<uint8_t>(guid.data1 & 0xFF);
    hash_value *= FNV_PRIME;
    hash_value ^= static_cast<uint8_t>((guid.data1 >> 8) & 0xFF);
    hash_value *= FNV_PRIME;
    hash_value ^= static_cast<uint8_t>((guid.data1 >> 16) & 0xFF);
    hash_value *= FNV_PRIME;
    hash_value ^= static_cast<uint8_t>((guid.data1 >> 24) & 0xFF);
    hash_value *= FNV_PRIME;

    // Hash data2 (2 bytes)
    hash_value ^= static_cast<uint8_t>(guid.data2 & 0xFF);
    hash_value *= FNV_PRIME;
    hash_value ^= static_cast<uint8_t>((guid.data2 >> 8) & 0xFF);
    hash_value *= FNV_PRIME;

    // Hash data3 (2 bytes)
    hash_value ^= static_cast<uint8_t>(guid.data3 & 0xFF);
    hash_value *= FNV_PRIME;
    hash_value ^= static_cast<uint8_t>((guid.data3 >> 8) & 0xFF);
    hash_value *= FNV_PRIME;

    // Hash data4 (8 bytes)
    for (int i = 0; i < 8; ++i)
    {
        hash_value ^= guid.data4[i];
        hash_value *= FNV_PRIME;
    }

    return hash_value;
}

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_METHOD_METADATA_H

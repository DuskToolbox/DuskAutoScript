#ifndef DAS_CORE_IPC_METHOD_METADATA_H
#define DAS_CORE_IPC_METHOD_METADATA_H

#include <cstdint>
#include <cstring>
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

inline constexpr uint32_t Fnv1aHash32(std::string_view data) noexcept
{
    constexpr uint32_t FNV_PRIME = 0x01000193;
    constexpr uint32_t FNV_OFFSET_BASIS = 0x811c9dc5;

    uint32_t hash_value = FNV_OFFSET_BASIS;
    for (char c : data)
    {
        hash_value ^= static_cast<uint8_t>(c);
        hash_value = (hash_value * FNV_PRIME) & 0xFFFFFFFF;
    }

    return hash_value;
}

// Compute interface_id from DasGuid binary data (FNV-1a hash)
// This is the header-only version of the algorithm, usable by any module
// without linking to DasCore.dll internals.
inline uint32_t ComputeInterfaceId(const DasGuid& guid) noexcept
{
    constexpr uint32_t FNV_PRIME = 0x01000193;
    constexpr uint32_t FNV_OFFSET_BASIS = 0x811c9dc5;

    uint32_t hash_value = FNV_OFFSET_BASIS;

    // Hash data1 (4 bytes)
    uint8_t bytes[4];
    std::memcpy(bytes, &guid.data1, 4);
    for (int i = 0; i < 4; ++i)
    {
        hash_value ^= bytes[i];
        hash_value *= FNV_PRIME;
    }

    // Hash data2 (2 bytes)
    std::memcpy(bytes, &guid.data2, 2);
    for (int i = 0; i < 2; ++i)
    {
        hash_value ^= bytes[i];
        hash_value *= FNV_PRIME;
    }

    // Hash data3 (2 bytes)
    std::memcpy(bytes, &guid.data3, 2);
    for (int i = 0; i < 2; ++i)
    {
        hash_value ^= bytes[i];
        hash_value *= FNV_PRIME;
    }

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

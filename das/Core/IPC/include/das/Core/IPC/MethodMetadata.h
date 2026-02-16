#ifndef DAS_CORE_IPC_METHOD_METADATA_H
#define DAS_CORE_IPC_METHOD_METADATA_H

#include <cstdint>
#include <string_view>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
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

        // Hash GUID string to generate interface_id from UUID
        // Handles both {...} and plain formats, case-insensitive
        inline constexpr uint32_t Fnv1aHashGuid(
            std::string_view guid_str) noexcept
        {
            constexpr uint32_t FNV_PRIME = 0x01000193;
            constexpr uint32_t FNV_OFFSET_BASIS = 0x811c9dc5;

            uint32_t hash_value = FNV_OFFSET_BASIS;
            for (char c : guid_str)
            {
                if (c == '{' || c == '}')
                    continue;
                char upper_c = (c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c;
                hash_value ^= static_cast<uint8_t>(upper_c);
                hash_value = (hash_value * FNV_PRIME) & 0xFFFFFFFF;
            }

            return hash_value;
        }

    }
}
DAS_NS_END

#endif // DAS_CORE_IPC_METHOD_METADATA_H

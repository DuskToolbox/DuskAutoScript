#ifndef DAS_CORE_IPC_OBJECT_ID_H
#define DAS_CORE_IPC_OBJECT_ID_H

#include <cstdint>
#include <das/IDasBase.h>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        struct ObjectId
        {
            uint16_t session_id;
            uint16_t generation;
            uint32_t local_id;
        };

        constexpr uint64_t EncodeObjectId(const ObjectId& obj_id) noexcept
        {
            return (static_cast<uint64_t>(obj_id.session_id) << 48)
                   | (static_cast<uint64_t>(obj_id.generation) << 32)
                   | static_cast<uint64_t>(obj_id.local_id);
        }

        constexpr ObjectId DecodeObjectId(uint64_t encoded_id) noexcept
        {
            return ObjectId{
                .session_id = static_cast<uint16_t>(encoded_id >> 48),
                .generation =
                    static_cast<uint16_t>((encoded_id >> 32) & 0xFFFF),
                .local_id = static_cast<uint32_t>(encoded_id & 0xFFFFFFFF)};
        }

        constexpr bool IsValidObjectId(
            const ObjectId& obj_id,
            uint16_t        expected_generation) noexcept
        {
            return obj_id.generation == expected_generation;
        }

        constexpr uint16_t IncrementGeneration(uint16_t generation) noexcept
        {
            return (generation == 0xFFFF) ? 1 : (generation + 1);
        }

        constexpr bool IsNullObjectId(const ObjectId& obj_id) noexcept
        {
            return obj_id.session_id == 0 && obj_id.generation == 0
                   && obj_id.local_id == 0;
        }

        constexpr bool IsNullObjectId(uint64_t encoded_id) noexcept
        {
            return encoded_id == 0;
        }
    }
}
DAS_NS_END

#endif // DAS_CORE_IPC_OBJECT_ID_H

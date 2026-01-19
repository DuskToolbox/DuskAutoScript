#ifndef DAS_CORE_IPC_IPC_MESSAGE_HEADER_H
#define DAS_CORE_IPC_IPC_MESSAGE_HEADER_H

#include <cstdint>
#include <cstring>
#include <das/IDasBase.h>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        enum class MessageType : uint8_t
        {
            REQUEST = 1,
            RESPONSE = 2,
            EVENT = 3,
            HEARTBEAT = 4
        };

        struct IPCMessageHeader
        {
            uint64_t    call_id;
            MessageType message_type;
            int32_t     error_code;
            DasGuid     type_id;
            uint32_t    interface_id;
            uint64_t    object_id;
            uint16_t    version;
            uint16_t    flags;
            uint32_t    body_size;

            static constexpr uint16_t CURRENT_VERSION = 1;
        };

        struct MessageHeaderV1
        {
            uint64_t call_id;
            uint8_t  message_type;
            int32_t  error_code;
            uint8_t  type_id[16];
            uint32_t interface_id;
            uint64_t object_id;
            uint16_t version;
            uint16_t flags;
            uint32_t body_size;
        };

        inline MessageHeaderV1 ToV1(const IPCMessageHeader& header)
        {
            MessageHeaderV1 v1{};
            v1.call_id = header.call_id;
            v1.message_type = static_cast<uint8_t>(header.message_type);
            v1.error_code = header.error_code;
            std::memcpy(v1.type_id, &header.type_id, sizeof(DasGuid));
            v1.interface_id = header.interface_id;
            v1.object_id = header.object_id;
            v1.version = header.version;
            v1.flags = header.flags;
            v1.body_size = header.body_size;
            return v1;
        }

        inline IPCMessageHeader FromV1(const MessageHeaderV1& v1)
        {
            IPCMessageHeader header{};
            header.call_id = v1.call_id;
            header.message_type = static_cast<MessageType>(v1.message_type);
            header.error_code = v1.error_code;
            std::memcpy(&header.type_id, v1.type_id, sizeof(DasGuid));
            header.interface_id = v1.interface_id;
            header.object_id = v1.object_id;
            header.version = v1.version;
            header.flags = v1.flags;
            header.body_size = v1.body_size;
            return header;
        };

    }
}
DAS_NS_END

#endif // DAS_CORE_IPC_IPC_MESSAGE_HEADER_H

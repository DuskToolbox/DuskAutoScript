#ifndef DAS_CORE_IPC_IPC_PROXY_BASE_H
#define DAS_CORE_IPC_IPC_PROXY_BASE_H

#include <cstdint>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/MethodMetadata.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/IDasBase.h>
#include <vector>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        class IpcRunLoop;

        class IPCProxyBase
        {
        public:
            virtual ~IPCProxyBase() = default;

            virtual uint32_t AddRef() = 0;
            virtual uint32_t Release() = 0;

            [[nodiscard]]
            uint32_t GetInterfaceId() const noexcept
            {
                return interface_id_;
            }

            [[nodiscard]]
            uint64_t GetObjectId() const noexcept
            {
                return EncodeObjectId(object_id_);
            }

            [[nodiscard]]
            const ObjectId& GetObjectIdStruct() const noexcept
            {
                return object_id_;
            }

            [[nodiscard]]
            uint16_t GetSessionId() const noexcept
            {
                return object_id_.session_id;
            }

        protected:
            IPCProxyBase(
                uint32_t        interface_id,
                const ObjectId& object_id,
                IpcRunLoop*     run_loop)
                : interface_id_(interface_id), object_id_(object_id),
                  run_loop_(run_loop), next_call_id_(1)
            {
            }

            DasResult SendRequest(
                uint16_t              method_id,
                const uint8_t*        body,
                size_t                body_size,
                std::vector<uint8_t>& response_body);

            DasResult SendRequestNoResponse(
                uint16_t       method_id,
                const uint8_t* body,
                size_t         body_size);

            [[nodiscard]]
            uint64_t AllocateCallId() noexcept
            {
                return next_call_id_++;
            }

            [[nodiscard]]
            IpcRunLoop* GetRunLoop() const noexcept
            {
                return run_loop_;
            }

            void FillMessageHeader(
                IPCMessageHeader& header,
                uint16_t          method_id,
                uint64_t          call_id,
                MessageType       message_type = MessageType::REQUEST,
                size_t            body_size = 0) const
            {
                header.magic = IPCMessageHeader::MAGIC;
                header.version = IPCMessageHeader::CURRENT_VERSION;
                header.message_type = static_cast<uint8_t>(message_type);
                header.header_flags = 0;
                header.call_id = call_id;
                header.interface_id = interface_id_;
                header.method_id = method_id;
                header.flags = 0;
                header.error_code = DAS_S_OK;
                header.body_size = static_cast<uint32_t>(body_size);
                header.session_id = object_id_.session_id;
                header.generation = object_id_.generation;
                header.local_id = object_id_.local_id;
            }

        private:
            uint32_t    interface_id_;
            ObjectId    object_id_;
            IpcRunLoop* run_loop_;
            uint64_t    next_call_id_;
        };
    }
}
DAS_NS_END

#endif // DAS_CORE_IPC_IPC_PROXY_BASE_H

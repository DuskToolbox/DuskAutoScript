#ifndef DAS_CORE_IPC_ISTUB_BASE_H
#define DAS_CORE_IPC_ISTUB_BASE_H

#include <cstdint>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/MethodMetadata.h>
#include <das/IDasBase.h>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        class IStubBase
        {
        public:
            virtual ~IStubBase() = default;

            [[nodiscard]]
            virtual uint32_t GetInterfaceId() const noexcept = 0;

            [[nodiscard]]
            virtual const MethodMetadata* GetMethodTable() const noexcept = 0;

            [[nodiscard]]
            virtual size_t GetMethodCount() const noexcept = 0;

            virtual void Dispatch(
                uint16_t          method_id,
                const void*       request,
                void*             response,
                IPCMessageHeader& out_response_header) = 0;

            [[nodiscard]]
            static const MethodMetadata* FindMethod(
                const MethodMetadata* table,
                size_t                count,
                uint16_t              method_id) noexcept
            {
                if (method_id >= count)
                    return nullptr;
                return &table[method_id];
            }
        };

    }
}
DAS_NS_END

#endif // DAS_CORE_IPC_ISTUB_BASE_H

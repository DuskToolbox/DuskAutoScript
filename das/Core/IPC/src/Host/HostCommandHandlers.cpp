#include <das/Core/IPC/Host/HostCommandHandlers.h>

#include <das/Core/IPC/HandshakeSerialization.h>
#include <das/Core/IPC/Host/IIpcContext.h>
#include <das/Core/IPC/IDistributedObjectManager.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/MethodMetadata.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/Logger/Logger.h>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>

#include <cstring>
#include <utility>

namespace Das::Core::IPC::Host
{
    namespace
    {
        void AppendUInt16(std::vector<uint8_t>& data, uint16_t value)
        {
            data.push_back(static_cast<uint8_t>(value & 0xFF));
            data.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        }

        void AppendUInt32(std::vector<uint8_t>& data, uint32_t value)
        {
            data.push_back(static_cast<uint8_t>(value & 0xFF));
            data.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
            data.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
            data.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
        }

        void AppendObjectId(std::vector<uint8_t>& data, const ObjectId& id)
        {
            AppendUInt16(data, id.session_id);
            AppendUInt16(data, id.generation);
            AppendUInt32(data, id.local_id);
        }

        void AppendBytes(
            std::vector<uint8_t>& data,
            const void*           source,
            size_t                size)
        {
            const auto* first = static_cast<const uint8_t*>(source);
            data.insert(data.end(), first, first + size);
        }

        DasResult HandleLoadPlugin(
            IIpcContext&                         ctx,
            const HostPluginLoader&              load_plugin,
            std::span<const uint8_t>             payload,
            IpcCommandResponse&                  response)
        {
            std::string manifest_path;
            size_t      offset = 0;
            if (!DeserializeString(payload, offset, manifest_path))
            {
                response.error_code = DAS_E_IPC_INVALID_MESSAGE_BODY;
                response.response_data.clear();
                return DAS_E_IPC_INVALID_MESSAGE_BODY;
            }

            DAS_LOG_INFO(
                DAS_FMT_NS::format("Loading plugin: {}", manifest_path)
                    .c_str());

            auto result = load_plugin(std::filesystem::path{manifest_path});
            if (!result.has_value())
            {
                DAS_LOG_ERROR(
                    DAS_FMT_NS::format(
                        "Plugin load failed: {}",
                        manifest_path)
                        .c_str());
                response.error_code = result.error();
                response.response_data.clear();
                return result.error();
            }

            auto plugin_ptr = std::move(result.value());

            ObjectId  object_id;
            DasResult reg_result =
                ctx.RegisterLocalObject(plugin_ptr.Get(), object_id);
            if (DAS::IsFailed(reg_result))
            {
                DAS_LOG_ERROR(
                    DAS_FMT_NS::format(
                        "[LOAD_PLUGIN] 对象注册失败: {}",
                        static_cast<uint32_t>(reg_result))
                        .c_str());
                response.error_code = reg_result;
                response.response_data.clear();
                return reg_result;
            }

            response.error_code = DAS_S_OK;
            response.response_data.clear();
            AppendObjectId(response.response_data, object_id);

            const DasGuid iid = DAS_IID_PLUGIN_PACKAGE;
            AppendBytes(response.response_data, &iid, sizeof(iid));

            AppendUInt16(response.response_data, object_id.session_id);
            AppendUInt16(response.response_data, 1);

            DAS_LOG_INFO(
                DAS_FMT_NS::format(
                    "[LOAD_PLUGIN] 插件已加载, object_id={{session:{}, gen:{}, local:{}}}",
                    object_id.session_id,
                    object_id.generation,
                    object_id.local_id)
                    .c_str());
            return DAS_S_OK;
        }

        DasResult HandleQueryInterface(
            IIpcContext&             ctx,
            std::span<const uint8_t> payload,
            IpcCommandResponse&      response)
        {
            if (payload.size() < sizeof(ObjectId) + sizeof(DasGuid))
            {
                DAS_LOG_ERROR(
                    DAS_FMT_NS::format(
                        "[QUERY_INTERFACE] payload too small: {}",
                        payload.size())
                        .c_str());
                response.error_code = DAS_E_IPC_INVALID_MESSAGE_BODY;
                response.response_data.clear();
                return DAS_E_IPC_INVALID_MESSAGE_BODY;
            }

            size_t offset = 0;

            ObjectId object_id;
            std::memcpy(&object_id, payload.data() + offset, sizeof(object_id));
            offset += sizeof(object_id);

            DasGuid iid;
            std::memcpy(&iid, payload.data() + offset, sizeof(iid));

            DAS::DasPtr<IDasBase> raw_obj;
            DasResult             lookup_result =
                ctx.GetObjectManager().LookupObject(object_id, raw_obj.Put());
            if (DAS::IsFailed(lookup_result))
            {
                DAS_LOG_ERROR(
                    DAS_FMT_NS::format(
                        "[QUERY_INTERFACE] LookupObject failed: session={}, local={}, result={}",
                        object_id.session_id,
                        object_id.local_id,
                        lookup_result)
                        .c_str());
                response.error_code = lookup_result;
                response.response_data.clear();
                return lookup_result;
            }

            DAS::DasPtr<IDasBase> new_obj;
            DasResult             qi_result =
                raw_obj->QueryInterface(iid, new_obj.PutVoid());
            if (DAS::IsFailed(qi_result))
            {
                DAS_LOG_INFO(
                    DAS_FMT_NS::format(
                        "[QUERY_INTERFACE] QueryInterface returned: {}",
                        qi_result)
                        .c_str());

                response.error_code = qi_result;
                response.response_data.clear();
                const int32_t fail_result = static_cast<int32_t>(qi_result);
                const uint32_t zero32 = 0;
                const uint64_t zero64 = 0;
                AppendBytes(
                    response.response_data,
                    &fail_result,
                    sizeof(fail_result));
                AppendBytes(response.response_data, &zero32, sizeof(zero32));
                AppendBytes(response.response_data, &zero64, sizeof(zero64));
                return qi_result;
            }

            ObjectId  new_obj_id;
            DasResult reg_result =
                ctx.RegisterLocalObject(new_obj.Get(), new_obj_id);
            if (DAS::IsFailed(reg_result))
            {
                response.error_code = reg_result;
                response.response_data.clear();
                return reg_result;
            }

            const uint32_t interface_id = ComputeInterfaceId(iid);
            const uint64_t encoded_id = EncodeObjectId(new_obj_id);
            const int32_t  ok_result = static_cast<int32_t>(DAS_S_OK);

            response.error_code = DAS_S_OK;
            response.response_data.clear();
            AppendBytes(response.response_data, &ok_result, sizeof(ok_result));
            AppendBytes(
                response.response_data,
                &interface_id,
                sizeof(interface_id));
            AppendBytes(response.response_data, &encoded_id, sizeof(encoded_id));

            DAS_LOG_INFO(
                DAS_FMT_NS::format(
                    "[QUERY_INTERFACE] success: iid_hash=0x{:08X}, new_obj_id={{session:{}, gen:{}, local:{}}}",
                    interface_id,
                    new_obj_id.session_id,
                    new_obj_id.generation,
                    new_obj_id.local_id)
                    .c_str());

            return DAS_S_OK;
        }
    } // namespace

    DasResult RegisterHostCommandHandlers(
        IIpcContext*              ctx,
        HostCommandHandlerOptions options)
    {
        if (ctx == nullptr || !options.load_plugin)
        {
            return DAS_E_INVALID_ARGUMENT;
        }

        auto load_plugin = std::move(options.load_plugin);
        ctx->RegisterCommandHandler(
            static_cast<uint32_t>(IpcCommandType::LOAD_PLUGIN),
            [ctx, load_plugin = std::move(load_plugin)](
                const ValidatedIPCMessageHeader&,
                std::span<const uint8_t> payload,
                IpcCommandResponse&      response) -> DasResult
            {
                return HandleLoadPlugin(
                    *ctx,
                    load_plugin,
                    payload,
                    response);
            });

        ctx->RegisterCommandHandler(
            static_cast<uint32_t>(IpcCommandType::QUERY_INTERFACE),
            [ctx](
                const ValidatedIPCMessageHeader&,
                std::span<const uint8_t> payload,
                IpcCommandResponse&      response) -> DasResult
            { return HandleQueryInterface(*ctx, payload, response); });

        return DAS_S_OK;
    }
} // namespace Das::Core::IPC::Host

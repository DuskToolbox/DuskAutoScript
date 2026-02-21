#include "das/Core/IPC/IpcCommandHandler.h"
#include <chrono>
#include <cstring>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <filesystem>
#include <unordered_map>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        namespace
        {
            // 序列化辅助函数
            template <typename T>
            void SerializeValue(std::vector<uint8_t>& buffer, const T& value)
            {
                const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&value);
                buffer.insert(buffer.end(), ptr, ptr + sizeof(T));
            }

            template <typename T>
            bool DeserializeValue(
                std::span<const uint8_t> buffer,
                size_t&                  offset,
                T&                       value)
            {
                if (offset + sizeof(T) > buffer.size())
                {
                    return false;
                }
                std::memcpy(&value, buffer.data() + offset, sizeof(T));
                offset += sizeof(T);
                return true;
            }

            void SerializeString(
                std::vector<uint8_t>& buffer,
                const std::string&    str)
            {
                SerializeValue(buffer, static_cast<uint16_t>(str.size()));
                buffer.insert(
                    buffer.end(),
                    reinterpret_cast<const uint8_t*>(str.data()),
                    reinterpret_cast<const uint8_t*>(str.data()) + str.size());
            }

            bool DeserializeString(
                std::span<const uint8_t> buffer,
                size_t&                  offset,
                std::string&             str,
                uint16_t                 max_len = 1024)
            {
                uint16_t len = 0;
                if (!DeserializeValue(buffer, offset, len))
                {
                    return false;
                }
                if (len > max_len || offset + len > buffer.size())
                {
                    return false;
                }
                str.assign(
                    reinterpret_cast<const char*>(buffer.data() + offset),
                    len);
                offset += len;
                return true;
            }

            // 自定义处理器映射
            std::
                unordered_map<IpcCommandType, IpcCommandHandler::CommandHandler>
                    g_custom_handlers;
        }

        IpcCommandHandler::IpcCommandHandler() : session_id_(0) {}

        void IpcCommandHandler::SetSessionId(uint16_t session_id)
        {
            session_id_ = session_id;
        }

        uint16_t IpcCommandHandler::GetSessionId() const { return session_id_; }

        IpcCommandType IpcCommandHandler::ExtractCommandType(
            const IPCMessageHeader& header)
        {
            return static_cast<IpcCommandType>(header.interface_id);
        }

        DasResult IpcCommandHandler::HandleCommand(
            const IPCMessageHeader&  header,
            std::span<const uint8_t> payload,
            IpcCommandResponse&      response)
        {
            IpcCommandType cmd_type = ExtractCommandType(header);

            // 检查是否有自定义处理器
            auto it = g_custom_handlers.find(cmd_type);
            if (it != g_custom_handlers.end())
            {
                return it->second(header, payload, response);
            }

            switch (cmd_type)
            {
            case IpcCommandType::REGISTER_OBJECT:
                return OnRegisterObject(header, payload, response);

            case IpcCommandType::UNREGISTER_OBJECT:
                return OnUnregisterObject(header, payload, response);

            case IpcCommandType::LOOKUP_OBJECT:
                return OnLookupObject(header, payload, response);

            case IpcCommandType::LOOKUP_BY_NAME:
                return OnLookupByName(header, payload, response);

            case IpcCommandType::LOOKUP_BY_INTERFACE:
                return OnLookupByInterface(header, payload, response);

            case IpcCommandType::LIST_OBJECTS:
                return OnListObjects(header, payload, response);

            case IpcCommandType::LIST_SESSION_OBJECTS:
                return OnListSessionObjects(header, payload, response);

            case IpcCommandType::CLEAR_SESSION:
                return OnClearSession(header, payload, response);

            case IpcCommandType::PING:
                return OnPing(header, payload, response);

            case IpcCommandType::GET_OBJECT_COUNT:
                return OnGetObjectCount(header, payload, response);

            case IpcCommandType::LOAD_PLUGIN:
                return OnLoadPlugin(header, payload, response);

            default:
                response.error_code = DAS_E_IPC_INVALID_MESSAGE_TYPE;
                return DAS_E_IPC_INVALID_MESSAGE_TYPE;
            }
        }

        void IpcCommandHandler::RegisterHandler(
            IpcCommandType command_type,
            CommandHandler handler)
        {
            g_custom_handlers[command_type] = std::move(handler);
        }

        DasResult IpcCommandHandler::OnRegisterObject(
            const IPCMessageHeader&  header,
            std::span<const uint8_t> payload,
            IpcCommandResponse&      response)
        {
            (void)header;

            if (payload.size()
                < sizeof(RegisterObjectPayload) - sizeof(uint16_t))
            {
                response.error_code = DAS_E_IPC_INVALID_MESSAGE_BODY;
                return DAS_E_IPC_INVALID_MESSAGE_BODY;
            }

            size_t offset = 0;

            ObjectId object_id;
            if (!DeserializeValue(payload, offset, object_id))
            {
                response.error_code = DAS_E_IPC_DESERIALIZATION_FAILED;
                return DAS_E_IPC_DESERIALIZATION_FAILED;
            }

            DasGuid iid;
            if (!DeserializeValue(payload, offset, iid))
            {
                response.error_code = DAS_E_IPC_DESERIALIZATION_FAILED;
                return DAS_E_IPC_DESERIALIZATION_FAILED;
            }

            uint16_t session_id = 0;
            if (!DeserializeValue(payload, offset, session_id))
            {
                response.error_code = DAS_E_IPC_DESERIALIZATION_FAILED;
                return DAS_E_IPC_DESERIALIZATION_FAILED;
            }

            uint16_t version = 0;
            if (!DeserializeValue(payload, offset, version))
            {
                response.error_code = DAS_E_IPC_DESERIALIZATION_FAILED;
                return DAS_E_IPC_DESERIALIZATION_FAILED;
            }

            std::string name;
            if (!DeserializeString(payload, offset, name))
            {
                response.error_code = DAS_E_IPC_DESERIALIZATION_FAILED;
                return DAS_E_IPC_DESERIALIZATION_FAILED;
            }

            RemoteObjectRegistry& registry =
                RemoteObjectRegistry::GetInstance();
            DasResult result =
                registry
                    .RegisterObject(object_id, iid, session_id, name, version);

            response.error_code = result;
            response.response_data.clear();

            return result;
        }

        DasResult IpcCommandHandler::OnUnregisterObject(
            const IPCMessageHeader&  header,
            std::span<const uint8_t> payload,
            IpcCommandResponse&      response)
        {
            (void)header;

            if (payload.size() < sizeof(UnregisterObjectPayload))
            {
                response.error_code = DAS_E_IPC_INVALID_MESSAGE_BODY;
                return DAS_E_IPC_INVALID_MESSAGE_BODY;
            }

            size_t offset = 0;

            ObjectId object_id;
            if (!DeserializeValue(payload, offset, object_id))
            {
                response.error_code = DAS_E_IPC_DESERIALIZATION_FAILED;
                return DAS_E_IPC_DESERIALIZATION_FAILED;
            }

            RemoteObjectRegistry& registry =
                RemoteObjectRegistry::GetInstance();
            DasResult result = registry.UnregisterObject(object_id);

            response.error_code = result;
            response.response_data.clear();

            return result;
        }

        DasResult IpcCommandHandler::OnLookupObject(
            const IPCMessageHeader&  header,
            std::span<const uint8_t> payload,
            IpcCommandResponse&      response)
        {
            (void)header;

            if (payload.size() < sizeof(LookupObjectPayload))
            {
                response.error_code = DAS_E_IPC_INVALID_MESSAGE_BODY;
                return DAS_E_IPC_INVALID_MESSAGE_BODY;
            }

            size_t offset = 0;

            ObjectId object_id;
            if (!DeserializeValue(payload, offset, object_id))
            {
                response.error_code = DAS_E_IPC_DESERIALIZATION_FAILED;
                return DAS_E_IPC_DESERIALIZATION_FAILED;
            }

            RemoteObjectRegistry& registry =
                RemoteObjectRegistry::GetInstance();
            RemoteObjectInfo info;
            DasResult        result = registry.GetObjectInfo(object_id, info);

            response.error_code = result;

            if (result == DAS_S_OK)
            {
                response.response_data.clear();
                SerializeValue(response.response_data, info.object_id);
                SerializeValue(response.response_data, info.iid);
                SerializeValue(response.response_data, info.session_id);
                SerializeValue(response.response_data, info.version);
                SerializeString(response.response_data, info.name);
            }
            else
            {
                response.response_data.clear();
            }

            return result;
        }

        DasResult IpcCommandHandler::OnLookupByName(
            const IPCMessageHeader&  header,
            std::span<const uint8_t> payload,
            IpcCommandResponse&      response)
        {
            (void)header;

            size_t offset = 0;

            std::string name;
            if (!DeserializeString(payload, offset, name))
            {
                response.error_code = DAS_E_IPC_DESERIALIZATION_FAILED;
                return DAS_E_IPC_DESERIALIZATION_FAILED;
            }

            RemoteObjectRegistry& registry =
                RemoteObjectRegistry::GetInstance();
            RemoteObjectInfo info;
            DasResult        result = registry.LookupByName(name, info);

            response.error_code = result;

            if (result == DAS_S_OK)
            {
                response.response_data.clear();
                SerializeValue(response.response_data, info.object_id);
                SerializeValue(response.response_data, info.iid);
                SerializeValue(response.response_data, info.session_id);
                SerializeValue(response.response_data, info.version);
                SerializeString(response.response_data, info.name);
            }
            else
            {
                response.response_data.clear();
            }

            return result;
        }

        DasResult IpcCommandHandler::OnLookupByInterface(
            const IPCMessageHeader&  header,
            std::span<const uint8_t> payload,
            IpcCommandResponse&      response)
        {
            (void)header;

            if (payload.size() < sizeof(LookupByInterfacePayload))
            {
                response.error_code = DAS_E_IPC_INVALID_MESSAGE_BODY;
                return DAS_E_IPC_INVALID_MESSAGE_BODY;
            }

            size_t offset = 0;

            DasGuid iid;
            if (!DeserializeValue(payload, offset, iid))
            {
                response.error_code = DAS_E_IPC_DESERIALIZATION_FAILED;
                return DAS_E_IPC_DESERIALIZATION_FAILED;
            }

            RemoteObjectRegistry& registry =
                RemoteObjectRegistry::GetInstance();
            uint32_t interface_id =
                RemoteObjectRegistry::ComputeInterfaceId(iid);
            RemoteObjectInfo info;
            DasResult result = registry.LookupByInterface(interface_id, info);

            response.error_code = result;

            if (result == DAS_S_OK)
            {
                response.response_data.clear();
                SerializeValue(response.response_data, info.object_id);
                SerializeValue(response.response_data, info.iid);
                SerializeValue(response.response_data, info.session_id);
                SerializeValue(response.response_data, info.version);
                SerializeString(response.response_data, info.name);
            }
            else
            {
                response.response_data.clear();
            }

            return result;
        }

        DasResult IpcCommandHandler::OnListObjects(
            const IPCMessageHeader&  header,
            std::span<const uint8_t> payload,
            IpcCommandResponse&      response)
        {
            (void)header;
            (void)payload;

            RemoteObjectRegistry& registry =
                RemoteObjectRegistry::GetInstance();
            std::vector<RemoteObjectInfo> objects;
            registry.ListAllObjects(objects);
            DasResult result = DAS_S_OK;

            response.error_code = result;

            if (result == DAS_S_OK)
            {
                response.response_data.clear();
                uint32_t count = static_cast<uint32_t>(objects.size());
                SerializeValue(response.response_data, count);

                for (const auto& info : objects)
                {
                    SerializeValue(response.response_data, info.object_id);
                    SerializeValue(response.response_data, info.iid);
                    SerializeValue(response.response_data, info.session_id);
                    SerializeValue(response.response_data, info.version);
                    SerializeString(response.response_data, info.name);
                }
            }
            else
            {
                response.response_data.clear();
            }

            return result;
        }

        DasResult IpcCommandHandler::OnListSessionObjects(
            const IPCMessageHeader&  header,
            std::span<const uint8_t> payload,
            IpcCommandResponse&      response)
        {
            (void)header;

            if (payload.size() < sizeof(ListSessionObjectsPayload))
            {
                response.error_code = DAS_E_IPC_INVALID_MESSAGE_BODY;
                return DAS_E_IPC_INVALID_MESSAGE_BODY;
            }

            size_t offset = 0;

            uint16_t session_id = 0;
            if (!DeserializeValue(payload, offset, session_id))
            {
                response.error_code = DAS_E_IPC_DESERIALIZATION_FAILED;
                return DAS_E_IPC_DESERIALIZATION_FAILED;
            }

            RemoteObjectRegistry& registry =
                RemoteObjectRegistry::GetInstance();
            std::vector<RemoteObjectInfo> objects;
            registry.ListObjectsBySession(session_id, objects);
            DasResult result = DAS_S_OK;

            response.error_code = result;

            if (result == DAS_S_OK)
            {
                response.response_data.clear();
                uint32_t count = static_cast<uint32_t>(objects.size());
                SerializeValue(response.response_data, count);

                for (const auto& info : objects)
                {
                    SerializeValue(response.response_data, info.object_id);
                    SerializeValue(response.response_data, info.iid);
                    SerializeValue(response.response_data, info.session_id);
                    SerializeValue(response.response_data, info.version);
                    SerializeString(response.response_data, info.name);
                }
            }
            else
            {
                response.response_data.clear();
            }

            return result;
        }

        DasResult IpcCommandHandler::OnClearSession(
            const IPCMessageHeader&  header,
            std::span<const uint8_t> payload,
            IpcCommandResponse&      response)
        {
            (void)header;

            if (payload.size() < sizeof(ClearSessionPayload))
            {
                response.error_code = DAS_E_IPC_INVALID_MESSAGE_BODY;
                return DAS_E_IPC_INVALID_MESSAGE_BODY;
            }

            size_t offset = 0;

            uint16_t session_id = 0;
            if (!DeserializeValue(payload, offset, session_id))
            {
                response.error_code = DAS_E_IPC_DESERIALIZATION_FAILED;
                return DAS_E_IPC_DESERIALIZATION_FAILED;
            }

            RemoteObjectRegistry& registry =
                RemoteObjectRegistry::GetInstance();
            registry.UnregisterAllFromSession(session_id);
            DasResult result = DAS_S_OK;

            response.error_code = result;
            response.response_data.clear();

            return result;
        }

        DasResult IpcCommandHandler::OnPing(
            const IPCMessageHeader&  header,
            std::span<const uint8_t> payload,
            IpcCommandResponse&      response)
        {
            (void)header;
            (void)payload;

            auto timestamp = static_cast<uint64_t>(
                std::chrono::system_clock::now().time_since_epoch().count());

            response.error_code = DAS_S_OK;
            response.response_data.clear();
            SerializeValue(response.response_data, timestamp);

            return DAS_S_OK;
        }

        DasResult IpcCommandHandler::OnGetObjectCount(
            const IPCMessageHeader&  header,
            std::span<const uint8_t> payload,
            IpcCommandResponse&      response)
        {
            (void)header;
            (void)payload;

            RemoteObjectRegistry& registry =
                RemoteObjectRegistry::GetInstance();
            uint64_t count = static_cast<uint64_t>(registry.GetObjectCount());

            response.error_code = DAS_S_OK;
            response.response_data.clear();
            SerializeValue(response.response_data, count);

            return DAS_S_OK;
        }

        DasResult IpcCommandHandler::OnLoadPlugin(
            const IPCMessageHeader&  header,
            std::span<const uint8_t> payload,
            IpcCommandResponse&      response)
        {
            (void)header;

            if (payload.size() < sizeof(LoadPluginPayload) - sizeof(uint16_t))
            {
                response.error_code = DAS_E_IPC_INVALID_MESSAGE_BODY;
                return DAS_E_IPC_INVALID_MESSAGE_BODY;
            }

            size_t offset = 0;

            uint16_t plugin_path_len = 0;
            if (!DeserializeValue(payload, offset, plugin_path_len))
            {
                response.error_code = DAS_E_IPC_DESERIALIZATION_FAILED;
                return DAS_E_IPC_DESERIALIZATION_FAILED;
            }

            if (plugin_path_len == 0 || plugin_path_len > 4096)
            {
                response.error_code = DAS_E_IPC_INVALID_ARGUMENT;
                return DAS_E_IPC_INVALID_ARGUMENT;
            }

            if (offset + plugin_path_len > payload.size())
            {
                response.error_code = DAS_E_IPC_DESERIALIZATION_FAILED;
                return DAS_E_IPC_DESERIALIZATION_FAILED;
            }

            std::string manifest_path;
            manifest_path.assign(
                reinterpret_cast<const char*>(payload.data() + offset),
                plugin_path_len);
            offset += plugin_path_len;

            auto& plugin_manager =
                ForeignInterfaceHost::PluginManager::GetInstance();

            Das::PluginInterface::IDasPluginPackage* p_package = nullptr;
            DasResult result = plugin_manager.LoadPlugin(
                std::filesystem::path(manifest_path),
                &p_package);

            if (result != DAS_S_OK)
            {
                response.error_code = result;
                response.response_data.clear();
                return result;
            }

            result = plugin_manager.RegisterPluginObjects(
                std::filesystem::path(manifest_path));

            if (result != DAS_S_OK)
            {
                response.error_code = result;
                response.response_data.clear();
                return result;
            }

            RemoteObjectRegistry& registry =
                RemoteObjectRegistry::GetInstance();

            std::vector<ForeignInterfaceHost::FeatureInfo> features;
            result = plugin_manager.GetPluginFeatures(
                std::filesystem::path(manifest_path),
                features);

            if (result != DAS_S_OK || features.empty())
            {
                response.error_code =
                    result == DAS_S_OK ? DAS_E_IPC_PLUGIN_ENTRY_POINT_NOT_FOUND
                                       : result;
                response.response_data.clear();
                return response.error_code;
            }

            const auto&      main_feature = features[0];
            RemoteObjectInfo info;
            result = registry.GetObjectInfo(main_feature.object_id, info);

            if (result != DAS_S_OK)
            {
                response.error_code = result;
                response.response_data.clear();
                return result;
            }

            response.error_code = DAS_S_OK;
            response.response_data.clear();

            SerializeValue(response.response_data, info.object_id);
            SerializeValue(response.response_data, info.iid);
            SerializeValue(response.response_data, info.session_id);
            SerializeValue(response.response_data, info.version);

            return DAS_S_OK;
        }

    }
}
DAS_NS_END

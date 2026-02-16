#include "das/Core/IPC/RemoteObjectRegistry.h"
#include <cstring>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        RemoteObjectRegistry& RemoteObjectRegistry::GetInstance()
        {
            static RemoteObjectRegistry instance;
            return instance;
        }

        DasResult RemoteObjectRegistry::RegisterObject(
            const ObjectId&    object_id,
            const DasGuid&     iid,
            uint16_t           session_id,
            const std::string& name,
            uint16_t           version)
        {
            uint32_t interface_id = ComputeInterfaceId(iid);
            return RegisterObject(
                object_id,
                iid,
                interface_id,
                session_id,
                name,
                version);
        }

        DasResult RemoteObjectRegistry::RegisterObject(
            const ObjectId&    object_id,
            const DasGuid&     iid,
            uint32_t           interface_id,
            uint16_t           session_id,
            const std::string& name,
            uint16_t           version)
        {
            // 检查参数有效性
            if (IsNullObjectId(object_id))
            {
                return DAS_E_IPC_INVALID_OBJECT_ID;
            }

            if (name.empty())
            {
                return DAS_E_INVALID_ARGUMENT;
            }

            // 编码ObjectId用于查找
            uint64_t encoded_id = EncodeObjectIdForLookup(object_id);

            // 检查对象是否已存在
            if (objects_by_id_.find(encoded_id) != objects_by_id_.end())
            {
                return DAS_E_DUPLICATE_ELEMENT;
            }

            // 创建对象信息
            RemoteObjectInfo info;
            info.iid = iid;
            info.interface_id = interface_id;
            info.object_id = object_id;
            info.session_id = session_id;
            info.name = name;
            info.version = version;

            // 添加到各个索引
            ObjectEntry entry;
            entry.info = info;
            entry.encoded_id = encoded_id;

            objects_by_id_[encoded_id] = entry;
            objects_by_name_[name] = encoded_id;
            objects_by_interface_[interface_id] = encoded_id;

            return DAS_S_OK;
        }

        DasResult RemoteObjectRegistry::UnregisterObject(
            const ObjectId& object_id)
        {
            if (IsNullObjectId(object_id))
            {
                return DAS_E_IPC_INVALID_OBJECT_ID;
            }

            uint64_t encoded_id = EncodeObjectIdForLookup(object_id);
            auto     it = objects_by_id_.find(encoded_id);

            if (it == objects_by_id_.end())
            {
                return DAS_E_IPC_OBJECT_NOT_FOUND;
            }

            // 从各个索引中移除
            const ObjectEntry& entry = it->second;

            // 从名称索引移除
            auto name_it = objects_by_name_.find(entry.info.name);
            if (name_it != objects_by_name_.end()
                && name_it->second == encoded_id)
            {
                objects_by_name_.erase(name_it);
            }

            // 从接口索引移除
            auto interface_it =
                objects_by_interface_.find(entry.info.interface_id);
            if (interface_it != objects_by_interface_.end()
                && interface_it->second == encoded_id)
            {
                objects_by_interface_.erase(interface_it);
            }

            // 从ID索引移除
            objects_by_id_.erase(it);

            return DAS_S_OK;
        }

        void RemoteObjectRegistry::UnregisterAllFromSession(uint16_t session_id)
        {
            std::vector<uint64_t> ids_to_remove;

            // 收集要删除的对象ID
            for (const auto& pair : objects_by_id_)
            {
                if (pair.second.info.session_id == session_id)
                {
                    ids_to_remove.push_back(pair.first);
                }
            }

            // 逐个删除
            for (uint64_t encoded_id : ids_to_remove)
            {
                auto it = objects_by_id_.find(encoded_id);
                if (it != objects_by_id_.end())
                {
                    const ObjectEntry& entry = it->second;

                    // 从名称索引移除
                    auto name_it = objects_by_name_.find(entry.info.name);
                    if (name_it != objects_by_name_.end()
                        && name_it->second == encoded_id)
                    {
                        objects_by_name_.erase(name_it);
                    }

                    // 从接口索引移除
                    auto interface_it =
                        objects_by_interface_.find(entry.info.interface_id);
                    if (interface_it != objects_by_interface_.end()
                        && interface_it->second == encoded_id)
                    {
                        objects_by_interface_.erase(interface_it);
                    }

                    // 从ID索引移除
                    objects_by_id_.erase(it);
                }
            }
        }

        DasResult RemoteObjectRegistry::LookupByName(
            const std::string& name,
            RemoteObjectInfo&  out_info) const
        {
            auto it = objects_by_name_.find(name);
            if (it == objects_by_name_.end())
            {
                return DAS_E_IPC_OBJECT_NOT_FOUND;
            }

            uint64_t encoded_id = it->second;
            auto     id_it = objects_by_id_.find(encoded_id);
            if (id_it == objects_by_id_.end())
            {
                return DAS_E_IPC_OBJECT_NOT_FOUND;
            }

            out_info = id_it->second.info;
            return DAS_S_OK;
        }

        DasResult RemoteObjectRegistry::LookupByInterface(
            uint32_t          interface_id,
            RemoteObjectInfo& out_info) const
        {
            auto it = objects_by_interface_.find(interface_id);
            if (it == objects_by_interface_.end())
            {
                return DAS_E_IPC_OBJECT_NOT_FOUND;
            }

            uint64_t encoded_id = it->second;
            auto     id_it = objects_by_id_.find(encoded_id);
            if (id_it == objects_by_id_.end())
            {
                return DAS_E_IPC_OBJECT_NOT_FOUND;
            }

            out_info = id_it->second.info;
            return DAS_S_OK;
        }

        DasResult RemoteObjectRegistry::GetObjectInfo(
            const ObjectId&   object_id,
            RemoteObjectInfo& out_info) const
        {
            if (IsNullObjectId(object_id))
            {
                return DAS_E_IPC_INVALID_OBJECT_ID;
            }

            uint64_t encoded_id = EncodeObjectIdForLookup(object_id);
            auto     it = objects_by_id_.find(encoded_id);

            if (it == objects_by_id_.end())
            {
                return DAS_E_IPC_OBJECT_NOT_FOUND;
            }

            out_info = it->second.info;
            return DAS_S_OK;
        }

        void RemoteObjectRegistry::ListAllObjects(
            std::vector<RemoteObjectInfo>& out_objects) const
        {
            out_objects.clear();
            out_objects.reserve(objects_by_id_.size());

            for (const auto& pair : objects_by_id_)
            {
                out_objects.push_back(pair.second.info);
            }
        }

        void RemoteObjectRegistry::ListObjectsBySession(
            uint16_t                       session_id,
            std::vector<RemoteObjectInfo>& out_objects) const
        {
            out_objects.clear();

            for (const auto& pair : objects_by_id_)
            {
                if (pair.second.info.session_id == session_id)
                {
                    out_objects.push_back(pair.second.info);
                }
            }
        }

        bool RemoteObjectRegistry::ObjectExists(const ObjectId& object_id) const
        {
            if (IsNullObjectId(object_id))
            {
                return false;
            }

            uint64_t encoded_id = EncodeObjectIdForLookup(object_id);
            return objects_by_id_.find(encoded_id) != objects_by_id_.end();
        }

        size_t RemoteObjectRegistry::GetObjectCount() const
        {
            return objects_by_id_.size();
        }

        void RemoteObjectRegistry::Clear()
        {
            objects_by_id_.clear();
            objects_by_name_.clear();
            objects_by_interface_.clear();
        }

        uint64_t RemoteObjectRegistry::EncodeObjectIdForLookup(
            const ObjectId& obj_id) const
        {
            return EncodeObjectId(obj_id);
        }

        uint32_t RemoteObjectRegistry::ComputeInterfaceId(const DasGuid& guid)
        {
            // FNV-1a hash of GUID binary data
            // This ensures consistent interface_id across all languages
            // GUID layout (16 bytes, little-endian):
            //   bytes 0-3:  data1 (uint32_t)
            //   bytes 4-5:  data2 (uint16_t)
            //   bytes 6-7:  data3 (uint16_t)
            //   bytes 8-15: data4 (uint8_t[8])
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

    }
}
DAS_NS_END
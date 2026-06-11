#include <das/Core/IPC/Host/HostCommandHandlers.h>

#include <das/Core/IPC/HandshakeSerialization.h>
#include <das/Core/IPC/Host/IIpcContext.h>
#include <das/Core/IPC/IDistributedObjectManager.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/MethodMetadata.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/Logger/Logger.h>
#include <das/Utils/fmt.h>

#include <cstring>
#include <filesystem>
#include <fstream>
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
            IIpcContext&             ctx,
            const HostPluginLoader&  load_plugin,
            std::span<const uint8_t> payload,
            IpcCommandResponse&      response)
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
                    DAS_FMT_NS::format("Plugin load failed: {}", manifest_path)
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
                        "Plugin object registration failed: result = {}",
                        static_cast<uint32_t>(reg_result))
                        .c_str());
                response.error_code = reg_result;
                response.response_data.clear();
                return reg_result;
            }

            response.error_code = DAS_S_OK;
            response.response_data.clear();
            AppendObjectId(response.response_data, object_id);

            const DasGuid iid = DAS_IID_BASE;
            AppendBytes(response.response_data, &iid, sizeof(iid));

            AppendUInt16(response.response_data, object_id.session_id);
            AppendUInt16(response.response_data, 1);

            DAS_LOG_INFO(
                DAS_FMT_NS::format(
                    "Plugin loaded: object_id = {{ session: {}, gen: {}, local: {} }}",
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
                        "Interface query payload too small: size = {}",
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
                        "Interface query object lookup failed: session = {}, local = {}, result = {}",
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
                        "Interface query returned: result = {}",
                        qi_result)
                        .c_str());

                response.error_code = qi_result;
                response.response_data.clear();
                const int32_t  fail_result = static_cast<int32_t>(qi_result);
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
            AppendBytes(
                response.response_data,
                &encoded_id,
                sizeof(encoded_id));

            DAS_LOG_INFO(
                DAS_FMT_NS::format(
                    "Interface query succeeded: iid_hash = 0x{:08X}, new_obj_id = {{ session: {}, gen: {}, local: {} }}",
                    interface_id,
                    new_obj_id.session_id,
                    new_obj_id.generation,
                    new_obj_id.local_id)
                    .c_str());

            return DAS_S_OK;
        }

        std::filesystem::path ValidateRelativePath(
            const std::filesystem::path& base_dir,
            const std::string&           relative_path_u8)
        {
            if (relative_path_u8.find("..") != std::string::npos)
            {
                return {};
            }
            if (!relative_path_u8.empty())
            {
                if (relative_path_u8[0] == '/' || relative_path_u8[0] == '\\')
                {
                    return {};
                }
                if (relative_path_u8.size() >= 2
                    && std::isalpha(
                        static_cast<unsigned char>(relative_path_u8[0]))
                    && relative_path_u8[1] == ':')
                {
                    return {};
                }
            }

            std::filesystem::path candidate =
                relative_path_u8.empty()
                    ? base_dir
                    : base_dir
                          / std::filesystem::path(
                              std::u8string_view(
                                  reinterpret_cast<const char8_t*>(
                                      relative_path_u8.data()),
                                  relative_path_u8.size()));

            std::error_code ec;
            auto resolved = std::filesystem::weakly_canonical(candidate, ec);
            if (ec)
            {
                return {};
            }
            auto canonical_base =
                std::filesystem::weakly_canonical(base_dir, ec);
            if (ec)
            {
                return {};
            }

            auto rel = std::filesystem::relative(resolved, canonical_base, ec);
            if (ec)
            {
                return {};
            }
            auto        rel_str = rel.u8string();
            std::string rel_u8(
                reinterpret_cast<const char*>(rel_str.data()),
                rel_str.size());
            if (!rel_u8.empty() && rel_u8.size() >= 2 && rel_u8[0] == '.'
                && rel_u8[1] == '.')
            {
                return {};
            }

            return resolved;
        }

        static constexpr size_t kMaxListEntries = 4096;
        static constexpr size_t kMaxReadFileSize = 4 * 1024 * 1024;

        DasResult HandleListFile(
            const std::filesystem::path& plugin_dir,
            std::span<const uint8_t>     payload,
            IpcCommandResponse&          response)
        {
            std::string relative_path;
            size_t      offset = 0;
            if (!DeserializeString(payload, offset, relative_path, 4096))
            {
                response.error_code = DAS_E_IPC_INVALID_MESSAGE_BODY;
                response.response_data.clear();
                return DAS_E_IPC_INVALID_MESSAGE_BODY;
            }

            uint8_t recursive_flag = 0;
            if (!DeserializeValue(payload, offset, recursive_flag))
            {
                response.error_code = DAS_E_IPC_INVALID_MESSAGE_BODY;
                response.response_data.clear();
                return DAS_E_IPC_INVALID_MESSAGE_BODY;
            }

            auto resolved = ValidateRelativePath(plugin_dir, relative_path);
            if (resolved.empty())
            {
                DAS_CORE_LOG_ERROR(
                    "HandleListFile: invalid relative path: {}",
                    relative_path);
                response.error_code = DAS_E_INVALID_PATH;
                response.response_data.clear();
                return DAS_E_INVALID_PATH;
            }

            std::error_code ec;
            response.error_code = DAS_S_OK;
            response.response_data.clear();

            auto        u8base = plugin_dir.u8string();
            std::string base_u8(
                reinterpret_cast<const char*>(u8base.data()),
                u8base.size());
            SerializeString(response.response_data, base_u8);

            std::vector<uint8_t> entries_buf;
            size_t               entry_count = 0;

            if (recursive_flag)
            {
                for (const auto& entry :
                     std::filesystem::recursive_directory_iterator(
                         resolved,
                         std::filesystem::directory_options::
                             skip_permission_denied,
                         ec))
                {
                    if (ec)
                    {
                        break;
                    }
                    if (entry_count >= kMaxListEntries)
                    {
                        DAS_CORE_LOG_WARN(
                            "HandleListFile: entry count exceeded {}, truncating",
                            kMaxListEntries);
                        break;
                    }
                    uint8_t type =
                        entry.is_directory() ? uint8_t{1} : uint8_t{0};
                    entries_buf.push_back(type);
                    auto        u8name = entry.path().filename().u8string();
                    std::string name_u8(
                        reinterpret_cast<const char*>(u8name.data()),
                        u8name.size());
                    SerializeString(entries_buf, name_u8);
                    auto        u8abs = entry.path().u8string();
                    std::string abs_u8(
                        reinterpret_cast<const char*>(u8abs.data()),
                        u8abs.size());
                    SerializeString(entries_buf, abs_u8);
                    ++entry_count;
                }
            }
            else
            {
                for (const auto& entry : std::filesystem::directory_iterator(
                         resolved,
                         std::filesystem::directory_options::
                             skip_permission_denied,
                         ec))
                {
                    if (ec)
                    {
                        break;
                    }
                    if (entry_count >= kMaxListEntries)
                    {
                        DAS_CORE_LOG_WARN(
                            "HandleListFile: entry count exceeded {}, truncating",
                            kMaxListEntries);
                        break;
                    }
                    uint8_t type =
                        entry.is_directory() ? uint8_t{1} : uint8_t{0};
                    entries_buf.push_back(type);
                    auto        u8name = entry.path().filename().u8string();
                    std::string name_u8(
                        reinterpret_cast<const char*>(u8name.data()),
                        u8name.size());
                    SerializeString(entries_buf, name_u8);
                    auto        u8abs = entry.path().u8string();
                    std::string abs_u8(
                        reinterpret_cast<const char*>(u8abs.data()),
                        u8abs.size());
                    SerializeString(entries_buf, abs_u8);
                    ++entry_count;
                }
            }

            AppendUInt16(
                response.response_data,
                static_cast<uint16_t>(entry_count));
            response.response_data.insert(
                response.response_data.end(),
                entries_buf.begin(),
                entries_buf.end());

            return DAS_S_OK;
        }

        DasResult HandleReadFile(
            const std::filesystem::path& plugin_dir,
            std::span<const uint8_t>     payload,
            IpcCommandResponse&          response)
        {
            std::string relative_path;
            size_t      offset = 0;
            if (!DeserializeString(payload, offset, relative_path, 4096))
            {
                response.error_code = DAS_E_IPC_INVALID_MESSAGE_BODY;
                response.response_data.clear();
                return DAS_E_IPC_INVALID_MESSAGE_BODY;
            }

            auto resolved = ValidateRelativePath(plugin_dir, relative_path);
            if (resolved.empty())
            {
                DAS_CORE_LOG_ERROR(
                    "HandleReadFile: invalid relative path: {}",
                    relative_path);
                response.error_code = DAS_E_INVALID_PATH;
                response.response_data.clear();
                return DAS_E_INVALID_PATH;
            }

            std::ifstream ifs(resolved, std::ios::binary);
            if (!ifs)
            {
                DAS_CORE_LOG_ERROR(
                    "HandleReadFile: cannot open file: {}",
                    relative_path);
                response.error_code = DAS_E_FILE_NOT_FOUND;
                response.response_data.clear();
                return DAS_E_FILE_NOT_FOUND;
            }

            ifs.seekg(0, std::ios::end);
            auto size = ifs.tellg();
            if (size < 0 || static_cast<size_t>(size) > kMaxReadFileSize)
            {
                DAS_CORE_LOG_ERROR(
                    "HandleReadFile: file too large or unreadable: {}, size={}",
                    relative_path,
                    static_cast<int64_t>(size));
                response.error_code = DAS_E_INVALID_SIZE;
                response.response_data.clear();
                return DAS_E_INVALID_SIZE;
            }
            ifs.seekg(0, std::ios::beg);

            auto                 file_size = static_cast<uint32_t>(size);
            std::vector<uint8_t> content(file_size);
            if (!ifs.read(reinterpret_cast<char*>(content.data()), file_size))
            {
                DAS_CORE_LOG_ERROR(
                    "HandleReadFile: read failed: {}",
                    relative_path);
                response.error_code = DAS_E_INVALID_FILE;
                response.response_data.clear();
                return DAS_E_INVALID_FILE;
            }

            response.error_code = DAS_S_OK;
            response.response_data.clear();
            AppendUInt32(response.response_data, file_size);
            response.response_data.insert(
                response.response_data.end(),
                content.begin(),
                content.end());

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
            { return HandleLoadPlugin(*ctx, load_plugin, payload, response); });

        ctx->RegisterCommandHandler(
            static_cast<uint32_t>(IpcCommandType::QUERY_INTERFACE),
            [ctx](
                const ValidatedIPCMessageHeader&,
                std::span<const uint8_t> payload,
                IpcCommandResponse&      response) -> DasResult
            { return HandleQueryInterface(*ctx, payload, response); });

        if (!options.plugin_dir.empty())
        {
            auto plugin_dir = options.plugin_dir;
            ctx->RegisterCommandHandler(
                static_cast<uint32_t>(IpcCommandType::LIST_FILE),
                [plugin_dir](
                    const ValidatedIPCMessageHeader&,
                    std::span<const uint8_t> payload,
                    IpcCommandResponse&      response) -> DasResult
                { return HandleListFile(plugin_dir, payload, response); });

            ctx->RegisterCommandHandler(
                static_cast<uint32_t>(IpcCommandType::READ_FILE),
                [plugin_dir](
                    const ValidatedIPCMessageHeader&,
                    std::span<const uint8_t> payload,
                    IpcCommandResponse&      response) -> DasResult
                { return HandleReadFile(plugin_dir, payload, response); });
        }

        return DAS_S_OK;
    }
} // namespace Das::Core::IPC::Host

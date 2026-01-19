#ifndef DAS_CORE_IPC_SERIALIZER_H
#define DAS_CORE_IPC_SERIALIZER_H

#include <cstdint>
#include <cstring>
#include <das/Core/IPC/IpcErrors.h>
#include <das/IDasBase.h>
#include <vector>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        class SerializerWriter
        {
        public:
            virtual ~SerializerWriter() = default;

            virtual DasResult Write(const void* data, size_t size) = 0;
            virtual size_t    GetPosition() const = 0;
            virtual DasResult Seek(size_t position) = 0;
            virtual DasResult Reserve(size_t size) = 0;

            DasResult WriteInt8(int8_t value)
            {
                return Write(&value, sizeof(value));
            }

            DasResult WriteUInt8(uint8_t value)
            {
                return Write(&value, sizeof(value));
            }

            DasResult WriteInt16(int16_t value)
            {
                return Write(&value, sizeof(value));
            }

            DasResult WriteUInt16(uint16_t value)
            {
                return Write(&value, sizeof(value));
            }

            DasResult WriteInt32(int32_t value)
            {
                return Write(&value, sizeof(value));
            }

            DasResult WriteUInt32(uint32_t value)
            {
                return Write(&value, sizeof(value));
            }

            DasResult WriteInt64(int64_t value)
            {
                return Write(&value, sizeof(value));
            }

            DasResult WriteUInt64(uint64_t value)
            {
                return Write(&value, sizeof(value));
            }

            DasResult WriteFloat(float value)
            {
                return Write(&value, sizeof(value));
            }

            DasResult WriteDouble(double value)
            {
                return Write(&value, sizeof(value));
            }

            DasResult WriteBool(bool value)
            {
                uint8_t bool_value = value ? 1 : 0;
                return Write(&bool_value, sizeof(bool_value));
            }

            DasResult WriteBytes(const uint8_t* data, size_t size)
            {
                WriteUInt64(static_cast<uint64_t>(size));
                return Write(data, size);
            }

            DasResult WriteString(const char* str, size_t length)
            {
                return WriteBytes(
                    reinterpret_cast<const uint8_t*>(str),
                    length);
            }
        };

        class SerializerReader
        {
        public:
            virtual ~SerializerReader() = default;

            virtual DasResult Read(void* data, size_t size) = 0;
            virtual size_t    GetPosition() const = 0;
            virtual size_t    GetRemaining() const = 0;
            virtual DasResult Seek(size_t position) = 0;

            DasResult ReadInt8(int8_t* value)
            {
                return Read(value, sizeof(*value));
            }

            DasResult ReadUInt8(uint8_t* value)
            {
                return Read(value, sizeof(*value));
            }

            DasResult ReadInt16(int16_t* value)
            {
                return Read(value, sizeof(*value));
            }

            DasResult ReadUInt16(uint16_t* value)
            {
                return Read(value, sizeof(*value));
            }

            DasResult ReadInt32(int32_t* value)
            {
                return Read(value, sizeof(*value));
            }

            DasResult ReadUInt32(uint32_t* value)
            {
                return Read(value, sizeof(*value));
            }

            DasResult ReadInt64(int64_t* value)
            {
                return Read(value, sizeof(*value));
            }

            DasResult ReadUInt64(uint64_t* value)
            {
                return Read(value, sizeof(*value));
            }

            DasResult ReadFloat(float* value)
            {
                return Read(value, sizeof(*value));
            }

            DasResult ReadDouble(double* value)
            {
                return Read(value, sizeof(*value));
            }

            DasResult ReadBool(bool* value)
            {
                uint8_t bool_value;
                auto    result = Read(&bool_value, sizeof(bool_value));
                if (result == DAS_S_OK)
                {
                    *value = bool_value != 0;
                }
                return result;
            }

            DasResult ReadBytes(std::vector<uint8_t>& buffer)
            {
                uint64_t size;
                auto     result = ReadUInt64(&size);
                if (result != DAS_S_OK)
                {
                    return result;
                }

                if (size > GetRemaining())
                {
                    return DAS_E_IPC_DESERIALIZATION_FAILED;
                }

                buffer.resize(static_cast<size_t>(size));
                return Read(buffer.data(), static_cast<size_t>(size));
            }

            DasResult ReadString(std::string& str)
            {
                std::vector<uint8_t> buffer;
                auto                 result = ReadBytes(buffer);
                if (result == DAS_S_OK)
                {
                    str.assign(
                        reinterpret_cast<char*>(buffer.data()),
                        buffer.size());
                }
                return result;
            }
        };
    }
}
DAS_NS_END

#endif // DAS_CORE_IPC_SERIALIZER_H

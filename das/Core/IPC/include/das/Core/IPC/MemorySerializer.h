#ifndef DAS_CORE_IPC_MEMORY_SERIALIZER_H
#define DAS_CORE_IPC_MEMORY_SERIALIZER_H

#include <cstdint>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/Serializer.h>
#include <das/DasConfig.h>
#include <das/DasTypes.hpp>
#include <das/IDasBase.h>
#include <vector>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        namespace IPC
        {
            /**
             * 内存序列化读取器 - 基于 std::vector<uint8_t> 实现的内存缓冲区读取
             */
            class MemorySerializerReader : public SerializerReader
            {
            private:
                const uint8_t* buffer_;
                size_t         size_;
                size_t         position_;

            public:
                /**
                 * 构造函数
                 * @param buffer 数据缓冲区
                 * @param size 缓冲区大小
                 */
                explicit MemorySerializerReader(
                    const uint8_t* buffer,
                    size_t         size)
                    : buffer_(buffer), size_(size), position_(0)
                {
                }

                /**
                 * 构造函数（std::vector版本）
                 * @param buffer 数据缓冲区
                 */
                explicit MemorySerializerReader(
                    const std::vector<uint8_t>& buffer)
                    : buffer_(buffer.data()), size_(buffer.size()), position_(0)
                {
                }

                DasResult Read(void* data, size_t size) override
                {
                    if (position_ + size > size_)
                    {
                        return DAS_E_IPC_DESERIALIZATION_FAILED;
                    }

                    uint8_t* bytes = static_cast<uint8_t*>(data);
                    std::memcpy(bytes, buffer_ + position_, size);
                    position_ += size;
                    return DAS_S_OK;
                }

                size_t GetPosition() const override { return position_; }

                size_t GetRemaining() const override
                {
                    return size_ - position_;
                }

                DasResult Seek(size_t position) override
                {
                    if (position > size_)
                    {
                        return DAS_E_IPC_DESERIALIZATION_FAILED;
                    }
                    position_ = position;
                    return DAS_S_OK;
                }
            };

            /**
             * 内存序列化写入器 - 基于 std::vector<uint8_t> 实现的内存缓冲区写入
             */
            class MemorySerializerWriter : public SerializerWriter
            {
            private:
                std::vector<uint8_t> buffer_;

            public:
                /**
                 * 构造函数
                 */
                MemorySerializerWriter() = default;

                /**
                 * 构造函数（预分配大小）
                 * @param size 预分配的缓冲区大小
                 */
                explicit MemorySerializerWriter(size_t size) : buffer_()
                {
                    buffer_.reserve(size);
                }

                DasResult Write(const void* data, size_t size) override
                {
                    const uint8_t* bytes = static_cast<const uint8_t*>(data);
                    buffer_.insert(buffer_.end(), bytes, bytes + size);
                    return DAS_S_OK;
                }

                size_t GetPosition() const override { return buffer_.size(); }

                DasResult Seek(size_t position) override
                {
                    if (position > buffer_.size())
                    {
                        return DAS_E_IPC_DESERIALIZATION_FAILED;
                    }
                    buffer_.resize(position);
                    return DAS_S_OK;
                }

                DasResult Reserve(size_t size) override
                {
                    buffer_.reserve(buffer_.size() + size);
                    return DAS_S_OK;
                }

                /**
                 * 获取内部缓冲区引用
                 * @return 缓冲区引用
                 */
                const std::vector<uint8_t>& GetBuffer() const
                {
                    return buffer_;
                }

                /**
                 * 获取内部缓冲区的可修改引用
                 * @return 缓冲区引用
                 */
                std::vector<uint8_t>& GetBuffer() { return buffer_; }

                /**
                 * 清空缓冲区
                 */
                void Clear() { buffer_.clear(); }

                /**
                 * 检查缓冲区是否为空
                 * @return 是否为空
                 */
                bool IsEmpty() const { return buffer_.empty(); }

                /**
                 * 获取缓冲区大小
                 * @return 缓冲区大小
                 */
                size_t Size() const { return buffer_.size(); }
            };

        }
    }
    DAS_NS_END

#endif // DAS_CORE_IPC_MEMORY_SERIALIZER_H
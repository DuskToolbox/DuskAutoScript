/**
 * @file HandshakeSerialization.h
 * @brief IPC 握手消息序列化/反序列化辅助函数
 *
 * 提供用于序列化和反序列化握手消息的模板函数。
 * 这些函数用于：
 * - HostLauncher（Client 侧发送握手消息）
 * - HandshakeHandler（Host 侧接收握手消息）
 * - IpcCommandHandler（命令消息处理）
 * - Host/main.cpp（插件加载响应）
 */

#ifndef DAS_CORE_IPC_HANDSHAKE_SERIALIZATION_H
#define DAS_CORE_IPC_HANDSHAKE_SERIALIZATION_H

#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/ObjectId.h>

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

DAS_CORE_IPC_NS_BEGIN

//=============================================================================
// 序列化函数
//=============================================================================

/**
 * @brief 将值序列化到字节缓冲区
 * @tparam T 值类型（必须是可平凡复制的）
 * @param buffer 目标缓冲区
 * @param value 要序列化的值
 */
template <typename T>
void SerializeValue(std::vector<uint8_t>& buffer, const T& value)
{
    static_assert(
        std::is_trivially_copyable_v<T>,
        "T must be trivially copyable");
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&value);
    buffer.insert(buffer.end(), ptr, ptr + sizeof(T));
}

/**
 * @brief 将字符串序列化到字节缓冲区
 * @param buffer 目标缓冲区
 * @param str 要序列化的字符串
 *
 * 格式: [uint16_t length][char data...]
 */
inline void SerializeString(
    std::vector<uint8_t>& buffer,
    const std::string&    str)
{
    SerializeValue(buffer, static_cast<uint16_t>(str.size()));
    buffer.insert(
        buffer.end(),
        reinterpret_cast<const uint8_t*>(str.data()),
        reinterpret_cast<const uint8_t*>(str.data()) + str.size());
}

//=============================================================================
// 反序列化函数
//=============================================================================

/**
 * @brief 从字节缓冲区反序列化值
 * @tparam T 值类型（必须是可平凡复制的）
 * @param buffer 源缓冲区
 * @param offset 当前偏移量（会被更新）
 * @param value 输出值
 * @return true 成功，false 缓冲区不足
 */
template <typename T>
bool DeserializeValue(std::span<const uint8_t> buffer, size_t& offset, T& value)
{
    static_assert(
        std::is_trivially_copyable_v<T>,
        "T must be trivially copyable");
    if (offset + sizeof(T) > buffer.size())
    {
        return false;
    }
    std::memcpy(&value, buffer.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

/**
 * @brief 从字节缓冲区反序列化字符串
 * @param buffer 源缓冲区
 * @param offset 当前偏移量（会被更新）
 * @param str 输出字符串
 * @param max_len 最大允许长度（防止恶意输入）
 * @return true 成功，false 缓冲区不足或超长
 *
 * 格式: [uint16_t length][char data...]
 */
inline bool DeserializeString(
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
    str.assign(reinterpret_cast<const char*>(buffer.data() + offset), len);
    offset += len;
    return true;
}

//=============================================================================
// 接口引用（12B，与 [in] 接口参数 wire 逐字节同构）
//=============================================================================

/**
 * @brief 将接口引用追加到字节缓冲区
 *
 * 格式: [uint16_t session_id][uint16_t generation][uint32_t local_id]
 *       [uint32_t interface_hash] —— 与 [in] 接口参数 wire（SetComponent
 *       等 COMPONENT 格式）逐字节同构。全空 ObjectId 语义 = 无引用。
 */
inline void SerializeInterfaceReference(
    std::vector<uint8_t>& buffer,
    const ObjectId&       object_id,
    uint32_t              interface_hash)
{
    SerializeValue(buffer, object_id.session_id);
    SerializeValue(buffer, object_id.generation);
    SerializeValue(buffer, object_id.local_id);
    SerializeValue(buffer, interface_hash);
}

/**
 * @brief 解析载荷末尾的可选接口引用
 *
 * 从 offset 到缓冲区末尾的剩余字节：
 * - 0 字节：无引用（out_object_id 置空），返回 DAS_S_OK
 * - ≥12 字节：解析首个 12 字节引用，返回 DAS_S_OK
 * - 1-11 字节：截断残尾，返回 DAS_E_IPC_INVALID_MESSAGE_BODY（fail-closed，
 *   不静默吞）
 *
 * @param buffer 源缓冲区
 * @param offset 当前偏移量（路径等前序字段已消费；解析成功时前移 12 字节）
 * @param out_object_id 输出对象ID（空 ObjectId = 无引用）
 * @param out_interface_hash 输出接口哈希（无引用时置 0）
 * @return DasResult DAS_S_OK 或 DAS_E_IPC_INVALID_MESSAGE_BODY
 */
inline DasResult DeserializeTrailingInterfaceReference(
    std::span<const uint8_t> buffer,
    size_t&                  offset,
    ObjectId&                out_object_id,
    uint32_t&                out_interface_hash)
{
    out_object_id = ObjectId{0, 0, 0};
    out_interface_hash = 0;
    const size_t remaining = buffer.size() - offset;

    constexpr size_t INTERFACE_REFERENCE_SIZE = 12;
    if (remaining == 0)
    {
        return DAS_S_OK;
    }
    if (remaining < INTERFACE_REFERENCE_SIZE)
    {
        return DAS_E_IPC_INVALID_MESSAGE_BODY;
    }

    if (!DeserializeValue(buffer, offset, out_object_id.session_id)
        || !DeserializeValue(buffer, offset, out_object_id.generation)
        || !DeserializeValue(buffer, offset, out_object_id.local_id)
        || !DeserializeValue(buffer, offset, out_interface_hash))
    {
        return DAS_E_IPC_INVALID_MESSAGE_BODY;
    }
    return DAS_S_OK;
}

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_HANDSHAKE_SERIALIZATION_H

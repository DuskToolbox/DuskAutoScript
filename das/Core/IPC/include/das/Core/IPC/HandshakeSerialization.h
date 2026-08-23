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

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_HANDSHAKE_SERIALIZATION_H

#ifndef DAS_CORE_IPC_OBJECT_ID_H
#define DAS_CORE_IPC_OBJECT_ID_H

#include <cstdint>
#include <das/IDasBase.h>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN
struct ObjectId
{
    uint16_t session_id;
    uint16_t generation;
    uint32_t local_id;
};

constexpr uint64_t EncodeObjectId(const ObjectId& obj_id) noexcept
{
    return (static_cast<uint64_t>(obj_id.session_id) << 48)
           | (static_cast<uint64_t>(obj_id.generation) << 32)
           | static_cast<uint64_t>(obj_id.local_id);
}

constexpr ObjectId DecodeObjectId(uint64_t encoded_id) noexcept
{
    return ObjectId{
        .session_id = static_cast<uint16_t>(encoded_id >> 48),
        .generation = static_cast<uint16_t>((encoded_id >> 32) & 0xFFFF),
        .local_id = static_cast<uint32_t>(encoded_id & 0xFFFFFFFF)};
}

constexpr bool IsValidObjectId(
    const ObjectId& obj_id,
    uint16_t        expected_generation) noexcept
{
    return obj_id.generation == expected_generation;
}

constexpr uint16_t IncrementGeneration(uint16_t generation) noexcept
{
    return (generation == 0xFFFF) ? 1 : (generation + 1);
}

constexpr bool IsNullObjectId(const ObjectId& obj_id) noexcept
{
    return obj_id.session_id == 0 && obj_id.generation == 0
           && obj_id.local_id == 0;
}

constexpr bool IsNullObjectId(uint64_t encoded_id) noexcept
{
    return encoded_id == 0;
}

/// @brief ObjectId 比较运算符（用于容器）
constexpr bool operator==(const ObjectId& lhs, const ObjectId& rhs) noexcept
{
    return lhs.session_id == rhs.session_id && lhs.generation == rhs.generation
           && lhs.local_id == rhs.local_id;
}

constexpr bool operator!=(const ObjectId& lhs, const ObjectId& rhs) noexcept
{
    return !(lhs == rhs);
}

/// @brief 从接口指针获取 ObjectId
/// @param interface_ptr 接口指针（可以是 Proxy 或本地对象）
/// @return ObjectId，如果是 nullptr 返回空 ObjectId
/// @note 此函数的完整实现需要 IPCProxyBase 的完整定义，
///       因此在生成的 Proxy 代码中使用内联方式调用
inline ObjectId GetObjectIdFromInterface(IDasBase* interface_ptr) noexcept
{
    // 默认实现：返回空 ObjectId
    // 实际的 Proxy 代码会内联展开具体的转换逻辑
    (void)interface_ptr;
    return ObjectId{0, 0, 0};
}
DAS_CORE_IPC_NS_END

/// @brief std::hash 特化，用于 unordered_map/unordered_set
template <>
struct std::hash<::Das::Core::IPC::ObjectId>
{
    size_t operator()(const ::Das::Core::IPC::ObjectId& obj_id) const noexcept
    {
        // 使用 EncodeObjectId 得到的 uint64_t 作为 hash 值
        return static_cast<size_t>(::Das::Core::IPC::EncodeObjectId(obj_id));
    }
};

#endif // DAS_CORE_IPC_OBJECT_ID_H

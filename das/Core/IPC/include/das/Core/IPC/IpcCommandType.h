#ifndef DAS_CORE_IPC_IPC_COMMAND_TYPE_H
#define DAS_CORE_IPC_IPC_COMMAND_TYPE_H

#include <cstdint>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN

/// @file IpcCommandType.h
/// @brief IPC 命令类型枚举（轻量叶子头）
///
/// IpcCommandHandler.h 经 ValidatedIPCMessageHeader.h 与
/// IpcHeaderValidator.h 互为 include 环，枚举独立成头供基础校验头引用。

/**
 * @brief IPC 命令类型枚举
 *
 * 定义主进程和 Host 进程之间的控制命令类型
 */
enum class IpcCommandType : uint8_t
{
    // 对象管理命令
    LookupObject = 3,       // 通过 ObjectId 查找对象
    LookupByName = 4,       // 通过名称查找对象
    LookupByInterface = 5,  // 通过接口类型查找对象
    ListObjects = 6,        // 列出所有对象
    ListSessionObjects = 7, // 列出指定会话的对象
    ClearSession = 8,       // 清除指定会话的所有对象
    LoadPlugin = 9,         // 加载插件

    // 心跳和状态命令
    Ping = 10, // 心跳请求
    Pong = 11, // 心跳响应

    // 查询命令
    GetObjectCount = 20, // 获取对象数量

    // 远端文件访问 (50-59)
    ListFile = 50, // 列出远端 Host 工作目录下的文件/目录
    ReadFile = 51, // 读取远端 Host 工作目录下的文件内容

    // 接口查询 (120-129)
    QueryInterface = 120, // 远程 QueryInterface 请求

    // 远程引用计数 (130-139)
    RemoteAddRef = 130,  // 远程端增加引用
    RemoteRelease = 132, // 远程端释放引用

    // 共享内存管理 (140-149)
    ReleaseShmBlock = 140, // 释放共享内存块（fire-and-forget EVENT）

    // 保留
    Unknown = 255
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_COMMAND_TYPE_H

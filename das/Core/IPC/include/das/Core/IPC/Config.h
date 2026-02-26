//! @file Config.h
//! @brief IPC 模块配置文件

#pragma once

#include <das/DasConfig.h>

//! @brief IPC 命名空间宏
#define DAS_CORE_IPC_NS Das::Core::IPC

//! @brief IPC 命名空间开始宏
//! @details 展开为：namespace Das { namespace Core { namespace IPC {
#define DAS_CORE_IPC_NS_BEGIN                                                  \
    DAS_NS_BEGIN                                                               \
    namespace Core                                                             \
    {                                                                          \
        namespace IPC                                                          \
        {

//! @brief IPC 命名空间结束宏
//! @details 展开为：} } }
#define DAS_CORE_IPC_NS_END                                                    \
    }                                                                          \
    }                                                                          \
    DAS_NS_END

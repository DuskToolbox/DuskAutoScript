/**
 * @file ScopedSessionCoordinator.h
 * @brief RAII guard for SessionCoordinator singleton
 */
#ifndef DAS_CORE_IPC_TEST_SCOPED_SESSION_COORDINATOR_H
#define DAS_CORE_IPC_TEST_SCOPED_SESSION_COORDINATOR_H

#include <cstdint>
#include <das/Core/IPC/SessionCoordinator.h>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        namespace Test
        {
            /**
             * @brief SessionCoordinator 的 RAII 封装
             *
             * 用于自动管理 SessionCoordinator 单例的生命周期，
             * 确保在作用域结束时自动重置 session_id，避免测试之间相互干扰。
             *
             * 使用示例：
             * @code
             * void SetUp() override {
             *     coordinator_ =
             * std::make_unique<ScopedSessionCoordinator>(ScopedSessionCoordinator::AsHost(5));
             * }
             *
             * void TearDown() override {
             *     coordinator_.reset();
             * }
             * @endcode
             */
            class ScopedSessionCoordinator
            {
            public:
                /**
                 * @brief 创建主进程类型的 ScopedSessionCoordinator
                 * @return ScopedSessionCoordinator 实例，session_id 设置为 1
                 */
                static ScopedSessionCoordinator AsMainProcess()
                {
                    return ScopedSessionCoordinator(1);
                }

                /**
                 * @brief 创建 Host 类型的 ScopedSessionCoordinator
                 * @param session_id Host 的 session_id（默认为 2）
                 * @return ScopedSessionCoordinator 实例
                 */
                static ScopedSessionCoordinator AsHost(uint16_t session_id = 2)
                {
                    return ScopedSessionCoordinator(session_id);
                }

                /**
                 * @brief 构造函数，设置本地 session_id
                 * @param session_id 要设置的本地 session_id
                 */
                explicit ScopedSessionCoordinator(uint16_t session_id)
                {
                    SessionCoordinator::GetInstance().SetLocalSessionId(
                        session_id);
                }

                /**
                 * @brief 析构函数，重置本地 session_id
                 */
                ~ScopedSessionCoordinator()
                {
                    SessionCoordinator::GetInstance().ResetLocalSessionId();
                }

                /**
                 * @brief 禁用拷贝构造
                 */
                ScopedSessionCoordinator(const ScopedSessionCoordinator&) =
                    delete;

                /**
                 * @brief 禁用拷贝赋值
                 */
                ScopedSessionCoordinator& operator=(
                    const ScopedSessionCoordinator&) = delete;

                /**
                 * @brief 禁用移动构造
                 */
                ScopedSessionCoordinator(ScopedSessionCoordinator&&) = delete;

                /**
                 * @brief 禁用移动赋值
                 */
                ScopedSessionCoordinator& operator=(
                    ScopedSessionCoordinator&&) = delete;
            };
        } // namespace Test
    } // namespace IPC
} // namespace Core
DAS_NS_END

#endif // DAS_CORE_IPC_TEST_SCOPED_SESSION_COORDINATOR_H

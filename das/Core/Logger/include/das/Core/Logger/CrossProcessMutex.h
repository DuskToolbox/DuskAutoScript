#ifndef DAS_CORE_LOGGER_CROSS_PROCESS_MUTEX_H
#define DAS_CORE_LOGGER_CROSS_PROCESS_MUTEX_H

// getenv/setenv is used to inherit the logger mutex identity into child
// processes. Windows stores the owner pid; Linux stores the System V semid.
#define _CRT_SECURE_NO_WARNINGS

#include <boost/process/v2/pid.hpp>
#include <cstdint>
#include <cstdlib>
#include <das/DasConfig.h>
#include <das/Utils/fmt.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef DAS_WINDOWS
#include <windows.h>
#else
#include <cerrno>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/types.h>
#endif

DAS_NS_BEGIN
namespace Core
{
    namespace Logger
    {
        namespace Details
        {
            inline constexpr const char* kLoggerMutexEnv = "_DAS_LOGGER_MUTEX";
            inline constexpr const char* kLoggerMutexPrefix =
                "_DAS_LOGGER_MUTEX-";

            inline std::string GetCurrentPidString()
            {
                return DAS::fmt::format(
                    "{}",
                    static_cast<unsigned long long>(
                        boost::process::v2::current_pid()));
            }

            inline void SetLoggerMutexEnv(const std::string& value)
            {
#ifdef DAS_WINDOWS
                if (_putenv_s(kLoggerMutexEnv, value.c_str()) != 0)
                {
                    throw std::runtime_error(
                        "Failed to set DAS logger mutex environment value");
                }
#else
                if (setenv(kLoggerMutexEnv, value.c_str(), 1) != 0)
                {
                    throw std::system_error(
                        errno,
                        std::generic_category(),
                        "setenv");
                }
#endif
            }

#ifdef DAS_WINDOWS
            struct LoggerMutexIdentity
            {
                std::string name;
                bool        owner = false;
            };

            inline LoggerMutexIdentity ResolveLoggerMutexIdentity()
            {
                if (const char* owner_pid = std::getenv(kLoggerMutexEnv);
                    owner_pid != nullptr && owner_pid[0] != '\0')
                {
                    return {
                        DAS::fmt::format("{}{}", kLoggerMutexPrefix, owner_pid),
                        false};
                }

                std::string pid = GetCurrentPidString();
                SetLoggerMutexEnv(pid);
                return {
                    DAS::fmt::format("{}{}", kLoggerMutexPrefix, pid),
                    true};
            }

            class NativeHandle
            {
            public:
                NativeHandle() = default;
                explicit NativeHandle(HANDLE handle) noexcept : handle_(handle)
                {
                }

                ~NativeHandle()
                {
                    if (handle_ != nullptr)
                    {
                        ::CloseHandle(handle_);
                    }
                }

                NativeHandle(const NativeHandle&) = delete;
                NativeHandle& operator=(const NativeHandle&) = delete;

                NativeHandle(NativeHandle&& other) noexcept
                    : handle_(other.handle_)
                {
                    other.handle_ = nullptr;
                }

                NativeHandle& operator=(NativeHandle&& other) noexcept
                {
                    if (this != &other)
                    {
                        if (handle_ != nullptr)
                        {
                            ::CloseHandle(handle_);
                        }
                        handle_ = other.handle_;
                        other.handle_ = nullptr;
                    }
                    return *this;
                }

                [[nodiscard]]
                HANDLE Get() const noexcept
                {
                    return handle_;
                }

            private:
                HANDLE handle_ = nullptr;
            };
#else
            union SemctlArgument
            {
                int              val;
                struct semid_ds* buf;
                unsigned short*  array;
            };

            inline int ParseSemaphoreIdFromEnv()
            {
                const char* raw_value = std::getenv(kLoggerMutexEnv);
                if (raw_value == nullptr || raw_value[0] == '\0')
                {
                    throw std::runtime_error(
                        DAS::fmt::format("{} is not set", kLoggerMutexEnv));
                }

                char* end = nullptr;
                errno = 0;
                const long parsed = std::strtol(raw_value, &end, 10);
                if (errno != 0 || end == raw_value || *end != '\0' || parsed < 0
                    || parsed > std::numeric_limits<int>::max())
                {
                    throw std::runtime_error(
                        DAS::fmt::format(
                            "Invalid {} value: {}",
                            kLoggerMutexEnv,
                            raw_value));
                }

                return static_cast<int>(parsed);
            }
#endif
        } // namespace Details

        class CrossProcessMutex
        {
        public:
            CrossProcessMutex()
#ifdef DAS_WINDOWS
                : identity_(Details::ResolveLoggerMutexIdentity())
#endif
            {
                Initialize();
            }

            ~CrossProcessMutex()
            {
#ifndef DAS_WINDOWS
                if (owner_ && sem_id_ >= 0)
                {
                    semctl(sem_id_, 0, IPC_RMID);
                }
#endif
            }

            CrossProcessMutex(const CrossProcessMutex&) = delete;
            CrossProcessMutex& operator=(const CrossProcessMutex&) = delete;

            void Lock()
            {
#ifdef DAS_WINDOWS
                const DWORD wait_result =
                    ::WaitForSingleObject(handle_.Get(), INFINITE);
                if (wait_result != WAIT_OBJECT_0
                    && wait_result != WAIT_ABANDONED)
                {
                    throw std::system_error(
                        static_cast<int>(::GetLastError()),
                        std::system_category(),
                        "WaitForSingleObject");
                }
#else
                struct sembuf operation{0, -1, SEM_UNDO};
                while (semop(sem_id_, &operation, 1) == -1)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    throw std::system_error(
                        errno,
                        std::generic_category(),
                        "semop lock");
                }
#endif
            }

            void Unlock() noexcept
            {
#ifdef DAS_WINDOWS
                ::ReleaseMutex(handle_.Get());
#else
                struct sembuf operation{0, 1, SEM_UNDO};
                while (semop(sem_id_, &operation, 1) == -1 && errno == EINTR)
                {
                }
#endif
            }

        private:
            class ScopedLock
            {
            public:
                explicit ScopedLock(CrossProcessMutex& mutex) : mutex_(mutex)
                {
                    mutex_.Lock();
                }

                ~ScopedLock() { mutex_.Unlock(); }

                ScopedLock(const ScopedLock&) = delete;
                ScopedLock& operator=(const ScopedLock&) = delete;

            private:
                CrossProcessMutex& mutex_;
            };

        public:
            [[nodiscard]]
            ScopedLock Acquire()
            {
                return ScopedLock{*this};
            }

        private:
            void Initialize()
            {
#ifdef DAS_WINDOWS
                HANDLE handle = nullptr;
                if (identity_.owner)
                {
                    handle =
                        ::CreateMutexA(nullptr, FALSE, identity_.name.c_str());
                }
                else
                {
                    handle = ::OpenMutexA(
                        SYNCHRONIZE | MUTEX_MODIFY_STATE,
                        FALSE,
                        identity_.name.c_str());
                }

                if (handle == nullptr)
                {
                    throw std::system_error(
                        static_cast<int>(::GetLastError()),
                        std::system_category(),
                        identity_.owner ? "CreateMutexA" : "OpenMutexA");
                }

                handle_ = Details::NativeHandle{handle};
#else
                if (const char* sem_id = std::getenv(Details::kLoggerMutexEnv);
                    sem_id != nullptr && sem_id[0] != '\0')
                {
                    sem_id_ = Details::ParseSemaphoreIdFromEnv();
                    return;
                }

                owner_ = true;
                sem_id_ = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
                if (sem_id_ < 0)
                {
                    throw std::system_error(
                        errno,
                        std::generic_category(),
                        "semget create");
                }

                Details::SemctlArgument arg{};
                arg.val = 1;
                if (semctl(sem_id_, 0, SETVAL, arg) == -1)
                {
                    const int error = errno;
                    semctl(sem_id_, 0, IPC_RMID);
                    throw std::system_error(
                        error,
                        std::generic_category(),
                        "semctl SETVAL");
                }

                Details::SetLoggerMutexEnv(DAS::fmt::format("{}", sem_id_));
#endif
            }

#ifdef DAS_WINDOWS
            Details::LoggerMutexIdentity identity_;
#endif

#ifdef DAS_WINDOWS
            Details::NativeHandle handle_;
#else
            int  sem_id_ = -1;
            bool owner_ = false;
#endif
        };
    } // namespace Logger
} // namespace Core
DAS_NS_END

#endif // DAS_CORE_LOGGER_CROSS_PROCESS_MUTEX_H

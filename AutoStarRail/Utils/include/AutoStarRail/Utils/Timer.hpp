#ifndef ASR_UTILS_TIMER_HPP
#define ASR_UTILS_TIMER_HPP

#include <AutoStarRail/Utils/Config.h>
#include <chrono>

ASR_UTILS_NS_BEGIN

class Timer
{
    std::chrono::high_resolution_clock::time_point start_{};

public:
    void Begin() { start_ = std::chrono::high_resolution_clock::now(); }
    /**
     * @return 过去的时间，单位毫秒
     */
    auto End()
    {
        const auto end = std::chrono::high_resolution_clock::now();
        const auto delta_time = end - start_;
        const auto delta_time_ms =
            std::chrono::duration_cast<std::chrono::microseconds>(delta_time);
        return delta_time_ms.count();
    }
};

ASR_UTILS_NS_END

#endif // ASR_UTILS_TIMER_HPP

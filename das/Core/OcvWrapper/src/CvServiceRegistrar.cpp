#include "CvCpuImpl.h"
#include <das/Core/Debug/DebugDecorators.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/OcvWrapper/Config.h>
#include <das/Core/OcvWrapper/CvServiceRegistrar.h>

#ifdef DAS_WITH_CUDA
#include "CvCudaImpl.h"
#include <opencv2/core/cuda.hpp>
#endif

DAS_CORE_OCVWRAPPER_NS_BEGIN

DasResult RegisterCvServices(IPC::MainProcess::IIpcContext& ipc_context)
{
    // 1. Always register CPU backend (per D-03: CPU is always available)
    {
        auto* cpu_impl = DAS::Core::Debug::MaybeDecorateCvRaw(CvCpuImpl::MakeRaw(), "cv.cpu");
        auto  result = ipc_context.RegisterServiceByName(
            cpu_impl,
            DasIidOf<ExportInterface::IDasCv>(),
            "cv.cpu");
        cpu_impl->Release();

        if (IsFailed(result))
        {
            DAS_CORE_LOG_ERROR(
                "Failed to register cv.cpu service: result={}",
                result);
            return result;
        }
        DAS_CORE_LOG_INFO("Registered cv.cpu service");
        // RegisterServiceByName stores its own reference in the service table.
    }

    // 2. Probe and register CUDA backend (per D-03: unavailable not registered)
#ifdef DAS_WITH_CUDA
    {
        int cuda_device_count = cv::cuda::getCudaEnabledDeviceCount();
        if (cuda_device_count > 0)
        {
            auto* cuda_impl = DAS::Core::Debug::MaybeDecorateCvRaw(CvCudaImpl::MakeRaw(), "cv.cuda");
            auto  result = ipc_context.RegisterServiceByName(
                cuda_impl,
                DasIidOf<ExportInterface::IDasCv>(),
                "cv.cuda");
            cuda_impl->Release();

            if (IsFailed(result))
            {
                DAS_CORE_LOG_ERROR(
                    "Failed to register cv.cuda service: result={}",
                    result);
                // Do not return error — CPU backend is still available
            }
            else
            {
                DAS_CORE_LOG_INFO(
                    "Registered cv.cuda service ({} CUDA devices)",
                    cuda_device_count);
            }
        }
        else
        {
            DAS_CORE_LOG_INFO("No CUDA devices found, cv.cuda not registered");
        }
    }
#else
    DAS_CORE_LOG_INFO("Built without CUDA support, cv.cuda not available");
#endif

    return DAS_S_OK;
}

DAS_CORE_OCVWRAPPER_NS_END

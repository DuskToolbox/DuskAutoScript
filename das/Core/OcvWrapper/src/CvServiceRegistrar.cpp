#include "CvCpuImpl.h"
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
        auto* cpu_impl = CvCpuImpl::MakeRaw();
        auto  result = ipc_context.RegisterServiceByName(
            cpu_impl,
            DasIidOf<ExportInterface::IDasCv>(),
            "cv.cpu");

        if (IsFailed(result))
        {
            DAS_CORE_LOG_ERROR(
                "Failed to register cv.cpu service: result={}",
                result);
            cpu_impl->Release();
            return result;
        }
        cpu_impl->Release();
        DAS_CORE_LOG_INFO("Registered cv.cpu service");
        // RegisterServiceByName stores its own reference in the service table.
    }

    // 2. Probe and register CUDA backend (per D-03: unavailable not registered)
#ifdef DAS_WITH_CUDA
    {
        int cuda_device_count = cv::cuda::getCudaEnabledDeviceCount();
        if (cuda_device_count > 0)
        {
            auto* cuda_impl = CvCudaImpl::MakeRaw();
            auto  result = ipc_context.RegisterServiceByName(
                cuda_impl,
                DasIidOf<ExportInterface::IDasCv>(),
                "cv.cuda");

            if (IsFailed(result))
            {
                DAS_CORE_LOG_ERROR(
                    "Failed to register cv.cuda service: result={}",
                    result);
                cuda_impl->Release();
                // Do not return error — CPU backend is still available
            }
            else
            {
                cuda_impl->Release();
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

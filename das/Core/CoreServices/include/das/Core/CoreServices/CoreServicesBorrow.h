#ifndef DAS_CORE_CORE_SERVICES_CORE_SERVICES_BORROW_H
#define DAS_CORE_CORE_SERVICES_CORE_SERVICES_BORROW_H

#include <das/DasExport.h>
#include <das/IDasCoreServices.h>

namespace Das::Core::TaskScheduler
{
    class SchedulerService;
}

namespace Das::Core::CoreServices
{

    /**
     * @brief 从 IDasCoreServices 实例借用其内部拥有的具体调度器服务。
     *
     * 主进程 HTTP 直连装配入口：DasHttp 控制器进程内直连具体
     * SchedulerService，不经调度器接口面（该接口已定退役，无 IPC 能力的
     * 进程内手写头）。
     *
     * 返回裸指针借用：生命周期随 CoreServices 对象——调用方须保持其
     * IDasCoreServices 引用存活期间使用。入参为空返回 nullptr。
     *
     * 跨 DLL 边界（DasCore -> DasHttp）：导出面仅此入口，方法面由
     * SchedulerService.h 自带（DAS_API 标注的直连方法集）。本函数与
     * CreateIDasCoreServices 同翻译单元实现——接口指针必源自
     * CoreServicesImpl 单继承上转，downcast 取回原对象安全。
     */
    DAS_API Das::Core::TaskScheduler::SchedulerService* BorrowSchedulerService(
        IDasCoreServices* p_services) noexcept;

} // namespace Das::Core::CoreServices

#endif // DAS_CORE_CORE_SERVICES_CORE_SERVICES_BORROW_H

#ifndef DAS_PLUGINS_DASFLOWCONTROL_FLOWCONTROLERRORLENSREGISTRATION_H
#define DAS_PLUGINS_DASFLOWCONTROL_FLOWCONTROLERRORLENSREGISTRATION_H

#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasErrorLens.h>

DAS_NS_BEGIN

namespace Plugins::DasFlowControl
{

    /// 创建已注册全部 FlowControl -10100 系列错误码（en + zh-cn）的
    /// BasicErrorLens。成功返回 DasPtr<IDasBasicErrorLens>，失败返回
    /// nullptr。
    DasPtr<PluginInterface::IDasBasicErrorLens>
    CreateRegisteredFlowControlErrorLens();

} // namespace Plugins::DasFlowControl

DAS_NS_END

#endif // DAS_PLUGINS_DASFLOWCONTROL_FLOWCONTROLERRORLENSREGISTRATION_H

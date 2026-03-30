%{
#include <memory>
#include <string>
#include <das/DasConfig.h>
#include <das/DasExport.h>
#include <das/DasTypes.hpp>
#include <das/IDasBase.h>
#include <das/DasString.hpp>
#include <das/DasException.hpp>

#include <das/DasApi.h>
#include <das/DasSwigApi.h>
#include <das/IDasAsyncOperation.h>
#include <das/IDasAsyncLoadPluginOperation.h>
#include <das/IDasAsyncHandshakeOperation.h>
#ifdef SWIGPYTHON
#include <das/Core/ForeignInterfaceHost/PythonHost.h>

#ifdef DEBUG

// 修复 dll 文件带 _d 后缀时，PyInit 函数名称不正确的问题。
// TODO: 后续在CMake中配置更名，而不再靠这里修复
// #ifdef __cplusplus
// extern "C"
// #endif
// SWIGEXPORT
// #if PY_VERSION_HEX >= 0x03000000
// PyObject*
// #else
// void
// #endif
// SWIG_init(void);

// #ifdef __cplusplus
// extern "C"
// #endif
// SWIGEXPORT
// #if PY_VERSION_HEX >= 0x03000000
// PyObject*
// #else
// void
// #endif
// PyInit__DasCorePythonExport(void) {
// #if PY_VERSION_HEX >= 0x03000000
//     return ::SWIG_init();
// #else
//     ::SWIG_init();
// #endif
// }

#endif // DEBUG

#endif // SWIGPYTHON

%}
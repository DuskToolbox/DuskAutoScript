#define DAS_BUILD_SHARED

#include "TestInterfaceImpl.h"
#include <cstring>
#include <das/Utils/CommonUtils.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

DAS_NS_BEGIN

TestInterfaceImpl::TestInterfaceImpl() : test_id_(0) {}

TestInterfaceImpl::~TestInterfaceImpl() = default;

DasResult TestInterfaceImpl::GetTestId(uint64_t* p_out_id)
{
    DAS_UTILS_CHECK_POINTER(p_out_id);
    *p_out_id = test_id_;
    return DAS_S_OK;
}

DasResult TestInterfaceImpl::SetTestId(uint64_t id)
{
    test_id_ = id;
    return DAS_S_OK;
}

DasResult TestInterfaceImpl::Add(int32_t a, int32_t b, int32_t* p_result)
{
    DAS_UTILS_CHECK_POINTER(p_result);
    *p_result = a + b;
    return DAS_S_OK;
}

DasResult TestInterfaceImpl::Multiply(int32_t a, int32_t b, int32_t* p_result)
{
    DAS_UTILS_CHECK_POINTER(p_result);
    *p_result = a * b;
    return DAS_S_OK;
}

DasResult TestInterfaceImpl::Concatenate(
    const char* a,
    const char* b,
    char**      p_result)
{
    DAS_UTILS_CHECK_POINTER(a);
    DAS_UTILS_CHECK_POINTER(b);
    DAS_UTILS_CHECK_POINTER(p_result);

    concatenate_result_ = std::string(a) + std::string(b);
    *p_result = new char[concatenate_result_.size() + 1];
    std::strcpy(*p_result, concatenate_result_.c_str());
    return DAS_S_OK;
}

DasResult TestInterfaceImpl::GetProcessId(uint32_t* p_out_pid)
{
    DAS_UTILS_CHECK_POINTER(p_out_pid);

#ifdef _WIN32
    *p_out_pid = static_cast<uint32_t>(GetCurrentProcessId());
#else
    *p_out_pid = static_cast<uint32_t>(getpid());
#endif

    return DAS_S_OK;
}

DasResult TestInterfaceImpl::Ping(bool* p_pong)
{
    DAS_UTILS_CHECK_POINTER(p_pong);
    *p_pong = true;
    return DAS_S_OK;
}

DAS_NS_END

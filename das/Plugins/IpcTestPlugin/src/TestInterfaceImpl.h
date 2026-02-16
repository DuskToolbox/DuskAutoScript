#ifndef DAS_PLUGINS_IPCTESTPLUGIN_TESTINTERFACEIMPL_H
#define DAS_PLUGINS_IPCTESTPLUGIN_TESTINTERFACEIMPL_H

#include <cstdint>
#include <das/DasApi.h>
#include <das/IDasBase.h>
#include <das/_autogen/idl/abi/IDasTestInterface.h>
#include <das/_autogen/idl/wrapper/Das.TestInterface.IDasTestInterface.Implements.hpp>
#include <string>

DAS_NS_BEGIN

class TestInterfaceImpl final
    : public TestInterface::IDasTestInterfaceImplBase<TestInterfaceImpl>
{
public:
    TestInterfaceImpl();
    ~TestInterfaceImpl() override;

    DAS_IMPL GetTestId(uint64_t* p_out_id) override;
    DAS_IMPL SetTestId(uint64_t id) override;
    DAS_IMPL Add(int32_t a, int32_t b, int32_t* p_result) override;
    DAS_IMPL Multiply(int32_t a, int32_t b, int32_t* p_result) override;
    DAS_IMPL Concatenate(const char* a, const char* b, char** p_result)
        override;
    DAS_IMPL GetProcessId(uint32_t* p_out_pid) override;
    DAS_IMPL Ping(bool* p_pong) override;

private:
    uint64_t    test_id_;
    std::string concatenate_result_;
};

DAS_NS_END

#endif // DAS_PLUGINS_IPCTESTPLUGIN_TESTINTERFACEIMPL_H

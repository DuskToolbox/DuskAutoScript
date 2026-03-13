/**
 * @file main.cpp
 * @brief IpcMultiProcessTest 自定义 main 函数
 *
 * 支持两种运行模式：
 * 1. 正常模式：运行 gtest 测试
 * 2. --fake-main 模式：作为假主进程运行，创建 IPC 服务器等待 Host 连接
 */

#include "FakeMainProcess.h"
#include <boost/program_options.hpp>
#include <gtest/gtest.h>
#include <iostream>

int main(int argc, char* argv[])
{
    boost::program_options::options_description desc("IpcMultiProcessTest");
    desc.add_options()(
        "fake-main",
        "Run as fake main process for KillParent test")(
        "signal-name",
        boost::program_options::value<std::string>(),
        "Signal name for cross-process sync")(
        "help",
        "Show this help message");

    boost::program_options::variables_map vm;
    // 使用 allow_unregistered() 允许未知参数（如 gtest 的 --gtest_list_tests）
    // 这样 gtest 参数可以先被 boost 忽略，再被 gtest 解析
    boost::program_options::store(
        boost::program_options::command_line_parser(argc, argv)
            .options(desc)
            .allow_unregistered()
            .run(),
        vm);
    boost::program_options::notify(vm);

    if (vm.count("help"))
    {
        std::cout << desc << std::endl;
        return 0;
    }

    if (vm.count("fake-main"))
    {
        std::string signal_name =
            vm.count("signal-name")
                ? vm["signal-name"].as<std::string>()
                : FakeMainProcess::GenerateUniqueSignalName();

        return FakeMainProcess::RunFakeMainProcessMode(signal_name);
    }

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

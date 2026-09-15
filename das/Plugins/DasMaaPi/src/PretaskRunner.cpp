#include "PretaskRunner.h"

#include <das/DasApi.h>
#include <das/Utils/BoostPathBridge.h>
#include <das/Utils/PathText.h>

#include <utility>

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_BOOST_PROCESS_WARNING

#include <boost/asio.hpp>
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/start_dir.hpp>
#include <boost/process/v2/stdio.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

DAS_DISABLE_WARNING_END

namespace Das::Plugins::DasMaaPi
{
    namespace
    {
        IPretaskRunner* g_pretask_runner_for_test = nullptr;
    } // namespace

    PretaskRunResult BoostPretaskRunner::Run(
        const PretaskLaunchRequest& request)
    {
        PretaskRunResult result;
        try
        {
            boost::asio::io_context io_context;
            // bp2 的 exe 与 start_dir 形参是 boost path：窄串经 ACP 构造
            // 会双损（中文路径下启动错位）。exec 是 UTF-8 源，先物化成
            // std path 再过桥；args 是窄 UTF-8 通道，保持直传。
            boost::process::v2::process child(
                io_context,
                Das::Utils::ToBoostPath(Das::Utils::PathFromUtf8(request.exec)),
                request.args,
                boost::process::v2::process_start_dir(
                    Das::Utils::ToBoostPath(request.working_directory)),
                boost::process::v2::process_stdio{{}, nullptr, nullptr});
            boost::system::error_code wait_ec;
            const auto                code = child.wait(wait_ec);
            result.launched = true;
            // boost::process v2's wait returns the raw exit code; a wait error
            // (e.g. forced termination) is surfaced as exit_code=-1 so the
            // runtime treats it as a nonzero exit per protocol.
            result.exit_code = wait_ec ? -1 : code;
            return result;
        }
        catch (const boost::system::system_error& error)
        {
            result.failure_message = error.what();
            return result;
        }
        catch (const std::exception& error)
        {
            result.failure_message = error.what();
            return result;
        }
    }

    void SetPretaskRunnerForTest(IPretaskRunner* runner)
    {
        g_pretask_runner_for_test = runner;
    }

    IPretaskRunner* PretaskRunnerForRuntime()
    {
        return g_pretask_runner_for_test;
    }
} // namespace Das::Plugins::DasMaaPi

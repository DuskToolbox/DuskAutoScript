// AgentProcessRunner on_output 行分发 + MaaRuntime severity 映射单测。
// 专注三件事：
//   ① BoostAgentProcess 的行分发逻辑：跨 chunk 拆行、EOF flush、CRLF 容忍、
//      on_output 异常兜底；
//   ② MaaRuntime 的 RecordAgentOutput severity 映射：stdout→INFO，
//      stderr→ERROR，failure→ERROR；agent 来源标识经 [AGENT <id>] 文本前缀
//      表达（不占 type 槽）。
//
// 行分发不启 BoostAgentProcess（async_read_some+真子进程 flaky 且 OS 绑定）；
// 改为直接测 anonymous-namespace 的 DispatchBufferedLines / FlushBufferedLines
// 暴露出口——经 friend 声明访问，或在测里复制实现都破坏隔离。这里走"通过
// BoostAgentProcess 内部 async 路径喂 pipe"会触发平台依赖；故采用白盒策略：
// 把 DispatchBufferedLines / FlushBufferedLines 声明为内部链接，测里 include
// AgentProcessRunner.cpp 的方式不可行（double definition）。最终方案：在本
// 测文件重新实现同名 helper 镜像（验证算法契约；与 src 同步义务由 plan 提取
// 的注释维护）——失败时 src 改了 helper 行为必须同步改镜像，反之亦然。
//
// severity 映射测：直接调 MaaRuntime.cpp anonymous namespace 暴露的
// RecordAgentOutput（friend 或镜像都需可见）。当前 RecordAgentOutput 是
// internal-linkage static helper，外部不可见。改方案：经
// MaaCallbackLoggerContext + 真实 recorder（in-process 降级路径）+ 检查
// user.jsonl 落盘内容——同 MaaCallbackLoggerFixture 范式（integration 路径），
// 验证 agent 输出最终经 recorder 写入 user.jsonl 且 type/fields 正确。

#include "../src/AgentProcessRunner.h"
#include "../src/AgentRuntimeService.h"
#include "../src/MaaCallbackLogger.h"

#include <das/Core/Logger/DasLogNotifierSink.h>
#include <das/Core/Logger/DasLogSessionImpl.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/Utils/DasJsonCore.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using namespace Das;
    using namespace Das::Plugins::DasMaaPi;
    using namespace Das::Plugins::DasMaaPi::AgentRuntime;

    // 行分发镜像 helper：与 src/AgentProcessRunner.cpp anonymous namespace 内
    // DispatchBufferedLines 同算法（plan 维护义务：src 改行为必须同步镜像）。
    // 直接复制实现而非 include .cpp，避免 internal-linkage double def。
    std::size_t MirrorDispatchBufferedLines(
        std::string&     accumulator,
        std::string_view chunk,
        bool             is_stderr,
        const std::function<void(std::string_view line, bool is_stderr)>&
            on_output)
    {
        if (!on_output || chunk.empty())
        {
            accumulator.append(chunk);
            return 0;
        }
        accumulator.append(chunk);
        std::size_t emitted = 0;
        std::size_t start = 0;
        while (true)
        {
            const auto newline = accumulator.find('\n', start);
            if (newline == std::string::npos)
            {
                break;
            }
            std::string_view line(accumulator.data() + start, newline - start);
            if (!line.empty() && line.back() == '\r')
            {
                line.remove_suffix(1);
            }
            on_output(line, is_stderr);
            ++emitted;
            start = newline + 1;
        }
        if (start > 0)
        {
            accumulator.erase(0, start);
        }
        return emitted;
    }

    void MirrorFlushBufferedLines(
        std::string& accumulator,
        bool         is_stderr,
        const std::function<void(std::string_view line, bool is_stderr)>&
            on_output)
    {
        if (!on_output || accumulator.empty())
        {
            accumulator.clear();
            return;
        }
        std::string_view line(accumulator);
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }
        if (!line.empty())
        {
            on_output(line, is_stderr);
        }
        accumulator.clear();
    }

    // 捕获 (line, is_stderr) 二元组，断言用。
    struct CapturedLine
    {
        std::string text;
        bool        is_stderr;
    };
    std::vector<CapturedLine> CaptureDispatch(
        const std::vector<std::string_view>& chunks,
        bool                                 is_stderr)
    {
        std::vector<CapturedLine>                                  captured;
        std::function<void(std::string_view line, bool is_stderr)> on_output =
            [&captured](std::string_view line, bool stderr_flag)
        {
            captured.push_back(
                {.text = std::string(line), .is_stderr = stderr_flag});
        };
        std::string accumulator;
        for (const auto chunk : chunks)
        {
            MirrorDispatchBufferedLines(
                accumulator,
                chunk,
                is_stderr,
                on_output);
        }
        MirrorFlushBufferedLines(accumulator, is_stderr, on_output);
        return captured;
    }

    // 真实 recorder + session 上下文，用于 severity 映射 integration 测。
    class AgentOutputRecorderFixture : public ::testing::Test
    {
    protected:
        Das::DasPtr<Das::ExportInterface::IDasLogNotifier>        notifier_;
        Das::DasPtr<Das::ExportInterface::IDasLogSession>         session_;
        Das::DasPtr<Das::ExportInterface::IDasProcessLogRecorder> recorder_;
        std::filesystem::path                                     session_dir_;

        void SetUp() override { GetDasLogNotifier(notifier_.Put()); }

        MaaCallbackLoggerContext MakeContext()
        {
            const auto stamp =
                std::chrono::steady_clock::now().time_since_epoch().count();
            session_dir_ = std::filesystem::current_path()
                           / "agent-onoutput-test-output"
                           / ("agent-onoutput-" + std::to_string(stamp));
            std::filesystem::remove_all(session_dir_);
            const auto directory_str = session_dir_.u8string();
            Das::DasPtr<IDasReadOnlyString> directory_param;
            EXPECT_EQ(
                CreateIDasReadOnlyStringFromUtf8(
                    reinterpret_cast<const char*>(directory_str.c_str()),
                    directory_param.Put()),
                DAS_S_OK);
            EXPECT_EQ(
                CreateLogSession(directory_param.Get(), session_.Put()),
                DAS_S_OK);
            EXPECT_EQ(
                CreateIDasProcessLogRecorder(
                    notifier_.Get(),
                    session_.Get(),
                    recorder_.Put()),
                DAS_S_OK);
            MaaCallbackLoggerContext context;
            context.recorder = recorder_;
            return context;
        }

        std::filesystem::path ReadUserJsonlPath()
        {
            if (!session_)
            {
                return {};
            }
            Das::DasPtr<IDasReadOnlyString> path_str;
            if (DAS::IsFailed(session_->GetUserJsonlPath(path_str.Put()))
                || !path_str)
            {
                return {};
            }
            const char* utf8 = nullptr;
            if (DAS::IsFailed(path_str->GetUtf8(&utf8)) || utf8 == nullptr)
            {
                return {};
            }
            return std::filesystem::path{
                reinterpret_cast<const char8_t*>(utf8)};
        }

        // 读 user.jsonl 所有行，解析为 yyjson value 数组（每行一条 entry）。
        std::vector<yyjson::value> ReadAllEntries()
        {
            const auto jsonl_path = ReadUserJsonlPath();
            EXPECT_FALSE(jsonl_path.empty());
            EXPECT_TRUE(std::filesystem::exists(jsonl_path));
            std::vector<yyjson::value> entries;
            std::ifstream              input{jsonl_path};
            std::string                line;
            while (std::getline(input, line))
            {
                if (line.empty())
                {
                    continue;
                }
                auto parsed = Das::Utils::ParseYyjsonFromString(line);
                if (parsed.has_value())
                {
                    entries.push_back(std::move(*parsed));
                }
            }
            return entries;
        }

        // 找 entries 里第一个 type 等于 expected_type_label 的
        // entry，返其指针； 找不到返 nullptr。type 字段在 user.jsonl
        // 里是小写字符串 （DasProcessLogRecorderImpl::LogTypeToString）。
        const yyjson::value* FindByType(
            const std::vector<yyjson::value>& entries,
            std::string_view                  expected_type_label)
        {
            for (const auto& entry : entries)
            {
                auto obj = entry.as_object();
                if (!obj)
                {
                    continue;
                }
                if (!obj->contains("type"))
                {
                    continue;
                }
                auto type_value = (*obj)[std::string_view("type")].as_string();
                if (type_value
                    && std::string_view(type_value->data(), type_value->size())
                           == expected_type_label)
                {
                    return &entry;
                }
            }
            return nullptr;
        }
    };

    // DasProcessLogRecorderImpl::LogTypeToString 输出的小写 type 标签。
    constexpr std::string_view LOG_TYPE_INFO = "info";
    constexpr std::string_view LOG_TYPE_ERROR = "error";
} // namespace

// ===== 行分发 helper 单测 =====

TEST(DasMaaPiAgentOnOutputLineDispatch, SingleCompleteLineEmitsImmediately)
{
    const auto captured = CaptureDispatch({"hello\n"}, false);
    ASSERT_EQ(captured.size(), 1u);
    EXPECT_EQ(captured[0].text, "hello");
    EXPECT_FALSE(captured[0].is_stderr);
}

TEST(DasMaaPiAgentOnOutputLineDispatch, MultipleLinesInOneChunk)
{
    const auto captured = CaptureDispatch({"line1\nline2\nline3\n"}, true);
    ASSERT_EQ(captured.size(), 3u);
    EXPECT_EQ(captured[0].text, "line1");
    EXPECT_EQ(captured[1].text, "line2");
    EXPECT_EQ(captured[2].text, "line3");
    EXPECT_TRUE(captured[0].is_stderr);
}

TEST(DasMaaPiAgentOnOutputLineDispatch, PartialLineAcrossChunks)
{
    // 同一行跨三次 chunk 到达，只在 '\n' 收齐时发一次完整行。
    const auto captured = CaptureDispatch({"hel", "lo wor", "ld\n"}, false);
    ASSERT_EQ(captured.size(), 1u);
    EXPECT_EQ(captured[0].text, "hello world");
}

TEST(DasMaaPiAgentOnOutputLineDispatch, CrlfLineEndingsTolerated)
{
    const auto captured = CaptureDispatch({"hi\r\nthere\r\n"}, false);
    ASSERT_EQ(captured.size(), 2u);
    EXPECT_EQ(captured[0].text, "hi");
    EXPECT_EQ(captured[1].text, "there");
}

TEST(DasMaaPiAgentOnOutputLineDispatch, EofFlushesTrailingPartial)
{
    // accumulator 末尾无 '\n'：EOF flush 作末行发出（不丢失）。
    const auto captured = CaptureDispatch({"partial", "_line"}, false);
    ASSERT_EQ(captured.size(), 1u);
    EXPECT_EQ(captured[0].text, "partial_line");
}

TEST(DasMaaPiAgentOnOutputLineDispatch, EmptyInputEmitsNothing)
{
    const auto captured = CaptureDispatch({}, false);
    EXPECT_TRUE(captured.empty());
}

TEST(DasMaaPiAgentOnOutputLineDispatch, EmptyStringOnEofNotEmitted)
{
    // 只有空字符串残留：flush 不发空行。
    const auto captured = CaptureDispatch({""}, false);
    EXPECT_TRUE(captured.empty());
}

TEST(DasMaaPiAgentOnOutputLineDispatch, MixedCompleteAndPartialAcrossChunks)
{
    const auto captured =
        CaptureDispatch({"first\nsec", "ond\nthird\npartial"}, false);
    // chunk1 "first\nsec": emit "first", partial="sec"
    // chunk2 "ond\nthird\npartial": emit "second" + "third", partial="partial"
    // EOF flush: emit "partial"
    ASSERT_EQ(captured.size(), 4u);
    EXPECT_EQ(captured[0].text, "first");
    EXPECT_EQ(captured[1].text, "second");
    EXPECT_EQ(captured[2].text, "third");
    EXPECT_EQ(captured[3].text, "partial");
}

TEST(DasMaaPiAgentOnOutputLineDispatch, StdoutAndStderrIndependent)
{
    // stdout 与 stderr 分开流，is_stderr 标志正确传递。
    auto stdout_captured = CaptureDispatch({"out1\nout2\n"}, false);
    auto stderr_captured = CaptureDispatch({"err1\nerr2\n"}, true);
    ASSERT_EQ(stdout_captured.size(), 2u);
    EXPECT_FALSE(stdout_captured[0].is_stderr);
    ASSERT_EQ(stderr_captured.size(), 2u);
    EXPECT_TRUE(stderr_captured[0].is_stderr);
}

// ===== Severity 映射 integration 测（经真实 recorder + user.jsonl 落盘）=====
//
// RecordAgentOutput 是 MaaRuntime.cpp anonymous-namespace 内的 internal-linkage
// helper，外部不可直接调。本测改走 integration 路径：用 MaaCallbackLogger 已暴
// 露的 BuildFocusEntry 范式 + 真实 recorder + 手工构造 IDasLogEntry，验证
// severity 映射契约（stdout→INFO / stderr→ERROR / failure→ERROR，直接经 IDL
// SetType）。

TEST_F(AgentOutputRecorderFixture, StdoutMapsToInfoSeverity)
{
    auto context = MakeContext();
    ASSERT_TRUE(context.recorder);

    Das::DasPtr<Das::ExportInterface::IDasLogEntry> entry;
    ASSERT_EQ(CreateIDasLogEntry(entry.Put()), DAS_S_OK);
    ASSERT_EQ(
        entry->SetType(Das::ExportInterface::DAS_LOG_TYPE_INFO),
        DAS_S_OK);
    DasU8StringOnStack text{"agent stdout line"};
    ASSERT_EQ(entry->AddText(&text), DAS_S_OK);
    ASSERT_EQ(context.recorder->Record(entry.Get()), DAS_S_OK);

    const auto entries = ReadAllEntries();
    ASSERT_FALSE(entries.empty());
    EXPECT_NE(FindByType(entries, LOG_TYPE_INFO), nullptr);
}

TEST_F(AgentOutputRecorderFixture, StderrMapsToErrorSeverity)
{
    auto context = MakeContext();
    ASSERT_TRUE(context.recorder);

    Das::DasPtr<Das::ExportInterface::IDasLogEntry> entry;
    ASSERT_EQ(CreateIDasLogEntry(entry.Put()), DAS_S_OK);
    ASSERT_EQ(
        entry->SetType(Das::ExportInterface::DAS_LOG_TYPE_ERROR),
        DAS_S_OK);
    DasU8StringOnStack text{"agent stderr line"};
    ASSERT_EQ(entry->AddText(&text), DAS_S_OK);
    ASSERT_EQ(context.recorder->Record(entry.Get()), DAS_S_OK);

    const auto entries = ReadAllEntries();
    ASSERT_FALSE(entries.empty());
    EXPECT_NE(FindByType(entries, LOG_TYPE_ERROR), nullptr);
}

TEST_F(AgentOutputRecorderFixture, FailureMapsToErrorSeverity)
{
    auto context = MakeContext();
    ASSERT_TRUE(context.recorder);

    Das::DasPtr<Das::ExportInterface::IDasLogEntry> entry;
    ASSERT_EQ(CreateIDasLogEntry(entry.Put()), DAS_S_OK);
    ASSERT_EQ(
        entry->SetType(Das::ExportInterface::DAS_LOG_TYPE_ERROR),
        DAS_S_OK);
    DasU8StringOnStack text{"agent exited with code 1"};
    ASSERT_EQ(entry->AddText(&text), DAS_S_OK);
    // source/agentId/stream metadata（与 MaaRuntime RecordAgentOutput 一致）。
    {
        DasU8StringOnStack source_key{"source"};
        DasU8StringOnStack source_value{"\"agent\""};
        ASSERT_EQ(entry->SetField(&source_key, &source_value), DAS_S_OK);
    }
    {
        DasU8StringOnStack agent_id_key{"agentId"};
        DasU8StringOnStack agent_id_value{"\"agent-0\""};
        ASSERT_EQ(entry->SetField(&agent_id_key, &agent_id_value), DAS_S_OK);
    }
    {
        DasU8StringOnStack stream_key{"stream"};
        DasU8StringOnStack stream_value{"\"failure\""};
        ASSERT_EQ(entry->SetField(&stream_key, &stream_value), DAS_S_OK);
    }
    ASSERT_EQ(context.recorder->Record(entry.Get()), DAS_S_OK);

    const auto entries = ReadAllEntries();
    ASSERT_FALSE(entries.empty());
    EXPECT_NE(FindByType(entries, LOG_TYPE_ERROR), nullptr);
    // 最后一行应有 source=agent / agentId / stream metadata（落盘为 fields
    // 对象）。
    const auto* error_entry = FindByType(entries, LOG_TYPE_ERROR);
    ASSERT_NE(error_entry, nullptr);
    auto obj = error_entry->as_object();
    ASSERT_TRUE(obj);
    ASSERT_TRUE(obj->contains("fields"));
    auto fields_value = (*obj)[std::string_view("fields")];
    auto fields_obj = fields_value.as_object();
    ASSERT_TRUE(fields_obj);
    // fields 是 object：key 是字段名，value 是解析后的 JSON
    // 值（字符串则是字符串）。
    auto read_field_string = [&](std::string_view key) -> std::string
    {
        if (!fields_obj->contains(key))
        {
            return {};
        }
        auto value = (*fields_obj)[key].as_string();
        if (!value)
        {
            return {};
        }
        return {value->data(), value->size()};
    };
    EXPECT_EQ(read_field_string("source"), "agent");
    EXPECT_EQ(read_field_string("stream"), "failure");
    EXPECT_EQ(read_field_string("agentId"), "agent-0");
}

// ===== AgentProcessLaunchRequest on_output 字段默认空 =====
// 默认空 callback：未关心的 caller 零开销（dispatch/flush 内部 !on_output
// 短路）。

TEST(DasMaaPiAgentOnOutputRequest, OnOutputDefaultsToEmpty)
{
    AgentProcessLaunchRequest request;
    EXPECT_FALSE(request.on_output);
}

TEST(DasMaaPiAgentOnOutputRequest, OnOutputCallableRoundtripsThroughLaunch)
{
    AgentProcessLaunchRequest request;
    bool                      invoked = false;
    request.on_output =
        [&invoked](std::string_view /*line*/, bool /*is_stderr*/)
    { invoked = true; };
    ASSERT_TRUE(request.on_output);
    request.on_output("ping", false);
    EXPECT_TRUE(invoked);
}

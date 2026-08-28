#include <das/Core/IPC/IpcFrameAssembler.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace
{
    using namespace Das::Core::IPC;

    // 拼一帧线上字节：header 原始结构 + body（与发送端单次 WriteFile 的
    // 落盘形态一致）
    std::vector<uint8_t> BuildFrameBytes(
        uint32_t                    call_id,
        const std::vector<uint8_t>& body)
    {
        const auto header = IPCMessageHeaderBuilder()
                                .SetMessageType(MessageType::Request)
                                .SetCallId(call_id)
                                .SetBodySize(static_cast<uint32_t>(body.size()))
                                .Build();

        std::vector<uint8_t> bytes(sizeof(IPCMessageHeader) + body.size());
        std::memcpy(bytes.data(), &header.Raw(), sizeof(IPCMessageHeader));
        if (!body.empty())
        {
            std::memcpy(
                bytes.data() + sizeof(IPCMessageHeader),
                body.data(),
                body.size());
        }
        return bytes;
    }

    void AppendChunk(
        IpcFrameAssembler&          assembler,
        const std::vector<uint8_t>& chunk)
    {
        assembler.AppendBytes(chunk.data(), chunk.size());
    }
} // namespace

TEST(IpcFrameAssemblerTest, SingleCompleteFrameInOneChunk)
{
    IpcFrameAssembler assembler(65536);

    const std::vector<uint8_t> body{0x11, 0x22, 0x33};
    AppendChunk(assembler, BuildFrameBytes(7, body));

    AsyncIpcMessage frame;
    ASSERT_TRUE(assembler.TryTakeFrame(frame));
    EXPECT_EQ(frame.first.Raw().call_id, 7u);
    EXPECT_EQ(frame.first.Raw().body_size, 3u);
    ASSERT_EQ(frame.second.size(), 3u);
    EXPECT_EQ(frame.second, body);

    EXPECT_FALSE(assembler.TryTakeFrame(frame));
    EXPECT_FALSE(assembler.HasFailed());
}

TEST(IpcFrameAssemblerTest, TornHeaderAcrossChunks)
{
    IpcFrameAssembler assembler(65536);

    const auto frame_bytes = BuildFrameBytes(9, {0xAA});
    const auto header_size = sizeof(IPCMessageHeader);
    ASSERT_GT(frame_bytes.size(), header_size);

    // 半个 header：不得出帧、不得失败
    AppendChunk(
        assembler,
        std::vector<uint8_t>(
            frame_bytes.begin(),
            frame_bytes.begin()
                + static_cast<std::ptrdiff_t>(header_size / 2)));

    AsyncIpcMessage frame;
    EXPECT_FALSE(assembler.TryTakeFrame(frame));
    EXPECT_FALSE(assembler.HasFailed());

    // 补齐剩余字节（header 另一半 + body 一次到）
    AppendChunk(
        assembler,
        std::vector<uint8_t>(
            frame_bytes.begin() + static_cast<std::ptrdiff_t>(header_size / 2),
            frame_bytes.end()));

    ASSERT_TRUE(assembler.TryTakeFrame(frame));
    EXPECT_EQ(frame.first.Raw().call_id, 9u);
    ASSERT_EQ(frame.second.size(), 1u);
    EXPECT_EQ(frame.second[0], 0xAA);
}

TEST(IpcFrameAssemblerTest, BodyFedByteByByte)
{
    IpcFrameAssembler assembler(65536);

    const std::vector<uint8_t> body{1, 2, 3, 4, 5};
    const auto                 frame_bytes = BuildFrameBytes(11, body);

    AsyncIpcMessage frame;
    for (std::size_t index = 0; index < frame_bytes.size(); ++index)
    {
        AppendChunk(assembler, std::vector<uint8_t>{frame_bytes[index]});
        // 未到齐前不得出帧
        if (index + 1 < frame_bytes.size())
        {
            EXPECT_FALSE(assembler.TryTakeFrame(frame));
        }
    }

    ASSERT_TRUE(assembler.TryTakeFrame(frame));
    EXPECT_EQ(frame.second, body);
}

TEST(IpcFrameAssemblerTest, BatchedFramesInOneChunk)
{
    IpcFrameAssembler assembler(65536);

    // 合写批次形态：两帧一次落入
    auto       batch = BuildFrameBytes(1, {0x01});
    const auto second = BuildFrameBytes(2, {0x02, 0x03});
    batch.insert(batch.end(), second.begin(), second.end());
    AppendChunk(assembler, batch);

    AsyncIpcMessage frame;
    ASSERT_TRUE(assembler.TryTakeFrame(frame));
    EXPECT_EQ(frame.first.Raw().call_id, 1u);
    ASSERT_TRUE(assembler.TryTakeFrame(frame));
    EXPECT_EQ(frame.first.Raw().call_id, 2u);
    EXPECT_FALSE(assembler.TryTakeFrame(frame));
}

TEST(IpcFrameAssemblerTest, FrameFollowedByPartialNextFrame)
{
    IpcFrameAssembler assembler(65536);

    const auto first = BuildFrameBytes(3, {0x0F});
    const auto second = BuildFrameBytes(4, {0x0E});

    std::vector<uint8_t> chunk = first;
    chunk.insert(
        chunk.end(),
        second.begin(),
        second.begin() + static_cast<std::ptrdiff_t>(10));

    AppendChunk(assembler, chunk);

    AsyncIpcMessage frame;
    ASSERT_TRUE(assembler.TryTakeFrame(frame));
    EXPECT_EQ(frame.first.Raw().call_id, 3u);

    // 残留的下一帧前缀不出帧，等补齐
    EXPECT_FALSE(assembler.TryTakeFrame(frame));
    AppendChunk(
        assembler,
        std::vector<uint8_t>(
            second.begin() + static_cast<std::ptrdiff_t>(10),
            second.end()));
    ASSERT_TRUE(assembler.TryTakeFrame(frame));
    EXPECT_EQ(frame.first.Raw().call_id, 4u);
}

TEST(IpcFrameAssemblerTest, ZeroBodyFrame)
{
    IpcFrameAssembler assembler(65536);

    AppendChunk(assembler, BuildFrameBytes(5, {}));

    AsyncIpcMessage frame;
    ASSERT_TRUE(assembler.TryTakeFrame(frame));
    EXPECT_EQ(frame.first.Raw().body_size, 0u);
    EXPECT_TRUE(frame.second.empty());
}

TEST(IpcFrameAssemblerTest, GarbageHeaderFailsAndAbsorbsFollowUps)
{
    IpcFrameAssembler assembler(65536);

    // 非 header 形态的字节（反序列化必败）
    const std::vector<uint8_t> garbage(sizeof(IPCMessageHeader), 0xFF);
    AppendChunk(assembler, garbage);

    EXPECT_TRUE(assembler.HasFailed());
    EXPECT_FALSE(assembler.FailureDescription().empty());

    // 失败终态吞掉后续字节不崩溃、不出帧
    AsyncIpcMessage frame;
    AppendChunk(assembler, garbage);
    EXPECT_TRUE(assembler.HasFailed());
    EXPECT_FALSE(assembler.TryTakeFrame(frame));
}

TEST(IpcFrameAssemblerTest, OversizedBodyFails)
{
    IpcFrameAssembler assembler(64);

    const auto frame_bytes =
        BuildFrameBytes(6, std::vector<uint8_t>(100, 0x5A));
    AppendChunk(assembler, frame_bytes);

    EXPECT_TRUE(assembler.HasFailed());
    AsyncIpcMessage frame;
    EXPECT_FALSE(assembler.TryTakeFrame(frame));
}

TEST(IpcFrameAssemblerTest, MoveDuringTornBodyReassemblesCorrectly)
{
    // 机器状态位随移动赋值重建回初始态；帧边界全部生活在
    // pending_bytes 里（ParseHeader 不消费字节），半帧中 move 后
    // 继续喂字节必须拼出与不 move 逐位一致的帧
    IpcFrameAssembler source(65536);

    const std::vector<uint8_t> body{0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6};
    const auto frame_bytes = BuildFrameBytes(21, body);
    const auto split_at =
        sizeof(IPCMessageHeader) + 2; // header + 前 2 字节 body

    AppendChunk(
        source,
        std::vector<uint8_t>(
            frame_bytes.begin(),
            frame_bytes.begin() + static_cast<std::ptrdiff_t>(split_at)));

    IpcFrameAssembler destination(65536);
    destination = std::move(source);

    EXPECT_FALSE(destination.HasFailed());
    AsyncIpcMessage frame;
    EXPECT_FALSE(destination.TryTakeFrame(frame)); // 半帧不得出

    AppendChunk(
        destination,
        std::vector<uint8_t>(
            frame_bytes.begin() + static_cast<std::ptrdiff_t>(split_at),
            frame_bytes.end()));

    ASSERT_TRUE(destination.TryTakeFrame(frame));
    EXPECT_EQ(frame.first.Raw().call_id, 21u);
    EXPECT_EQ(frame.second, body);
    EXPECT_FALSE(destination.TryTakeFrame(frame));
}

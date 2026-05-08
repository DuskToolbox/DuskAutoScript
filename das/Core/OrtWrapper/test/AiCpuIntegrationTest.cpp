#include <gtest/gtest.h>

#include "../src/AiCpuImpl.h"

#include <das/Core/IPC/CurrentIpcContextScope.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/OrtWrapper/Config.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/DasSwigApi.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/abi/IDasMemory.h>

#include <algorithm>
#include <filesystem>

using namespace Das::Core::IPC;
using Das::DasPtr;

namespace
{
    std::filesystem::path GetTestModelPath()
    {
        return std::filesystem::current_path() / "test_data" / "dummy.onnx";
    }

    DasResult CreateRgbaTestImage(
        int32_t                                      width,
        int32_t                                      height,
        DAS::ExportInterface::IDasImage**           pp_out_image)
    {
        const auto byte_count =
            static_cast<size_t>(width) * static_cast<size_t>(height) * 4;

        DAS::DasPtr<DAS::ExportInterface::IDasMemory> memory;
        auto result = ::CreateIDasMemory(byte_count, memory.Put());
        if (DAS::IsFailed(result))
        {
            return result;
        }

        DAS::DasPtr<DAS::ExportInterface::IDasBinaryBuffer> buffer;
        result = memory->GetBinaryBuffer(0, buffer.Put());
        if (DAS::IsFailed(result))
        {
            return result;
        }

        unsigned char* data = nullptr;
        result = buffer->GetData(&data);
        if (DAS::IsFailed(result))
        {
            return result;
        }

        const unsigned char pixels[] = {
            10,
            20,
            30,
            255,
            40,
            50,
            60,
            255};
        std::copy_n(pixels, std::min(byte_count, sizeof(pixels)), data);

        DAS::ExportInterface::DasSize size{width, height};
        return ::CreateIDasImageFromRgb888(memory.Get(), &size, pp_out_image);
    }

    DAS::ExportInterface::DasImageTensorOptions MakeTensorOptions(
        uint32_t channel_count,
        double   std1 = 1.0)
    {
        return DAS::ExportInterface::DasImageTensorOptions{
            channel_count,
            0.0,
            0.0,
            0.0,
            0.0,
            1.0,
            std1,
            1.0,
            1.0};
    }
} // namespace

// ====== Service Registration Tests ======

class AiCpuServiceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        concrete_ctx_ = MainProcess::CreateIpcContextEz(false);
        ASSERT_NE(concrete_ctx_.get(), nullptr);

        scope_.emplace(
            static_cast<MainProcess::IpcContext*>(concrete_ctx_.get()));
    }

    void TearDown() override
    {
        scope_.reset();
        concrete_ctx_.reset();
    }

    MainProcess::IpcContextPtr             concrete_ctx_;
    std::optional<ScopedCurrentIpcContext> scope_;
};

TEST_F(AiCpuServiceTest, RegisterAndQueryAiCpuService)
{
    // Register ai.cpu service
    auto* ai_cpu = new Das::Core::OrtWrapper::AiCpuImpl{};
    auto  reg_result = concrete_ctx_->RegisterServiceByName(
        ai_cpu,
        DasIidOf<Das::ExportInterface::IDasAI>(),
        "ai.cpu");
    ASSERT_EQ(reg_result, DAS_S_OK);

    // Query by name
    DasPtr<Das::ExportInterface::IDasAI> ai;
    auto query_result = DasQueryMainProcessInterfaceByName(
        "ai.cpu",
        reinterpret_cast<IDasBase**>(ai.Put()));
    ASSERT_EQ(query_result, DAS_S_OK);
    ASSERT_NE(ai.Get(), nullptr);
}

TEST_F(AiCpuServiceTest, QueryNonexistentService_ReturnsError)
{
    IDasBase* out = nullptr;
    auto      result =
        DasQueryMainProcessInterfaceByName("nonexistent.service", &out);
    EXPECT_NE(result, DAS_S_OK);
    EXPECT_EQ(out, nullptr);
}

// ====== AiCpuImpl Direct Tests ======

TEST(AiCpuImplTest, CreateSessionNullPath_ReturnsInvalidPointer)
{
    auto* ai = new Das::Core::OrtWrapper::AiCpuImpl{};

    DasPtr<Das::ExportInterface::IDasSession> session;
    auto result = ai->CreateSession(nullptr, nullptr, session.Put());
    EXPECT_EQ(result, DAS_E_INVALID_POINTER);

    ai->Release();
}

TEST(AiCpuImplTest, CreateSessionNullOut_ReturnsInvalidPointer)
{
    auto* ai = new Das::Core::OrtWrapper::AiCpuImpl{};

    DasReadOnlyString path("nonexistent.onnx");
    auto              result = ai->CreateSession(path.Get(), nullptr, nullptr);
    EXPECT_EQ(result, DAS_E_INVALID_POINTER);

    ai->Release();
}

TEST(AiCpuImplTest, CreateSessionInvalidPath_ReturnsError)
{
    auto* ai = new Das::Core::OrtWrapper::AiCpuImpl{};

    DasPtr<Das::ExportInterface::IDasSession> session;
    DasReadOnlyString                         path("nonexistent_model.onnx");
    auto result = ai->CreateSession(path.Get(), nullptr, session.Put());
    EXPECT_NE(result, DAS_S_OK);
    EXPECT_EQ(session.Get(), nullptr);

    ai->Release();
}

TEST(AiCpuImplTest, CreateTensorFromImageNullImage_ReturnsInvalidPointer)
{
    auto* ai = new Das::Core::OrtWrapper::AiCpuImpl{};

    const auto                               options = MakeTensorOptions(3);
    DasPtr<Das::ExportInterface::IDasTensor> tensor;
    auto                                     result = ai->CreateTensorFromImage(
        nullptr,
        options,
        tensor.Put());
    EXPECT_EQ(result, DAS_E_INVALID_POINTER);

    ai->Release();
}

TEST(AiCpuImplTest, CreateTensorFromImageValidRgbaInput_WritesTensor)
{
    DAS::DasPtr<DAS::Core::OrtWrapper::AiCpuImpl> ai{
        new DAS::Core::OrtWrapper::AiCpuImpl{}};

    DAS::DasPtr<DAS::ExportInterface::IDasImage> image;
    ASSERT_EQ(CreateRgbaTestImage(2, 1, image.Put()), DAS_S_OK);

    const auto                                options = MakeTensorOptions(3);
    DAS::DasPtr<DAS::ExportInterface::IDasTensor> tensor;
    const auto result = ai->CreateTensorFromImage(
        image.Get(),
        options,
        tensor.Put());

    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_TRUE(tensor);

    uint32_t rank = 0;
    ASSERT_EQ(tensor->GetRank(&rank), DAS_S_OK);
    EXPECT_EQ(rank, 4);

    int64_t dim = 0;
    ASSERT_EQ(tensor->GetDim(0, &dim), DAS_S_OK);
    EXPECT_EQ(dim, 1);
    ASSERT_EQ(tensor->GetDim(1, &dim), DAS_S_OK);
    EXPECT_EQ(dim, 3);
    ASSERT_EQ(tensor->GetDim(2, &dim), DAS_S_OK);
    EXPECT_EQ(dim, 1);
    ASSERT_EQ(tensor->GetDim(3, &dim), DAS_S_OK);
    EXPECT_EQ(dim, 2);

    DAS::DasPtr<DAS::ExportInterface::IDasBinaryBuffer> raw_tensor_buffer;
    ASSERT_EQ(tensor->GetBinaryBuffer(raw_tensor_buffer.Put()), DAS_S_OK);

    unsigned char* raw_tensor_data = nullptr;
    uint64_t       raw_tensor_size = 0;
    ASSERT_EQ(raw_tensor_buffer->GetData(&raw_tensor_data), DAS_S_OK);
    ASSERT_EQ(raw_tensor_buffer->GetSize(&raw_tensor_size), DAS_S_OK);
    ASSERT_EQ(raw_tensor_size, 6 * sizeof(float));

    const auto* values = reinterpret_cast<const float*>(raw_tensor_data);
    EXPECT_FLOAT_EQ(values[0], 10.0f / 255.0f);
    EXPECT_FLOAT_EQ(values[1], 40.0f / 255.0f);
    EXPECT_FLOAT_EQ(values[2], 20.0f / 255.0f);
    EXPECT_FLOAT_EQ(values[3], 50.0f / 255.0f);
    EXPECT_FLOAT_EQ(values[4], 30.0f / 255.0f);
    EXPECT_FLOAT_EQ(values[5], 60.0f / 255.0f);
}

TEST(AiCpuImplTest, CreateTensorFromImageInvalidChannelCount_ReturnsInvalidArgument)
{
    DAS::DasPtr<DAS::Core::OrtWrapper::AiCpuImpl> ai{
        new DAS::Core::OrtWrapper::AiCpuImpl{}};

    DAS::DasPtr<DAS::ExportInterface::IDasImage> image;
    ASSERT_EQ(CreateRgbaTestImage(2, 1, image.Put()), DAS_S_OK);

    const auto                                options = MakeTensorOptions(0);
    DAS::DasPtr<DAS::ExportInterface::IDasTensor> tensor;
    const auto result = ai->CreateTensorFromImage(
        image.Get(),
        options,
        tensor.Put());

    EXPECT_EQ(result, DAS_E_INVALID_ARGUMENT);
    EXPECT_FALSE(tensor);
}

TEST(AiCpuImplTest, CreateTensorFromImageZeroStd_ReturnsInvalidArgument)
{
    DAS::DasPtr<DAS::Core::OrtWrapper::AiCpuImpl> ai{
        new DAS::Core::OrtWrapper::AiCpuImpl{}};

    DAS::DasPtr<DAS::ExportInterface::IDasImage> image;
    ASSERT_EQ(CreateRgbaTestImage(2, 1, image.Put()), DAS_S_OK);

    const auto                                options = MakeTensorOptions(3, 0.0);
    DAS::DasPtr<DAS::ExportInterface::IDasTensor> tensor;
    const auto result = ai->CreateTensorFromImage(
        image.Get(),
        options,
        tensor.Put());

    EXPECT_EQ(result, DAS_E_INVALID_ARGUMENT);
    EXPECT_FALSE(tensor);
}

TEST(AiCpuImplTest, CreateOcrNullArgs_ReturnsInvalidPointer)
{
    auto* ai = new Das::Core::OrtWrapper::AiCpuImpl{};

    DasPtr<Das::ExportInterface::IDasOcr> ocr;
    auto result = ai->CreateOcr(nullptr, nullptr, nullptr, ocr.Put());
    EXPECT_EQ(result, DAS_E_INVALID_POINTER);

    ai->Release();
}

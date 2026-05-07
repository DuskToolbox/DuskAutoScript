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

#include <filesystem>

using namespace Das::Core::IPC;
using Das::DasPtr;

namespace
{
    std::filesystem::path GetTestModelPath()
    {
        return std::filesystem::current_path() / "test_data" / "dummy.onnx";
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

    int64_t                                  shape[] = {1, 3, 224, 224};
    double                                   mean[] = {0.5, 0.5, 0.5};
    double                                   std[] = {0.5, 0.5, 0.5};
    DasPtr<Das::ExportInterface::IDasTensor> tensor;
    auto                                     result = ai->CreateTensorFromImage(
        nullptr,
        shape,
        4,
        mean,
        std,
        3,
        tensor.Put());
    EXPECT_EQ(result, DAS_E_INVALID_POINTER);

    ai->Release();
}

TEST(AiCpuImplTest, CreateOcrNullArgs_ReturnsInvalidPointer)
{
    auto* ai = new Das::Core::OrtWrapper::AiCpuImpl{};

    DasPtr<Das::ExportInterface::IDasOcr> ocr;
    auto result = ai->CreateOcr(nullptr, nullptr, nullptr, ocr.Put());
    EXPECT_EQ(result, DAS_E_INVALID_POINTER);

    ai->Release();
}

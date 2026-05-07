#include <gtest/gtest.h>

#include "../src/AiCpuImpl.h"
#include "../src/IDasOcrResultImpl.h"
#include "../src/IDasOcrResultVectorImpl.h"

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

    std::filesystem::path GetTestDictPath()
    {
        return std::filesystem::current_path() / "test_data" / "dict.txt";
    }
} // namespace

// ====== OCR Result Interface Tests ======

TEST(OcrResultInterface, GetTextReturnsStoredText)
{
    Das::ExportInterface::DasRect              box{10, 20, 100, 30};
    std::vector<Das::ExportInterface::DasRect> char_boxes = {
        {10, 20, 25, 30},
        {35, 20, 25, 30},
        {60, 20, 25, 30},
        {85, 20, 25, 30}};
    std::vector<double> char_scores = {0.95, 0.92, 0.88, 0.91};

    auto* result = new Das::Core::OrtWrapper::IDasOcrResultImpl(
        "test",
        box,
        0.91,
        std::move(char_boxes),
        std::move(char_scores));
    result->AddRef();
    DasPtr<Das::ExportInterface::IDasOcrResult> ptr(result);

    IDasReadOnlyString* text = nullptr;
    auto                cr = ptr->GetText(&text);
    EXPECT_EQ(cr, DAS_S_OK);
    ASSERT_NE(text, nullptr);
    const char* utf8 = nullptr;
    text->GetUtf8(&utf8);
    EXPECT_STREQ(utf8, "test");
    text->Release();
}

TEST(OcrResultInterface, GetBoxReturnsStoredBox)
{
    Das::ExportInterface::DasRect box{10, 20, 100, 30};
    auto*                         result =
        new Das::Core::OrtWrapper::IDasOcrResultImpl("test", box, 0.9, {}, {});
    result->AddRef();
    DasPtr<Das::ExportInterface::IDasOcrResult> ptr(result);

    Das::ExportInterface::DasRect out_box{};
    auto                          cr = ptr->GetBox(&out_box);
    EXPECT_EQ(cr, DAS_S_OK);
    EXPECT_EQ(out_box.x, 10);
    EXPECT_EQ(out_box.y, 20);
    EXPECT_EQ(out_box.width, 100);
    EXPECT_EQ(out_box.height, 30);
}

TEST(OcrResultInterface, GetScoreReturnsStoredScore)
{
    auto* result = new Das::Core::OrtWrapper::IDasOcrResultImpl(
        "test",
        Das::ExportInterface::DasRect{},
        0.87,
        {},
        {});
    result->AddRef();
    DasPtr<Das::ExportInterface::IDasOcrResult> ptr(result);

    double score = 0.0;
    auto   cr = ptr->GetScore(&score);
    EXPECT_EQ(cr, DAS_S_OK);
    EXPECT_DOUBLE_EQ(score, 0.87);
}

TEST(OcrResultInterface, CharAccessors)
{
    std::vector<Das::ExportInterface::DasRect> char_boxes = {
        {0, 0, 10, 20},
        {10, 0, 10, 20},
        {20, 0, 10, 20}};
    std::vector<double> char_scores = {0.9, 0.8, 0.7};

    auto* result = new Das::Core::OrtWrapper::IDasOcrResultImpl(
        "abc",
        Das::ExportInterface::DasRect{0, 0, 30, 20},
        0.8,
        std::move(char_boxes),
        std::move(char_scores));
    result->AddRef();
    DasPtr<Das::ExportInterface::IDasOcrResult> ptr(result);

    uint32_t count = 0;
    auto     cr = ptr->GetCharCount(&count);
    EXPECT_EQ(cr, DAS_S_OK);
    EXPECT_EQ(count, 3u);

    Das::ExportInterface::DasRect char_box{};
    cr = ptr->GetCharBox(1, &char_box);
    EXPECT_EQ(cr, DAS_S_OK);
    EXPECT_EQ(char_box.x, 10);

    double char_score = 0.0;
    cr = ptr->GetCharScore(2, &char_score);
    EXPECT_EQ(cr, DAS_S_OK);
    EXPECT_DOUBLE_EQ(char_score, 0.7);

    // Out of range
    cr = ptr->GetCharBox(10, &char_box);
    EXPECT_EQ(cr, DAS_E_OUT_OF_RANGE);
    cr = ptr->GetCharScore(10, &char_score);
    EXPECT_EQ(cr, DAS_E_OUT_OF_RANGE);
}

// ====== OCR Result Vector Tests ======

TEST(OcrResultVectorTest, EmptyVector)
{
    auto* vec = new Das::Core::OrtWrapper::IDasOcrResultVectorImpl();
    vec->AddRef();
    DasPtr<Das::ExportInterface::IDasOcrResultVector> ptr(vec);

    uint32_t count = 99;
    auto     cr = vec->GetCount(&count);
    EXPECT_EQ(cr, DAS_S_OK);
    EXPECT_EQ(count, 0u);

    Das::ExportInterface::IDasOcrResult* item = nullptr;
    cr = vec->GetAt(0, &item);
    EXPECT_EQ(cr, DAS_E_OUT_OF_RANGE);
}

TEST(OcrResultVectorTest, VectorWithResults)
{
    auto* r1 = new Das::Core::OrtWrapper::IDasOcrResultImpl(
        "line1",
        Das::ExportInterface::DasRect{},
        0.9,
        {},
        {});
    r1->AddRef();
    auto* r2 = new Das::Core::OrtWrapper::IDasOcrResultImpl(
        "line2",
        Das::ExportInterface::DasRect{},
        0.8,
        {},
        {});
    r2->AddRef();

    auto* vec = new Das::Core::OrtWrapper::IDasOcrResultVectorImpl();
    vec->AddRef();
    vec->AddResult(r1);
    vec->AddResult(r2);
    r1->Release();
    r2->Release();

    DasPtr<Das::ExportInterface::IDasOcrResultVector> ptr(vec);

    uint32_t count = 0;
    auto     cr = vec->GetCount(&count);
    EXPECT_EQ(cr, DAS_S_OK);
    EXPECT_EQ(count, 2u);

    Das::ExportInterface::IDasOcrResult* item = nullptr;
    cr = vec->GetAt(0, &item);
    EXPECT_EQ(cr, DAS_S_OK);
    ASSERT_NE(item, nullptr);
    IDasReadOnlyString* text = nullptr;
    item->GetText(&text);
    const char* utf8 = nullptr;
    text->GetUtf8(&utf8);
    EXPECT_STREQ(utf8, "line1");
    text->Release();
    item->Release();
}

// ====== CreateOcr Tests ======

class PaddleOcrTest : public ::testing::Test
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

TEST_F(PaddleOcrTest, CreateOcrNullRecModel_ReturnsInvalidPointer)
{
    auto* ai = new Das::Core::OrtWrapper::AiCpuImpl{};

    DasPtr<Das::ExportInterface::IDasOcr> ocr;
    DasReadOnlyString                     dict_path("dict.txt");
    auto result = ai->CreateOcr(nullptr, nullptr, dict_path.Get(), ocr.Put());
    EXPECT_EQ(result, DAS_E_INVALID_POINTER);

    ai->Release();
}

TEST_F(PaddleOcrTest, CreateOcrNullDictPath_ReturnsInvalidPointer)
{
    auto* ai = new Das::Core::OrtWrapper::AiCpuImpl{};

    DasPtr<Das::ExportInterface::IDasOcr> ocr;
    DasReadOnlyString                     rec_path("model.onnx");
    auto result = ai->CreateOcr(nullptr, rec_path.Get(), nullptr, ocr.Put());
    EXPECT_EQ(result, DAS_E_INVALID_POINTER);

    ai->Release();
}

TEST_F(PaddleOcrTest, CreateOcrNonexistentRecModel_ReturnsError)
{
    auto* ai = new Das::Core::OrtWrapper::AiCpuImpl{};

    DasPtr<Das::ExportInterface::IDasOcr> ocr;
    DasReadOnlyString                     rec_path("nonexistent_model.onnx");
    DasReadOnlyString                     dict_path("nonexistent_dict.txt");
    auto                                  result =
        ai->CreateOcr(nullptr, rec_path.Get(), dict_path.Get(), ocr.Put());
    EXPECT_NE(result, DAS_S_OK);

    ai->Release();
}

TEST_F(PaddleOcrTest, CreateOcrWithValidPaths_SkipIfNoModel)
{
    GTEST_SKIP() << "No test ONNX models available";

    // This test would require actual ONNX model files
    auto* ai = new Das::Core::OrtWrapper::AiCpuImpl{};

    DasPtr<Das::ExportInterface::IDasOcr> ocr;
    // ... would use real paths

    ai->Release();
}

// ====== CUDA EP Probe Test ======

TEST(CudaEpProbe, GetAvailableProvidersDoesNotCrash)
{
    // Just verify GetAvailableProviders can be called without crash
    try
    {
        auto providers = Ort::GetAvailableProviders();
        // Check that CPU EP is always available
        bool has_cpu = std::find(
                           providers.begin(),
                           providers.end(),
                           "CPUExecutionProvider")
                       != providers.end();
        EXPECT_TRUE(has_cpu);
    }
    catch (const Ort::Exception& e)
    {
        // If ORT is not properly initialized, this may throw
        GTEST_SKIP() << "ORT not available: " << e.what();
    }
}

#include <gtest/gtest.h>

#include "../src/CpuImageImpl.h"
#include "../src/CudaImageImpl.h"
#include "../src/CvCpuImpl.h"
#include "../src/IDasTemplateMatchResultImpl.h"
#include "../src/IDasTemplateMatchResultsImpl.h"
#include "../src/IImageBackend.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <cstdint>

namespace Das::Core::OcvWrapper::Test
{
    namespace
    {
        //
        // Helper: build a simple BGR Mat filled with a solid value
        //
        auto MakeTestImage(int h, int w, uint8_t b, uint8_t g, uint8_t r)
            -> cv::Mat
        {
            cv::Mat m(h, w, CV_8UC3);
            m = cv::Scalar(b, g, r);
            return m;
        }

        //
        // Helper: create a CpuImageImpl with known pixel format
        //
        auto MakeTestCpuImage(
            int                                       h,
            int                                       w,
            DAS::ExportInterface::DasImagePixelFormat fmt =
                DAS::ExportInterface::DAS_PIXEL_FORMAT_BGR) -> CpuImageImpl*
        {
            return CpuImageImpl::MakeFromCpuMat(
                MakeTestImage(h, w, 128, 64, 32),
                fmt);
        }

    } // unnamed namespace

    // ==================== IImageBackend QI ====================

    TEST(ImageBackendTest, CpuImageImpl_QI_Returns_IImageBackend)
    {
        auto* img = MakeTestCpuImage(64, 64);
        EXPECT_NE(img->GetCpuMat().data, nullptr);

        void* p_backend = nullptr;
        auto  result = img->QueryInterface(
            DasIidOf<Das::Core::OcvWrapper::IImageBackend>(),
            &p_backend);
        EXPECT_EQ(result, DAS_S_OK);
        EXPECT_NE(p_backend, nullptr);
        static_cast<IImageBackend*>(p_backend)->Release();
        img->Release();
    }

    TEST(ImageBackendTest, CpuImageImpl_QI_Returns_IDasImage)
    {
        auto* img = MakeTestCpuImage(64, 64);
        void* p_image = nullptr;
        auto  result = img->QueryInterface(
            DasIidOf<DAS::ExportInterface::IDasImage>(),
            &p_image);
        EXPECT_EQ(result, DAS_S_OK);
        EXPECT_NE(p_image, nullptr);
        static_cast<DAS::ExportInterface::IDasImage*>(p_image)->Release();
        img->Release();
    }

    // ==================== GetPixelFormat ====================

    TEST(PixelFormatTest, BGR_default)
    {
        auto* img = MakeTestCpuImage(32, 32);
        DAS::ExportInterface::DasImagePixelFormat fmt{};
        ASSERT_EQ(img->GetPixelFormat(&fmt), DAS_S_OK);
        EXPECT_EQ(fmt, DAS::ExportInterface::DAS_PIXEL_FORMAT_BGR);
        img->Release();
    }

    TEST(PixelFormatTest, RGBA_explicit)
    {
        auto* img = MakeTestCpuImage(
            32,
            32,
            DAS::ExportInterface::DAS_PIXEL_FORMAT_RGBA);
        DAS::ExportInterface::DasImagePixelFormat fmt{};
        ASSERT_EQ(img->GetPixelFormat(&fmt), DAS_S_OK);
        EXPECT_EQ(fmt, DAS::ExportInterface::DAS_PIXEL_FORMAT_RGBA);
        img->Release();
    }

    // ==================== ConvertColor Routing ====================

    class ConvertColorTest : public ::testing::Test
    {
    protected:
        void SetUp() override { impl_ = CvCpuImpl::MakeRaw(); }

        void TearDown() override
        {
            if (impl_)
            {
                impl_->Release();
            }
        }

        CvCpuImpl* impl_ = nullptr;
    };

    TEST_F(ConvertColorTest, known_pair_BGR_to_RGB_ok)
    {
        auto* src = MakeTestCpuImage(
            32,
            32,
            DAS::ExportInterface::DAS_PIXEL_FORMAT_BGR);
        DAS::ExportInterface::IDasImage* out = nullptr;
        auto                             result = impl_->ConvertColor(
            src,
            DAS::ExportInterface::DAS_PIXEL_FORMAT_RGB,
            &out);
        EXPECT_EQ(result, DAS_S_OK);
        EXPECT_NE(out, nullptr);
        out->Release();
        src->Release();
    }

    TEST_F(ConvertColorTest, known_pair_BGR_to_GRAY_ok)
    {
        auto* src = MakeTestCpuImage(
            32,
            32,
            DAS::ExportInterface::DAS_PIXEL_FORMAT_BGR);
        DAS::ExportInterface::IDasImage* out = nullptr;
        auto                             result = impl_->ConvertColor(
            src,
            DAS::ExportInterface::DAS_PIXEL_FORMAT_GRAY,
            &out);
        EXPECT_EQ(result, DAS_S_OK);
        EXPECT_NE(out, nullptr);
        out->Release();
        src->Release();
    }

    TEST_F(ConvertColorTest, same_format_returns_invalid_argument)
    {
        auto* src = MakeTestCpuImage(
            32,
            32,
            DAS::ExportInterface::DAS_PIXEL_FORMAT_BGR);
        DAS::ExportInterface::IDasImage* out = nullptr;
        auto                             result = impl_->ConvertColor(
            src,
            DAS::ExportInterface::DAS_PIXEL_FORMAT_BGR,
            &out);
        EXPECT_EQ(result, DAS_E_INVALID_ARGUMENT);
        EXPECT_EQ(out, nullptr);
        src->Release();
    }

    TEST_F(ConvertColorTest, unsupported_pair_returns_invalid_argument)
    {
        // HSV→GRAY is not in the routing table
        auto* src = MakeTestCpuImage(
            32,
            32,
            DAS::ExportInterface::DAS_PIXEL_FORMAT_HSV);
        DAS::ExportInterface::IDasImage* out = nullptr;
        auto                             result = impl_->ConvertColor(
            src,
            DAS::ExportInterface::DAS_PIXEL_FORMAT_GRAY,
            &out);
        EXPECT_EQ(result, DAS_E_INVALID_ARGUMENT);
        EXPECT_EQ(out, nullptr);
        src->Release();
    }

    // ==================== ColorFilter Mask ====================

    class ColorFilterTest : public ::testing::Test
    {
    protected:
        void SetUp() override { impl_ = CvCpuImpl::MakeRaw(); }

        void TearDown() override
        {
            if (impl_)
            {
                impl_->Release();
            }
        }

        CvCpuImpl* impl_ = nullptr;
    };

    TEST_F(ColorFilterTest, output_is_single_channel)
    {
        auto* src = MakeTestCpuImage(
            32,
            32,
            DAS::ExportInterface::DAS_PIXEL_FORMAT_BGR);
        DAS::ExportInterface::DasColorRange range{
            {0, 0, 0, 0},
            {255, 255, 255, 255}};
        DAS::ExportInterface::IDasImage* out = nullptr;
        auto result = impl_->ColorFilter(src, &range, &out);
        EXPECT_EQ(result, DAS_S_OK);
        EXPECT_NE(out, nullptr);

        int32_t channels = 0;
        out->GetChannelCount(&channels);
        EXPECT_EQ(channels, 1);
        out->Release();
        src->Release();
    }

    TEST_F(ColorFilterTest, null_source_returns_invalid_pointer)
    {
        DAS::ExportInterface::IDasImage* out = nullptr;
        auto result = impl_->ColorFilter(nullptr, nullptr, &out);
        EXPECT_NE(result, DAS_S_OK);
    }

    // ==================== TemplateMatchAll ====================

    class TemplateMatchAllTest : public ::testing::Test
    {
    protected:
        void SetUp() override { impl_ = CvCpuImpl::MakeRaw(); }

        void TearDown() override
        {
            if (impl_)
            {
                impl_->Release();
            }
        }

        CvCpuImpl* impl_ = nullptr;
    };

    TEST_F(TemplateMatchAllTest, template_larger_than_source_returns_error)
    {
        auto* src = MakeTestCpuImage(32, 32);
        auto* tmpl = MakeTestCpuImage(64, 64);
        DAS::ExportInterface::IDasTemplateMatchResults* results = nullptr;
        auto result = impl_->TemplateMatchAll(
            src,
            tmpl,
            DAS::ExportInterface::DAS_TEMPLATE_MATCH_TYPE_CCOEFF_NORMED,
            0.5f,
            1,
            &results);
        EXPECT_EQ(result, DAS_E_INVALID_SIZE);
        EXPECT_EQ(results, nullptr);
        src->Release();
        tmpl->Release();
    }

    TEST_F(TemplateMatchAllTest, max_count_zero_unlimited)
    {
        // 8×8 template over 16×16 image → 9×9=81 candidates
        auto* src = MakeTestCpuImage(16, 16);
        auto* tmpl = MakeTestCpuImage(8, 8);
        DAS::ExportInterface::IDasTemplateMatchResults* results = nullptr;
        auto result = impl_->TemplateMatchAll(
            src,
            tmpl,
            DAS::ExportInterface::DAS_TEMPLATE_MATCH_TYPE_CCOEFF_NORMED,
            0.0f,
            0, // unlimited
            &results);
        ASSERT_EQ(result, DAS_S_OK);
        uint32_t count = 0;
        results->GetCount(&count);
        EXPECT_GT(count, 0);
        results->Release();
        src->Release();
        tmpl->Release();
    }

    TEST_F(TemplateMatchAllTest, threshold_filters_candidates)
    {
        auto* src = MakeTestCpuImage(16, 16);
        auto* tmpl = MakeTestCpuImage(8, 8);
        DAS::ExportInterface::IDasTemplateMatchResults* results = nullptr;
        // High threshold should filter everything out
        auto result = impl_->TemplateMatchAll(
            src,
            tmpl,
            DAS::ExportInterface::DAS_TEMPLATE_MATCH_TYPE_CCOEFF_NORMED,
            0.99f,
            10,
            &results);
        ASSERT_EQ(result, DAS_S_OK);
        uint32_t count = 0;
        results->GetCount(&count);
        EXPECT_LE(count, 10);
        uint32_t raw = 0;
        results->GetRawMatchCount(&raw);
        EXPECT_GE(raw, count);
        results->Release();
        src->Release();
        tmpl->Release();
    }

    TEST_F(TemplateMatchAllTest, max_count_truncates)
    {
        auto* src = MakeTestCpuImage(16, 16);
        auto* tmpl = MakeTestCpuImage(8, 8);
        DAS::ExportInterface::IDasTemplateMatchResults* results = nullptr;
        auto result = impl_->TemplateMatchAll(
            src,
            tmpl,
            DAS::ExportInterface::DAS_TEMPLATE_MATCH_TYPE_CCOEFF_NORMED,
            0.0f,
            3, // only 3 results
            &results);
        ASSERT_EQ(result, DAS_S_OK);
        uint32_t count = 0;
        results->GetCount(&count);
        EXPECT_LE(count, 3);
        results->Release();
        src->Release();
        tmpl->Release();
    }

    TEST_F(TemplateMatchAllTest, GetAt_out_of_range)
    {
        auto* src = MakeTestCpuImage(16, 16);
        auto* tmpl = MakeTestCpuImage(8, 8);
        DAS::ExportInterface::IDasTemplateMatchResults* results = nullptr;
        auto result = impl_->TemplateMatchAll(
            src,
            tmpl,
            DAS::ExportInterface::DAS_TEMPLATE_MATCH_TYPE_CCOEFF_NORMED,
            0.0f,
            5,
            &results);
        ASSERT_EQ(result, DAS_S_OK);

        uint32_t count = 0;
        results->GetCount(&count);
        DAS::ExportInterface::IDasTemplateMatchResult* match = nullptr;
        auto get_result = results->GetAt(count + 999, &match);
        EXPECT_EQ(get_result, DAS_E_OUT_OF_RANGE);
        EXPECT_EQ(match, nullptr);
        results->Release();
        src->Release();
        tmpl->Release();
    }

    TEST_F(TemplateMatchAllTest, null_out_pointer)
    {
        auto* src = MakeTestCpuImage(16, 16);
        auto* tmpl = MakeTestCpuImage(8, 8);
        auto  result = impl_->TemplateMatchAll(
            src,
            tmpl,
            DAS::ExportInterface::DAS_TEMPLATE_MATCH_TYPE_CCOEFF_NORMED,
            0.0f,
            5,
            nullptr);
        EXPECT_EQ(result, DAS_E_INVALID_POINTER);
        src->Release();
        tmpl->Release();
    }

    // ==================== IDasTemplateMatchResults ====================

    TEST(ResultsImplTest, default_has_zero_count)
    {
        auto*    results = IDasTemplateMatchResultsImpl::MakeRaw();
        uint32_t count = -1;
        ASSERT_EQ(results->GetCount(&count), DAS_S_OK);
        EXPECT_EQ(count, 0);
        uint32_t raw = -1;
        ASSERT_EQ(results->GetRawMatchCount(&raw), DAS_S_OK);
        EXPECT_EQ(raw, 0);
        results->Release();
    }

    // ==================== Cross-device materialize ====================

    TEST(CrossDeviceTest, CpuImageImpl_has_cpu_mat)
    {
        auto* img = MakeTestCpuImage(64, 64);
        void* p_backend = nullptr;
        ASSERT_EQ(
            img->QueryInterface(DasIidOf<IImageBackend>(), &p_backend),
            DAS_S_OK);
        auto* backend = static_cast<IImageBackend*>(p_backend);
        EXPECT_TRUE(backend->HasCpuMat());
        EXPECT_FALSE(backend->HasGpuMat());
        backend->Release();
        img->Release();
    }

    TEST(CrossDeviceTest, GetCpuMat_returns_valid_data)
    {
        auto* img = MakeTestCpuImage(64, 64);
        void* p_backend = nullptr;
        ASSERT_EQ(
            img->QueryInterface(DasIidOf<IImageBackend>(), &p_backend),
            DAS_S_OK);
        auto* backend = static_cast<IImageBackend*>(p_backend);
        auto& mat = backend->GetCpuMat();
        EXPECT_FALSE(mat.empty());
        EXPECT_EQ(mat.rows, 64);
        EXPECT_EQ(mat.cols, 64);
        backend->Release();
        img->Release();
    }

} // namespace Das::Core::OcvWrapper::Test

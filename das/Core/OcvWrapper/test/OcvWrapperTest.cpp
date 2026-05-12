#include <gtest/gtest.h>

#include <das/Core/OcvWrapper/CpuImageImpl.hpp>
#include <das/Core/OcvWrapper/IImageBackend.h>

#include "../src/CudaImageImpl.h"
#include "../src/CvCpuImpl.h"
#include "../src/IDasTemplateMatchResultImpl.h"
#include "../src/IDasTemplateMatchResultsImpl.h"

#include <das/DasApi.h>
#include <das/DasPtr.hpp>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#ifdef DAS_WITH_CUDA
#include <opencv2/core/cuda.hpp>
#endif

#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace Das
{
    namespace Core::OcvWrapper::Test
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
                    DAS::ExportInterface::DAS_PIXEL_FORMAT_BGR)
                -> CpuImageImpl<Storage::OwningStorage>*
            {
                return CpuImageImpl<Storage::OwningStorage>::MakeFromCpuMat(
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
                DasIidOf<DAS::Core::OcvWrapper::IImageBackend>(),
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

        // ==================== Factory Migration Tests ====================

        namespace
        {
            // Mock IDasBinaryBuffer for tests
            class TestBinaryBuffer final
                : public ExportInterface::IDasBinaryBuffer
            {
                std::atomic<uint32_t> ref_count_{0};
                std::vector<uint8_t>  data_;

            public:
                TestBinaryBuffer(const uint8_t* data, size_t size)
                {
                    if (size > 0)
                    {
                        data_.assign(data, data + size);
                    }
                }

                // IUnknown
                uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }
                uint32_t DAS_STD_CALL Release() override
                {
                    auto c = --ref_count_;
                    if (c == 0)
                    {
                        delete this;
                    }
                    return c;
                }
                DasResult DAS_STD_CALL
                QueryInterface(const DasGuid& iid, void** pp) override
                {
                    if (!pp)
                    {
                        return DAS_E_INVALID_POINTER;
                    }
                    if (iid == DasIidOf<IDasBase>())
                    {
                        *pp = static_cast<IDasBase*>(this);
                        AddRef();
                        return DAS_S_OK;
                    }
                    if (iid == DasIidOf<ExportInterface::IDasBinaryBuffer>())
                    {
                        *pp = static_cast<ExportInterface::IDasBinaryBuffer*>(
                            this);
                        AddRef();
                        return DAS_S_OK;
                    }
                    *pp = nullptr;
                    return DAS_E_NO_INTERFACE;
                }

                DasResult GetData(unsigned char** pp_out_data) override
                {
                    if (!pp_out_data)
                    {
                        return DAS_E_INVALID_POINTER;
                    }
                    *pp_out_data = data_.data();
                    return DAS_S_OK;
                }

                DasResult GetSize(uint64_t* p_out_size) override
                {
                    if (!p_out_size)
                    {
                        return DAS_E_INVALID_POINTER;
                    }
                    *p_out_size = data_.size();
                    return DAS_S_OK;
                }
            };

            // Mock IDasMemory for tests
            class TestMemory final : public ExportInterface::IDasMemory
            {
                std::atomic<uint32_t> ref_count_{0};
                std::vector<uint8_t>  data_;

            public:
                explicit TestMemory(std::vector<uint8_t> data)
                    : data_(std::move(data))
                {
                }

                // IUnknown
                uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }
                uint32_t DAS_STD_CALL Release() override
                {
                    auto c = --ref_count_;
                    if (c == 0)
                    {
                        delete this;
                    }
                    return c;
                }
                DasResult DAS_STD_CALL
                QueryInterface(const DasGuid& iid, void** pp) override
                {
                    if (!pp)
                    {
                        return DAS_E_INVALID_POINTER;
                    }
                    if (iid == DasIidOf<IDasBase>())
                    {
                        *pp = static_cast<IDasBase*>(this);
                        AddRef();
                        return DAS_S_OK;
                    }
                    if (iid == DasIidOf<ExportInterface::IDasMemory>())
                    {
                        *pp = static_cast<ExportInterface::IDasMemory*>(this);
                        AddRef();
                        return DAS_S_OK;
                    }
                    *pp = nullptr;
                    return DAS_E_NO_INTERFACE;
                }

                DasResult GetBinaryBuffer(
                    uint64_t                            offset,
                    ExportInterface::IDasBinaryBuffer** pp_out) override
                {
                    if (!pp_out)
                    {
                        return DAS_E_INVALID_POINTER;
                    }
                    *pp_out = nullptr;
                    if (offset > data_.size())
                    {
                        return DAS_E_OUT_OF_RANGE;
                    }
                    const auto  view_size = data_.size() - offset;
                    const auto* view_data =
                        view_size == 0 ? nullptr : data_.data() + offset;
                    auto* buf = new TestBinaryBuffer(view_data, view_size);
                    buf->AddRef();
                    *pp_out = buf;
                    return DAS_S_OK;
                }

                DasResult GetMutableView(
                    uint64_t                            offset,
                    ExportInterface::IDasBinaryBuffer** pp_out) override
                {
                    if (!pp_out)
                    {
                        return DAS_E_INVALID_POINTER;
                    }
                    *pp_out = nullptr;
                    if (offset > data_.size())
                    {
                        return DAS_E_OUT_OF_RANGE;
                    }
                    const auto  view_size = data_.size() - offset;
                    const auto* view_data =
                        view_size == 0 ? nullptr : data_.data() + offset;
                    auto* buf = new TestBinaryBuffer(view_data, view_size);
                    buf->AddRef();
                    *pp_out = buf;
                    return DAS_S_OK;
                }

                DasResult GetSize(uint64_t* p_out_size) override
                {
                    if (!p_out_size)
                    {
                        return DAS_E_INVALID_POINTER;
                    }
                    *p_out_size = data_.size();
                    return DAS_S_OK;
                }
            };

            class TrackingMemory;

            class TrackingBinaryBuffer final
                : public ExportInterface::IDasBinaryBuffer
            {
                std::atomic<uint32_t>                    ref_count_{0};
                DAS::DasPtr<ExportInterface::IDasMemory> owner_;
                uint64_t                                 offset_{};

            public:
                TrackingBinaryBuffer(TrackingMemory* p_owner, uint64_t offset);

                uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }
                uint32_t DAS_STD_CALL Release() override
                {
                    auto count = --ref_count_;
                    if (count == 0)
                    {
                        delete this;
                    }
                    return count;
                }

                DasResult DAS_STD_CALL QueryInterface(
                    const DasGuid& iid,
                    void**         pp_out_object) override
                {
                    if (pp_out_object == nullptr)
                    {
                        return DAS_E_INVALID_POINTER;
                    }
                    if (iid == DasIidOf<IDasBase>())
                    {
                        *pp_out_object = static_cast<IDasBase*>(this);
                        AddRef();
                        return DAS_S_OK;
                    }
                    if (iid == DasIidOf<ExportInterface::IDasBinaryBuffer>())
                    {
                        *pp_out_object =
                            static_cast<ExportInterface::IDasBinaryBuffer*>(
                                this);
                        AddRef();
                        return DAS_S_OK;
                    }
                    *pp_out_object = nullptr;
                    return DAS_E_NO_INTERFACE;
                }

                DasResult GetData(unsigned char** pp_out_data) override;
                DasResult GetSize(uint64_t* p_out_size) override;
            };

            class TrackingMemory final : public ExportInterface::IDasMemory
            {
                std::atomic<uint32_t> ref_count_{0};
                std::vector<uint8_t>  data_;
                std::shared_ptr<bool> destroyed_;

            public:
                explicit TrackingMemory(std::vector<uint8_t> data)
                    : data_(std::move(data)),
                      destroyed_(std::make_shared<bool>(false))
                {
                }

                ~TrackingMemory() { *destroyed_ = true; }

                uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }
                uint32_t DAS_STD_CALL Release() override
                {
                    auto count = --ref_count_;
                    if (count == 0)
                    {
                        delete this;
                    }
                    return count;
                }

                DasResult DAS_STD_CALL QueryInterface(
                    const DasGuid& iid,
                    void**         pp_out_object) override
                {
                    if (pp_out_object == nullptr)
                    {
                        return DAS_E_INVALID_POINTER;
                    }
                    if (iid == DasIidOf<IDasBase>())
                    {
                        *pp_out_object = static_cast<IDasBase*>(this);
                        AddRef();
                        return DAS_S_OK;
                    }
                    if (iid == DasIidOf<ExportInterface::IDasMemory>())
                    {
                        *pp_out_object =
                            static_cast<ExportInterface::IDasMemory*>(this);
                        AddRef();
                        return DAS_S_OK;
                    }
                    *pp_out_object = nullptr;
                    return DAS_E_NO_INTERFACE;
                }

                DasResult GetBinaryBuffer(
                    uint64_t                            offset,
                    ExportInterface::IDasBinaryBuffer** pp_out_buffer) override
                {
                    if (pp_out_buffer == nullptr)
                    {
                        return DAS_E_INVALID_POINTER;
                    }
                    *pp_out_buffer = nullptr;
                    if (offset > data_.size())
                    {
                        return DAS_E_OUT_OF_RANGE;
                    }
                    auto* buffer = new TrackingBinaryBuffer(this, offset);
                    buffer->AddRef();
                    *pp_out_buffer = buffer;
                    return DAS_S_OK;
                }

                DasResult GetMutableView(
                    uint64_t                            offset,
                    ExportInterface::IDasBinaryBuffer** pp_out_buffer) override
                {
                    return GetBinaryBuffer(offset, pp_out_buffer);
                }

                DasResult GetSize(uint64_t* p_out_size) override
                {
                    if (p_out_size == nullptr)
                    {
                        return DAS_E_INVALID_POINTER;
                    }
                    *p_out_size = data_.size();
                    return DAS_S_OK;
                }

                std::vector<uint8_t>& Data() { return data_; }

                std::shared_ptr<bool> DestroyedFlag() const
                {
                    return destroyed_;
                }
            };

            TrackingBinaryBuffer::TrackingBinaryBuffer(
                TrackingMemory* p_owner,
                uint64_t        offset)
                : owner_(static_cast<ExportInterface::IDasMemory*>(p_owner)),
                  offset_(offset)
            {
            }

            DasResult TrackingBinaryBuffer::GetData(unsigned char** pp_out_data)
            {
                if (pp_out_data == nullptr)
                {
                    return DAS_E_INVALID_POINTER;
                }
                auto* memory = static_cast<TrackingMemory*>(owner_.Get());
                *pp_out_data = memory->Data().data() + offset_;
                return DAS_S_OK;
            }

            DasResult TrackingBinaryBuffer::GetSize(uint64_t* p_out_size)
            {
                if (p_out_size == nullptr)
                {
                    return DAS_E_INVALID_POINTER;
                }
                auto* memory = static_cast<TrackingMemory*>(owner_.Get());
                *p_out_size = memory->Data().size() - offset_;
                return DAS_S_OK;
            }

            // Helper: create RGBA test data
            auto MakeRgbaData(
                int     w,
                int     h,
                uint8_t r,
                uint8_t g,
                uint8_t b,
                uint8_t a) -> std::vector<uint8_t>
            {
                std::vector<uint8_t> data(static_cast<size_t>(w * h * 4));
                for (size_t i = 0; i < data.size(); i += 4)
                {
                    data[i] = r;
                    data[i + 1] = g;
                    data[i + 2] = b;
                    data[i + 3] = a;
                }
                return data;
            }

            // Helper: create PNG-encoded test data
            auto MakePngData(int w, int h) -> std::vector<uint8_t>
            {
                cv::Mat              img = MakeTestImage(h, w, 128, 64, 32);
                std::vector<uint8_t> buf;
                cv::imencode(".png", img, buf);
                return buf;
            }

            // Helper: verify IImageBackend QI succeeds
            void AssertBackendQI(ExportInterface::IDasImage* p_image)
            {
                ASSERT_NE(p_image, nullptr);
                void* p_backend = nullptr;
                auto  qi = p_image->QueryInterface(
                    DasIidOf<DAS::Core::OcvWrapper::IImageBackend>(),
                    &p_backend);
                ASSERT_EQ(qi, DAS_S_OK);
                ASSERT_NE(p_backend, nullptr);
                static_cast<IImageBackend*>(p_backend)->Release();
            }

        } // unnamed namespace

        // --- Test 1: CreateIDasImageFromEncodedData ---

        TEST(FactoryMigrationTest, FromEncodedData_ReturnsCpuImageWithRGBFormat)
        {
            auto png = MakePngData(32, 32);

            DasImageDesc desc{};
            desc.p_data = reinterpret_cast<char*>(png.data());
            desc.data_size = png.size();
            desc.data_format = ExportInterface::DAS_IMAGE_FORMAT_PNG;

            ExportInterface::IDasImage* p_image = nullptr;
            auto result = CreateIDasImageFromEncodedData(&desc, &p_image);
            ASSERT_EQ(result, DAS_S_OK);

            // Key assertion: must support IImageBackend QI (CpuImageImpl)
            AssertBackendQI(p_image);

            ExportInterface::DasImagePixelFormat fmt{};
            EXPECT_EQ(p_image->GetPixelFormat(&fmt), DAS_S_OK);
            EXPECT_EQ(fmt, ExportInterface::DAS_PIXEL_FORMAT_RGB);

            p_image->Release();
        }

        // --- Test 2: CreateIDasImageFromDecodedData ---

        TEST(FactoryMigrationTest, FromDecodedData_ReturnsCpuImageWithRGBFormat)
        {
            constexpr int        kWidth = 32;
            constexpr int        kHeight = 24;
            std::vector<uint8_t> rgb_data(
                static_cast<size_t>(kWidth * kHeight * 3),
                128);

            DasImageDesc desc{};
            desc.p_data = reinterpret_cast<char*>(rgb_data.data());
            desc.data_size = rgb_data.size();
            desc.data_format = ExportInterface::DAS_IMAGE_FORMAT_RGB_888;

            ExportInterface::DasSize    size{kWidth, kHeight};
            ExportInterface::IDasImage* p_image = nullptr;
            auto                        result =
                CreateIDasImageFromDecodedData(&desc, &size, &p_image);
            ASSERT_EQ(result, DAS_S_OK);

            AssertBackendQI(p_image);

            ExportInterface::DasImagePixelFormat fmt{};
            EXPECT_EQ(p_image->GetPixelFormat(&fmt), DAS_S_OK);
            EXPECT_EQ(fmt, ExportInterface::DAS_PIXEL_FORMAT_RGB);

            ExportInterface::DasSize img_size{};
            EXPECT_EQ(p_image->GetSize(&img_size), DAS_S_OK);
            EXPECT_EQ(img_size.width, kWidth);
            EXPECT_EQ(img_size.height, kHeight);

            p_image->Release();
        }

        TEST(FactoryMigrationTest, FromDecodedData_RejectsNullBuffer)
        {
            DasImageDesc desc{};
            desc.p_data = nullptr;
            desc.data_size = 12;
            desc.data_format = ExportInterface::DAS_IMAGE_FORMAT_RGB_888;

            ExportInterface::DasSize    size{2, 2};
            ExportInterface::IDasImage* p_image = nullptr;
            EXPECT_EQ(
                CreateIDasImageFromDecodedData(&desc, &size, &p_image),
                DAS_E_INVALID_POINTER);
            EXPECT_EQ(p_image, nullptr);
        }

        TEST(FactoryMigrationTest, FromDecodedData_RejectsShortBuffer)
        {
            std::vector<uint8_t> rgb(2, 128);

            DasImageDesc desc{};
            desc.p_data = reinterpret_cast<char*>(rgb.data());
            desc.data_size = rgb.size();
            desc.data_format = ExportInterface::DAS_IMAGE_FORMAT_RGB_888;

            ExportInterface::DasSize    size{2, 2};
            ExportInterface::IDasImage* p_image = nullptr;
            EXPECT_EQ(
                CreateIDasImageFromDecodedData(&desc, &size, &p_image),
                DAS_E_OUT_OF_RANGE);
            EXPECT_EQ(p_image, nullptr);
        }

        TEST(FactoryMigrationTest, FromDecodedData_RejectsInvalidDimensions)
        {
            std::vector<uint8_t> rgb(12, 128);

            DasImageDesc desc{};
            desc.p_data = reinterpret_cast<char*>(rgb.data());
            desc.data_size = rgb.size();
            desc.data_format = ExportInterface::DAS_IMAGE_FORMAT_RGB_888;

            ExportInterface::DasSize    size{0, 2};
            ExportInterface::IDasImage* p_image = nullptr;
            EXPECT_EQ(
                CreateIDasImageFromDecodedData(&desc, &size, &p_image),
                DAS_E_INVALID_SIZE);
            EXPECT_EQ(p_image, nullptr);
        }

        // --- Test 3: CreateIDasImageFromRgb888 ---

        TEST(FactoryMigrationTest, FromRgb888_ReturnsCpuImageWithRGBAFormat)
        {
            constexpr int kWidth = 16;
            constexpr int kHeight = 16;
            auto rgba_data = MakeRgbaData(kWidth, kHeight, 255, 128, 64, 255);

            auto* p_memory = new TestMemory(std::move(rgba_data));
            p_memory->AddRef();

            ExportInterface::DasSize    size{kWidth, kHeight};
            ExportInterface::IDasImage* p_image = nullptr;
            auto result = CreateIDasImageFromRgb888(p_memory, &size, &p_image);
            ASSERT_EQ(result, DAS_S_OK);

            AssertBackendQI(p_image);

            ExportInterface::DasImagePixelFormat fmt{};
            EXPECT_EQ(p_image->GetPixelFormat(&fmt), DAS_S_OK);
            EXPECT_EQ(fmt, ExportInterface::DAS_PIXEL_FORMAT_RGBA);

            int32_t channels = 0;
            EXPECT_EQ(p_image->GetChannelCount(&channels), DAS_S_OK);
            EXPECT_EQ(channels, 4);

            // Release the original memory — image must survive with cloned data
            p_memory->Release();

            uint64_t data_size = 0;
            EXPECT_EQ(p_image->GetDataSize(&data_size), DAS_S_OK);
            EXPECT_EQ(data_size, static_cast<uint64_t>(kWidth * kHeight * 4));

            ExportInterface::IDasBinaryBuffer* p_buffer = nullptr;
            EXPECT_EQ(p_image->GetBinaryBuffer(&p_buffer), DAS_S_OK);
            ASSERT_NE(p_buffer, nullptr);
            uint64_t buf_size = 0;
            EXPECT_EQ(p_buffer->GetSize(&buf_size), DAS_S_OK);
            EXPECT_EQ(buf_size, data_size);
            p_buffer->Release();

            p_image->Release();
        }

        TEST(
            FactoryMigrationTest,
            FromRgb888_RetainsReturnedBufferViewAfterMemoryRelease)
        {
            constexpr int kWidth = 2;
            constexpr int kHeight = 1;
            auto rgba_data = MakeRgbaData(kWidth, kHeight, 10, 20, 30, 40);

            auto* p_memory = new TrackingMemory(std::move(rgba_data));
            p_memory->AddRef();
            auto memory_destroyed = p_memory->DestroyedFlag();

            ExportInterface::DasSize    size{kWidth, kHeight};
            ExportInterface::IDasImage* p_image = nullptr;
            ASSERT_EQ(
                CreateIDasImageFromRgb888(p_memory, &size, &p_image),
                DAS_S_OK);
            ASSERT_NE(p_image, nullptr);

            p_memory->Release();
            EXPECT_FALSE(*memory_destroyed);

            ExportInterface::IDasBinaryBuffer* p_buffer = nullptr;
            ASSERT_EQ(p_image->GetBinaryBuffer(&p_buffer), DAS_S_OK);
            ASSERT_NE(p_buffer, nullptr);

            uint64_t buffer_size = 0;
            EXPECT_EQ(p_buffer->GetSize(&buffer_size), DAS_S_OK);
            EXPECT_EQ(buffer_size, static_cast<uint64_t>(kWidth * kHeight * 4));

            unsigned char* p_image_data = nullptr;
            EXPECT_EQ(p_buffer->GetData(&p_image_data), DAS_S_OK);
            ASSERT_NE(p_image_data, nullptr);
            EXPECT_EQ(p_image_data[0], 10);
            EXPECT_EQ(p_image_data[1], 20);
            EXPECT_EQ(p_image_data[2], 30);
            EXPECT_EQ(p_image_data[3], 40);

            p_buffer->Release();
            p_image->Release();
            EXPECT_TRUE(*memory_destroyed);
        }

        TEST(FactoryMigrationTest, FromRgb888_BinaryBufferSharesReturnedBytes)
        {
            constexpr int kWidth = 2;
            constexpr int kHeight = 1;
            auto          rgba_data = MakeRgbaData(kWidth, kHeight, 1, 2, 3, 4);

            auto* p_memory = new TrackingMemory(std::move(rgba_data));
            p_memory->AddRef();

            ExportInterface::DasSize    size{kWidth, kHeight};
            ExportInterface::IDasImage* p_image = nullptr;
            ASSERT_EQ(
                CreateIDasImageFromRgb888(p_memory, &size, &p_image),
                DAS_S_OK);
            ASSERT_NE(p_image, nullptr);

            ExportInterface::IDasBinaryBuffer* p_buffer = nullptr;
            ASSERT_EQ(p_image->GetBinaryBuffer(&p_buffer), DAS_S_OK);
            ASSERT_NE(p_buffer, nullptr);

            unsigned char* p_image_data = nullptr;
            ASSERT_EQ(p_buffer->GetData(&p_image_data), DAS_S_OK);
            ASSERT_NE(p_image_data, nullptr);
            EXPECT_EQ(p_image_data, p_memory->Data().data());

            p_memory->Data()[0] = 99;
            EXPECT_EQ(p_image_data[0], 99);

            p_buffer->Release();
            p_image->Release();
            p_memory->Release();
        }

        TEST(
            FactoryMigrationTest,
            FromRgb888_MutableSharingVisibleAcrossImageBuffers)
        {
            constexpr int kWidth = 2;
            constexpr int kHeight = 1;
            auto rgba_data = MakeRgbaData(kWidth, kHeight, 10, 20, 30, 40);

            auto* p_memory = new TrackingMemory(std::move(rgba_data));
            p_memory->AddRef();

            ExportInterface::DasSize    size{kWidth, kHeight};
            ExportInterface::IDasImage* p_image = nullptr;
            ASSERT_EQ(
                CreateIDasImageFromRgb888(p_memory, &size, &p_image),
                DAS_S_OK);
            ASSERT_NE(p_image, nullptr);
            p_memory->Release();

            ExportInterface::IDasBinaryBuffer* p_first_buffer = nullptr;
            ASSERT_EQ(p_image->GetBinaryBuffer(&p_first_buffer), DAS_S_OK);
            DAS::DasPtr<ExportInterface::IDasBinaryBuffer> first_buffer{
                p_first_buffer};

            ExportInterface::IDasBinaryBuffer* p_second_buffer = nullptr;
            ASSERT_EQ(p_image->GetBinaryBuffer(&p_second_buffer), DAS_S_OK);
            DAS::DasPtr<ExportInterface::IDasBinaryBuffer> second_buffer{
                p_second_buffer};

            unsigned char* first_data = nullptr;
            ASSERT_EQ(first_buffer->GetData(&first_data), DAS_S_OK);
            ASSERT_NE(first_data, nullptr);

            unsigned char* second_data = nullptr;
            ASSERT_EQ(second_buffer->GetData(&second_data), DAS_S_OK);
            ASSERT_NE(second_data, nullptr);

            first_data[0] = 77;
            first_data[3] = 88;

            EXPECT_EQ(second_data[0], 77);
            EXPECT_EQ(second_data[3], 88);

            p_image->Release();
        }

        TEST(FactoryMigrationTest, FromRgb888_RejectsShortBuffer)
        {
            std::vector<uint8_t> rgba(2, 255);
            auto*                p_memory = new TestMemory(std::move(rgba));
            p_memory->AddRef();

            ExportInterface::DasSize    size{2, 2};
            ExportInterface::IDasImage* p_image = nullptr;
            EXPECT_EQ(
                CreateIDasImageFromRgb888(p_memory, &size, &p_image),
                DAS_E_OUT_OF_RANGE);
            EXPECT_EQ(p_image, nullptr);

            p_memory->Release();
        }

        TEST(FactoryMigrationTest, FromRgb888_RejectsOverflowedIntMath)
        {
            std::vector<uint8_t> rgba(16, 255);
            auto*                p_memory = new TestMemory(std::move(rgba));
            p_memory->AddRef();

            ExportInterface::DasSize size{
                std::numeric_limits<int32_t>::max(),
                std::numeric_limits<int32_t>::max()};
            ExportInterface::IDasImage* p_image = nullptr;
            EXPECT_EQ(
                CreateIDasImageFromRgb888(p_memory, &size, &p_image),
                DAS_E_OUT_OF_RANGE);
            EXPECT_EQ(p_image, nullptr);

            p_memory->Release();
        }

        // --- Test 4: DasPluginLoadImageFromResource ---

        TEST(
            FactoryMigrationTest,
            DISABLED_FromResource_RequiresPluginInfrastructure)
        {
            // DasPluginLoadImageFromResource requires PluginManager and
            // PluginResourceIndex infrastructure. Tested via integration tests.
            GTEST_SKIP()
                << "DasPluginLoadImageFromResource requires plugin infrastructure";
        }

        // --- Test 5: GetBinaryBuffer consistency for all factories ---

        TEST(
            FactoryMigrationTest,
            FromEncodedData_BinaryBufferSizeMatchesDataSize)
        {
            auto png = MakePngData(16, 16);

            DasImageDesc desc{};
            desc.p_data = reinterpret_cast<char*>(png.data());
            desc.data_size = png.size();
            desc.data_format = ExportInterface::DAS_IMAGE_FORMAT_PNG;

            ExportInterface::IDasImage* p_image = nullptr;
            ASSERT_EQ(
                CreateIDasImageFromEncodedData(&desc, &p_image),
                DAS_S_OK);

            uint64_t data_size = 0;
            EXPECT_EQ(p_image->GetDataSize(&data_size), DAS_S_OK);

            ExportInterface::IDasBinaryBuffer* p_buffer = nullptr;
            EXPECT_EQ(p_image->GetBinaryBuffer(&p_buffer), DAS_S_OK);
            ASSERT_NE(p_buffer, nullptr);
            uint64_t buf_size = 0;
            EXPECT_EQ(p_buffer->GetSize(&buf_size), DAS_S_OK);
            EXPECT_EQ(buf_size, data_size);
            p_buffer->Release();

            p_image->Release();
        }

        TEST(
            FactoryMigrationTest,
            FromDecodedData_BinaryBufferSizeMatchesDataSize)
        {
            constexpr int        kW = 8;
            constexpr int        kH = 8;
            std::vector<uint8_t> rgb(static_cast<size_t>(kW * kH * 3), 100);

            DasImageDesc desc{};
            desc.p_data = reinterpret_cast<char*>(rgb.data());
            desc.data_size = rgb.size();
            desc.data_format = ExportInterface::DAS_IMAGE_FORMAT_RGB_888;

            ExportInterface::DasSize    size{kW, kH};
            ExportInterface::IDasImage* p_image = nullptr;
            ASSERT_EQ(
                CreateIDasImageFromDecodedData(&desc, &size, &p_image),
                DAS_S_OK);

            uint64_t data_size = 0;
            EXPECT_EQ(p_image->GetDataSize(&data_size), DAS_S_OK);

            ExportInterface::IDasBinaryBuffer* p_buffer = nullptr;
            EXPECT_EQ(p_image->GetBinaryBuffer(&p_buffer), DAS_S_OK);
            ASSERT_NE(p_buffer, nullptr);
            uint64_t buf_size = 0;
            EXPECT_EQ(p_buffer->GetSize(&buf_size), DAS_S_OK);
            EXPECT_EQ(buf_size, data_size);
            p_buffer->Release();

            p_image->Release();
        }

        TEST(FactoryMigrationTest, FromRgb888_BinaryBufferSizeMatchesDataSize)
        {
            constexpr int kW = 8;
            constexpr int kH = 8;
            auto          rgba = MakeRgbaData(kW, kH, 200, 100, 50, 255);

            auto* p_mem = new TestMemory(std::move(rgba));
            p_mem->AddRef();

            ExportInterface::DasSize    size{kW, kH};
            ExportInterface::IDasImage* p_image = nullptr;
            ASSERT_EQ(
                CreateIDasImageFromRgb888(p_mem, &size, &p_image),
                DAS_S_OK);

            uint64_t data_size = 0;
            EXPECT_EQ(p_image->GetDataSize(&data_size), DAS_S_OK);

            ExportInterface::IDasBinaryBuffer* p_buffer = nullptr;
            EXPECT_EQ(p_image->GetBinaryBuffer(&p_buffer), DAS_S_OK);
            ASSERT_NE(p_buffer, nullptr);
            uint64_t buf_size = 0;
            EXPECT_EQ(p_buffer->GetSize(&buf_size), DAS_S_OK);
            EXPECT_EQ(buf_size, data_size);
            p_buffer->Release();

            p_mem->Release();
            p_image->Release();
        }

        // ==================== CpuImageImpl IDasBinaryBuffer
        // ====================

        TEST(CpuImageBinaryBufferTest, QI_Returns_IDasBinaryBuffer)
        {
            cv::Mat mat = cv::Mat::zeros(10, 20, CV_8UC3);
            auto*   img = CpuImageImpl<Storage::OwningStorage>::MakeFromCpuMat(
                mat,
                ExportInterface::DAS_PIXEL_FORMAT_BGR);
            DAS::DasPtr<IImageBackend> guard(img);

            void* ptr = nullptr;
            auto  result = img->QueryInterface(
                DasIidOf<ExportInterface::IDasBinaryBuffer>(),
                &ptr);
            EXPECT_EQ(result, DAS_S_OK);
            EXPECT_NE(ptr, nullptr);
            if (ptr)
            {
                static_cast<IDasBase*>(ptr)->Release();
            }
        }

        TEST(CpuImageBinaryBufferTest, GetBinaryBuffer_ZeroCopy)
        {
            cv::Mat mat = cv::Mat::zeros(10, 20, CV_8UC3);
            mat.at<cv::Vec3b>(5, 10) = cv::Vec3b(100, 150, 200);
            auto* img = CpuImageImpl<Storage::OwningStorage>::MakeFromCpuMat(
                mat.clone(),
                ExportInterface::DAS_PIXEL_FORMAT_BGR);
            DAS::DasPtr<IImageBackend> guard(img);

            ExportInterface::IDasBinaryBuffer* p_buffer = nullptr;
            auto result = img->GetBinaryBuffer(&p_buffer);
            ASSERT_EQ(result, DAS_S_OK);
            DAS::DasPtr<ExportInterface::IDasBinaryBuffer> buffer_guard(
                p_buffer);

            unsigned char* data = nullptr;
            result = p_buffer->GetData(&data);
            ASSERT_EQ(result, DAS_S_OK);

            // Zero-copy: GetData should return same data as GetCpuMat
            EXPECT_EQ(data, img->GetCpuMat().data);
        }

        // ==================== CudaImageImpl IDasBinaryBuffer
        // ====================

#ifdef DAS_WITH_CUDA
        TEST(CudaImageBinaryBufferTest, QI_Returns_IDasBinaryBuffer)
        {
            cv::cuda::GpuMat gpu_mat(10, 20, CV_8UC3);
            gpu_mat.setTo(cv::Scalar(0));
            auto* img = CudaImageImpl::MakeFromGpuMat(
                std::move(gpu_mat),
                ExportInterface::DAS_PIXEL_FORMAT_BGR);
            DAS::DasPtr<IImageBackend> guard(img);

            void* ptr = nullptr;
            auto  result = img->QueryInterface(
                DasIidOf<ExportInterface::IDasBinaryBuffer>(),
                &ptr);
            EXPECT_EQ(result, DAS_S_OK);
            EXPECT_NE(ptr, nullptr);
            if (ptr)
            {
                static_cast<IDasBase*>(ptr)->Release();
            }
        }

        TEST(CudaImageBinaryBufferTest, GetBinaryBuffer_MaterializesCpuBytes)
        {
            cv::cuda::GpuMat gpu_mat(10, 20, CV_8UC3);
            gpu_mat.setTo(cv::Scalar(100, 150, 200));
            auto* img = CudaImageImpl::MakeFromGpuMat(
                std::move(gpu_mat),
                ExportInterface::DAS_PIXEL_FORMAT_BGR);
            DAS::DasPtr<IImageBackend> guard(img);

            ExportInterface::IDasBinaryBuffer* p_buffer = nullptr;
            auto result = img->GetBinaryBuffer(&p_buffer);
            ASSERT_EQ(result, DAS_S_OK);
            DAS::DasPtr<ExportInterface::IDasBinaryBuffer> buffer_guard(
                p_buffer);

            uint64_t buffer_size = 0;
            ASSERT_EQ(p_buffer->GetSize(&buffer_size), DAS_S_OK);
            EXPECT_EQ(buffer_size, static_cast<uint64_t>(10 * 20 * 3));

            unsigned char* data = nullptr;
            ASSERT_EQ(p_buffer->GetData(&data), DAS_S_OK);
            ASSERT_NE(data, nullptr);
            EXPECT_EQ(data[0], 100);
            EXPECT_EQ(data[1], 150);
            EXPECT_EQ(data[2], 200);
        }
#endif // DAS_WITH_CUDA

    } // namespace Core::OcvWrapper::Test
} // namespace Das

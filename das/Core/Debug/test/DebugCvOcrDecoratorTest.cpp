#include <das/Core/Debug/DebugDecorators.h>
#include <das/Core/Debug/DebugRuntime.h>
#include <das/Core/IPC/CurrentIpcContextScope.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/Core/OcvWrapper/CpuImageImpl.hpp>
#include <das/Core/OcvWrapper/CvServiceRegistrar.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasAI.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasBinaryBuffer.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasOcr.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasOcrResult.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasOcrResultVector.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasSession.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasTensor.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasTensorVector.Implements.hpp>
#include <gtest/gtest.h>

#include "../src/DebugWriterImpl.h"
#include "../../OcvWrapper/src/CvCpuImpl.h"
#include "../../OrtWrapper/src/AiCpuImpl.h"

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_OPENCV_WARNING
#include <opencv2/imgproc.hpp>
DAS_DISABLE_WARNING_END

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#ifdef DAS_WINDOWS
#include <stdlib.h>
#endif

namespace Das::Core::Debug::Test
{
    namespace
    {
        auto UniqueTempDir(const char* test_name) -> std::filesystem::path
        {
            const auto stamp =
                std::chrono::steady_clock::now().time_since_epoch().count();
            auto path = std::filesystem::current_path()
                / "debug-test-output"
                / (std::string{test_name} + "-" + std::to_string(stamp));
            std::filesystem::remove_all(path);
            return path;
        }

        void SetDasDebug(const char* value)
        {
#ifdef DAS_WINDOWS
            _putenv_s("DAS_DEBUG", value);
#else
            setenv("DAS_DEBUG", value, 1);
#endif
        }

        void UnsetDasDebug()
        {
#ifdef DAS_WINDOWS
            _putenv_s("DAS_DEBUG", "");
#else
            unsetenv("DAS_DEBUG");
#endif
        }

        void ResetRuntime()
        {
            DebugRuntime::Shutdown();
            DebugRuntime::ResetForTest();
            UnsetDasDebug();
        }

        class DebugCvOcrDecoratorTest : public ::testing::Test
        {
        protected:
            void TearDown() override { ResetRuntime(); }
        };

        void InitializeRuntime(
            const std::filesystem::path& dir,
            const char*                  enabled)
        {
            SetDasDebug(enabled);
            DebugRuntimeOptions options{};
            options.debug_dir = dir;
            ASSERT_EQ(DebugRuntime::Initialize(options), DAS_S_OK);
        }

        auto ReadLines(const std::filesystem::path& path)
            -> std::vector<std::string>
        {
            std::ifstream input{path};
            std::vector<std::string> lines;
            std::string              line;
            while (std::getline(input, line))
            {
                lines.emplace_back(std::move(line));
            }
            return lines;
        }

        bool AnyLineContains(
            const std::vector<std::string>& lines,
            const std::string&              needle)
        {
            for (const auto& line : lines)
            {
                if (line.find(needle) != std::string::npos)
                {
                    return true;
                }
            }
            return false;
        }

        bool HasAnyPng(const std::filesystem::path& dir)
        {
            if (!std::filesystem::exists(dir))
            {
                return false;
            }

            for (const auto& entry : std::filesystem::directory_iterator(dir))
            {
                if (entry.path().extension() == ".png")
                {
                    return true;
                }
            }
            return false;
        }

        auto MakePatternImage()
            -> Das::DasPtr<Das::ExportInterface::IDasImage>
        {
            cv::Mat image = cv::Mat::zeros(32, 32, CV_8UC3);
            cv::Mat templ(8, 8, CV_8UC3, cv::Scalar{10, 20, 30});
            cv::line(
                templ,
                cv::Point{0, 0},
                cv::Point{7, 7},
                cv::Scalar{200, 120, 60},
                1);
            templ.copyTo(image(cv::Rect{10, 10, 8, 8}));

            auto* raw =
                OcvWrapper::CpuImageImpl<OcvWrapper::Storage::OwningStorage>::
                    MakeFromCpuMat(
                        image,
                        Das::ExportInterface::DAS_PIXEL_FORMAT_BGR);
            return Das::DasPtr<Das::ExportInterface::IDasImage>::Attach(raw);
        }

        auto MakePatternTemplate()
            -> Das::DasPtr<Das::ExportInterface::IDasImage>
        {
            cv::Mat templ(8, 8, CV_8UC3, cv::Scalar{10, 20, 30});
            cv::line(
                templ,
                cv::Point{0, 0},
                cv::Point{7, 7},
                cv::Scalar{200, 120, 60},
                1);
            auto* raw =
                OcvWrapper::CpuImageImpl<OcvWrapper::Storage::OwningStorage>::
                    MakeFromCpuMat(
                        templ,
                        Das::ExportInterface::DAS_PIXEL_FORMAT_BGR);
            return Das::DasPtr<Das::ExportInterface::IDasImage>::Attach(raw);
        }

        auto MakeOcrImage() -> Das::DasPtr<Das::ExportInterface::IDasImage>
        {
            cv::Mat image(16, 32, CV_8UC3, cv::Scalar{30, 30, 30});
            cv::rectangle(
                image,
                cv::Rect{4, 4, 16, 8},
                cv::Scalar{220, 220, 220},
                cv::FILLED);
            auto* raw =
                OcvWrapper::CpuImageImpl<OcvWrapper::Storage::OwningStorage>::
                    MakeFromCpuMat(
                        image,
                        Das::ExportInterface::DAS_PIXEL_FORMAT_BGR);
            return Das::DasPtr<Das::ExportInterface::IDasImage>::Attach(raw);
        }

        class FakeTensorBuffer final
            : public Das::ExportInterface::DasBinaryBufferImplBase<
                  FakeTensorBuffer>
        {
        public:
            explicit FakeTensorBuffer(std::vector<float> data)
                : data_(std::move(data))
            {
            }

            DAS_IMPL GetData(unsigned char** pp_out_data) override
            {
                if (!pp_out_data)
                {
                    return DAS_E_INVALID_POINTER;
                }
                *pp_out_data =
                    reinterpret_cast<unsigned char*>(data_.data());
                return DAS_S_OK;
            }

            DAS_IMPL GetSize(uint64_t* p_out_size) override
            {
                if (!p_out_size)
                {
                    return DAS_E_INVALID_POINTER;
                }
                *p_out_size =
                    static_cast<uint64_t>(data_.size() * sizeof(float));
                return DAS_S_OK;
            }

        private:
            std::vector<float> data_;
        };

        class FakeTensor final
            : public Das::ExportInterface::DasTensorImplBase<FakeTensor>
        {
        public:
            FakeTensor(std::vector<int64_t> dims, std::vector<float> data)
                : dims_(std::move(dims)), data_(std::move(data))
            {
            }

            DAS_IMPL GetDim(uint32_t index, int64_t* p_value) override
            {
                if (!p_value)
                {
                    return DAS_E_INVALID_POINTER;
                }
                if (index >= dims_.size())
                {
                    return DAS_E_OUT_OF_RANGE;
                }
                *p_value = dims_[index];
                return DAS_S_OK;
            }

            DAS_IMPL GetRank(uint32_t* p_rank) override
            {
                if (!p_rank)
                {
                    return DAS_E_INVALID_POINTER;
                }
                *p_rank = static_cast<uint32_t>(dims_.size());
                return DAS_S_OK;
            }

            DAS_IMPL GetDataType(
                Das::ExportInterface::DasTensorDataType* p_type) override
            {
                if (!p_type)
                {
                    return DAS_E_INVALID_POINTER;
                }
                *p_type = Das::ExportInterface::DAS_TENSOR_TYPE_FLOAT;
                return DAS_S_OK;
            }

            DAS_IMPL GetBinaryBuffer(
                Das::ExportInterface::IDasBinaryBuffer** pp_out_buffer)
                override
            {
                if (!pp_out_buffer)
                {
                    return DAS_E_INVALID_POINTER;
                }
                *pp_out_buffer = FakeTensorBuffer::MakeRaw(data_);
                return DAS_S_OK;
            }

        private:
            std::vector<int64_t> dims_;
            std::vector<float>   data_;
        };

        class FakeTensorVector final
            : public Das::ExportInterface::DasTensorVectorImplBase<
                  FakeTensorVector>
        {
        public:
            explicit FakeTensorVector(Das::ExportInterface::IDasTensor* tensor)
                : tensor_(tensor)
            {
            }

            DAS_IMPL GetCount(uint32_t* p_count) override
            {
                if (!p_count)
                {
                    return DAS_E_INVALID_POINTER;
                }
                *p_count = tensor_ ? 1U : 0U;
                return DAS_S_OK;
            }

            DAS_IMPL GetAt(
                uint32_t                         index,
                Das::ExportInterface::IDasTensor** pp_out_value) override
            {
                if (!pp_out_value)
                {
                    return DAS_E_INVALID_POINTER;
                }
                if (index != 0 || !tensor_)
                {
                    return DAS_E_OUT_OF_RANGE;
                }
                *pp_out_value = tensor_.Get();
                (*pp_out_value)->AddRef();
                return DAS_S_OK;
            }

        private:
            Das::DasPtr<Das::ExportInterface::IDasTensor> tensor_;
        };

        class FakeSession final
            : public Das::ExportInterface::DasSessionImplBase<FakeSession>
        {
        public:
            DAS_IMPL Run(
                Das::ExportInterface::IDasReadOnlyStringVector*,
                Das::ExportInterface::IDasTensorVector*,
                Das::ExportInterface::IDasReadOnlyStringVector*,
                Das::ExportInterface::IDasTensorVector** pp_outputs) override
            {
                if (!pp_outputs)
                {
                    return DAS_E_INVALID_POINTER;
                }

                auto* tensor = FakeTensor::MakeRaw(
                    std::vector<int64_t>{1, 2, 2},
                    std::vector<float>{0.1F, 0.9F, 0.8F, 0.2F});
                auto* outputs = FakeTensorVector::MakeRaw(tensor);
                tensor->Release();
                *pp_outputs = outputs;
                return DAS_S_OK;
            }
        };

        class FakeAI final
            : public Das::ExportInterface::DasAIImplBase<FakeAI>
        {
        public:
            DAS_IMPL CreateSession(
                IDasReadOnlyString*,
                Das::ExportInterface::IDasJson*,
                Das::ExportInterface::IDasSession** pp_session) override
            {
                if (!pp_session)
                {
                    return DAS_E_INVALID_POINTER;
                }
                *pp_session = FakeSession::MakeRaw();
                return DAS_S_OK;
            }

            DAS_IMPL CreateTensorFromImage(
                Das::ExportInterface::IDasImage*,
                const Das::ExportInterface::DasImageTensorOptions&,
                Das::ExportInterface::IDasTensor**) override
            {
                return DAS_E_FAIL;
            }

            DAS_IMPL CreateOcr(
                IDasReadOnlyString*,
                IDasReadOnlyString*,
                IDasReadOnlyString*,
                Das::ExportInterface::IDasOcr**) override
            {
                return DAS_E_FAIL;
            }
        };

        class FakeOcrResult final
            : public Das::ExportInterface::DasOcrResultImplBase<FakeOcrResult>
        {
        public:
            DAS_IMPL GetText(IDasReadOnlyString** pp_text) override
            {
                if (!pp_text)
                {
                    return DAS_E_INVALID_POINTER;
                }
                DasReadOnlyString value{"A"};
                value.GetImpl(pp_text);
                return DAS_S_OK;
            }

            DAS_IMPL GetBox(Das::ExportInterface::DasRect* p_box) override
            {
                if (!p_box)
                {
                    return DAS_E_INVALID_POINTER;
                }
                *p_box = {2, 2, 12, 8};
                return DAS_S_OK;
            }

            DAS_IMPL GetScore(double* p_score) override
            {
                if (!p_score)
                {
                    return DAS_E_INVALID_POINTER;
                }
                *p_score = 0.9;
                return DAS_S_OK;
            }

            DAS_IMPL GetCharCount(uint32_t* p_count) override
            {
                if (!p_count)
                {
                    return DAS_E_INVALID_POINTER;
                }
                *p_count = 1;
                return DAS_S_OK;
            }

            DAS_IMPL GetCharBox(
                uint32_t index,
                Das::ExportInterface::DasRect* p_box) override
            {
                if (!p_box)
                {
                    return DAS_E_INVALID_POINTER;
                }
                if (index != 0)
                {
                    return DAS_E_OUT_OF_RANGE;
                }
                *p_box = {2, 2, 12, 8};
                return DAS_S_OK;
            }

            DAS_IMPL GetCharScore(uint32_t index, double* p_score) override
            {
                if (!p_score)
                {
                    return DAS_E_INVALID_POINTER;
                }
                if (index != 0)
                {
                    return DAS_E_OUT_OF_RANGE;
                }
                *p_score = 0.9;
                return DAS_S_OK;
            }
        };

        class FakeOcrResultVector final
            : public Das::ExportInterface::DasOcrResultVectorImplBase<
                  FakeOcrResultVector>
        {
        public:
            explicit FakeOcrResultVector(
                Das::ExportInterface::IDasOcrResult* result)
                : result_(result)
            {
            }

            DAS_IMPL GetCount(uint32_t* p_count) override
            {
                if (!p_count)
                {
                    return DAS_E_INVALID_POINTER;
                }
                *p_count = result_ ? 1U : 0U;
                return DAS_S_OK;
            }

            DAS_IMPL GetAt(
                uint32_t                              index,
                Das::ExportInterface::IDasOcrResult** pp_result) override
            {
                if (!pp_result)
                {
                    return DAS_E_INVALID_POINTER;
                }
                if (index != 0 || !result_)
                {
                    return DAS_E_OUT_OF_RANGE;
                }
                *pp_result = result_.Get();
                (*pp_result)->AddRef();
                return DAS_S_OK;
            }

        private:
            Das::DasPtr<Das::ExportInterface::IDasOcrResult> result_;
        };

        class FakeOcr final
            : public Das::ExportInterface::DasOcrImplBase<FakeOcr>
        {
        public:
            DAS_IMPL Recognize(
                Das::ExportInterface::IDasImage*,
                Das::ExportInterface::IDasOcrResultVector** pp_results)
                override
            {
                if (!pp_results)
                {
                    return DAS_E_INVALID_POINTER;
                }
                auto* result = FakeOcrResult::MakeRaw();
                auto* vector = FakeOcrResultVector::MakeRaw(result);
                result->Release();
                *pp_results = vector;
                return DAS_S_OK;
            }
        };

    } // namespace

    TEST_F(
        DebugCvOcrDecoratorTest,
        DebugCvRegistrationTest_EnabledWrapsCvServicesByName)
    {
        const auto dir = UniqueTempDir("EnabledWrapsCvServicesByName");
        InitializeRuntime(dir, "1");

        auto ctx = DAS::Core::IPC::MainProcess::CreateIpcContextEz(false);
        ASSERT_NE(ctx.get(), nullptr);
        DAS::Core::IPC::ScopedCurrentIpcContext scope(
            static_cast<DAS::Core::IPC::MainProcess::IpcContext*>(ctx.get()));

        ASSERT_EQ(RegisterDebugWriterService(*ctx), DAS_S_OK);
        ASSERT_EQ(DAS::Core::OcvWrapper::RegisterCvServices(*ctx), DAS_S_OK);

        IDasBase* base = nullptr;
        ASSERT_EQ(DasQueryMainProcessInterfaceByName("cv.cpu", &base), DAS_S_OK);
        auto base_guard = Das::DasPtr<IDasBase>::Attach(base);

        Das::DasPtr<Das::ExportInterface::IDasCv> cv;
        ASSERT_EQ(base_guard.As(cv), DAS_S_OK);

        auto image = MakePatternImage();
        auto templ = MakePatternTemplate();
        Das::ExportInterface::IDasTemplateMatchResults* raw_results = nullptr;
        ASSERT_EQ(
            cv->TemplateMatchAll(
                image.Get(),
                templ.Get(),
                Das::ExportInterface::DAS_TEMPLATE_MATCH_TYPE_CCOEFF_NORMED,
                0.8,
                3,
                &raw_results),
            DAS_S_OK);
        auto results =
            Das::DasPtr<Das::ExportInterface::IDasTemplateMatchResults>::Attach(
                raw_results);

        ASSERT_EQ(DebugRuntime::Flush(), DAS_S_OK);
        const auto lines = ReadLines(dir / "debug.jsonl");
        EXPECT_TRUE(AnyLineContains(lines, "\"type\":\"template_match_all\""));
        EXPECT_TRUE(AnyLineContains(lines, "\"image_filename\""));
        EXPECT_TRUE(HasAnyPng(dir / "img"));

        static_cast<void>(ctx->UnregisterServiceByName("cv.cpu"));
        static_cast<void>(ctx->UnregisterServiceByName("cv.cuda"));
        static_cast<void>(ctx->UnregisterServiceByName("debug.writer"));
    }

    TEST_F(
        DebugCvOcrDecoratorTest,
        DebugOcrCreateTest_EnabledWrapsCreateOcrResult)
    {
        const auto dir = UniqueTempDir("EnabledWrapsCreateOcrResult");
        InitializeRuntime(dir, "1");

        auto ctx = DAS::Core::IPC::MainProcess::CreateIpcContextEz(false);
        ASSERT_NE(ctx.get(), nullptr);
        DAS::Core::IPC::ScopedCurrentIpcContext scope(
            static_cast<DAS::Core::IPC::MainProcess::IpcContext*>(ctx.get()));
        ASSERT_EQ(RegisterDebugWriterService(*ctx), DAS_S_OK);

        const auto dict_path = dir / "dict.txt";
        std::filesystem::create_directories(dir);
        {
            std::ofstream dict{dict_path};
            dict << "__blank__\nA\n";
        }

        auto* fake_ai = FakeAI::MakeRaw();
        auto  fake_ai_guard =
            Das::DasPtr<Das::ExportInterface::IDasAI>::Attach(fake_ai);

        const std::string rec_model_path = "fake-rec.onnx";
        const std::string dict_path_string = dict_path.string();
        DasReadOnlyString rec_model{rec_model_path.c_str()};
        DasReadOnlyString dict_model{dict_path_string.c_str()};

        Das::ExportInterface::IDasOcr* raw_ocr = nullptr;
        ASSERT_EQ(
            DAS::Core::OrtWrapper::CreateOcrImpl(
                fake_ai_guard.Get(),
                nullptr,
                rec_model.Get(),
                dict_model.Get(),
                &raw_ocr),
            DAS_S_OK);
        auto ocr = Das::DasPtr<Das::ExportInterface::IDasOcr>::Attach(raw_ocr);

        auto image = MakeOcrImage();
        Das::ExportInterface::IDasOcrResultVector* raw_results = nullptr;
        ASSERT_EQ(ocr->Recognize(image.Get(), &raw_results), DAS_S_OK);
        auto results =
            Das::DasPtr<Das::ExportInterface::IDasOcrResultVector>::Attach(
                raw_results);

        ASSERT_EQ(DebugRuntime::Flush(), DAS_S_OK);
        const auto lines = ReadLines(dir / "debug.jsonl");
        EXPECT_TRUE(AnyLineContains(lines, "\"type\":\"ocr_recognize\""));
        EXPECT_TRUE(AnyLineContains(lines, "\"text\":\"A\""));
        EXPECT_TRUE(AnyLineContains(lines, "\"image_filename\""));
        EXPECT_TRUE(HasAnyPng(dir / "img"));

        static_cast<void>(ctx->UnregisterServiceByName("debug.writer"));
    }

    TEST_F(
        DebugCvOcrDecoratorTest,
        DebugCvOcrDisabledNoopTest_DisabledReturnsRawCvAndOcr)
    {
        const auto dir = UniqueTempDir("DisabledReturnsRawCvAndOcr");
        InitializeRuntime(dir, "0");

        auto* raw_cv = DAS::Core::OcvWrapper::CvCpuImpl::MakeRaw();
        auto* maybe_cv = MaybeDecorateCvRaw(raw_cv, "cv.cpu");
        EXPECT_EQ(maybe_cv, raw_cv);
        auto cv = Das::DasPtr<Das::ExportInterface::IDasCv>::Attach(maybe_cv);

        auto image = MakePatternImage();
        auto templ = MakePatternTemplate();
        Das::ExportInterface::IDasTemplateMatchResults* raw_cv_results =
            nullptr;
        ASSERT_EQ(
            cv->TemplateMatchAll(
                image.Get(),
                templ.Get(),
                Das::ExportInterface::DAS_TEMPLATE_MATCH_TYPE_CCOEFF_NORMED,
                0.8,
                1,
                &raw_cv_results),
            DAS_S_OK);
        auto cv_results =
            Das::DasPtr<Das::ExportInterface::IDasTemplateMatchResults>::Attach(
                raw_cv_results);

        auto* raw_ocr = FakeOcr::MakeRaw();
        auto* maybe_ocr = MaybeDecorateOcrRaw(raw_ocr, "ocr");
        EXPECT_EQ(maybe_ocr, raw_ocr);
        auto ocr =
            Das::DasPtr<Das::ExportInterface::IDasOcr>::Attach(maybe_ocr);

        Das::ExportInterface::IDasOcrResultVector* raw_ocr_results = nullptr;
        ASSERT_EQ(ocr->Recognize(MakeOcrImage().Get(), &raw_ocr_results),
                  DAS_S_OK);
        auto ocr_results =
            Das::DasPtr<Das::ExportInterface::IDasOcrResultVector>::Attach(
                raw_ocr_results);

        ASSERT_EQ(DebugRuntime::Flush(), DAS_S_OK);
        EXPECT_FALSE(std::filesystem::exists(dir / "debug.jsonl"));
        EXPECT_FALSE(std::filesystem::exists(dir / "img"));
    }

} // namespace Das::Core::Debug::Test

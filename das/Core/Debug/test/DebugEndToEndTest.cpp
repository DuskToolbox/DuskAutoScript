#include <das/Core/Debug/DebugDecorators.h>
#include <das/Core/Debug/DebugRuntime.h>
#include <das/Core/IPC/CurrentIpcContextScope.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/Core/OcvWrapper/CpuImageImpl.hpp>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasOcr.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasOcrResult.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasOcrResultVector.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasCapture.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasInput.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTouch.Implements.hpp>
#include <gtest/gtest.h>

#include "../../OcvWrapper/src/CvCpuImpl.h"
#include "../src/DebugWriterImpl.h"

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_OPENCV_WARNING
#include <opencv2/imgproc.hpp>
DAS_DISABLE_WARNING_END

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef DAS_WINDOWS
#include <stdlib.h>
#endif

DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Core::Debug::Test::EndToEndDetail,
    FakeCapture,
    0x65050001,
    0x1000,
    0x4000,
    0x80,
    0x00,
    0x65,
    0x05,
    0x00,
    0x00,
    0x00,
    0x01);

DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Core::Debug::Test::EndToEndDetail,
    FakeTouch,
    0x65050002,
    0x1000,
    0x4000,
    0x80,
    0x00,
    0x65,
    0x05,
    0x00,
    0x00,
    0x00,
    0x02);

namespace Das::Core::Debug::Test
{
    namespace EndToEndDetail
    {
        auto UniqueTempDir(const char* test_name) -> std::filesystem::path
        {
            const auto stamp =
                std::chrono::steady_clock::now().time_since_epoch().count();
            auto path =
                std::filesystem::current_path() / "debug-test-output"
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

        class DebugEndToEndTest : public ::testing::Test
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

        auto RegisterWriterForRuntime(const std::filesystem::path& dir)
            -> DAS::Core::IPC::MainProcess::IpcContextPtr
        {
            InitializeRuntime(dir, "1");
            auto ctx = DAS::Core::IPC::MainProcess::CreateIpcContextEz(false);
            EXPECT_NE(ctx.get(), nullptr);
            EXPECT_EQ(RegisterDebugWriterService(*ctx), DAS_S_OK);
            return ctx;
        }

        auto MakeImage(int width = 48, int height = 32)
            -> Das::DasPtr<Das::ExportInterface::IDasImage>
        {
            cv::Mat image = cv::Mat::zeros(height, width, CV_8UC3);
            image.setTo(cv::Scalar{24, 48, 96});
            cv::rectangle(
                image,
                cv::Rect{10, 8, 12, 10},
                cv::Scalar{210, 180, 80},
                cv::FILLED);
            auto* raw =
                OcvWrapper::CpuImageImpl<OcvWrapper::Storage::OwningStorage>::
                    MakeFromCpuMat(
                        image,
                        Das::ExportInterface::DAS_PIXEL_FORMAT_BGR);
            return Das::DasPtr<Das::ExportInterface::IDasImage>::Attach(raw);
        }

        auto MakeTemplate() -> Das::DasPtr<Das::ExportInterface::IDasImage>
        {
            cv::Mat templ(10, 12, CV_8UC3, cv::Scalar{210, 180, 80});
            auto*   raw =
                OcvWrapper::CpuImageImpl<OcvWrapper::Storage::OwningStorage>::
                    MakeFromCpuMat(
                        templ,
                        Das::ExportInterface::DAS_PIXEL_FORMAT_BGR);
            return Das::DasPtr<Das::ExportInterface::IDasImage>::Attach(raw);
        }

        class FakeCapture final
            : public Das::PluginInterface::DasCaptureImplBase<FakeCapture>
        {
        public:
            DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
            {
                if (!p_out_guid)
                {
                    return DAS_E_INVALID_POINTER;
                }
                *p_out_guid = DasIidOf<FakeCapture>();
                return DAS_S_OK;
            }

            DasResult DAS_STD_CALL
            GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
            {
                return CreateIDasReadOnlyStringFromUtf8(
                    "EndToEndFakeCapture",
                    pp_out_name);
            }

            DasResult DAS_STD_CALL
            Capture(Das::ExportInterface::IDasImage** pp_out_image) override
            {
                if (!pp_out_image)
                {
                    return DAS_E_INVALID_POINTER;
                }
                auto image = MakeImage();
                *pp_out_image = image.Get();
                (*pp_out_image)->AddRef();
                return DAS_S_OK;
            }
        };

        class FakeTouch final
            : public Das::PluginInterface::DasTouchImplBase<FakeTouch>
        {
        public:
            DasResult DAS_STD_CALL
            QueryInterface(const DasGuid& iid, void** pp_out_object) override
            {
                if (!pp_out_object)
                {
                    return DAS_E_INVALID_POINTER;
                }

                if (iid == DasIidOf<IDasBase>())
                {
                    *pp_out_object =
                        static_cast<IDasBase*>(static_cast<IDasTypeInfo*>(
                            static_cast<Das::PluginInterface::IDasInput*>(
                                this)));
                }
                else if (iid == DasIidOf<IDasTypeInfo>())
                {
                    *pp_out_object = static_cast<IDasTypeInfo*>(
                        static_cast<Das::PluginInterface::IDasInput*>(this));
                }
                else if (iid == DasIidOf<Das::PluginInterface::IDasInput>())
                {
                    *pp_out_object =
                        static_cast<Das::PluginInterface::IDasInput*>(this);
                }
                else if (iid == DasIidOf<Das::PluginInterface::IDasTouch>())
                {
                    *pp_out_object =
                        static_cast<Das::PluginInterface::IDasTouch*>(this);
                }
                else
                {
                    *pp_out_object = nullptr;
                    return DAS_E_NO_INTERFACE;
                }

                AddRef();
                return DAS_S_OK;
            }

            DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
            {
                if (!p_out_guid)
                {
                    return DAS_E_INVALID_POINTER;
                }
                *p_out_guid = DasIidOf<FakeTouch>();
                return DAS_S_OK;
            }

            DasResult DAS_STD_CALL
            GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
            {
                return CreateIDasReadOnlyStringFromUtf8(
                    "EndToEndFakeTouch",
                    pp_out_name);
            }

            DasResult DAS_STD_CALL Click(int32_t, int32_t) override
            {
                return DAS_S_OK;
            }

            DasResult DAS_STD_CALL Swipe(
                Das::PluginInterface::DasPoint,
                Das::PluginInterface::DasPoint,
                int32_t) override
            {
                return DAS_S_OK;
            }
        };

        class FakeOcrResult final
            : public Das::ExportInterface::DasOcrResultImplBase<FakeOcrResult>
        {
        public:
            DAS_IMPL GetText(IDasReadOnlyString** pp_text) override
            {
                return CreateIDasReadOnlyStringFromUtf8("OK", pp_text);
            }

            DAS_IMPL GetBox(Das::ExportInterface::DasRect* p_box) override
            {
                if (!p_box)
                {
                    return DAS_E_INVALID_POINTER;
                }
                *p_box = {6, 6, 20, 10};
                return DAS_S_OK;
            }

            DAS_IMPL GetScore(double* p_score) override
            {
                if (!p_score)
                {
                    return DAS_E_INVALID_POINTER;
                }
                *p_score = 0.93;
                return DAS_S_OK;
            }

            DAS_IMPL GetCharCount(uint32_t* p_count) override
            {
                if (!p_count)
                {
                    return DAS_E_INVALID_POINTER;
                }
                *p_count = 2;
                return DAS_S_OK;
            }

            DAS_IMPL GetCharBox(
                uint32_t                       index,
                Das::ExportInterface::DasRect* p_box) override
            {
                if (!p_box)
                {
                    return DAS_E_INVALID_POINTER;
                }
                if (index >= 2)
                {
                    return DAS_E_OUT_OF_RANGE;
                }
                *p_box = index == 0
                             ? Das::ExportInterface::DasRect{6, 6, 9, 10}
                             : Das::ExportInterface::DasRect{17, 6, 9, 10};
                return DAS_S_OK;
            }

            DAS_IMPL GetCharScore(uint32_t index, double* p_score) override
            {
                if (!p_score)
                {
                    return DAS_E_INVALID_POINTER;
                }
                if (index >= 2)
                {
                    return DAS_E_OUT_OF_RANGE;
                }
                *p_score = index == 0 ? 0.94 : 0.91;
                return DAS_S_OK;
            }
        };

        class FakeOcrResultVector final
            : public Das::ExportInterface::DasOcrResultVectorImplBase<
                  FakeOcrResultVector>
        {
        public:
            FakeOcrResultVector()
                : result_(
                      Das::ExportInterface::DasOcrResultImplBase<
                          FakeOcrResult>::MakeRaw())
            {
            }

            DAS_IMPL GetCount(uint32_t* p_count) override
            {
                if (!p_count)
                {
                    return DAS_E_INVALID_POINTER;
                }
                *p_count = 1;
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
                if (index != 0)
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
                Das::ExportInterface::IDasOcrResultVector** pp_results) override
            {
                if (!pp_results)
                {
                    return DAS_E_INVALID_POINTER;
                }
                *pp_results = FakeOcrResultVector::MakeRaw();
                return DAS_S_OK;
            }
        };

        auto ReadLines(const std::filesystem::path& path)
            -> std::vector<std::string>
        {
            std::ifstream            input{path};
            std::vector<std::string> lines;
            std::string              line;
            while (std::getline(input, line))
            {
                lines.emplace_back(std::move(line));
            }
            return lines;
        }

        auto ParseJsonLines(const std::filesystem::path& path)
            -> std::vector<yyjson::value>
        {
            std::vector<yyjson::value> values;
            for (const auto& line : ReadLines(path))
            {
                auto parsed = Das::Utils::ParseYyjsonFromString(line);
                EXPECT_TRUE(parsed.has_value()) << line;
                if (parsed)
                {
                    values.emplace_back(std::move(*parsed));
                }
            }
            return values;
        }

        bool HasEventType(
            const std::vector<yyjson::value>& values,
            std::string_view                  expected_type)
        {
            for (const auto& value : values)
            {
                auto object = value.as_object();
                if (!object || !object->contains("type"))
                {
                    continue;
                }
                const auto type =
                    (*object)[std::string_view("type")].as_string();
                if (type && *type == expected_type)
                {
                    return true;
                }
            }
            return false;
        }

        void RunAllFamilies(const std::filesystem::path& dir)
        {
            auto ctx = RegisterWriterForRuntime(dir);
            ASSERT_NE(ctx.get(), nullptr);
            DAS::Core::IPC::ScopedCurrentIpcContext scope(
                static_cast<DAS::Core::IPC::MainProcess::IpcContext*>(
                    ctx.get()));

            auto* decorated_capture = MaybeDecorateCapture(
                FakeCapture::MakeRaw(),
                "end_to_end_capture");
            auto capture =
                Das::DasPtr<Das::PluginInterface::IDasCapture>::Attach(
                    decorated_capture);

            Das::ExportInterface::IDasImage* raw_capture_image = nullptr;
            ASSERT_EQ(capture->Capture(&raw_capture_image), DAS_S_OK);
            auto capture_image =
                Das::DasPtr<Das::ExportInterface::IDasImage>::Attach(
                    raw_capture_image);

            auto* decorated_input =
                MaybeDecorateInput(FakeTouch::MakeRaw(), "end_to_end_touch");
            auto input = Das::DasPtr<Das::PluginInterface::IDasInput>::Attach(
                decorated_input);
            ASSERT_EQ(input->Click(8, 9), DAS_S_OK);

            Das::DasPtr<Das::PluginInterface::IDasTouch> touch;
            ASSERT_EQ(input.As(touch), DAS_S_OK);
            ASSERT_EQ(
                touch->Swipe(
                    Das::PluginInterface::DasPoint{4, 4},
                    Das::PluginInterface::DasPoint{28, 20},
                    120),
                DAS_S_OK);

            auto* decorated_cv =
                MaybeDecorateCvRaw(OcvWrapper::CvCpuImpl::MakeRaw(), "cv.cpu");
            auto cv =
                Das::DasPtr<Das::ExportInterface::IDasCv>::Attach(decorated_cv);
            auto image = MakeImage();
            auto templ = MakeTemplate();

            Das::ExportInterface::IDasTemplateMatchResults* raw_matches =
                nullptr;
            ASSERT_EQ(
                cv->TemplateMatchAll(
                    image.Get(),
                    templ.Get(),
                    Das::ExportInterface::DAS_TEMPLATE_MATCH_TYPE_CCORR_NORMED,
                    0.8,
                    3,
                    &raw_matches),
                DAS_S_OK);
            auto matches =
                Das::DasPtr<Das::ExportInterface::IDasTemplateMatchResults>::
                    Attach(raw_matches);

            Das::ExportInterface::DasColorRange range{
                {0, 0, 0, 0},
                {255, 255, 255, 255}};
            Das::ExportInterface::IDasImage* raw_mask = nullptr;
            ASSERT_EQ(
                cv->ColorFilter(image.Get(), &range, &raw_mask),
                DAS_S_OK);
            auto mask =
                Das::DasPtr<Das::ExportInterface::IDasImage>::Attach(raw_mask);

            Das::ExportInterface::IDasImage* raw_converted = nullptr;
            ASSERT_EQ(
                cv->ConvertColor(
                    image.Get(),
                    Das::ExportInterface::DAS_PIXEL_FORMAT_RGB,
                    &raw_converted),
                DAS_S_OK);
            auto converted =
                Das::DasPtr<Das::ExportInterface::IDasImage>::Attach(
                    raw_converted);

            auto* decorated_ocr =
                MaybeDecorateOcrRaw(FakeOcr::MakeRaw(), "ocr");
            auto ocr = Das::DasPtr<Das::ExportInterface::IDasOcr>::Attach(
                decorated_ocr);
            Das::ExportInterface::IDasOcrResultVector* raw_ocr_results =
                nullptr;
            ASSERT_EQ(ocr->Recognize(image.Get(), &raw_ocr_results), DAS_S_OK);
            auto ocr_results =
                Das::DasPtr<Das::ExportInterface::IDasOcrResultVector>::Attach(
                    raw_ocr_results);

            ASSERT_EQ(DebugRuntime::Flush(), DAS_S_OK);
            std::cout << "DAS_DEBUG_DIR=" << dir.string() << '\n';

            static_cast<void>(ctx->UnregisterServiceByName("debug.writer"));
        }

        template <class TObject>
        void ExpectImageFileIfPresent(
            const std::filesystem::path& dir,
            const TObject&               object,
            std::string_view             key)
        {
            if (!object.contains(key))
            {
                return;
            }
            const auto filename = object[key].as_string();
            if (!filename || filename->empty())
            {
                return;
            }
            EXPECT_TRUE(
                std::filesystem::exists(dir / "img" / std::string{*filename}))
                << *filename;
        }

        auto FindSourceRoot() -> std::filesystem::path
        {
            auto current = std::filesystem::current_path();
            for (;;)
            {
                if (std::filesystem::exists(
                        current / "das" / "Plugins" / "DasAdbTouch"))
                {
                    return current;
                }
                if (!current.has_parent_path()
                    || current.parent_path() == current)
                {
                    return std::filesystem::current_path();
                }
                current = current.parent_path();
            }
        }

    } // namespace EndToEndDetail

    using namespace EndToEndDetail;

    TEST_F(DebugEndToEndTest, DebugDecoratorInjection_AllFourFamiliesEmitJsonl)
    {
        const auto dir = UniqueTempDir("AllFourFamiliesEmitJsonl");
        RunAllFamilies(dir);

        const auto values = ParseJsonLines(dir / "debug.jsonl");
        ASSERT_FALSE(values.empty());

        EXPECT_TRUE(HasEventType(values, "capture"));
        EXPECT_TRUE(HasEventType(values, "input_click"));
        EXPECT_TRUE(HasEventType(values, "input_swipe"));
        EXPECT_TRUE(HasEventType(values, "template_match_all"));
        EXPECT_TRUE(HasEventType(values, "color_filter"));
        EXPECT_TRUE(HasEventType(values, "convert_color"));
        EXPECT_TRUE(HasEventType(values, "ocr_recognize"));
    }

    TEST_F(DebugEndToEndTest, DebugWriterJsonl_RequiredSchemaAndImageLinks)
    {
        const auto dir = UniqueTempDir("RequiredSchemaAndImageLinks");
        RunAllFamilies(dir);

        const auto values = ParseJsonLines(dir / "debug.jsonl");
        ASSERT_FALSE(values.empty());

        for (const auto& value : values)
        {
            auto object = value.as_object();
            ASSERT_TRUE(object);

            for (const auto key :
                 {std::string_view{"step"},
                  std::string_view{"type"},
                  std::string_view{"timestamp"},
                  std::string_view{"params"},
                  std::string_view{"result"},
                  std::string_view{"elapsed_ms"},
                  std::string_view{"thread_id"},
                  std::string_view{"process_pid"},
                  std::string_view{"image_filename"}})
            {
                EXPECT_TRUE(object->contains(key)) << key;
            }

            ExpectImageFileIfPresent(dir, *object, "image_filename");

            auto result = (*object)[std::string_view("result")].as_object();
            if (result)
            {
                ExpectImageFileIfPresent(
                    dir,
                    *result,
                    "original_image_filename");
                ExpectImageFileIfPresent(dir, *result, "image_filename");
            }
        }
    }

    TEST_F(DebugEndToEndTest, DebugDisabledNoop_NoArtifactsAndRawObjects)
    {
        const auto dir = UniqueTempDir("NoArtifactsAndRawObjects");
        InitializeRuntime(dir, "0");

        auto* raw_capture = FakeCapture::MakeRaw();
        auto* maybe_capture =
            MaybeDecorateCapture(raw_capture, "disabled_capture");
        EXPECT_EQ(maybe_capture, raw_capture);
        auto capture = Das::DasPtr<Das::PluginInterface::IDasCapture>::Attach(
            maybe_capture);
        Das::ExportInterface::IDasImage* raw_image = nullptr;
        ASSERT_EQ(capture->Capture(&raw_image), DAS_S_OK);
        auto image =
            Das::DasPtr<Das::ExportInterface::IDasImage>::Attach(raw_image);

        auto* raw_input = FakeTouch::MakeRaw();
        auto* maybe_input = MaybeDecorateInput(raw_input, "disabled_input");
        EXPECT_EQ(
            maybe_input,
            static_cast<Das::PluginInterface::IDasInput*>(raw_input));
        auto input =
            Das::DasPtr<Das::PluginInterface::IDasInput>::Attach(maybe_input);
        ASSERT_EQ(input->Click(1, 2), DAS_S_OK);

        auto* raw_cv = OcvWrapper::CvCpuImpl::MakeRaw();
        auto* maybe_cv = MaybeDecorateCvRaw(raw_cv, "cv.cpu");
        EXPECT_EQ(maybe_cv, raw_cv);
        auto cv = Das::DasPtr<Das::ExportInterface::IDasCv>::Attach(maybe_cv);
        auto templ = MakeTemplate();
        Das::ExportInterface::IDasTemplateMatchResults* raw_matches = nullptr;
        ASSERT_EQ(
            cv->TemplateMatchAll(
                image.Get(),
                templ.Get(),
                Das::ExportInterface::DAS_TEMPLATE_MATCH_TYPE_CCORR_NORMED,
                0.8,
                1,
                &raw_matches),
            DAS_S_OK);
        auto matches =
            Das::DasPtr<Das::ExportInterface::IDasTemplateMatchResults>::Attach(
                raw_matches);

        auto* raw_ocr = FakeOcr::MakeRaw();
        auto* maybe_ocr = MaybeDecorateOcrRaw(raw_ocr, "ocr");
        EXPECT_EQ(maybe_ocr, raw_ocr);
        auto ocr =
            Das::DasPtr<Das::ExportInterface::IDasOcr>::Attach(maybe_ocr);
        Das::ExportInterface::IDasOcrResultVector* raw_ocr_results = nullptr;
        ASSERT_EQ(ocr->Recognize(image.Get(), &raw_ocr_results), DAS_S_OK);
        auto ocr_results =
            Das::DasPtr<Das::ExportInterface::IDasOcrResultVector>::Attach(
                raw_ocr_results);

        ASSERT_EQ(DebugRuntime::Flush(), DAS_S_OK);
        EXPECT_FALSE(std::filesystem::exists(dir / "debug.jsonl"));
        EXPECT_FALSE(std::filesystem::exists(dir / "img"));
    }

    TEST_F(DebugEndToEndTest, DebugPluginTransparent_SourceAudit)
    {
        const auto                               root = FindSourceRoot();
        const std::vector<std::filesystem::path> plugin_dirs{
            root / "das" / "Plugins" / "DasWindowsCapture",
            root / "das" / "Plugins" / "DasAdbCapture",
            root / "das" / "Plugins" / "DasAdbTouch"};
        const std::vector<std::string_view> needles{
            "Debug",
            "DasDebug",
            "DebugDecorator"};

        for (const auto& plugin_dir : plugin_dirs)
        {
            ASSERT_TRUE(std::filesystem::exists(plugin_dir))
                << plugin_dir.string();
            for (const auto& entry :
                 std::filesystem::recursive_directory_iterator(plugin_dir))
            {
                if (!entry.is_regular_file())
                {
                    continue;
                }
                std::ifstream     input(entry.path(), std::ios::binary);
                const std::string content{
                    std::istreambuf_iterator<char>{input},
                    std::istreambuf_iterator<char>{}};
                for (const auto needle : needles)
                {
                    EXPECT_EQ(content.find(needle), std::string::npos)
                        << entry.path().string() << " contains " << needle;
                }
            }
        }
    }

} // namespace Das::Core::Debug::Test

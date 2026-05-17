#include <das/Core/Debug/DebugImageAnnotator.h>
#include <das/Core/Debug/DebugRuntime.h>
#include <das/Core/OcvWrapper/CpuImageImpl.hpp>
#include <das/DasPtr.hpp>
#include <gtest/gtest.h>

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_OPENCV_WARNING
#include <opencv2/imgcodecs.hpp>
DAS_DISABLE_WARNING_END

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

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

        class DebugImageAnnotationTest : public ::testing::Test
        {
        protected:
            void TearDown() override { ResetRuntime(); }
        };

        void InitializeEnabledRuntime(const std::filesystem::path& dir)
        {
            SetDasDebug("1");
            DebugRuntimeOptions options{};
            options.debug_dir = dir;
            ASSERT_EQ(DebugRuntime::Initialize(options), DAS_S_OK);
        }

        auto MakeImage(int width = 48, int height = 32)
            -> Das::DasPtr<Das::ExportInterface::IDasImage>
        {
            cv::Mat mat = cv::Mat::zeros(height, width, CV_8UC3);
            auto*   image =
                OcvWrapper::CpuImageImpl<OcvWrapper::Storage::OwningStorage>::
                    MakeFromCpuMat(
                        mat,
                        Das::ExportInterface::DAS_PIXEL_FORMAT_BGR);
            return Das::DasPtr<Das::ExportInterface::IDasImage>::Attach(image);
        }

        void ExpectBgrPixel(
            const cv::Mat&   image,
            int              x,
            int              y,
            const cv::Vec3b& expected)
        {
            ASSERT_FALSE(image.empty());
            ASSERT_LT(y, image.rows);
            ASSERT_LT(x, image.cols);
            EXPECT_EQ(image.at<cv::Vec3b>(y, x), expected);
        }

    } // namespace

    TEST_F(
        DebugImageAnnotationTest,
        TemplateMatchAllWritesOriginalAndAnnotatedPng)
    {
        const auto dir = UniqueTempDir("TemplateMatchAllWritesPng");
        InitializeEnabledRuntime(dir);

        auto         snapshot = CaptureImageSnapshot(MakeImage().Get());
        DebugDrawBox box{};
        box.rect = {4, 4, 20, 16};
        box.color = DebugAnnotationColor::Green;

        const auto image_result =
            SaveOriginalAndAnnotated("template_match_all", snapshot, {box});
        ASSERT_EQ(DebugRuntime::Flush(), DAS_S_OK);

        EXPECT_NE(
            image_result.original_image_filename.find("template_match_all"),
            std::string::npos);
        EXPECT_NE(
            image_result.image_filename.find("annotated_"),
            std::string::npos);
        EXPECT_NE(
            image_result.image_filename.find("template_match_all"),
            std::string::npos);

        const auto original_path =
            dir / "img" / image_result.original_image_filename;
        const auto annotated_path = dir / "img" / image_result.image_filename;
        EXPECT_TRUE(std::filesystem::exists(original_path));
        ASSERT_TRUE(std::filesystem::exists(annotated_path));

        const auto annotated = cv::imread(annotated_path.string());
        ExpectBgrPixel(annotated, 4, 4, cv::Vec3b{0, 255, 0});
    }

    TEST_F(DebugImageAnnotationTest, OcrWritesLineAndCharBoxes)
    {
        const auto dir = UniqueTempDir("OcrWritesLineAndCharBoxes");
        InitializeEnabledRuntime(dir);

        auto snapshot = CaptureImageSnapshot(MakeImage().Get());

        DebugDrawBox line{};
        line.rect = {3, 3, 36, 18};
        line.color = DebugAnnotationColor::Green;

        DebugDrawBox ch{};
        ch.rect = {8, 8, 8, 10};
        ch.color = DebugAnnotationColor::Yellow;

        const auto image_result =
            SaveOriginalAndAnnotated("ocr_recognize", snapshot, {line, ch});
        ASSERT_EQ(DebugRuntime::Flush(), DAS_S_OK);

        const auto annotated_path = dir / "img" / image_result.image_filename;
        ASSERT_TRUE(std::filesystem::exists(annotated_path));

        const auto annotated = cv::imread(annotated_path.string());
        ExpectBgrPixel(annotated, 3, 3, cv::Vec3b{0, 255, 0});
        ExpectBgrPixel(annotated, 8, 8, cv::Vec3b{0, 255, 255});
    }

} // namespace Das::Core::Debug::Test

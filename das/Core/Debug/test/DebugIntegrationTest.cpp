#include <das/Core/Debug/DebugDecorators.h>
#include <das/Core/Debug/DebugImageAnnotator.h>
#include <das/Core/Debug/DebugRuntime.h>
#include <das/Core/ForeignInterfaceHost/IDasCaptureManagerImpl.h>
#include <das/Core/ForeignInterfaceHost/InputFactoryManager.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/ForeignInterfaceHost/PluginManagerServiceImpl.h>
#include <das/Core/IPC/CurrentIpcContextScope.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/Core/OcvWrapper/CpuImageImpl.hpp>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasSharedRef.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasCapture.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasCaptureFactory.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasInput.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasInputFactory.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTouch.Implements.hpp>
#include <gtest/gtest.h>

#include "../src/DebugWriterImpl.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_OPENCV_WARNING
#include <opencv2/core/mat.hpp>
DAS_DISABLE_WARNING_END

#ifdef DAS_WINDOWS
#include <stdlib.h>
#endif

DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Core::Debug::Test::Detail,
    FakeCapture,
    0x65040001,
    0x1000,
    0x4000,
    0x80,
    0x00,
    0x65,
    0x04,
    0x00,
    0x00,
    0x00,
    0x01);

DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Core::Debug::Test::Detail,
    FakeCaptureFactory,
    0x65040002,
    0x1000,
    0x4000,
    0x80,
    0x00,
    0x65,
    0x04,
    0x00,
    0x00,
    0x00,
    0x02);

DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Core::Debug::Test::Detail,
    FakeInput,
    0x65040003,
    0x1000,
    0x4000,
    0x80,
    0x00,
    0x65,
    0x04,
    0x00,
    0x00,
    0x00,
    0x03);

DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Core::Debug::Test::Detail,
    FakeInputFactory,
    0x65040005,
    0x1000,
    0x4000,
    0x80,
    0x00,
    0x65,
    0x04,
    0x00,
    0x00,
    0x00,
    0x05);

namespace Das::Core::Debug::Test
{
    namespace Detail
    {
        auto MakeGuid(uint32_t value) -> DasGuid
        {
            DasGuid guid{};
            guid.data1 = value;
            return guid;
        }

        auto UniqueTempDir(const char* test_name) -> std::filesystem::path
        {
            const auto stamp =
                std::chrono::steady_clock::now().time_since_epoch().count();
            return std::filesystem::current_path()
                / "debug-integration-output"
                / (std::string{test_name} + "-" + std::to_string(stamp));
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

        void InitializeRuntime(const std::filesystem::path& dir, bool enabled)
        {
            SetDasDebug(enabled ? "1" : "0");
            DebugRuntimeOptions options{};
            options.debug_dir = dir;
            ASSERT_EQ(DebugRuntime::Initialize(options), DAS_S_OK);
        }

        auto RegisterWriterForRuntime(const std::filesystem::path& dir)
            -> DAS::Core::IPC::MainProcess::IpcContextPtr
        {
            InitializeRuntime(dir, true);
            auto ctx = DAS::Core::IPC::MainProcess::CreateIpcContextEz(false);
            EXPECT_NE(ctx.get(), nullptr);
            DAS::Core::IPC::ScopedCurrentIpcContext scope(
                static_cast<DAS::Core::IPC::MainProcess::IpcContext*>(
                    ctx.get()));
            EXPECT_EQ(RegisterDebugWriterService(*ctx), DAS_S_OK);
            return ctx;
        }

        auto ReadLines(const std::filesystem::path& path)
            -> std::vector<std::string>
        {
            std::ifstream input{path};
            std::vector<std::string> lines;
            std::string line;
            while (std::getline(input, line))
            {
                lines.emplace_back(std::move(line));
            }
            return lines;
        }

        void ExpectFileContains(
            const std::filesystem::path& path,
            std::string_view             needle)
        {
            const auto lines = ReadLines(path);
            ASSERT_FALSE(lines.empty()) << path.string();

            bool found = false;
            for (const auto& line : lines)
            {
                found = found || line.find(needle) != std::string::npos;
            }
            EXPECT_TRUE(found) << "Missing " << needle << " in "
                               << path.string();
        }

        auto CountPngFiles(const std::filesystem::path& dir) -> size_t
        {
            if (!std::filesystem::exists(dir))
            {
                return 0;
            }

            size_t count = 0;
            for (const auto& entry : std::filesystem::directory_iterator(dir))
            {
                if (entry.is_regular_file()
                    && entry.path().extension() == ".png")
                {
                    ++count;
                }
            }
            return count;
        }

        auto MakeSnapshot() -> std::shared_ptr<DebugImageSnapshot>
        {
            cv::Mat mat = cv::Mat::zeros(40, 60, CV_8UC3);
            mat.setTo(cv::Scalar{20, 40, 80});
            auto* image =
                OcvWrapper::CpuImageImpl<OcvWrapper::Storage::OwningStorage>::
                    MakeFromCpuMat(
                        mat,
                        ExportInterface::DAS_PIXEL_FORMAT_BGR);
            DasPtr<ExportInterface::IDasImage> image_guard =
                DasPtr<ExportInterface::IDasImage>::Attach(image);
            return CaptureImageSnapshot(image);
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

        class FakeCapture final
            : public PluginInterface::DasCaptureImplBase<FakeCapture>
        {
        public:
            DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
            {
                if (!p_out_guid)
                {
                    return DAS_E_INVALID_POINTER;
                }
                *p_out_guid = MakeGuid(0x65040001);
                return DAS_S_OK;
            }

            DasResult DAS_STD_CALL
            GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
            {
                return CreateIDasReadOnlyStringFromUtf8(
                    "FakeCapture",
                    pp_out_name);
            }

            DasResult DAS_STD_CALL
            Capture(ExportInterface::IDasImage** pp_out_image) override
            {
                if (!pp_out_image)
                {
                    return DAS_E_INVALID_POINTER;
                }

                cv::Mat mat = cv::Mat::zeros(32, 48, CV_8UC3);
                mat.setTo(cv::Scalar{30, 60, 90});
                auto* image = OcvWrapper::CpuImageImpl<
                    OcvWrapper::Storage::OwningStorage>::MakeFromCpuMat(
                    mat,
                    ExportInterface::DAS_PIXEL_FORMAT_BGR);
                *pp_out_image = image;
                ++capture_count;
                return DAS_S_OK;
            }

            int capture_count{0};
        };

        class FakeCaptureFactory final
            : public PluginInterface::DasCaptureFactoryImplBase<
                  FakeCaptureFactory>
        {
        public:
            DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
            {
                if (!p_out_guid)
                {
                    return DAS_E_INVALID_POINTER;
                }
                *p_out_guid = MakeGuid(0x65040002);
                return DAS_S_OK;
            }

            DasResult DAS_STD_CALL
            GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
            {
                return CreateIDasReadOnlyStringFromUtf8(
                    "FakeCaptureFactory",
                    pp_out_name);
            }

            DasResult DAS_STD_CALL CreateInstance(
                IDasReadOnlyString*,
                IDasReadOnlyString*,
                PluginInterface::IDasCapture** pp_out_object) override
            {
                if (!pp_out_object)
                {
                    return DAS_E_INVALID_POINTER;
                }

                auto* capture = FakeCapture::MakeRaw();
                last_created = capture;
                *pp_out_object = capture;
                return DAS_S_OK;
            }

            FakeCapture* last_created{nullptr};
        };

        class FakeInput final
            : public PluginInterface::DasInputImplBase<FakeInput>
        {
        public:
            DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
            {
                if (!p_out_guid)
                {
                    return DAS_E_INVALID_POINTER;
                }
                *p_out_guid = MakeGuid(0x65040003);
                return DAS_S_OK;
            }

            DasResult DAS_STD_CALL
            GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
            {
                return CreateIDasReadOnlyStringFromUtf8(
                    "FakeInput",
                    pp_out_name);
            }

            DasResult DAS_STD_CALL Click(int32_t x, int32_t y) override
            {
                last_x = x;
                last_y = y;
                ++click_count;
                return DAS_S_OK;
            }

            int32_t last_x{0};
            int32_t last_y{0};
            int     click_count{0};
        };

        class FakeTouch final
            : public PluginInterface::DasTouchImplBase<FakeTouch>
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
                    *pp_out_object = static_cast<IDasBase*>(
                        static_cast<IDasTypeInfo*>(
                            static_cast<PluginInterface::IDasInput*>(this)));
                }
                else if (iid == DasIidOf<IDasTypeInfo>())
                {
                    *pp_out_object = static_cast<IDasTypeInfo*>(
                        static_cast<PluginInterface::IDasInput*>(this));
                }
                else if (iid == DasIidOf<PluginInterface::IDasInput>())
                {
                    *pp_out_object =
                        static_cast<PluginInterface::IDasInput*>(this);
                }
                else if (iid == DasIidOf<PluginInterface::IDasTouch>())
                {
                    *pp_out_object =
                        static_cast<PluginInterface::IDasTouch*>(this);
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
                *p_out_guid = MakeGuid(0x65040004);
                return DAS_S_OK;
            }

            DasResult DAS_STD_CALL
            GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
            {
                return CreateIDasReadOnlyStringFromUtf8(
                    "FakeTouch",
                    pp_out_name);
            }

            DasResult DAS_STD_CALL Click(int32_t x, int32_t y) override
            {
                last_x = x;
                last_y = y;
                ++click_count;
                return DAS_S_OK;
            }

            DasResult DAS_STD_CALL Swipe(
                PluginInterface::DasPoint from,
                PluginInterface::DasPoint to,
                int32_t                   duration_ms) override
            {
                last_from = from;
                last_to = to;
                last_duration_ms = duration_ms;
                ++swipe_count;
                return DAS_S_OK;
            }

            PluginInterface::DasPoint last_from{};
            PluginInterface::DasPoint last_to{};
            int32_t                   last_duration_ms{0};
            int32_t                   last_x{0};
            int32_t                   last_y{0};
            int                       click_count{0};
            int                       swipe_count{0};
        };

        class FakeInputFactory final
            : public PluginInterface::DasInputFactoryImplBase<
                  FakeInputFactory>
        {
        public:
            explicit FakeInputFactory(
                bool create_touch = false,
                bool return_null_success = false)
                : create_touch_(create_touch),
                  return_null_success_(return_null_success)
            {
            }

            DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
            {
                if (!p_out_guid)
                {
                    return DAS_E_INVALID_POINTER;
                }
                *p_out_guid = MakeGuid(0x65040005);
                return DAS_S_OK;
            }

            DasResult DAS_STD_CALL
            GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
            {
                return CreateIDasReadOnlyStringFromUtf8(
                    "FakeInputFactory",
                    pp_out_name);
            }

            DasResult DAS_STD_CALL CreateInstance(
                IDasReadOnlyString*,
                PluginInterface::IDasInput** pp_out_input) override
            {
                if (!pp_out_input)
                {
                    return DAS_E_INVALID_POINTER;
                }

                if (return_null_success_)
                {
                    *pp_out_input = nullptr;
                    return DAS_S_OK;
                }

                if (create_touch_)
                {
                    auto* touch = FakeTouch::MakeRaw();
                    last_touch = touch;
                    last_input = static_cast<PluginInterface::IDasInput*>(
                        touch);
                    *pp_out_input = last_input;
                    return DAS_S_OK;
                }

                auto* input = FakeInput::MakeRaw();
                last_plain_input = input;
                last_input = input;
                *pp_out_input = input;
                return DAS_S_OK;
            }

            FakeInput*                  last_plain_input{nullptr};
            FakeTouch*                  last_touch{nullptr};
            PluginInterface::IDasInput* last_input{nullptr};

        private:
            bool create_touch_{false};
            bool return_null_success_{false};
        };

        class PluginManagerHarness
        {
        public:
            PluginManagerHarness()
                : settings_dir_(UniqueTempDir("settings")),
                  settings_manager_(settings_dir_),
                  ipc_sp_(
                      DAS::Core::IPC::MainProcess::CreateIpcContextShared(
                          false)),
                  plugin_manager_(
                      settings_manager_,
                      Das::DasSharedRef<
                          DAS::Core::IPC::MainProcess::IIpcContext>(ipc_sp_))
            {
            }

            ~PluginManagerHarness()
            {
                plugin_manager_.Shutdown();
                std::filesystem::remove_all(settings_dir_);
            }

            ForeignInterfaceHost::PluginManager& PluginManager()
            {
                return plugin_manager_;
            }

        private:
            std::filesystem::path settings_dir_;
            DAS::Core::SettingsManager::SettingsManager settings_manager_;
            std::shared_ptr<DAS::Core::IPC::MainProcess::IIpcContext> ipc_sp_;
            ForeignInterfaceHost::PluginManager plugin_manager_;
        };

        class DebugIntegrationFixture : public ::testing::Test
        {
        protected:
            void TearDown() override { ResetRuntime(); }
        };

        class DebugCaptureManagerTest : public DebugIntegrationFixture
        {
        };

        class DebugInputFactoryTest : public DebugIntegrationFixture
        {
        };

        class DebugTouchDecoratorTest : public DebugIntegrationFixture
        {
        };

        class DebugPluginTransparentTest : public DebugIntegrationFixture
        {
        };

        class DebugCaptureInputDisabledNoopTest
            : public DebugIntegrationFixture
        {
        };

    } // namespace Detail

    using namespace Detail;

    TEST_F(
        DebugCaptureManagerTest,
        EnabledWrapsSuccessfulCaptureBeforeAddInstance)
    {
        const auto dir = UniqueTempDir("CaptureManagerEnabled");
        auto ctx = RegisterWriterForRuntime(dir);

        PluginManagerHarness harness;
        auto* raw_factory = FakeCaptureFactory::MakeRaw();
        DasPtr<PluginInterface::IDasCaptureFactory> factory_guard =
            DasPtr<PluginInterface::IDasCaptureFactory>::Attach(raw_factory);
        harness.PluginManager().RegisterTestFeature(
            PluginInterface::DAS_PLUGIN_FEATURE_CAPTURE_FACTORY,
            MakeGuid(0x6504F001),
            static_cast<IDasBase*>(factory_guard.Get()));

        ForeignInterfaceHost::PluginManagerServiceImpl service(
            harness.PluginManager(),
            dir);
        ExportInterface::IDasCaptureManager* p_raw_manager = nullptr;
        ASSERT_EQ(service.CreateCaptureManager(nullptr, &p_raw_manager), DAS_S_OK);
        auto manager =
            DasPtr<ExportInterface::IDasCaptureManager>::Attach(p_raw_manager);

        PluginInterface::IDasCapture* p_raw_capture = nullptr;
        ASSERT_EQ(manager->EnumInterface(0, &p_raw_capture), DAS_S_OK);
        auto capture =
            DasPtr<PluginInterface::IDasCapture>::Attach(p_raw_capture);
        EXPECT_NE(capture.Get(), raw_factory->last_created);

        ExportInterface::IDasImage* p_raw_image = nullptr;
        ASSERT_EQ(capture->Capture(&p_raw_image), DAS_S_OK);
        auto image_guard =
            DasPtr<ExportInterface::IDasImage>::Attach(p_raw_image);

        ASSERT_EQ(DebugRuntime::Flush(), DAS_S_OK);
        ExpectFileContains(dir / "debug.jsonl", "\"type\":\"capture\"");
        EXPECT_GE(CountPngFiles(dir / "img"), 2U);
        EXPECT_NE(DebugRuntime::GetLatestImage(), nullptr);

        ctx->UnregisterServiceByName("debug.writer");
    }

    TEST_F(
        DebugInputFactoryTest,
        EnabledCreateInstanceReturnsDecoratedInput)
    {
        const auto dir = UniqueTempDir("InputFactoryEnabled");
        auto ctx = RegisterWriterForRuntime(dir);
        DebugRuntime::SetLatestImage(MakeSnapshot());

        auto* raw_factory = FakeInputFactory::MakeRaw(false);
        DasPtr<PluginInterface::IDasInputFactory> factory_guard =
            DasPtr<PluginInterface::IDasInputFactory>::Attach(raw_factory);

        ForeignInterfaceHost::InputFactoryManager manager;
        ASSERT_EQ(manager.Register(raw_factory), DAS_S_OK);

        DasPtr<PluginInterface::IDasInputFactory> stored_factory;
        manager.At(0, stored_factory);
        EXPECT_NE(stored_factory.Get(), raw_factory);

        PluginInterface::IDasInput* p_raw_input = nullptr;
        ASSERT_EQ(stored_factory->CreateInstance(nullptr, &p_raw_input), DAS_S_OK);
        auto input = DasPtr<PluginInterface::IDasInput>::Attach(p_raw_input);
        EXPECT_NE(input.Get(), raw_factory->last_input);

        ASSERT_EQ(input->Click(10, 11), DAS_S_OK);
        ASSERT_EQ(DebugRuntime::Flush(), DAS_S_OK);
        ExpectFileContains(dir / "debug.jsonl", "\"type\":\"input_click\"");
        EXPECT_GE(CountPngFiles(dir / "img"), 2U);

        ctx->UnregisterServiceByName("debug.writer");
    }

    TEST_F(
        DebugInputFactoryTest,
        EnabledCreateInstanceNullSuccessReturnsInvalidPointer)
    {
        const auto dir = UniqueTempDir("InputFactoryNullSuccess");
        auto ctx = RegisterWriterForRuntime(dir);

        auto* raw_factory = FakeInputFactory::MakeRaw(false, true);
        DasPtr<PluginInterface::IDasInputFactory> factory_guard =
            DasPtr<PluginInterface::IDasInputFactory>::Attach(raw_factory);

        ForeignInterfaceHost::InputFactoryManager manager;
        ASSERT_EQ(manager.Register(raw_factory), DAS_S_OK);

        DasPtr<PluginInterface::IDasInputFactory> stored_factory;
        manager.At(0, stored_factory);

        PluginInterface::IDasInput* p_raw_input = nullptr;
        EXPECT_EQ(
            stored_factory->CreateInstance(nullptr, &p_raw_input),
            DAS_E_INVALID_POINTER);
        EXPECT_EQ(p_raw_input, nullptr);

        ctx->UnregisterServiceByName("debug.writer");
    }

    TEST_F(DebugInputFactoryTest, EnabledGetObjectByFeatureWrapsDirectFactory)
    {
        const auto dir = UniqueTempDir("InputFactoryDirectFeature");
        auto ctx = RegisterWriterForRuntime(dir);
        DebugRuntime::SetLatestImage(MakeSnapshot());

        PluginManagerHarness harness;
        auto* raw_factory = FakeInputFactory::MakeRaw(false);
        DasPtr<PluginInterface::IDasInputFactory> factory_guard =
            DasPtr<PluginInterface::IDasInputFactory>::Attach(raw_factory);
        harness.PluginManager().RegisterTestFeature(
            PluginInterface::DAS_PLUGIN_FEATURE_INPUT_FACTORY,
            MakeGuid(0x6504F002),
            static_cast<IDasBase*>(factory_guard.Get()));

        PluginInterface::IDasInputFactory* p_raw_feature_factory = nullptr;
        ASSERT_EQ(
            harness.PluginManager().GetObjectByFeature(
                PluginInterface::DAS_PLUGIN_FEATURE_INPUT_FACTORY,
                DasIidOf<PluginInterface::IDasInputFactory>(),
                reinterpret_cast<void**>(&p_raw_feature_factory)),
            DAS_S_OK);
        auto feature_factory =
            DasPtr<PluginInterface::IDasInputFactory>::Attach(
                p_raw_feature_factory);
        EXPECT_NE(feature_factory.Get(), raw_factory);

        PluginInterface::IDasInput* p_raw_input = nullptr;
        ASSERT_EQ(
            feature_factory->CreateInstance(nullptr, &p_raw_input),
            DAS_S_OK);
        auto input = DasPtr<PluginInterface::IDasInput>::Attach(p_raw_input);
        ASSERT_EQ(input->Click(12, 13), DAS_S_OK);
        ASSERT_EQ(DebugRuntime::Flush(), DAS_S_OK);
        ExpectFileContains(dir / "debug.jsonl", "\"type\":\"input_click\"");

        ctx->UnregisterServiceByName("debug.writer");
    }

    TEST_F(DebugTouchDecoratorTest, PreservesIDasTouchAndLogsSwipe)
    {
        const auto dir = UniqueTempDir("TouchDecoratorEnabled");
        auto ctx = RegisterWriterForRuntime(dir);
        DebugRuntime::SetLatestImage(MakeSnapshot());

        auto* raw_touch = FakeTouch::MakeRaw();
        auto* raw_input = static_cast<PluginInterface::IDasInput*>(raw_touch);
        auto* decorated_raw = MaybeDecorateInput(raw_input, "fake_touch");
        auto decorated_input =
            DasPtr<PluginInterface::IDasInput>::Attach(decorated_raw);

        DasPtr<PluginInterface::IDasTouch> decorated_touch;
        ASSERT_EQ(decorated_input.As(decorated_touch), DAS_S_OK);
        ASSERT_NE(decorated_touch.Get(), nullptr);

        PluginInterface::DasPoint from{1, 2};
        PluginInterface::DasPoint to{30, 31};
        ASSERT_EQ(decorated_touch->Swipe(from, to, 250), DAS_S_OK);
        ASSERT_EQ(DebugRuntime::Flush(), DAS_S_OK);
        ExpectFileContains(dir / "debug.jsonl", "\"type\":\"input_swipe\"");
        EXPECT_GE(CountPngFiles(dir / "img"), 2U);

        ctx->UnregisterServiceByName("debug.writer");
    }

    TEST_F(DebugPluginTransparentTest, NoPluginSourceReferencesDebug)
    {
        const auto root = FindSourceRoot();
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

                std::ifstream input(entry.path(), std::ios::binary);
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

    TEST_F(
        DebugCaptureInputDisabledNoopTest,
        DisabledReturnsRawFactoryAndCapture)
    {
        const auto dir = UniqueTempDir("CaptureInputDisabledNoop");
        InitializeRuntime(dir, false);

        auto* raw_input_factory = FakeInputFactory::MakeRaw(false);
        DasPtr<PluginInterface::IDasInputFactory> input_factory_guard =
            DasPtr<PluginInterface::IDasInputFactory>::Attach(
                raw_input_factory);
        ForeignInterfaceHost::InputFactoryManager input_manager;
        ASSERT_EQ(input_manager.Register(raw_input_factory), DAS_S_OK);

        DasPtr<PluginInterface::IDasInputFactory> stored_factory;
        input_manager.At(0, stored_factory);
        EXPECT_EQ(stored_factory.Get(), raw_input_factory);

        PluginInterface::IDasInput* p_raw_input = nullptr;
        ASSERT_EQ(stored_factory->CreateInstance(nullptr, &p_raw_input), DAS_S_OK);
        auto input = DasPtr<PluginInterface::IDasInput>::Attach(p_raw_input);
        EXPECT_EQ(input.Get(), raw_input_factory->last_input);
        ASSERT_EQ(input->Click(3, 4), DAS_S_OK);

        PluginManagerHarness harness;
        auto* raw_capture_factory = FakeCaptureFactory::MakeRaw();
        DasPtr<PluginInterface::IDasCaptureFactory> capture_factory_guard =
            DasPtr<PluginInterface::IDasCaptureFactory>::Attach(
                raw_capture_factory);
        harness.PluginManager().RegisterTestFeature(
            PluginInterface::DAS_PLUGIN_FEATURE_CAPTURE_FACTORY,
            MakeGuid(0x6504F003),
            static_cast<IDasBase*>(capture_factory_guard.Get()));

        ForeignInterfaceHost::PluginManagerServiceImpl service(
            harness.PluginManager(),
            dir);
        ExportInterface::IDasCaptureManager* p_raw_manager = nullptr;
        ASSERT_EQ(service.CreateCaptureManager(nullptr, &p_raw_manager), DAS_S_OK);
        auto capture_manager =
            DasPtr<ExportInterface::IDasCaptureManager>::Attach(p_raw_manager);

        PluginInterface::IDasCapture* p_raw_capture = nullptr;
        ASSERT_EQ(capture_manager->EnumInterface(0, &p_raw_capture), DAS_S_OK);
        auto capture =
            DasPtr<PluginInterface::IDasCapture>::Attach(p_raw_capture);
        EXPECT_EQ(capture.Get(), raw_capture_factory->last_created);

        ExportInterface::IDasImage* p_raw_image = nullptr;
        ASSERT_EQ(capture->Capture(&p_raw_image), DAS_S_OK);
        auto image_guard =
            DasPtr<ExportInterface::IDasImage>::Attach(p_raw_image);

        ASSERT_EQ(DebugRuntime::Flush(), DAS_S_OK);
        EXPECT_FALSE(std::filesystem::exists(dir / "debug.jsonl"));
        EXPECT_FALSE(std::filesystem::exists(dir / "img"));
    }

} // namespace Das::Core::Debug::Test

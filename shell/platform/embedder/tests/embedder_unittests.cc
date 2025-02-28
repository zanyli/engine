// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define FML_USED_ON_EMBEDDER

#include <string>

#include "embedder.h"
#include "flutter/fml/file.h"
#include "flutter/fml/make_copyable.h"
#include "flutter/fml/mapping.h"
#include "flutter/fml/message_loop.h"
#include "flutter/fml/paths.h"
#include "flutter/fml/synchronization/count_down_latch.h"
#include "flutter/fml/synchronization/waitable_event.h"
#include "flutter/fml/thread.h"
#include "flutter/runtime/dart_vm.h"
#include "flutter/shell/platform/embedder/tests/embedder_assertions.h"
#include "flutter/shell/platform/embedder/tests/embedder_config_builder.h"
#include "flutter/shell/platform/embedder/tests/embedder_test.h"
#include "flutter/testing/assertions_skia.h"
#include "flutter/testing/testing.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/tonic/converter/dart_converter.h"

namespace flutter {
namespace testing {

using EmbedderTest = testing::EmbedderTest;

TEST(EmbedderTestNoFixture, MustNotRunWithInvalidArgs) {
  EmbedderTestContext context;
  EmbedderConfigBuilder builder(
      context, EmbedderConfigBuilder::InitializationPreference::kNoInitialize);
  auto engine = builder.LaunchEngine();
  ASSERT_FALSE(engine.is_valid());
}

TEST_F(EmbedderTest, CanLaunchAndShutdownWithValidProjectArgs) {
  auto& context = GetEmbedderContext();
  fml::AutoResetWaitableEvent latch;
  context.AddIsolateCreateCallback([&latch]() { latch.Signal(); });
  EmbedderConfigBuilder builder(context);
  builder.SetSoftwareRendererConfig();
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
  // Wait for the root isolate to launch.
  latch.Wait();
  engine.reset();
}

// TODO(41999): Disabled because flaky.
TEST_F(EmbedderTest, DISABLED_CanLaunchAndShutdownMultipleTimes) {
  EmbedderConfigBuilder builder(GetEmbedderContext());
  builder.SetSoftwareRendererConfig();
  for (size_t i = 0; i < 3; ++i) {
    auto engine = builder.LaunchEngine();
    ASSERT_TRUE(engine.is_valid());
    FML_LOG(INFO) << "Engine launch count: " << i + 1;
  }
}

TEST_F(EmbedderTest, CanInvokeCustomEntrypoint) {
  auto& context = GetEmbedderContext();
  static fml::AutoResetWaitableEvent latch;
  Dart_NativeFunction entrypoint = [](Dart_NativeArguments args) {
    latch.Signal();
  };
  context.AddNativeCallback("SayHiFromCustomEntrypoint", entrypoint);
  EmbedderConfigBuilder builder(context);
  builder.SetSoftwareRendererConfig();
  builder.SetDartEntrypoint("customEntrypoint");
  auto engine = builder.LaunchEngine();
  latch.Wait();
  ASSERT_TRUE(engine.is_valid());
}

TEST_F(EmbedderTest, CanInvokeCustomEntrypointMacro) {
  auto& context = GetEmbedderContext();

  fml::AutoResetWaitableEvent latch1;
  fml::AutoResetWaitableEvent latch2;
  fml::AutoResetWaitableEvent latch3;

  // Can be defined separately.
  auto entry1 = [&latch1](Dart_NativeArguments args) {
    FML_LOG(INFO) << "In Callback 1";
    latch1.Signal();
  };
  auto native_entry1 = CREATE_NATIVE_ENTRY(entry1);
  context.AddNativeCallback("SayHiFromCustomEntrypoint1", native_entry1);

  // Can be wrapped in in the args.
  auto entry2 = [&latch2](Dart_NativeArguments args) {
    FML_LOG(INFO) << "In Callback 2";
    latch2.Signal();
  };
  context.AddNativeCallback("SayHiFromCustomEntrypoint2",
                            CREATE_NATIVE_ENTRY(entry2));

  // Everything can be inline.
  context.AddNativeCallback(
      "SayHiFromCustomEntrypoint3",
      CREATE_NATIVE_ENTRY([&latch3](Dart_NativeArguments args) {
        FML_LOG(INFO) << "In Callback 3";
        latch3.Signal();
      }));

  EmbedderConfigBuilder builder(context);
  builder.SetSoftwareRendererConfig();
  builder.SetDartEntrypoint("customEntrypoint1");
  auto engine = builder.LaunchEngine();
  latch1.Wait();
  latch2.Wait();
  latch3.Wait();
  ASSERT_TRUE(engine.is_valid());
}

//------------------------------------------------------------------------------
/// @brief      A task runner that we expect the embedder to provide but whose
///             implementation is a real FML task runner.
///
class EmbedderTestTaskRunner {
 public:
  using TaskExpiryCallback = std::function<void(FlutterTask)>;
  EmbedderTestTaskRunner(fml::RefPtr<fml::TaskRunner> real_task_runner,
                         TaskExpiryCallback on_task_expired)
      : identifier_(++sEmbedderTaskRunnerIdentifiers),
        real_task_runner_(real_task_runner),
        on_task_expired_(on_task_expired) {
    FML_CHECK(real_task_runner_);
    FML_CHECK(on_task_expired_);

    task_runner_description_.struct_size = sizeof(FlutterTaskRunnerDescription);
    task_runner_description_.user_data = this;
    task_runner_description_.runs_task_on_current_thread_callback =
        [](void* user_data) -> bool {
      return reinterpret_cast<EmbedderTestTaskRunner*>(user_data)
          ->real_task_runner_->RunsTasksOnCurrentThread();
    };
    task_runner_description_.post_task_callback = [](FlutterTask task,
                                                     uint64_t target_time_nanos,
                                                     void* user_data) -> void {
      auto thiz = reinterpret_cast<EmbedderTestTaskRunner*>(user_data);

      auto target_time = fml::TimePoint::FromEpochDelta(
          fml::TimeDelta::FromNanoseconds(target_time_nanos));
      auto on_task_expired = thiz->on_task_expired_;
      auto invoke_task = [task, on_task_expired]() { on_task_expired(task); };
      auto real_task_runner = thiz->real_task_runner_;

      real_task_runner->PostTaskForTime(invoke_task, target_time);
    };
    task_runner_description_.identifier = identifier_;
  }

  const FlutterTaskRunnerDescription& GetFlutterTaskRunnerDescription() {
    return task_runner_description_;
  }

 private:
  static std::atomic_size_t sEmbedderTaskRunnerIdentifiers;
  const size_t identifier_;
  fml::RefPtr<fml::TaskRunner> real_task_runner_;
  TaskExpiryCallback on_task_expired_;
  FlutterTaskRunnerDescription task_runner_description_ = {};

  FML_DISALLOW_COPY_AND_ASSIGN(EmbedderTestTaskRunner);
};

std::atomic_size_t EmbedderTestTaskRunner::sEmbedderTaskRunnerIdentifiers = {};

TEST_F(EmbedderTest, CanSpecifyCustomPlatformTaskRunner) {
  auto& context = GetEmbedderContext();
  fml::AutoResetWaitableEvent latch;

  // Run the test on its own thread with a message loop so that it can safely
  // pump its event loop while we wait for all the conditions to be checked.
  auto platform_task_runner = CreateNewThread("test_platform_thread");
  static std::mutex engine_mutex;
  static bool signaled_once = false;
  UniqueEngine engine;

  EmbedderTestTaskRunner test_task_runner(
      platform_task_runner, [&](FlutterTask task) {
        std::scoped_lock lock(engine_mutex);
        if (!engine.is_valid()) {
          return;
        }
        // There may be multiple tasks posted but we only need to check
        // assertions once.
        if (signaled_once) {
          FlutterEngineRunTask(engine.get(), &task);
          return;
        }

        signaled_once = true;
        ASSERT_TRUE(engine.is_valid());
        ASSERT_EQ(FlutterEngineRunTask(engine.get(), &task), kSuccess);
        latch.Signal();
      });

  platform_task_runner->PostTask([&]() {
    EmbedderConfigBuilder builder(context);
    const auto task_runner_description =
        test_task_runner.GetFlutterTaskRunnerDescription();
    builder.SetSoftwareRendererConfig();
    builder.SetPlatformTaskRunner(&task_runner_description);
    builder.SetDartEntrypoint("invokePlatformTaskRunner");
    std::scoped_lock lock(engine_mutex);
    engine = builder.LaunchEngine();
    ASSERT_TRUE(engine.is_valid());
  });

  // Signaled when all the assertions are checked.
  latch.Wait();
  ASSERT_TRUE(engine.is_valid());

  // Since the engine was started on its own thread, it must be killed there as
  // well.
  fml::AutoResetWaitableEvent kill_latch;
  platform_task_runner->PostTask(fml::MakeCopyable([&]() mutable {
    std::scoped_lock lock(engine_mutex);
    engine.reset();

    // There may still be pending tasks on the platform thread that were queued
    // by the test_task_runner.  Signal the latch after these tasks have been
    // consumed.
    platform_task_runner->PostTask([&kill_latch] { kill_latch.Signal(); });
  }));
  kill_latch.Wait();

  ASSERT_TRUE(signaled_once);
  signaled_once = false;
}

TEST(EmbedderTestNoFixture, CanGetCurrentTimeInNanoseconds) {
  auto point1 = fml::TimePoint::FromEpochDelta(
      fml::TimeDelta::FromNanoseconds(FlutterEngineGetCurrentTime()));
  auto point2 = fml::TimePoint::Now();

  ASSERT_LT((point2 - point1), fml::TimeDelta::FromMilliseconds(1));
}

TEST_F(EmbedderTest, CanReloadSystemFonts) {
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);
  builder.SetSoftwareRendererConfig();
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());

  auto result = FlutterEngineReloadSystemFonts(engine.get());
  ASSERT_EQ(result, kSuccess);
}

TEST_F(EmbedderTest, CanCreateOpenGLRenderingEngine) {
  EmbedderConfigBuilder builder(GetEmbedderContext());
  builder.SetOpenGLRendererConfig(SkISize::Make(1, 1));
  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
}

TEST_F(EmbedderTest, IsolateServiceIdSent) {
  auto& context = GetEmbedderContext();
  fml::AutoResetWaitableEvent latch;

  fml::Thread thread;
  UniqueEngine engine;
  std::string isolate_message;

  thread.GetTaskRunner()->PostTask([&]() {
    EmbedderConfigBuilder builder(context);
    builder.SetSoftwareRendererConfig();
    builder.SetDartEntrypoint("main");
    builder.SetPlatformMessageCallback(
        [&](const FlutterPlatformMessage* message) {
          if (strcmp(message->channel, "flutter/isolate") == 0) {
            isolate_message = {reinterpret_cast<const char*>(message->message),
                               message->message_size};
            latch.Signal();
          }
        });
    engine = builder.LaunchEngine();
    ASSERT_TRUE(engine.is_valid());
  });

  // Wait for the isolate ID message and check its format.
  latch.Wait();
  ASSERT_EQ(isolate_message.find("isolates/"), 0ul);

  // Since the engine was started on its own thread, it must be killed there as
  // well.
  fml::AutoResetWaitableEvent kill_latch;
  thread.GetTaskRunner()->PostTask(
      fml::MakeCopyable([&engine, &kill_latch]() mutable {
        engine.reset();
        kill_latch.Signal();
      }));
  kill_latch.Wait();
}

//------------------------------------------------------------------------------
/// Creates a platform message response callbacks, does NOT send them, and
/// immediately collects the same.
///
TEST_F(EmbedderTest, CanCreateAndCollectCallbacks) {
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);
  builder.SetSoftwareRendererConfig();
  builder.SetDartEntrypoint("platform_messages_response");
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY([](Dart_NativeArguments args) {}));

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());

  FlutterPlatformMessageResponseHandle* response_handle = nullptr;
  auto callback = [](const uint8_t* data, size_t size,
                     void* user_data) -> void {};
  auto result = FlutterPlatformMessageCreateResponseHandle(
      engine.get(), callback, nullptr, &response_handle);
  ASSERT_EQ(result, kSuccess);
  ASSERT_NE(response_handle, nullptr);

  result = FlutterPlatformMessageReleaseResponseHandle(engine.get(),
                                                       response_handle);
  ASSERT_EQ(result, kSuccess);
}

//------------------------------------------------------------------------------
/// Sends platform messages to Dart code than simply echoes the contents of the
/// message back to the embedder. The embedder registers a native callback to
/// intercept that message.
///
TEST_F(EmbedderTest, PlatformMessagesCanReceiveResponse) {
  struct Captures {
    fml::AutoResetWaitableEvent latch;
    std::thread::id thread_id;
  };
  Captures captures;

  CreateNewThread()->PostTask([&]() {
    captures.thread_id = std::this_thread::get_id();
    auto& context = GetEmbedderContext();
    EmbedderConfigBuilder builder(context);
    builder.SetSoftwareRendererConfig();
    builder.SetDartEntrypoint("platform_messages_response");

    fml::AutoResetWaitableEvent ready;
    context.AddNativeCallback(
        "SignalNativeTest",
        CREATE_NATIVE_ENTRY(
            [&ready](Dart_NativeArguments args) { ready.Signal(); }));

    auto engine = builder.LaunchEngine();
    ASSERT_TRUE(engine.is_valid());

    static std::string kMessageData = "Hello from embedder.";

    FlutterPlatformMessageResponseHandle* response_handle = nullptr;
    auto callback = [](const uint8_t* data, size_t size,
                       void* user_data) -> void {
      ASSERT_EQ(size, kMessageData.size());
      ASSERT_EQ(strncmp(reinterpret_cast<const char*>(kMessageData.data()),
                        reinterpret_cast<const char*>(data), size),
                0);
      auto captures = reinterpret_cast<Captures*>(user_data);
      ASSERT_EQ(captures->thread_id, std::this_thread::get_id());
      captures->latch.Signal();
    };
    auto result = FlutterPlatformMessageCreateResponseHandle(
        engine.get(), callback, &captures, &response_handle);
    ASSERT_EQ(result, kSuccess);

    FlutterPlatformMessage message = {};
    message.struct_size = sizeof(FlutterPlatformMessage);
    message.channel = "test_channel";
    message.message = reinterpret_cast<const uint8_t*>(kMessageData.data());
    message.message_size = kMessageData.size();
    message.response_handle = response_handle;

    ready.Wait();
    result = FlutterEngineSendPlatformMessage(engine.get(), &message);
    ASSERT_EQ(result, kSuccess);

    result = FlutterPlatformMessageReleaseResponseHandle(engine.get(),
                                                         response_handle);
    ASSERT_EQ(result, kSuccess);
  });

  captures.latch.Wait();
}

//------------------------------------------------------------------------------
/// Tests that a platform message can be sent with no response handle. Instead
/// of the platform message integrity checked via a response handle, a native
/// callback with the response is invoked to assert integrity.
///
TEST_F(EmbedderTest, PlatformMessagesCanBeSentWithoutResponseHandles) {
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);
  builder.SetSoftwareRendererConfig();
  builder.SetDartEntrypoint("platform_messages_no_response");

  const std::string message_data = "Hello but don't call me back.";

  fml::AutoResetWaitableEvent ready, message;
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&ready](Dart_NativeArguments args) { ready.Signal(); }));
  context.AddNativeCallback(
      "SignalNativeMessage",
      CREATE_NATIVE_ENTRY(
          ([&message, &message_data](Dart_NativeArguments args) {
            auto received_message = tonic::DartConverter<std::string>::FromDart(
                Dart_GetNativeArgument(args, 0));
            ASSERT_EQ(received_message, message_data);
            message.Signal();
          })));

  auto engine = builder.LaunchEngine();

  ASSERT_TRUE(engine.is_valid());
  ready.Wait();

  FlutterPlatformMessage platform_message = {};
  platform_message.struct_size = sizeof(FlutterPlatformMessage);
  platform_message.channel = "test_channel";
  platform_message.message =
      reinterpret_cast<const uint8_t*>(message_data.data());
  platform_message.message_size = message_data.size();
  platform_message.response_handle = nullptr;  // No response needed.

  auto result =
      FlutterEngineSendPlatformMessage(engine.get(), &platform_message);
  ASSERT_EQ(result, kSuccess);
  message.Wait();
}

//------------------------------------------------------------------------------
/// Tests that a null platform message can be sent.
///
TEST_F(EmbedderTest, NullPlatformMessagesCanBeSent) {
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);
  builder.SetSoftwareRendererConfig();
  builder.SetDartEntrypoint("null_platform_messages");

  fml::AutoResetWaitableEvent ready, message;
  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&ready](Dart_NativeArguments args) { ready.Signal(); }));
  context.AddNativeCallback(
      "SignalNativeMessage",
      CREATE_NATIVE_ENTRY(([&message](Dart_NativeArguments args) {
        auto received_message = tonic::DartConverter<std::string>::FromDart(
            Dart_GetNativeArgument(args, 0));
        ASSERT_EQ("true", received_message);
        message.Signal();
      })));

  auto engine = builder.LaunchEngine();

  ASSERT_TRUE(engine.is_valid());
  ready.Wait();

  FlutterPlatformMessage platform_message = {};
  platform_message.struct_size = sizeof(FlutterPlatformMessage);
  platform_message.channel = "test_channel";
  platform_message.message = nullptr;
  platform_message.message_size = 0;
  platform_message.response_handle = nullptr;  // No response needed.

  auto result =
      FlutterEngineSendPlatformMessage(engine.get(), &platform_message);
  ASSERT_EQ(result, kSuccess);
  message.Wait();
}

//------------------------------------------------------------------------------
/// Tests that a null platform message cannot be send if the message_size
/// isn't equals to 0.
///
TEST_F(EmbedderTest, InvalidPlatformMessages) {
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);
  builder.SetSoftwareRendererConfig();
  auto engine = builder.LaunchEngine();

  ASSERT_TRUE(engine.is_valid());

  FlutterPlatformMessage platform_message = {};
  platform_message.struct_size = sizeof(FlutterPlatformMessage);
  platform_message.channel = "test_channel";
  platform_message.message = nullptr;
  platform_message.message_size = 1;
  platform_message.response_handle = nullptr;  // No response needed.

  auto result =
      FlutterEngineSendPlatformMessage(engine.get(), &platform_message);
  ASSERT_EQ(result, kInvalidArguments);
}

//------------------------------------------------------------------------------
/// Asserts behavior of FlutterProjectArgs::shutdown_dart_vm_when_done (which is
/// set to true by default in these unit-tests).
///
TEST_F(EmbedderTest, VMShutsDownWhenNoEnginesInProcess) {
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);
  builder.SetSoftwareRendererConfig();
  const auto launch_count = DartVM::GetVMLaunchCount();

  {
    auto engine = builder.LaunchEngine();
    ASSERT_EQ(launch_count + 1u, DartVM::GetVMLaunchCount());
  }

  {
    auto engine = builder.LaunchEngine();
    ASSERT_EQ(launch_count + 2u, DartVM::GetVMLaunchCount());
  }
}

//------------------------------------------------------------------------------
/// These snapshots may be materialized from symbols and the size field may not
/// be relevant. Since this information is redundant, engine launch should not
/// be gated on a non-zero buffer size.
///
TEST_F(EmbedderTest, VMAndIsolateSnapshotSizesAreRedundantInAOTMode) {
  if (!DartVM::IsRunningPrecompiledCode()) {
    GTEST_SKIP();
    return;
  }
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);
  builder.SetSoftwareRendererConfig();

  // The fixture sets this up correctly. Intentionally mess up the args.
  builder.GetProjectArgs().vm_snapshot_data_size = 0;
  builder.GetProjectArgs().vm_snapshot_instructions_size = 0;
  builder.GetProjectArgs().isolate_snapshot_data_size = 0;
  builder.GetProjectArgs().isolate_snapshot_instructions_size = 0;

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());
}

//------------------------------------------------------------------------------
/// If an incorrectly configured compositor is set on the engine, the engine
/// must fail to launch instead of failing to render a frame at a later point in
/// time.
///
TEST_F(EmbedderTest,
       MustPreventEngineLaunchWhenRequiredCompositorArgsAreAbsent) {
  auto& context = GetEmbedderContext();
  EmbedderConfigBuilder builder(context);
  builder.SetOpenGLRendererConfig(SkISize::Make(1, 1));
  builder.SetCompositor();
  builder.GetCompositor().create_backing_store_callback = nullptr;
  builder.GetCompositor().collect_backing_store_callback = nullptr;
  builder.GetCompositor().present_layers_callback = nullptr;
  auto engine = builder.LaunchEngine();
  ASSERT_FALSE(engine.is_valid());
}

//------------------------------------------------------------------------------
/// Must be able to render to a custom compositor whose render targets are fully
/// complete OpenGL textures.
///
TEST_F(EmbedderTest, CompositorMustBeAbleToRenderToOpenGLFramebuffer) {
  auto& context = GetEmbedderContext();

  EmbedderConfigBuilder builder(context);
  builder.SetOpenGLRendererConfig(SkISize::Make(800, 600));
  builder.SetCompositor();
  builder.SetDartEntrypoint("can_composite_platform_views");

  context.GetCompositor().SetRenderTargetType(
      EmbedderTestCompositor::RenderTargetType::kOpenGLFramebuffer);

  fml::CountDownLatch latch(3);
  context.GetCompositor().SetNextPresentCallback(
      [&](const FlutterLayer** layers, size_t layers_count) {
        ASSERT_EQ(layers_count, 3u);

        {
          FlutterBackingStore backing_store = *layers[0]->backing_store;
          backing_store.struct_size = sizeof(backing_store);
          backing_store.type = kFlutterBackingStoreTypeOpenGL;
          backing_store.did_update = true;
          backing_store.open_gl.type = kFlutterOpenGLTargetTypeFramebuffer;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(800.0, 600.0);
          layer.offset = FlutterPointMake(0, 0);

          ASSERT_EQ(*layers[0], layer);
        }

        {
          FlutterPlatformView platform_view = {};
          platform_view.struct_size = sizeof(platform_view);
          platform_view.identifier = 42;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypePlatformView;
          layer.platform_view = &platform_view;
          layer.size = FlutterSizeMake(123.0, 456.0);
          layer.offset = FlutterPointMake(1.0, 2.0);

          ASSERT_EQ(*layers[1], layer);
        }

        {
          FlutterBackingStore backing_store = *layers[2]->backing_store;
          backing_store.struct_size = sizeof(backing_store);
          backing_store.type = kFlutterBackingStoreTypeOpenGL;
          backing_store.did_update = true;
          backing_store.open_gl.type = kFlutterOpenGLTargetTypeFramebuffer;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(800.0, 600.0);
          layer.offset = FlutterPointMake(0.0, 0.0);

          ASSERT_EQ(*layers[2], layer);
        }

        latch.CountDown();
      });

  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&latch](Dart_NativeArguments args) { latch.CountDown(); }));

  auto engine = builder.LaunchEngine();

  // Send a window metrics events so frames may be scheduled.
  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_TRUE(engine.is_valid());

  latch.Wait();
}

//------------------------------------------------------------------------------
/// Must be able to render using a custom compositor whose render targets for
/// the individual layers are OpenGL textures.
///
TEST_F(EmbedderTest, CompositorMustBeAbleToRenderToOpenGLTexture) {
  auto& context = GetEmbedderContext();

  EmbedderConfigBuilder builder(context);
  builder.SetOpenGLRendererConfig(SkISize::Make(800, 600));
  builder.SetCompositor();
  builder.SetDartEntrypoint("can_composite_platform_views");

  context.GetCompositor().SetRenderTargetType(
      EmbedderTestCompositor::RenderTargetType::kOpenGLTexture);

  fml::CountDownLatch latch(3);
  context.GetCompositor().SetNextPresentCallback(
      [&](const FlutterLayer** layers, size_t layers_count) {
        ASSERT_EQ(layers_count, 3u);

        {
          FlutterBackingStore backing_store = *layers[0]->backing_store;
          backing_store.struct_size = sizeof(backing_store);
          backing_store.type = kFlutterBackingStoreTypeOpenGL;
          backing_store.did_update = true;
          backing_store.open_gl.type = kFlutterOpenGLTargetTypeTexture;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(800.0, 600.0);
          layer.offset = FlutterPointMake(0, 0);

          ASSERT_EQ(*layers[0], layer);
        }

        {
          FlutterPlatformView platform_view = {};
          platform_view.struct_size = sizeof(platform_view);
          platform_view.identifier = 42;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypePlatformView;
          layer.platform_view = &platform_view;
          layer.size = FlutterSizeMake(123.0, 456.0);
          layer.offset = FlutterPointMake(1.0, 2.0);

          ASSERT_EQ(*layers[1], layer);
        }

        {
          FlutterBackingStore backing_store = *layers[2]->backing_store;
          backing_store.struct_size = sizeof(backing_store);
          backing_store.type = kFlutterBackingStoreTypeOpenGL;
          backing_store.did_update = true;
          backing_store.open_gl.type = kFlutterOpenGLTargetTypeTexture;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(800.0, 600.0);
          layer.offset = FlutterPointMake(0.0, 0.0);

          ASSERT_EQ(*layers[2], layer);
        }

        latch.CountDown();
      });

  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&latch](Dart_NativeArguments args) { latch.CountDown(); }));

  auto engine = builder.LaunchEngine();

  // Send a window metrics events so frames may be scheduled.
  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_TRUE(engine.is_valid());

  latch.Wait();
}

//------------------------------------------------------------------------------
/// Must be able to render using a custom compositor whose render target for the
/// individual layers are software buffers.
///
TEST_F(EmbedderTest, CompositorMustBeAbleToRenderToSoftwareBuffer) {
  auto& context = GetEmbedderContext();

  EmbedderConfigBuilder builder(context);
  builder.SetOpenGLRendererConfig(SkISize::Make(800, 600));
  builder.SetCompositor();
  builder.SetDartEntrypoint("can_composite_platform_views");

  context.GetCompositor().SetRenderTargetType(
      EmbedderTestCompositor::RenderTargetType::kSoftwareBuffer);

  fml::CountDownLatch latch(3);
  context.GetCompositor().SetNextPresentCallback(
      [&](const FlutterLayer** layers, size_t layers_count) {
        ASSERT_EQ(layers_count, 3u);

        {
          FlutterBackingStore backing_store = *layers[0]->backing_store;
          backing_store.struct_size = sizeof(backing_store);
          backing_store.type = kFlutterBackingStoreTypeSoftware;
          backing_store.did_update = true;
          ASSERT_FLOAT_EQ(
              backing_store.software.row_bytes * backing_store.software.height,
              800 * 4 * 600.0);

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(800.0, 600.0);
          layer.offset = FlutterPointMake(0, 0);

          ASSERT_EQ(*layers[0], layer);
        }

        {
          FlutterPlatformView platform_view = {};
          platform_view.struct_size = sizeof(platform_view);
          platform_view.identifier = 42;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypePlatformView;
          layer.platform_view = &platform_view;
          layer.size = FlutterSizeMake(123.0, 456.0);
          layer.offset = FlutterPointMake(1.0, 2.0);

          ASSERT_EQ(*layers[1], layer);
        }

        {
          FlutterBackingStore backing_store = *layers[2]->backing_store;
          backing_store.struct_size = sizeof(backing_store);
          backing_store.type = kFlutterBackingStoreTypeSoftware;
          backing_store.did_update = true;
          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(800.0, 600.0);
          layer.offset = FlutterPointMake(0.0, 0.0);

          ASSERT_EQ(*layers[2], layer);
        }

        latch.CountDown();
      });

  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&latch](Dart_NativeArguments args) { latch.CountDown(); }));

  auto engine = builder.LaunchEngine();

  // Send a window metrics events so frames may be scheduled.
  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_TRUE(engine.is_valid());

  latch.Wait();
}

static sk_sp<SkSurface> CreateRenderSurface(const FlutterLayer& layer,
                                            GrContext* context) {
  const auto image_info =
      SkImageInfo::MakeN32Premul(layer.size.width, layer.size.height);
  auto surface = context ? SkSurface::MakeRenderTarget(
                               context,                   // context
                               SkBudgeted::kNo,           // budgeted
                               image_info,                // image info
                               1,                         // sample count
                               kTopLeft_GrSurfaceOrigin,  // surface origin
                               nullptr,                   // surface properties
                               false                      // mipmaps

                               )
                         : SkSurface::MakeRaster(image_info);
  FML_CHECK(surface != nullptr);
  return surface;
}

static bool RasterImagesAreSame(sk_sp<SkImage> a, sk_sp<SkImage> b) {
  FML_CHECK(!a->isTextureBacked());
  FML_CHECK(!b->isTextureBacked());

  if (!a || !b) {
    return false;
  }

  SkPixmap pixmapA;
  SkPixmap pixmapB;

  if (!a->peekPixels(&pixmapA)) {
    FML_LOG(ERROR) << "Could not peek pixels of image A.";
    return false;
  }

  if (!b->peekPixels(&pixmapB)) {
    FML_LOG(ERROR) << "Could not peek pixels of image B.";

    return false;
  }

  const auto sizeA = pixmapA.rowBytes() * pixmapA.height();
  const auto sizeB = pixmapB.rowBytes() * pixmapB.height();

  if (sizeA != sizeB) {
    FML_LOG(ERROR) << "Pixmap sizes were inconsistent.";
    return false;
  }

  return ::memcmp(pixmapA.addr(), pixmapB.addr(), sizeA) == 0;
}

static bool WriteImageToDisk(const fml::UniqueFD& directory,
                             const std::string& name,
                             sk_sp<SkImage> image) {
  if (!image) {
    return false;
  }

  auto data = image->encodeToData(SkEncodedImageFormat::kPNG, 100);

  if (!data) {
    return false;
  }

  fml::NonOwnedMapping mapping(static_cast<const uint8_t*>(data->data()),
                               data->size());
  return WriteAtomically(directory, name.c_str(), mapping);
}

static bool ImageMatchesFixture(const std::string& fixture_file_name,
                                sk_sp<SkImage> scene_image) {
  fml::FileMapping fixture_image_mapping(OpenFixture(fixture_file_name));

  FML_CHECK(fixture_image_mapping.GetSize() != 0u)
      << "Could not find fixture: " << fixture_file_name;

  auto encoded_image = SkData::MakeWithoutCopy(
      fixture_image_mapping.GetMapping(), fixture_image_mapping.GetSize());
  auto fixture_image =
      SkImage::MakeFromEncoded(std::move(encoded_image))->makeRasterImage();

  FML_CHECK(fixture_image) << "Could not create image from fixture: "
                           << fixture_file_name;

  auto scene_image_subset = scene_image->makeSubset(
      SkIRect::MakeWH(fixture_image->width(), fixture_image->height()));

  FML_CHECK(scene_image_subset)
      << "Could not create image subset for fixture comparison: "
      << scene_image_subset;

  const auto images_are_same =
      RasterImagesAreSame(scene_image_subset, fixture_image);

  // If the images are not the same, this predicate is going to indicate test
  // failure. Dump both the actual image and the expectation to disk to the
  // test author can figure out what went wrong.
  if (!images_are_same) {
    const auto fixtures_path = GetFixturesPath();

    const auto actual_file_name = "actual_" + fixture_file_name;
    const auto expect_file_name = "expectation_" + fixture_file_name;

    auto fixtures_fd = OpenFixturesDirectory();

    FML_CHECK(
        WriteImageToDisk(fixtures_fd, actual_file_name, scene_image_subset))
        << "Could not write file to disk: " << actual_file_name;

    FML_CHECK(WriteImageToDisk(fixtures_fd, expect_file_name, fixture_image))
        << "Could not write file to disk: " << expect_file_name;

    FML_LOG(ERROR) << "Image did not match expectation." << std::endl
                   << "Expected:"
                   << fml::paths::JoinPaths({fixtures_path, expect_file_name})
                   << std::endl
                   << "Got:"
                   << fml::paths::JoinPaths({fixtures_path, actual_file_name})
                   << std::endl;
  }
  return images_are_same;
}

//------------------------------------------------------------------------------
/// Test the layer structure and pixels rendered when using a custom compositor.
///
TEST_F(EmbedderTest, CompositorMustBeAbleToRenderKnownScene) {
  auto& context = GetEmbedderContext();

  EmbedderConfigBuilder builder(context);
  builder.SetOpenGLRendererConfig(SkISize::Make(800, 600));
  builder.SetCompositor();
  builder.SetDartEntrypoint("can_composite_platform_views_with_known_scene");

  context.GetCompositor().SetRenderTargetType(
      EmbedderTestCompositor::RenderTargetType::kOpenGLTexture);

  fml::CountDownLatch latch(6);

  sk_sp<SkImage> scene_image;
  context.SetNextSceneCallback([&](sk_sp<SkImage> scene) {
    scene_image = std::move(scene);
    latch.CountDown();
  });

  context.GetCompositor().SetNextPresentCallback(
      [&](const FlutterLayer** layers, size_t layers_count) {
        ASSERT_EQ(layers_count, 5u);

        // Layer Root
        {
          FlutterBackingStore backing_store = *layers[0]->backing_store;
          backing_store.type = kFlutterBackingStoreTypeOpenGL;
          backing_store.did_update = true;
          backing_store.open_gl.type = kFlutterOpenGLTargetTypeTexture;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(800.0, 600.0);
          layer.offset = FlutterPointMake(0.0, 0.0);

          ASSERT_EQ(*layers[0], layer);
        }

        // Layer 1
        {
          FlutterPlatformView platform_view = {};
          platform_view.struct_size = sizeof(platform_view);
          platform_view.identifier = 1;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypePlatformView;
          layer.platform_view = &platform_view;
          layer.size = FlutterSizeMake(50.0, 150.0);
          layer.offset = FlutterPointMake(20.0, 20.0);

          ASSERT_EQ(*layers[1], layer);
        }

        // Layer 2
        {
          FlutterBackingStore backing_store = *layers[2]->backing_store;
          backing_store.type = kFlutterBackingStoreTypeOpenGL;
          backing_store.did_update = true;
          backing_store.open_gl.type = kFlutterOpenGLTargetTypeTexture;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(800.0, 600.0);
          layer.offset = FlutterPointMake(0.0, 0.0);

          ASSERT_EQ(*layers[2], layer);
        }

        // Layer 3
        {
          FlutterPlatformView platform_view = {};
          platform_view.struct_size = sizeof(platform_view);
          platform_view.identifier = 2;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypePlatformView;
          layer.platform_view = &platform_view;
          layer.size = FlutterSizeMake(50.0, 150.0);
          layer.offset = FlutterPointMake(40.0, 40.0);

          ASSERT_EQ(*layers[3], layer);
        }

        // Layer 4
        {
          FlutterBackingStore backing_store = *layers[4]->backing_store;
          backing_store.type = kFlutterBackingStoreTypeOpenGL;
          backing_store.did_update = true;
          backing_store.open_gl.type = kFlutterOpenGLTargetTypeTexture;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(800.0, 600.0);
          layer.offset = FlutterPointMake(0.0, 0.0);

          ASSERT_EQ(*layers[4], layer);
        }

        latch.CountDown();
      });

  context.GetCompositor().SetPlatformViewRendererCallback(
      [&](const FlutterLayer& layer, GrContext* context) -> sk_sp<SkImage> {
        auto surface = CreateRenderSurface(layer, context);
        auto canvas = surface->getCanvas();
        FML_CHECK(canvas != nullptr);

        switch (layer.platform_view->identifier) {
          case 1: {
            SkPaint paint;
            // See dart test for total order.
            paint.setColor(SK_ColorGREEN);
            paint.setAlpha(127);
            const auto& rect =
                SkRect::MakeWH(layer.size.width, layer.size.height);
            canvas->drawRect(rect, paint);
            latch.CountDown();
          } break;
          case 2: {
            SkPaint paint;
            // See dart test for total order.
            paint.setColor(SK_ColorMAGENTA);
            paint.setAlpha(127);
            const auto& rect =
                SkRect::MakeWH(layer.size.width, layer.size.height);
            canvas->drawRect(rect, paint);
            latch.CountDown();
          } break;
          default:
            // Asked to render an unknown platform view.
            FML_CHECK(false)
                << "Test was asked to composite an unknown platform view.";
        }

        return surface->makeImageSnapshot();
      });

  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&latch](Dart_NativeArguments args) { latch.CountDown(); }));

  auto engine = builder.LaunchEngine();

  // Send a window metrics events so frames may be scheduled.
  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_TRUE(engine.is_valid());

  latch.Wait();

  ASSERT_TRUE(ImageMatchesFixture("compositor.png", scene_image));

  // There should no present calls on the root surface.
  ASSERT_EQ(context.GetSoftwareSurfacePresentCount(), 0u);
  ASSERT_EQ(context.GetGLSurfacePresentCount(), 0u);
}

//------------------------------------------------------------------------------
/// Test the layer structure and pixels rendered when using a custom software
/// compositor.
///
TEST_F(EmbedderTest,
       CompositorMustBeAbleToRenderKnownSceneWithSoftwareCompositor) {
  auto& context = GetEmbedderContext();

  EmbedderConfigBuilder builder(context);
  builder.SetSoftwareRendererConfig(SkISize::Make(800, 600));
  builder.SetCompositor();
  builder.SetDartEntrypoint("can_composite_platform_views_with_known_scene");

  context.GetCompositor().SetRenderTargetType(
      EmbedderTestCompositor::RenderTargetType::kSoftwareBuffer);

  fml::CountDownLatch latch(6);

  sk_sp<SkImage> scene_image;
  context.SetNextSceneCallback([&](sk_sp<SkImage> scene) {
    scene_image = std::move(scene);
    latch.CountDown();
  });

  context.GetCompositor().SetNextPresentCallback(
      [&](const FlutterLayer** layers, size_t layers_count) {
        ASSERT_EQ(layers_count, 5u);

        // Layer Root
        {
          FlutterBackingStore backing_store = *layers[0]->backing_store;
          backing_store.type = kFlutterBackingStoreTypeSoftware;
          backing_store.did_update = true;
          backing_store.software.height = 600;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(800.0, 600.0);
          layer.offset = FlutterPointMake(0.0, 0.0);

          ASSERT_EQ(*layers[0], layer);
        }

        // Layer 1
        {
          FlutterPlatformView platform_view = {};
          platform_view.struct_size = sizeof(platform_view);
          platform_view.identifier = 1;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypePlatformView;
          layer.platform_view = &platform_view;
          layer.size = FlutterSizeMake(50.0, 150.0);
          layer.offset = FlutterPointMake(20.0, 20.0);

          ASSERT_EQ(*layers[1], layer);
        }

        // Layer 2
        {
          FlutterBackingStore backing_store = *layers[2]->backing_store;
          backing_store.type = kFlutterBackingStoreTypeSoftware;
          backing_store.did_update = true;
          backing_store.software.height = 600;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(800.0, 600.0);
          layer.offset = FlutterPointMake(0.0, 0.0);

          ASSERT_EQ(*layers[2], layer);
        }

        // Layer 3
        {
          FlutterPlatformView platform_view = {};
          platform_view.struct_size = sizeof(platform_view);
          platform_view.identifier = 2;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypePlatformView;
          layer.platform_view = &platform_view;
          layer.size = FlutterSizeMake(50.0, 150.0);
          layer.offset = FlutterPointMake(40.0, 40.0);

          ASSERT_EQ(*layers[3], layer);
        }

        // Layer 4
        {
          FlutterBackingStore backing_store = *layers[4]->backing_store;
          backing_store.type = kFlutterBackingStoreTypeSoftware;
          backing_store.did_update = true;
          backing_store.software.height = 600;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(800.0, 600.0);
          layer.offset = FlutterPointMake(0.0, 0.0);

          ASSERT_EQ(*layers[4], layer);
        }

        latch.CountDown();
      });

  context.GetCompositor().SetPlatformViewRendererCallback(
      [&](const FlutterLayer& layer, GrContext *
          /* don't use because software compositor */) -> sk_sp<SkImage> {
        auto surface = CreateRenderSurface(
            layer, nullptr /* null because software compositor */);
        auto canvas = surface->getCanvas();
        FML_CHECK(canvas != nullptr);

        switch (layer.platform_view->identifier) {
          case 1: {
            SkPaint paint;
            // See dart test for total order.
            paint.setColor(SK_ColorGREEN);
            paint.setAlpha(127);
            const auto& rect =
                SkRect::MakeWH(layer.size.width, layer.size.height);
            canvas->drawRect(rect, paint);
            latch.CountDown();
          } break;
          case 2: {
            SkPaint paint;
            // See dart test for total order.
            paint.setColor(SK_ColorMAGENTA);
            paint.setAlpha(127);
            const auto& rect =
                SkRect::MakeWH(layer.size.width, layer.size.height);
            canvas->drawRect(rect, paint);
            latch.CountDown();
          } break;
          default:
            // Asked to render an unknown platform view.
            FML_CHECK(false)
                << "Test was asked to composite an unknown platform view.";
        }

        return surface->makeImageSnapshot();
      });

  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&latch](Dart_NativeArguments args) { latch.CountDown(); }));

  auto engine = builder.LaunchEngine();

  // Send a window metrics events so frames may be scheduled.
  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_TRUE(engine.is_valid());

  latch.Wait();

  ASSERT_TRUE(ImageMatchesFixture("compositor_software.png", scene_image));

  // There should no present calls on the root surface.
  ASSERT_EQ(context.GetSoftwareSurfacePresentCount(), 0u);
  ASSERT_EQ(context.GetGLSurfacePresentCount(), 0u);
}

//------------------------------------------------------------------------------
/// Custom compositor must play nicely with a custom task runner. The GPU thread
/// merging mechanism must not interfere with the custom compositor.
///
TEST_F(EmbedderTest, CustomCompositorMustWorkWithCustomTaskRunner) {
  auto& context = GetEmbedderContext();

  EmbedderConfigBuilder builder(context);

  builder.SetOpenGLRendererConfig(SkISize::Make(800, 600));
  builder.SetCompositor();
  builder.SetDartEntrypoint("can_composite_platform_views");

  auto platform_task_runner = CreateNewThread("test_platform_thread");
  static std::mutex engine_mutex;
  UniqueEngine engine;
  fml::AutoResetWaitableEvent sync_latch;

  EmbedderTestTaskRunner test_task_runner(
      platform_task_runner, [&](FlutterTask task) {
        std::scoped_lock lock(engine_mutex);
        if (!engine.is_valid()) {
          return;
        }
        ASSERT_EQ(FlutterEngineRunTask(engine.get(), &task), kSuccess);
      });

  context.GetCompositor().SetRenderTargetType(
      EmbedderTestCompositor::RenderTargetType::kOpenGLTexture);

  fml::CountDownLatch latch(3);
  context.GetCompositor().SetNextPresentCallback(
      [&](const FlutterLayer** layers, size_t layers_count) {
        ASSERT_EQ(layers_count, 3u);

        {
          FlutterBackingStore backing_store = *layers[0]->backing_store;
          backing_store.struct_size = sizeof(backing_store);
          backing_store.type = kFlutterBackingStoreTypeOpenGL;
          backing_store.did_update = true;
          backing_store.open_gl.type = kFlutterOpenGLTargetTypeTexture;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(800.0, 600.0);
          layer.offset = FlutterPointMake(0, 0);

          ASSERT_EQ(*layers[0], layer);
        }

        {
          FlutterPlatformView platform_view = {};
          platform_view.struct_size = sizeof(platform_view);
          platform_view.identifier = 42;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypePlatformView;
          layer.platform_view = &platform_view;
          layer.size = FlutterSizeMake(123.0, 456.0);
          layer.offset = FlutterPointMake(1.0, 2.0);

          ASSERT_EQ(*layers[1], layer);
        }

        {
          FlutterBackingStore backing_store = *layers[2]->backing_store;
          backing_store.struct_size = sizeof(backing_store);
          backing_store.type = kFlutterBackingStoreTypeOpenGL;
          backing_store.did_update = true;
          backing_store.open_gl.type = kFlutterOpenGLTargetTypeTexture;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(800.0, 600.0);
          layer.offset = FlutterPointMake(0.0, 0.0);

          ASSERT_EQ(*layers[2], layer);
        }

        latch.CountDown();
      });

  const auto task_runner_description =
      test_task_runner.GetFlutterTaskRunnerDescription();

  builder.SetPlatformTaskRunner(&task_runner_description);

  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&latch](Dart_NativeArguments args) { latch.CountDown(); }));

  platform_task_runner->PostTask([&]() {
    std::scoped_lock lock(engine_mutex);
    engine = builder.LaunchEngine();
    ASSERT_TRUE(engine.is_valid());

    // Send a window metrics events so frames may be scheduled.
    FlutterWindowMetricsEvent event = {};
    event.struct_size = sizeof(event);
    event.width = 800;
    event.height = 600;

    ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
              kSuccess);
    ASSERT_TRUE(engine.is_valid());
    sync_latch.Signal();
  });
  sync_latch.Wait();

  latch.Wait();

  platform_task_runner->PostTask([&]() {
    std::scoped_lock lock(engine_mutex);
    engine.reset();
    sync_latch.Signal();
  });
  sync_latch.Wait();
}

//------------------------------------------------------------------------------
/// Test the layer structure and pixels rendered when using a custom compositor
/// and a single layer.
///
TEST_F(EmbedderTest, CompositorMustBeAbleToRenderWithRootLayerOnly) {
  auto& context = GetEmbedderContext();

  EmbedderConfigBuilder builder(context);
  builder.SetOpenGLRendererConfig(SkISize::Make(800, 600));
  builder.SetCompositor();
  builder.SetDartEntrypoint(
      "can_composite_platform_views_with_root_layer_only");

  context.GetCompositor().SetRenderTargetType(
      EmbedderTestCompositor::RenderTargetType::kOpenGLTexture);

  fml::CountDownLatch latch(4);

  sk_sp<SkImage> scene_image;
  context.SetNextSceneCallback([&](sk_sp<SkImage> scene) {
    scene_image = std::move(scene);
    latch.CountDown();
  });

  context.GetCompositor().SetNextPresentCallback(
      [&](const FlutterLayer** layers, size_t layers_count) {
        ASSERT_EQ(layers_count, 1u);

        // Layer Root
        {
          FlutterBackingStore backing_store = *layers[0]->backing_store;
          backing_store.type = kFlutterBackingStoreTypeOpenGL;
          backing_store.did_update = true;
          backing_store.open_gl.type = kFlutterOpenGLTargetTypeTexture;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(800.0, 600.0);
          layer.offset = FlutterPointMake(0.0, 0.0);

          ASSERT_EQ(*layers[0], layer);
        }

        latch.CountDown();
      });

  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&latch](Dart_NativeArguments args) { latch.CountDown(); }));

  auto engine = builder.LaunchEngine();

  // Send a window metrics events so frames may be scheduled.
  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_TRUE(engine.is_valid());

  latch.Wait();

  ASSERT_TRUE(
      ImageMatchesFixture("compositor_with_root_layer_only.png", scene_image));
}

//------------------------------------------------------------------------------
/// Test the layer structure and pixels rendered when using a custom compositor
/// and ensure that a redundant layer is not added.
///
TEST_F(EmbedderTest, CompositorMustBeAbleToRenderWithPlatformLayerOnBottom) {
  auto& context = GetEmbedderContext();

  EmbedderConfigBuilder builder(context);
  builder.SetOpenGLRendererConfig(SkISize::Make(800, 600));
  builder.SetCompositor();
  builder.SetDartEntrypoint(
      "can_composite_platform_views_with_platform_layer_on_bottom");

  context.GetCompositor().SetRenderTargetType(
      EmbedderTestCompositor::RenderTargetType::kOpenGLTexture);

  fml::CountDownLatch latch(4);

  sk_sp<SkImage> scene_image;
  context.SetNextSceneCallback([&](sk_sp<SkImage> scene) {
    scene_image = std::move(scene);
    latch.CountDown();
  });

  context.GetCompositor().SetNextPresentCallback(
      [&](const FlutterLayer** layers, size_t layers_count) {
        ASSERT_EQ(layers_count, 2u);

        // Layer Root
        {
          FlutterBackingStore backing_store = *layers[0]->backing_store;
          backing_store.type = kFlutterBackingStoreTypeOpenGL;
          backing_store.did_update = true;
          backing_store.open_gl.type = kFlutterOpenGLTargetTypeTexture;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(800.0, 600.0);
          layer.offset = FlutterPointMake(0.0, 0.0);

          ASSERT_EQ(*layers[0], layer);
        }

        // Layer 1
        {
          FlutterPlatformView platform_view = {};
          platform_view.struct_size = sizeof(platform_view);
          platform_view.identifier = 1;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypePlatformView;
          layer.platform_view = &platform_view;
          layer.size = FlutterSizeMake(50.0, 150.0);
          layer.offset = FlutterPointMake(20.0, 20.0);

          ASSERT_EQ(*layers[1], layer);
        }

        latch.CountDown();
      });

  context.GetCompositor().SetPlatformViewRendererCallback(
      [&](const FlutterLayer& layer, GrContext* context) -> sk_sp<SkImage> {
        auto surface = CreateRenderSurface(layer, context);
        auto canvas = surface->getCanvas();
        FML_CHECK(canvas != nullptr);

        switch (layer.platform_view->identifier) {
          case 1: {
            SkPaint paint;
            // See dart test for total order.
            paint.setColor(SK_ColorGREEN);
            paint.setAlpha(127);
            const auto& rect =
                SkRect::MakeWH(layer.size.width, layer.size.height);
            canvas->drawRect(rect, paint);
            latch.CountDown();
          } break;
          default:
            // Asked to render an unknown platform view.
            FML_CHECK(false)
                << "Test was asked to composite an unknown platform view.";
        }

        return surface->makeImageSnapshot();
      });

  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&latch](Dart_NativeArguments args) { latch.CountDown(); }));

  auto engine = builder.LaunchEngine();

  // Send a window metrics events so frames may be scheduled.
  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_TRUE(engine.is_valid());

  latch.Wait();

  ASSERT_TRUE(ImageMatchesFixture(
      "compositor_with_platform_layer_on_bottom.png", scene_image));

  ASSERT_EQ(context.GetCompositor().GetBackingStoresCount(), 1u);
}

//------------------------------------------------------------------------------
/// Test the layer structure and pixels rendered when using a custom compositor
/// with a root surface transformation.
///
TEST_F(EmbedderTest,
       CompositorMustBeAbleToRenderKnownSceneWithRootSurfaceTransformation) {
  auto& context = GetEmbedderContext();

  EmbedderConfigBuilder builder(context);
  builder.SetOpenGLRendererConfig(SkISize::Make(600, 800));
  builder.SetCompositor();
  builder.SetDartEntrypoint("can_composite_platform_views_with_known_scene");

  context.GetCompositor().SetRenderTargetType(
      EmbedderTestCompositor::RenderTargetType::kOpenGLTexture);

  // This must match the transformation provided in the
  // |CanRenderGradientWithoutCompositorWithXform| test to ensure that
  // transforms are consistent respected.
  const auto root_surface_transformation =
      SkMatrix().preTranslate(0, 800).preRotate(-90, 0, 0);

  context.SetRootSurfaceTransformation(root_surface_transformation);

  fml::CountDownLatch latch(6);

  sk_sp<SkImage> scene_image;
  context.SetNextSceneCallback([&](sk_sp<SkImage> scene) {
    scene_image = std::move(scene);
    latch.CountDown();
  });

  context.GetCompositor().SetNextPresentCallback(
      [&](const FlutterLayer** layers, size_t layers_count) {
        ASSERT_EQ(layers_count, 5u);

        // Layer Root
        {
          FlutterBackingStore backing_store = *layers[0]->backing_store;
          backing_store.type = kFlutterBackingStoreTypeOpenGL;
          backing_store.did_update = true;
          backing_store.open_gl.type = kFlutterOpenGLTargetTypeTexture;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(600.0, 800.0);
          layer.offset = FlutterPointMake(0.0, 0.0);

          ASSERT_EQ(*layers[0], layer);
        }

        // Layer 1
        {
          FlutterPlatformView platform_view = {};
          platform_view.struct_size = sizeof(platform_view);
          platform_view.identifier = 1;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypePlatformView;
          layer.platform_view = &platform_view;
          layer.size = FlutterSizeMake(150.0, 50.0);
          layer.offset = FlutterPointMake(20.0, 730.0);

          ASSERT_EQ(*layers[1], layer);
        }

        // Layer 2
        {
          FlutterBackingStore backing_store = *layers[2]->backing_store;
          backing_store.type = kFlutterBackingStoreTypeOpenGL;
          backing_store.did_update = true;
          backing_store.open_gl.type = kFlutterOpenGLTargetTypeTexture;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(600.0, 800.0);
          layer.offset = FlutterPointMake(0.0, 0.0);

          ASSERT_EQ(*layers[2], layer);
        }

        // Layer 3
        {
          FlutterPlatformView platform_view = {};
          platform_view.struct_size = sizeof(platform_view);
          platform_view.identifier = 2;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypePlatformView;
          layer.platform_view = &platform_view;
          layer.size = FlutterSizeMake(150.0, 50.0);
          layer.offset = FlutterPointMake(40.0, 710.0);

          ASSERT_EQ(*layers[3], layer);
        }

        // Layer 4
        {
          FlutterBackingStore backing_store = *layers[4]->backing_store;
          backing_store.type = kFlutterBackingStoreTypeOpenGL;
          backing_store.did_update = true;
          backing_store.open_gl.type = kFlutterOpenGLTargetTypeTexture;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(600.0, 800.0);
          layer.offset = FlutterPointMake(0.0, 0.0);

          ASSERT_EQ(*layers[4], layer);
        }

        latch.CountDown();
      });

  context.GetCompositor().SetPlatformViewRendererCallback(
      [&](const FlutterLayer& layer, GrContext* context) -> sk_sp<SkImage> {
        auto surface = CreateRenderSurface(layer, context);
        auto canvas = surface->getCanvas();
        FML_CHECK(canvas != nullptr);

        switch (layer.platform_view->identifier) {
          case 1: {
            SkPaint paint;
            // See dart test for total order.
            paint.setColor(SK_ColorGREEN);
            paint.setAlpha(127);
            const auto& rect =
                SkRect::MakeWH(layer.size.width, layer.size.height);
            canvas->drawRect(rect, paint);
            latch.CountDown();
          } break;
          case 2: {
            SkPaint paint;
            // See dart test for total order.
            paint.setColor(SK_ColorMAGENTA);
            paint.setAlpha(127);
            const auto& rect =
                SkRect::MakeWH(layer.size.width, layer.size.height);
            canvas->drawRect(rect, paint);
            latch.CountDown();
          } break;
          default:
            // Asked to render an unknown platform view.
            FML_CHECK(false)
                << "Test was asked to composite an unknown platform view.";
        }

        return surface->makeImageSnapshot();
      });

  context.AddNativeCallback(
      "SignalNativeTest",
      CREATE_NATIVE_ENTRY(
          [&latch](Dart_NativeArguments args) { latch.CountDown(); }));

  auto engine = builder.LaunchEngine();

  // Send a window metrics events so frames may be scheduled.
  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  // Flutter still thinks it is 800 x 600. Only the root surface is rotated.
  event.width = 800;
  event.height = 600;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_TRUE(engine.is_valid());

  latch.Wait();

  ASSERT_TRUE(ImageMatchesFixture("compositor_root_surface_xformation.png",
                                  scene_image));
}

TEST_F(EmbedderTest, CanRenderSceneWithoutCustomCompositor) {
  auto& context = GetEmbedderContext();

  EmbedderConfigBuilder builder(context);

  builder.SetDartEntrypoint("can_render_scene_without_custom_compositor");
  builder.SetOpenGLRendererConfig(SkISize::Make(800, 600));

  fml::CountDownLatch latch(1);

  sk_sp<SkImage> renderered_scene;
  context.SetNextSceneCallback([&](auto image) {
    renderered_scene = std::move(image);
    latch.CountDown();
  });

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());

  // Send a window metrics events so frames may be scheduled.
  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);

  latch.Wait();

  ASSERT_NE(renderered_scene, nullptr);

  ASSERT_TRUE(ImageMatchesFixture("scene_without_custom_compositor.png",
                                  renderered_scene));
}

TEST_F(EmbedderTest, CanRenderSceneWithoutCustomCompositorWithTransformation) {
  auto& context = GetEmbedderContext();

  const auto root_surface_transformation =
      SkMatrix().preTranslate(0, 800).preRotate(-90, 0, 0);

  context.SetRootSurfaceTransformation(root_surface_transformation);

  EmbedderConfigBuilder builder(context);

  builder.SetDartEntrypoint("can_render_scene_without_custom_compositor");
  builder.SetOpenGLRendererConfig(SkISize::Make(600, 800));

  fml::CountDownLatch latch(1);

  sk_sp<SkImage> renderered_scene;
  context.SetNextSceneCallback([&](auto image) {
    renderered_scene = std::move(image);
    latch.CountDown();
  });

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());

  // Send a window metrics events so frames may be scheduled.
  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);

  // Flutter still thinks it is 800 x 600.
  event.width = 800;
  event.height = 600;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);

  latch.Wait();

  ASSERT_NE(renderered_scene, nullptr);

  ASSERT_TRUE(ImageMatchesFixture(
      "scene_without_custom_compositor_with_xform.png", renderered_scene));
}

TEST_F(EmbedderTest, CanRenderGradientWithoutCompositor) {
  auto& context = GetEmbedderContext();

  EmbedderConfigBuilder builder(context);

  builder.SetDartEntrypoint("render_gradient");
  builder.SetOpenGLRendererConfig(SkISize::Make(800, 600));
  fml::CountDownLatch latch(1);

  sk_sp<SkImage> renderered_scene;
  context.SetNextSceneCallback([&](auto image) {
    renderered_scene = std::move(image);
    latch.CountDown();
  });

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());

  // Send a window metrics events so frames may be scheduled.
  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);

  latch.Wait();

  ASSERT_NE(renderered_scene, nullptr);

  ASSERT_TRUE(ImageMatchesFixture("gradient.png", renderered_scene));
}

TEST_F(EmbedderTest, CanRenderGradientWithoutCompositorWithXform) {
  auto& context = GetEmbedderContext();

  const auto root_surface_transformation =
      SkMatrix().preTranslate(0, 800).preRotate(-90, 0, 0);

  context.SetRootSurfaceTransformation(root_surface_transformation);

  EmbedderConfigBuilder builder(context);

  const auto surface_size = SkISize::Make(600, 800);

  builder.SetDartEntrypoint("render_gradient");
  builder.SetOpenGLRendererConfig(surface_size);

  fml::CountDownLatch latch(1);

  sk_sp<SkImage> renderered_scene;
  context.SetNextSceneCallback([&](auto image) {
    renderered_scene = std::move(image);
    latch.CountDown();
  });

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());

  // Send a window metrics events so frames may be scheduled.
  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  // Flutter still thinks it is 800 x 600.
  event.width = 800;
  event.height = 600;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);

  latch.Wait();

  ASSERT_NE(renderered_scene, nullptr);

  ASSERT_TRUE(ImageMatchesFixture("gradient_xform.png", renderered_scene));
}

TEST_F(EmbedderTest, CanRenderGradientWithCompositor) {
  auto& context = GetEmbedderContext();

  EmbedderConfigBuilder builder(context);

  builder.SetDartEntrypoint("render_gradient");
  builder.SetOpenGLRendererConfig(SkISize::Make(800, 600));
  builder.SetCompositor();
  fml::CountDownLatch latch(1);

  sk_sp<SkImage> renderered_scene;
  context.SetNextSceneCallback([&](auto image) {
    renderered_scene = std::move(image);
    latch.CountDown();
  });

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());

  // Send a window metrics events so frames may be scheduled.
  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);

  latch.Wait();

  ASSERT_NE(renderered_scene, nullptr);

  ASSERT_TRUE(ImageMatchesFixture("gradient.png", renderered_scene));
}

TEST_F(EmbedderTest, CanRenderGradientWithCompositorWithXform) {
  auto& context = GetEmbedderContext();

  // This must match the transformation provided in the
  // |CanRenderGradientWithoutCompositorWithXform| test to ensure that
  // transforms are consistent respected.
  const auto root_surface_transformation =
      SkMatrix().preTranslate(0, 800).preRotate(-90, 0, 0);

  context.SetRootSurfaceTransformation(root_surface_transformation);

  EmbedderConfigBuilder builder(context);

  builder.SetDartEntrypoint("render_gradient");
  builder.SetOpenGLRendererConfig(SkISize::Make(600, 800));
  builder.SetCompositor();
  fml::CountDownLatch latch(1);

  sk_sp<SkImage> renderered_scene;
  context.SetNextSceneCallback([&](auto image) {
    renderered_scene = std::move(image);
    latch.CountDown();
  });

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());

  // Send a window metrics events so frames may be scheduled.
  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  // Flutter still thinks it is 800 x 600.
  event.width = 800;
  event.height = 600;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);

  latch.Wait();

  ASSERT_NE(renderered_scene, nullptr);

  ASSERT_TRUE(ImageMatchesFixture("gradient_xform.png", renderered_scene));
}

TEST_F(EmbedderTest, CanRenderGradientWithCompositorOnNonRootLayer) {
  auto& context = GetEmbedderContext();

  EmbedderConfigBuilder builder(context);

  builder.SetDartEntrypoint("render_gradient_on_non_root_backing_store");
  builder.SetOpenGLRendererConfig(SkISize::Make(800, 600));
  builder.SetCompositor();
  fml::CountDownLatch latch(1);

  context.GetCompositor().SetNextPresentCallback(
      [&](const FlutterLayer** layers, size_t layers_count) {
        ASSERT_EQ(layers_count, 3u);

        // Layer Root
        {
          FlutterBackingStore backing_store = *layers[0]->backing_store;
          backing_store.type = kFlutterBackingStoreTypeOpenGL;
          backing_store.did_update = true;
          backing_store.open_gl.type = kFlutterOpenGLTargetTypeFramebuffer;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(800.0, 600.0);
          layer.offset = FlutterPointMake(0.0, 0.0);

          ASSERT_EQ(*layers[0], layer);
        }

        // Layer 1
        {
          FlutterPlatformView platform_view = {};
          platform_view.struct_size = sizeof(platform_view);
          platform_view.identifier = 1;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypePlatformView;
          layer.platform_view = &platform_view;
          layer.size = FlutterSizeMake(100.0, 200.0);
          layer.offset = FlutterPointMake(0.0, 0.0);

          ASSERT_EQ(*layers[1], layer);
        }

        // Layer 2
        {
          FlutterBackingStore backing_store = *layers[2]->backing_store;
          backing_store.type = kFlutterBackingStoreTypeOpenGL;
          backing_store.did_update = true;
          backing_store.open_gl.type = kFlutterOpenGLTargetTypeFramebuffer;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(800.0, 600.0);
          layer.offset = FlutterPointMake(0.0, 0.0);

          ASSERT_EQ(*layers[2], layer);
        }
      });

  context.GetCompositor().SetPlatformViewRendererCallback(
      [&](const FlutterLayer& layer, GrContext* context) -> sk_sp<SkImage> {
        auto surface = CreateRenderSurface(layer, context);
        auto canvas = surface->getCanvas();
        FML_CHECK(canvas != nullptr);

        switch (layer.platform_view->identifier) {
          case 1: {
            FML_CHECK(layer.size.width == 100);
            FML_CHECK(layer.size.height == 200);
            // This is occluded anyway. We just want to make sure we see this.
          } break;
          default:
            // Asked to render an unknown platform view.
            FML_CHECK(false)
                << "Test was asked to composite an unknown platform view.";
        }

        return surface->makeImageSnapshot();
      });

  sk_sp<SkImage> renderered_scene;
  context.SetNextSceneCallback([&](auto image) {
    renderered_scene = std::move(image);
    latch.CountDown();
  });

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());

  // Send a window metrics events so frames may be scheduled.
  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);

  latch.Wait();

  ASSERT_NE(renderered_scene, nullptr);

  ASSERT_TRUE(ImageMatchesFixture("gradient.png", renderered_scene));
}

TEST_F(EmbedderTest, CanRenderGradientWithCompositorOnNonRootLayerWithXform) {
  auto& context = GetEmbedderContext();

  // This must match the transformation provided in the
  // |CanRenderGradientWithoutCompositorWithXform| test to ensure that
  // transforms are consistent respected.
  const auto root_surface_transformation =
      SkMatrix().preTranslate(0, 800).preRotate(-90, 0, 0);

  context.SetRootSurfaceTransformation(root_surface_transformation);

  EmbedderConfigBuilder builder(context);

  builder.SetDartEntrypoint("render_gradient_on_non_root_backing_store");
  builder.SetOpenGLRendererConfig(SkISize::Make(600, 800));
  builder.SetCompositor();
  fml::CountDownLatch latch(1);

  context.GetCompositor().SetNextPresentCallback(
      [&](const FlutterLayer** layers, size_t layers_count) {
        ASSERT_EQ(layers_count, 3u);

        // Layer Root
        {
          FlutterBackingStore backing_store = *layers[0]->backing_store;
          backing_store.type = kFlutterBackingStoreTypeOpenGL;
          backing_store.did_update = true;
          backing_store.open_gl.type = kFlutterOpenGLTargetTypeFramebuffer;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(600.0, 800.0);
          layer.offset = FlutterPointMake(0.0, 0.0);

          ASSERT_EQ(*layers[0], layer);
        }

        // Layer 1
        {
          FlutterPlatformView platform_view = {};
          platform_view.struct_size = sizeof(platform_view);
          platform_view.identifier = 1;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypePlatformView;
          layer.platform_view = &platform_view;
          layer.size = FlutterSizeMake(200.0, 100.0);
          layer.offset = FlutterPointMake(0.0, 700.0);

          ASSERT_EQ(*layers[1], layer);
        }

        // Layer 2
        {
          FlutterBackingStore backing_store = *layers[2]->backing_store;
          backing_store.type = kFlutterBackingStoreTypeOpenGL;
          backing_store.did_update = true;
          backing_store.open_gl.type = kFlutterOpenGLTargetTypeFramebuffer;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;
          layer.backing_store = &backing_store;
          layer.size = FlutterSizeMake(600.0, 800.0);
          layer.offset = FlutterPointMake(0.0, 0.0);

          ASSERT_EQ(*layers[2], layer);
        }
      });

  context.GetCompositor().SetPlatformViewRendererCallback(
      [&](const FlutterLayer& layer, GrContext* context) -> sk_sp<SkImage> {
        auto surface = CreateRenderSurface(layer, context);
        auto canvas = surface->getCanvas();
        FML_CHECK(canvas != nullptr);

        switch (layer.platform_view->identifier) {
          case 1: {
            FML_CHECK(layer.size.width == 200);
            FML_CHECK(layer.size.height == 100);
            // This is occluded anyway. We just want to make sure we see this.
          } break;
          default:
            // Asked to render an unknown platform view.
            FML_CHECK(false)
                << "Test was asked to composite an unknown platform view.";
        }

        return surface->makeImageSnapshot();
      });

  sk_sp<SkImage> renderered_scene;
  context.SetNextSceneCallback([&](auto image) {
    renderered_scene = std::move(image);
    latch.CountDown();
  });

  auto engine = builder.LaunchEngine();
  ASSERT_TRUE(engine.is_valid());

  // Send a window metrics events so frames may be scheduled.
  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  // Flutter still thinks it is 800 x 600.
  event.width = 800;
  event.height = 600;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);

  latch.Wait();

  ASSERT_NE(renderered_scene, nullptr);

  ASSERT_TRUE(ImageMatchesFixture("gradient_xform.png", renderered_scene));
}

TEST_F(EmbedderTest, VerifyB141980393) {
  auto& context = GetEmbedderContext();

  EmbedderConfigBuilder builder(context);

  // The Flutter application is 800 x 600 but rendering on a surface that is 600
  // x 800 achieved using a root surface transformation.
  const auto root_surface_transformation =
      SkMatrix().preTranslate(0, 800).preRotate(-90, 0, 0);
  const auto flutter_application_rect = SkRect::MakeWH(800, 600);
  const auto root_surface_rect =
      root_surface_transformation.mapRect(flutter_application_rect);

  ASSERT_DOUBLE_EQ(root_surface_rect.width(), 600.0);
  ASSERT_DOUBLE_EQ(root_surface_rect.height(), 800.0);

  // Configure the fixture for the surface transformation.
  context.SetRootSurfaceTransformation(root_surface_transformation);

  // Configure the Flutter project args for the root surface transformation.
  builder.SetOpenGLRendererConfig(
      SkISize::Make(root_surface_rect.width(), root_surface_rect.height()));

  // Use a compositor instead of rendering directly to the surface.
  builder.SetCompositor();

  builder.SetDartEntrypoint("verify_b141980393");

  fml::AutoResetWaitableEvent latch;

  context.GetCompositor().SetNextPresentCallback(
      [&](const FlutterLayer** layers, size_t layers_count) {
        ASSERT_EQ(layers_count, 2u);

        // Layer Root
        {
          FlutterLayer layer = {};
          FlutterBackingStore backing_store = *layers[0]->backing_store;
          layer.backing_store = &backing_store;
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypeBackingStore;

          // Our root surface has been rotated.
          layer.size = FlutterSizeMake(600.0, 800.0);
          layer.offset = FlutterPointMake(0.0, 0.0);

          ASSERT_EQ(*layers[0], layer);
        }

        // Layer 1
        {
          FlutterPlatformView platform_view = {};
          platform_view.struct_size = sizeof(platform_view);
          platform_view.identifier = 1337;

          FlutterLayer layer = {};
          layer.struct_size = sizeof(layer);
          layer.type = kFlutterLayerContentTypePlatformView;
          layer.platform_view = &platform_view;

          // From the Dart side. These dimensions match those specified in Dart
          // code and are free of root surface transformations.
          const double unxformed_top_margin = 31.0;
          const double unxformed_bottom_margin = 37.0;
          const auto unxformed_platform_view_rect = SkRect::MakeXYWH(
              0.0,                   // x
              unxformed_top_margin,  // y (top margin)
              800,                   // width
              600 - unxformed_top_margin - unxformed_bottom_margin  // height
          );

          // The platform views are in the coordinate space of the root surface
          // with top-left origin. The embedder has specified a transformation
          // to this surface which it must account for in the coordinates it
          // receives here.
          const auto xformed_platform_view_rect =
              root_surface_transformation.mapRect(unxformed_platform_view_rect);

          // Spell out the value that we are going to be checking below for
          // clarity.
          ASSERT_EQ(xformed_platform_view_rect,
                    SkRect::MakeXYWH(31.0,   // x
                                     0.0,    // y
                                     532.0,  // width
                                     800.0   // height
                                     ));

          // Verify that the engine is giving us the right size and offset.
          layer.offset = FlutterPointMake(xformed_platform_view_rect.x(),
                                          xformed_platform_view_rect.y());
          layer.size = FlutterSizeMake(xformed_platform_view_rect.width(),
                                       xformed_platform_view_rect.height());

          ASSERT_EQ(*layers[1], layer);
        }

        latch.Signal();
      });

  auto engine = builder.LaunchEngine();

  // Send a window metrics events so frames may be scheduled.
  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);

  // The Flutter application is 800 x 600 rendering on a surface 600 x 800
  // achieved via a root surface transformation.
  event.width = flutter_application_rect.width();
  event.height = flutter_application_rect.height();

  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  ASSERT_TRUE(engine.is_valid());

  latch.Wait();
}

//------------------------------------------------------------------------------
/// Test that an engine can be initialized but not run.
///
TEST_F(EmbedderTest, CanCreateInitializedEngine) {
  EmbedderConfigBuilder builder(GetEmbedderContext());
  builder.SetSoftwareRendererConfig();
  auto engine = builder.InitializeEngine();
  ASSERT_TRUE(engine.is_valid());
  engine.reset();
}

//------------------------------------------------------------------------------
/// Test that an initialized engine can be run exactly once.
///
TEST_F(EmbedderTest, CanRunInitializedEngine) {
  EmbedderConfigBuilder builder(GetEmbedderContext());
  builder.SetSoftwareRendererConfig();
  auto engine = builder.InitializeEngine();
  ASSERT_TRUE(engine.is_valid());
  ASSERT_EQ(FlutterEngineRunInitialized(engine.get()), kSuccess);
  // Cannot re-run an already running engine.
  ASSERT_EQ(FlutterEngineRunInitialized(engine.get()), kInvalidArguments);
  engine.reset();
}

//------------------------------------------------------------------------------
/// Test that an engine can be deinitialized.
///
TEST_F(EmbedderTest, CaDeinitializeAnEngine) {
  EmbedderConfigBuilder builder(GetEmbedderContext());
  builder.SetSoftwareRendererConfig();
  auto engine = builder.InitializeEngine();
  ASSERT_TRUE(engine.is_valid());
  ASSERT_EQ(FlutterEngineRunInitialized(engine.get()), kSuccess);
  // Cannot re-run an already running engine.
  ASSERT_EQ(FlutterEngineRunInitialized(engine.get()), kInvalidArguments);
  ASSERT_EQ(FlutterEngineDeinitialize(engine.get()), kSuccess);
  // It is ok to deinitialize an engine multiple times.
  ASSERT_EQ(FlutterEngineDeinitialize(engine.get()), kSuccess);

  // Sending events to a deinitalized engine fails.
  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kInvalidArguments);
  engine.reset();
}

//------------------------------------------------------------------------------
/// Asserts that embedders can provide a task runner for the render thread.
///
TEST_F(EmbedderTest, CanCreateEmbedderWithCustomRenderTaskRunner) {
  std::mutex engine_mutex;
  UniqueEngine engine;
  fml::AutoResetWaitableEvent task_latch;
  bool task_executed = false;
  EmbedderTestTaskRunner render_task_runner(
      CreateNewThread("custom_render_thread"), [&](FlutterTask task) {
        std::scoped_lock engine_lock(engine_mutex);
        if (engine.is_valid()) {
          ASSERT_EQ(FlutterEngineRunTask(engine.get(), &task), kSuccess);
          task_executed = true;
          task_latch.Signal();
        }
      });
  EmbedderConfigBuilder builder(GetEmbedderContext());
  builder.SetDartEntrypoint("can_render_scene_without_custom_compositor");
  builder.SetOpenGLRendererConfig(SkISize::Make(800, 600));
  builder.SetRenderTaskRunner(
      &render_task_runner.GetFlutterTaskRunnerDescription());

  {
    std::scoped_lock lock(engine_mutex);
    engine = builder.InitializeEngine();
  }

  ASSERT_EQ(FlutterEngineRunInitialized(engine.get()), kSuccess);

  ASSERT_TRUE(engine.is_valid());

  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = 800;
  event.height = 600;
  ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
            kSuccess);
  task_latch.Wait();
  ASSERT_TRUE(task_executed);
  ASSERT_EQ(FlutterEngineDeinitialize(engine.get()), kSuccess);

  {
    std::scoped_lock engine_lock(engine_mutex);
    engine.reset();
  }
}

//------------------------------------------------------------------------------
/// Asserts that the render task runner can be the same as the platform task
/// runner.
///
TEST_F(EmbedderTest,
       CanCreateEmbedderWithCustomRenderTaskRunnerTheSameAsPlatformTaskRunner) {
  // A new thread needs to be created for the platform thread because the test
  // can't wait for assertions to be completed on the same thread that services
  // platform task runner tasks.
  auto platform_task_runner = CreateNewThread("platform_thread");

  static std::mutex engine_mutex;
  static UniqueEngine engine;
  fml::AutoResetWaitableEvent task_latch;
  bool task_executed = false;
  EmbedderTestTaskRunner common_task_runner(
      platform_task_runner, [&](FlutterTask task) {
        std::scoped_lock engine_lock(engine_mutex);
        if (engine.is_valid()) {
          ASSERT_EQ(FlutterEngineRunTask(engine.get(), &task), kSuccess);
          task_executed = true;
          task_latch.Signal();
        }
      });

  platform_task_runner->PostTask([&]() {
    EmbedderConfigBuilder builder(GetEmbedderContext());
    builder.SetDartEntrypoint("can_render_scene_without_custom_compositor");
    builder.SetOpenGLRendererConfig(SkISize::Make(800, 600));
    builder.SetRenderTaskRunner(
        &common_task_runner.GetFlutterTaskRunnerDescription());
    builder.SetPlatformTaskRunner(
        &common_task_runner.GetFlutterTaskRunnerDescription());

    {
      std::scoped_lock lock(engine_mutex);
      engine = builder.InitializeEngine();
    }

    ASSERT_EQ(FlutterEngineRunInitialized(engine.get()), kSuccess);

    ASSERT_TRUE(engine.is_valid());

    FlutterWindowMetricsEvent event = {};
    event.struct_size = sizeof(event);
    event.width = 800;
    event.height = 600;
    ASSERT_EQ(FlutterEngineSendWindowMetricsEvent(engine.get(), &event),
              kSuccess);
  });

  task_latch.Wait();

  // Don't use the task latch because that may be called multiple time
  // (including during the shutdown process).
  fml::AutoResetWaitableEvent shutdown_latch;

  platform_task_runner->PostTask([&]() {
    ASSERT_TRUE(task_executed);
    ASSERT_EQ(FlutterEngineDeinitialize(engine.get()), kSuccess);

    {
      std::scoped_lock engine_lock(engine_mutex);
      engine.reset();
    }
    shutdown_latch.Signal();
  });

  shutdown_latch.Wait();

  {
    std::scoped_lock engine_lock(engine_mutex);
    // Engine should have been killed by this point.
    ASSERT_FALSE(engine.is_valid());
  }
}

}  // namespace testing
}  // namespace flutter

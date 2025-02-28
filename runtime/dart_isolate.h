// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_RUNTIME_DART_ISOLATE_H_
#define FLUTTER_RUNTIME_DART_ISOLATE_H_

#include <memory>
#include <set>
#include <string>

#include "flutter/common/task_runners.h"
#include "flutter/fml/compiler_specific.h"
#include "flutter/fml/macros.h"
#include "flutter/fml/mapping.h"
#include "flutter/lib/ui/io_manager.h"
#include "flutter/lib/ui/ui_dart_state.h"
#include "flutter/lib/ui/window/window.h"
#include "flutter/runtime/dart_snapshot.h"
#include "third_party/dart/runtime/include/dart_api.h"
#include "third_party/tonic/dart_state.h"

namespace flutter {

class DartVM;

//------------------------------------------------------------------------------
/// @brief      Represents an instance of a live isolate. An isolate is a
///             separate Dart execution context. Different Dart isolates don't
///             share memory and can be scheduled concurrently by the Dart VM on
///             one of the Dart VM managed worker pool threads.
///
///             The entire lifecycle of a Dart isolate is controlled by the Dart
///             VM. Because of this, the engine never holds a strong pointer to
///             the Dart VM for extended periods of time. This allows the VM (or
///             the isolates themselves) to terminate Dart execution without
///             consulting the engine.
///
///             The isolate that the engine creates to act as the host for the
///             Flutter application code with UI bindings is called the root
///             isolate.
///
///             The root isolate is special in the following ways:
///             * The root isolate forms a new isolate group. Child isolates are
///               added to their parents groups. When the root isolate dies, all
///               isolates in its group are terminated.
///             * Only root isolates get UI bindings.
///             * Root isolates execute their code on engine managed threads.
///               All other isolates run their Dart code on Dart VM managed
///               thread pool workers that the engine has no control over.
///             * Since the engine does not know the thread on which non-root
///               isolates are run, the engine has no opportunity to get a
///               reference to non-root isolates. Such isolates can only be
///               terminated if they terminate themselves or their isolate group
///               is torn down.
///
class DartIsolate : public UIDartState {
 public:
  //----------------------------------------------------------------------------
  /// @brief      The engine represents all dart isolates as being in one of the
  ///             known phases. By invoking various methods on the Dart isolate,
  ///             the engine transition the Dart isolate from one phase to the
  ///             next. The Dart isolate will only move from one phase to the
  ///             next in the order specified in the `DartIsolate::Phase` enum.
  ///             That is, once the isolate has moved out of a particular phase,
  ///             it can never transition back to that phase in the future.
  ///             There is no error recovery mechanism and callers that find
  ///             their isolates in an undesirable phase must discard the
  ///             isolate and start over.
  ///
  enum class Phase {
    //--------------------------------------------------------------------------
    /// The initial phase of all Dart isolates. This is an internal phase and
    /// callers can never get a reference to a Dart isolate in this phase.
    ///
    Unknown,
    //--------------------------------------------------------------------------
    /// The Dart isolate has been created but none of the library tag or message
    /// handers have been set yet. The is an internal phase and callers can
    /// never get a reference to a Dart isolate in this phase.
    ///
    Uninitialized,
    //--------------------------------------------------------------------------
    /// The Dart isolate has been been fully initialized but none of the
    /// libraries referenced by that isolate have been loaded yet. This is an
    /// internal phase and callers can never get a reference to a Dart isolate
    /// in this phase.
    ///
    Initialized,
    //--------------------------------------------------------------------------
    /// The isolate has been fully initialized and is waiting for the caller to
    /// associate isolate snapshots with the same. The isolate will only be
    /// ready to execute Dart code once one of the `Prepare` calls are
    /// successfully made.
    ///
    LibrariesSetup,
    //--------------------------------------------------------------------------
    /// The isolate is fully ready to start running Dart code. Callers can
    /// transition the isolate to the next state by calling the `Run` or
    /// `RunFromLibrary` methods.
    ///
    Ready,
    //--------------------------------------------------------------------------
    /// The isolate is currently running Dart code.
    ///
    Running,
    //--------------------------------------------------------------------------
    /// The isolate is no longer running Dart code and is in the middle of being
    /// collected. This is in internal phase and callers can never get a
    /// reference to a Dart isolate in this phase.
    ///
    Shutdown,
  };

  //----------------------------------------------------------------------------
  /// @brief      Creates an instance of a root isolate and returns a weak
  ///             pointer to the same. The isolate instance may only be used
  ///             safely on the engine thread on which it was created. In the
  ///             shell, this is the UI thread and task runner. Using the
  ///             isolate on any other thread is user error.
  ///
  ///             The isolate that the engine creates to act as the host for the
  ///             Flutter application code with UI bindings is called the root
  ///             isolate.
  ///
  ///             The root isolate is special in the following ways:
  ///             * The root isolate forms a new isolate group. Child isolates
  ///               are added to their parents groups. When the root isolate
  ///               dies, all isolates in its group are terminated.
  ///             * Only root isolates get UI bindings.
  ///             * Root isolates execute their code on engine managed threads.
  ///               All other isolates run their Dart code on Dart VM managed
  ///               thread pool workers that the engine has no control over.
  ///             * Since the engine does not know the thread on which non-root
  ///               isolates are run, the engine has no opportunity to get a
  ///               reference to non-root isolates. Such isolates can only be
  ///               terminated if they terminate themselves or their isolate
  ///               group is torn down.
  ///
  /// @param[in]  settings                    The settings used to create the
  ///                                         isolate.
  /// @param[in]  isolate_snapshot            The isolate snapshot. This is
  ///                                         usually obtained from the
  ///                                         DartVMData associated with the
  ///                                         running Dart VM instance.
  /// @param[in]  shared_snapshot             The shared snapshot. This is
  ///                                         usually obtained from the
  ///                                         DartVMData associated with the
  ///                                         running Dart VM instance.
  /// @param[in]  task_runners                The task runners used by the
  ///                                         isolate. Via UI bindings, the
  ///                                         isolate will use the IO task
  ///                                         runner to scheduled texture
  ///                                         decompression jobs and post tasks
  ///                                         back to the UI task runner.
  /// @param[in]  window                      The weak pointer to the window
  ///                                         associated with this root isolate.
  /// @param[in]  io_manager                  The i/o manager.
  /// @param[in]  image_decoder               The image decoder.
  /// @param[in]  advisory_script_uri         The advisory script uri. This is
  ///                                         only used in instrumentation.
  /// @param[in]  advisory_script_entrypoint  The advisory script entrypoint.
  ///                                         This is only used in
  ///                                         instrumentation. Notably, this is
  ///                                         NOT the main entrypoint of Dart
  ///                                         code in the isolate. That is
  ///                                         specified when making one of the
  ///                                         `Run` calls.
  /// @param[in]  flags                       The Dart isolate flags for this
  ///                                         isolate instance.
  /// @param[in]  isolate_create_callback     The isolate create callback. This
  ///                                         will be called when the before the
  ///                                         main Dart entrypoint is invoked in
  ///                                         the root isolate. The isolate is
  ///                                         already in the running state at
  ///                                         this point and an isolate scope is
  ///                                         current.
  /// @param[in]  isolate_shutdown_callback   The isolate shutdown callback.
  ///                                         This will be called before the
  ///                                         isolate is about to transition
  ///                                         into the Shutdown phase. The
  ///                                         isolate is still running at this
  ///                                         point and an isolate scope is
  ///                                         current.
  ///
  /// @return     A weak pointer to the root Dart isolate. The caller must
  ///             ensure that the isolate is not referenced for long periods of
  ///             time as it prevents isolate collection when the isolate
  ///             terminates itself. The caller may also only use the isolate on
  ///             the thread on which the isolate was created.
  ///
  static std::weak_ptr<DartIsolate> CreateRootIsolate(
      const Settings& settings,
      fml::RefPtr<const DartSnapshot> isolate_snapshot,
      fml::RefPtr<const DartSnapshot> shared_snapshot,
      TaskRunners task_runners,
      std::unique_ptr<Window> window,
      fml::WeakPtr<IOManager> io_manager,
      fml::WeakPtr<ImageDecoder> image_decoder,
      std::string advisory_script_uri,
      std::string advisory_script_entrypoint,
      Dart_IsolateFlags* flags,
      fml::closure isolate_create_callback,
      fml::closure isolate_shutdown_callback);

  // |UIDartState|
  ~DartIsolate() override;

  //----------------------------------------------------------------------------
  /// @brief      Get the settings used to create this isolate instance.
  ///
  /// @return     The settings used in the `CreateRootIsolate` call.
  ///
  const Settings& GetSettings() const;

  //----------------------------------------------------------------------------
  /// @brief      The current phase of the isolate. The engine represents all
  ///             dart isolates as being in one of the known phases. By invoking
  ///             various methods on the Dart isolate, the engine transitions
  ///             the Dart isolate from one phase to the next. The Dart isolate
  ///             will only move from one phase to the next in the order
  ///             specified in the `DartIsolate::Phase` enum. That is, the once
  ///             the isolate has moved out of a particular phase, it can never
  ///             transition back to that phase in the future. There is no error
  ///             recovery mechanism and callers that find their isolates in an
  ///             undesirable phase must discard the isolate and start over.
  ///
  /// @return     The current isolate phase.
  ///
  Phase GetPhase() const;

  //----------------------------------------------------------------------------
  /// @brief      Returns the ID for an isolate which is used to query the
  ///             service protocol.
  ///
  /// @return     The service identifier for this isolate.
  ///
  std::string GetServiceId();

  //----------------------------------------------------------------------------
  /// @brief      Prepare the isolate for running for a precompiled code bundle.
  ///             The Dart VM must be configured for running precompiled code.
  ///
  ///             The isolate must already be in the `Phase::LibrariesSetup`
  ///             phase. After a successful call to this method, the isolate
  ///             will transition to the `Phase::Ready` phase.
  ///
  /// @return     Whether the isolate was prepared and the described phase
  ///             transition made.
  ///
  FML_WARN_UNUSED_RESULT
  bool PrepareForRunningFromPrecompiledCode();

  //----------------------------------------------------------------------------
  /// @brief      Prepare the isolate for running for a a list of kernel files.
  ///
  ///             The Dart VM must be configured for running from kernel
  ///             snapshots.
  ///
  ///             The isolate must already be in the `Phase::LibrariesSetup`
  ///             phase. This call can be made multiple times. After a series of
  ///             successful calls to this method, the caller can specify the
  ///             last kernel file mapping by specifying `last_piece` to `true`.
  ///             On success, the isolate will transition to the `Phase::Ready`
  ///             phase.
  ///
  /// @param[in]  kernel      The kernel mapping.
  /// @param[in]  last_piece  Indicates if this is the last kernel mapping
  ///                         expected. After this point, the isolate will
  ///                         attempt a transition to the `Phase::Ready` phase.
  ///
  /// @return     If the kernel mapping supplied was successfully used to
  ///             prepare the isolate.
  ///
  FML_WARN_UNUSED_RESULT
  bool PrepareForRunningFromKernel(std::shared_ptr<const fml::Mapping> kernel,
                                   bool last_piece = true);

  //----------------------------------------------------------------------------
  /// @brief      Prepare the isolate for running for a a list of kernel files.
  ///
  ///             The Dart VM must be configured for running from kernel
  ///             snapshots.
  ///
  ///             The isolate must already be in the `Phase::LibrariesSetup`
  ///             phase. After a successful call to this method, the isolate
  ///             will transition to the `Phase::Ready` phase.
  ///
  /// @param[in]  kernels  The kernels
  ///
  /// @return     If the kernel mappings supplied were successfully used to
  ///             prepare the isolate.
  ///
  FML_WARN_UNUSED_RESULT
  bool PrepareForRunningFromKernels(
      std::vector<std::shared_ptr<const fml::Mapping>> kernels);

  //----------------------------------------------------------------------------
  /// @brief      Prepare the isolate for running for a a list of kernel files.
  ///
  ///             The Dart VM must be configured for running from kernel
  ///             snapshots.
  ///
  ///             The isolate must already be in the `Phase::LibrariesSetup`
  ///             phase. After a successful call to this method, the isolate
  ///             will transition to the `Phase::Ready` phase.
  ///
  /// @param[in]  kernels  The kernels
  ///
  /// @return     If the kernel mappings supplied were successfully used to
  ///             prepare the isolate.
  ///
  FML_WARN_UNUSED_RESULT
  bool PrepareForRunningFromKernels(
      std::vector<std::unique_ptr<const fml::Mapping>> kernels);

  //----------------------------------------------------------------------------
  /// @brief      Transition the root isolate to the `Phase::Running` phase and
  ///             invoke the main entrypoint (the "main" method) in the root
  ///             library. The isolate must already be in the `Phase::Ready`
  ///             phase.
  ///
  /// @param[in]  entrypoint  The entrypoint in the root library.
  /// @param[in]  args        A list of string arguments to the entrypoint.
  /// @param[in]  on_run      A callback to run in isolate scope after the main
  ///                         entrypoint has been invoked. There is no isolate
  ///                         scope current on the thread once this method
  ///                         returns.
  ///
  /// @return     If the isolate successfully transitioned to the running phase
  ///             and the main entrypoint was invoked.
  ///
  FML_WARN_UNUSED_RESULT
  bool Run(const std::string& entrypoint,
           const std::vector<std::string>& args,
           fml::closure on_run = nullptr);

  //----------------------------------------------------------------------------
  /// @brief      Transition the root isolate to the `Phase::Running` phase and
  ///             invoke the main entrypoint (the "main" method) in the
  ///             specified library. The isolate must already be in the
  ///             `Phase::Ready` phase.
  ///
  /// @param[in]  library_name  The name of the library in which to invoke the
  ///                           supplied entrypoint.
  /// @param[in]  entrypoint    The entrypoint in `library_name`
  /// @param[in]  args          A list of string arguments to the entrypoint.
  /// @param[in]  on_run        A callback to run in isolate scope after the
  ///                           main entrypoint has been invoked. There is no
  ///                           isolate scope current on the thread once this
  ///                           method returns.
  ///
  /// @return     If the isolate successfully transitioned to the running phase
  ///             and the main entrypoint was invoked.
  ///
  FML_WARN_UNUSED_RESULT
  bool RunFromLibrary(const std::string& library_name,
                      const std::string& entrypoint,
                      const std::vector<std::string>& args,
                      fml::closure on_run = nullptr);

  //----------------------------------------------------------------------------
  /// @brief      Transition the isolate to the `Phase::Shutdown` phase. The
  ///             only thing left to do is to collect the isolate.
  ///
  /// @return     If the isolate succesfully transitioned to the shutdown phase.
  ///
  FML_WARN_UNUSED_RESULT
  bool Shutdown();

  //----------------------------------------------------------------------------
  /// @brief      Registers a callback that will be invoked in isolate scope
  ///             just before the isolate transitions to the `Phase::Shutdown`
  ///             phase.
  ///
  /// @param[in]  closure  The callback to invoke on isolate shutdown.
  ///
  void AddIsolateShutdownCallback(fml::closure closure);

  //----------------------------------------------------------------------------
  /// @brief      The snapshot used to launch this isolate. This is referenced
  ///             by any child isolates launched by the root isolate.
  ///
  /// @return     The isolate snapshot.
  ///
  fml::RefPtr<const DartSnapshot> GetIsolateSnapshot() const;

  //----------------------------------------------------------------------------
  /// @brief      Get the shared snapshot used to launch this isolate. This is
  ///             referenced by any child isolates launched by the root isolate.
  ///
  /// @return     The shared snapshot.
  ///
  fml::RefPtr<const DartSnapshot> GetSharedSnapshot() const;

  //----------------------------------------------------------------------------
  /// @brief      A weak pointer to the Dart isolate instance. This instance may
  ///             only be used on the task runner that created the root isolate.
  ///
  /// @return     The weak isolate pointer.
  ///
  std::weak_ptr<DartIsolate> GetWeakIsolatePtr();

  //----------------------------------------------------------------------------
  /// @brief      The task runner on which the Dart code for the root isolate is
  ///             running. For the root isolate, this is the UI task runner for
  ///             the shell that owns the root isolate.
  ///
  /// @return     The message handling task runner.
  ///
  fml::RefPtr<fml::TaskRunner> GetMessageHandlingTaskRunner() const;

 private:
  using ChildIsolatePreparer = std::function<bool(DartIsolate*)>;

  class AutoFireClosure {
   public:
    AutoFireClosure(fml::closure closure);

    ~AutoFireClosure();

   private:
    fml::closure closure_;
    FML_DISALLOW_COPY_AND_ASSIGN(AutoFireClosure);
  };
  friend class DartVM;

  Phase phase_ = Phase::Unknown;
  const Settings settings_;
  const fml::RefPtr<const DartSnapshot> isolate_snapshot_;
  const fml::RefPtr<const DartSnapshot> shared_snapshot_;
  std::vector<std::shared_ptr<const fml::Mapping>> kernel_buffers_;
  std::vector<std::unique_ptr<AutoFireClosure>> shutdown_callbacks_;
  ChildIsolatePreparer child_isolate_preparer_ = nullptr;
  fml::RefPtr<fml::TaskRunner> message_handling_task_runner_;
  const fml::closure isolate_create_callback_;
  const fml::closure isolate_shutdown_callback_;

  DartIsolate(const Settings& settings,
              fml::RefPtr<const DartSnapshot> isolate_snapshot,
              fml::RefPtr<const DartSnapshot> shared_snapshot,
              TaskRunners task_runners,
              fml::WeakPtr<IOManager> io_manager,
              fml::WeakPtr<ImageDecoder> image_decoder,
              std::string advisory_script_uri,
              std::string advisory_script_entrypoint,
              ChildIsolatePreparer child_isolate_preparer,
              fml::closure isolate_create_callback,
              fml::closure isolate_shutdown_callback);

  FML_WARN_UNUSED_RESULT bool Initialize(Dart_Isolate isolate,
                                         bool is_root_isolate);

  void SetMessageHandlingTaskRunner(fml::RefPtr<fml::TaskRunner> runner,
                                    bool is_root_isolate);

  bool LoadKernel(std::shared_ptr<const fml::Mapping> mapping, bool last_piece);

  FML_WARN_UNUSED_RESULT
  bool LoadLibraries(bool is_root_isolate);

  bool UpdateThreadPoolNames() const;

  FML_WARN_UNUSED_RESULT
  bool MarkIsolateRunnable();

  void OnShutdownCallback();

  // |Dart_IsolateGroupCreateCallback|
  static Dart_Isolate DartIsolateGroupCreateCallback(
      const char* advisory_script_uri,
      const char* advisory_script_entrypoint,
      const char* package_root,
      const char* package_config,
      Dart_IsolateFlags* flags,
      std::shared_ptr<DartIsolate>* embedder_isolate,
      char** error);

  static Dart_Isolate DartCreateAndStartServiceIsolate(
      const char* package_root,
      const char* package_config,
      Dart_IsolateFlags* flags,
      char** error);

  static std::pair<Dart_Isolate /* vm */,
                   std::weak_ptr<DartIsolate> /* embedder */>
  CreateDartVMAndEmbedderObjectPair(
      const char* advisory_script_uri,
      const char* advisory_script_entrypoint,
      const char* package_root,
      const char* package_config,
      Dart_IsolateFlags* flags,
      std::shared_ptr<DartIsolate>* parent_embedder_isolate,
      bool is_root_isolate,
      char** error);

  // |Dart_IsolateShutdownCallback|
  static void DartIsolateShutdownCallback(
      std::shared_ptr<DartIsolate>* isolate_group_data,
      std::shared_ptr<DartIsolate>* isolate_data);

  // |Dart_IsolateGroupCleanupCallback|
  static void DartIsolateGroupCleanupCallback(
      std::shared_ptr<DartIsolate>* isolate_group_data);

  FML_DISALLOW_COPY_AND_ASSIGN(DartIsolate);
};

}  // namespace flutter

#endif  // FLUTTER_RUNTIME_DART_ISOLATE_H_

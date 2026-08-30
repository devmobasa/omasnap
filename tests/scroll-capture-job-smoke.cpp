#include "scroll-capture-job-smoke.hpp"

#include "scroll-capture-job.hpp"
#include "scroll-capture.hpp"
#include "scroll-inject.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFutureWatcher>
#include <QImage>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <poll.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <vector>

namespace {
class StableProcessIdentity final {
public:
  StableProcessIdentity() = default;
  ~StableProcessIdentity() {
    if (fd_ >= 0)
      ::close(fd_);
  }

  StableProcessIdentity(const StableProcessIdentity &) = delete;
  StableProcessIdentity &operator=(const StableProcessIdentity &) = delete;

  bool open(const pid_t processId) {
    fd_ = static_cast<int>(::syscall(SYS_pidfd_open, processId, 0));
    return fd_ >= 0;
  }

  bool exited() const {
    pollfd descriptor{fd_, POLLIN, 0};
    const int result = ::poll(&descriptor, 1, 0);
    return result == 1 && (descriptor.revents & (POLLIN | POLLHUP)) != 0;
  }

  void killForCleanup() const {
    if (!exited())
      ::syscall(SYS_pidfd_send_signal, fd_, SIGKILL, nullptr, 0);
  }

private:
  int fd_ = -1;
};

bool readProcessId(const QString &path, pid_t &processId) {
  QFile file(path);
  bool valid = false;
  if (!file.open(QIODevice::ReadOnly))
    return false;
  const qint64 value = file.readAll().trimmed().toLongLong(&valid);
  if (!valid || value <= 0 || value > std::numeric_limits<pid_t>::max())
    return false;
  processId = static_cast<pid_t>(value);
  return true;
}

double elapsedMs(const QElapsedTimer &timer) {
  return static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
}

bool waitForFinished(QFutureWatcher<ScrollCaptureJobResult> &watcher,
                     const int timeoutMs) {
  if (watcher.isFinished())
    return true;
  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  QObject::connect(&watcher, &QFutureWatcher<ScrollCaptureJobResult>::finished,
                   &loop, &QEventLoop::quit);
  QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
  timeout.start(timeoutMs);
  loop.exec();
  return watcher.isFinished();
}

bool waitForStage(const std::shared_ptr<ScrollCaptureJobControl> &control,
                  const ScrollCaptureJobStage stage, const int timeoutMs) {
  if (control->snapshot().stage == stage)
    return true;
  QEventLoop loop;
  QTimer poll;
  QTimer timeout;
  poll.setInterval(2);
  timeout.setSingleShot(true);
  QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
    if (control->snapshot().stage == stage)
      loop.quit();
  });
  QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
  poll.start();
  timeout.start(timeoutMs);
  loop.exec();
  return control->snapshot().stage == stage;
}

template <typename Predicate>
bool waitUntil(Predicate predicate, const int timeoutMs) {
  QElapsedTimer timeout;
  timeout.start();
  while (!predicate() && timeout.elapsed() < timeoutMs)
    QTest::qWait(2);
  return predicate();
}

bool writeExecutable(const QString &path, const QByteArray &contents) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size())
    return false;
  file.close();
  return QFile::setPermissions(
      path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                QFileDevice::ExeOwner);
}

class ProductionSetupTestGuard final {
public:
  ProductionSetupTestGuard()
      : path_(qgetenv("PATH")), signature_(qgetenv("HYPRLAND_INSTANCE_SIGNATURE")),
        hadSignature_(qEnvironmentVariableIsSet("HYPRLAND_INSTANCE_SIGNATURE")) {}
  ~ProductionSetupTestGuard() {
    OutputCapture::clearOpenTestDoubleForTest();
    qputenv("PATH", path_);
    if (hadSignature_)
      qputenv("HYPRLAND_INSTANCE_SIGNATURE", signature_);
    else
      qunsetenv("HYPRLAND_INSTANCE_SIGNATURE");
  }

private:
  QByteArray path_;
  QByteArray signature_;
  bool hadSignature_ = false;
};
} // namespace

bool runScrollCaptureJobChecks(QString &error) {
  QImage expected(64, 64, QImage::Format_RGBA8888);
  expected.fill(QColor(20, 80, 160));
  if (pointerEventsAfterCancelDuringParkForTest() != 1) {
    error = QStringLiteral("Cancel during pointer park emitted a late nudge");
    return false;
  }
  if (!runtimeInjectionFailureBecomesFailedForTest()) {
    error = QStringLiteral("Runtime injector failure did not fail the session");
    return false;
  }
  if (!injectorOwnerExceptionBecomesFailedForTest()) {
    error = QStringLiteral(
        "injector-owner exception escaped instead of failing the session");
    return false;
  }
  const ScrollCaptureJobResult allocationFailure =
      runFailingScrollCaptureJobForTest(
          std::make_shared<ScrollCaptureJobControl>());
  if (allocationFailure.completion != ScrollCaptureJobCompletion::Failed ||
      !allocationFailure.error.contains(QStringLiteral("memory"),
                                        Qt::CaseInsensitive)) {
    error = QStringLiteral("std::bad_alloc escaped the scroll job boundary");
    return false;
  }
  if (!autoInjectorOwnerUnwindsForTest()) {
    error = QStringLiteral("allocation unwind did not stop the injector owner");
    return false;
  }
  if (!terminalCommandStopsActiveEpisodeForTest(
          ScrollCaptureJobCommand::Done) ||
      !terminalCommandStopsActiveEpisodeForTest(
          ScrollCaptureJobCommand::Cancel)) {
    error = QStringLiteral("terminal command left an injection episode active");
    return false;
  }
  if (!terminalCommandRejectsLateEpisodeForTest(
          ScrollCaptureJobCommand::Done) ||
      !terminalCommandRejectsLateEpisodeForTest(
          ScrollCaptureJobCommand::Cancel)) {
    error = QStringLiteral(
        "terminal command allowed a new injection episode to register");
    return false;
  }
  for (const auto &[command, completion] : {
           std::pair{ScrollCaptureJobCommand::Done,
                     ScrollCaptureJobCompletion::NoFrames},
           std::pair{ScrollCaptureJobCommand::Cancel,
                     ScrollCaptureJobCompletion::Cancelled},
           std::pair{ScrollCaptureJobCommand::Back,
                     ScrollCaptureJobCompletion::Back},
       }) {
    const ScrollCaptureJobResult race =
        runTerminalEpisodeRegistrationRaceForTest(command);
    if (race.completion != completion) {
      error = QStringLiteral(
          "terminal command did not drain rejected episode registration");
      return false;
    }
  }
  if (!debugImagesRemainUnpublishedOnCancelForTest()) {
    error = QStringLiteral(
        "Cancel published one or more staged scroll debug images");
    return false;
  }
  auto stitchFailureControl = std::make_shared<ScrollCaptureJobControl>();
  auto stitchFailureEffects = std::make_shared<std::atomic<int>>(0);
  QFutureWatcher<ScrollCaptureJobResult> stitchFailureWatcher;
  stitchFailureWatcher.setFuture(
      QtConcurrent::run([stitchFailureControl, stitchFailureEffects] {
        return runDelayedScrollCaptureJobForTest(stitchFailureControl, 0, 0, {},
                                                 stitchFailureEffects);
      }));
  if (!waitForStage(stitchFailureControl, ScrollCaptureJobStage::Capturing,
                    1000)) {
    error = QStringLiteral("stitch-failure job did not become ready");
    return false;
  }
  stitchFailureControl->request(ScrollCaptureJobCommand::Done);
  if (!waitForFinished(stitchFailureWatcher, 1000) ||
      stitchFailureWatcher.result().completion !=
          ScrollCaptureJobCompletion::Failed) {
    error = QStringLiteral("stitch failure did not terminate the capture job");
    return false;
  }

  ScrollCapturePanel finishingPanel(MonitorInfo{}, nullptr, nullptr);
  finishingPanel.resize(800, 600);
  finishingPanel.show();
  auto finishingEffects = std::make_shared<std::atomic<int>>(0);
  QSignalSpy dismissedSpy(&finishingPanel, &ScrollCapturePanel::dismissed);
  QSignalSpy stitchedSpy(&finishingPanel, &ScrollCapturePanel::stitched);
  finishingPanel.startDelayedCaptureForTest(0, 250, expected, finishingEffects);
  if (!waitUntil([&] { return finishingPanel.capturingForTest(); }, 1000)) {
    error = QStringLiteral("finalization-cancel panel did not enter Capturing");
    return false;
  }
  QApplication::processEvents();
  const QPoint doneCenter = finishingPanel.doneButtonCenterForTest();
  QMouseEvent donePress(QEvent::MouseButtonPress, QPointF(doneCenter),
                        QPointF(doneCenter), Qt::LeftButton, Qt::LeftButton,
                        Qt::NoModifier);
  QElapsedTimer doneHandler;
  doneHandler.start();
  QApplication::sendEvent(&finishingPanel, &donePress);
  const double doneHandlerMs = elapsedMs(doneHandler);
  const QPoint cancelCenter = finishingPanel.cancelButtonCenterForTest();
  QMouseEvent cancelPress(QEvent::MouseButtonPress, QPointF(cancelCenter),
                          QPointF(cancelCenter), Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
  QElapsedTimer cancelHandler;
  cancelHandler.start();
  QApplication::sendEvent(&finishingPanel, &cancelPress);
  const double cancelHandlerMs = elapsedMs(cancelHandler);
  if (dismissedSpy.count() != 1 ||
      !waitUntil([&] { return !finishingPanel.workerRunningForTest(); },
                 1000) ||
      stitchedSpy.count() != 0 || doneHandlerMs >= 2.0 ||
      cancelHandlerMs >= 2.0) {
    error = QStringLiteral("visible Cancel did not cancel finalization");
    return false;
  }

  // Preparing exposes Cancel only. A premature Done request must not turn a
  // slow injector/setup path into NoFrames and dismiss the capture.
  ScrollCapturePanel preparingPanel(MonitorInfo{}, nullptr, nullptr);
  auto preparingEffects = std::make_shared<std::atomic<int>>(0);
  preparingPanel.startDelayedCaptureForTest(250, 0, expected, preparingEffects);
  preparingPanel.finishCaptureForTest();
  QTest::qWait(20);
  if (!preparingPanel.workerRunningForTest() ||
      preparingEffects->load(std::memory_order_acquire) != 0) {
    error = QStringLiteral("Done escaped the Preparing-only-cancel state");
    return false;
  }
  QSignalSpy preparingDismissedSpy(&preparingPanel,
                                   &ScrollCapturePanel::dismissed);
  QTest::mouseClick(&preparingPanel, Qt::LeftButton, Qt::NoModifier,
                    preparingPanel.cancelButtonCenterForTest());
  if (preparingDismissedSpy.count() != 1 ||
      !waitUntil([&] { return !preparingPanel.workerRunningForTest(); },
                 1000)) {
    error = QStringLiteral("Preparing Cancel did not stop the worker");
    return false;
  }

  // Exercise the actual QObject/QFutureWatcher lifetime: destruction must be
  // prompt, must cancel setup, and must leave no receiver for a late callback.
  auto panelEffects = std::make_shared<std::atomic<int>>(0);
  auto *panel = new ScrollCapturePanel(MonitorInfo{}, nullptr, nullptr);
  panel->startDelayedCaptureForTest(250, 0, expected, panelEffects);
  QElapsedTimer destruction;
  destruction.start();
  delete panel;
  if (elapsedMs(destruction) >= 16.0) {
    error = QStringLiteral("destroying an active scroll panel blocked the GUI");
    return false;
  }
  QTest::qWait(300);
  if (panelEffects->load(std::memory_order_acquire) != 0) {
    error =
        QStringLiteral("destroyed scroll panel produced a late side effect");
    return false;
  }

  // A worker-side stitch failure returns the real panel to mode selection with
  // a visible error; it must not remain in Finishing.
  ScrollCapturePanel failurePanel(MonitorInfo{}, nullptr, nullptr);
  auto failureEffects = std::make_shared<std::atomic<int>>(0);
  failurePanel.startDelayedCaptureForTest(0, 0, {}, failureEffects);
  if (!waitUntil([&] { return failurePanel.capturingForTest(); }, 1000)) {
    error = QStringLiteral("failure panel did not enter Capturing");
    return false;
  }
  failurePanel.finishCaptureForTest();
  if (!waitUntil(
          [&] {
            return !failurePanel.workerRunningForTest() &&
                   failurePanel.selectedForTest();
          },
          1000) ||
      !failurePanel.statusForTest().contains(QStringLiteral("Stitch failed"))) {
    error = QStringLiteral("stitch failure left the panel state unusable");
    return false;
  }

  // Exercise the production coordinator with delayed component-level setup:
  // OutputCapture::open is an inert delayed Wayland double, while the actual
  // stop-aware process-group path launches a delayed fake hyprctl. The GUI
  // heartbeat must stay live, Cancel must terminate before any backend can
  // inject, and a stubborn child must exit before the worker drains.
  {
    QTemporaryDir commands;
    if (!commands.isValid()) {
      error = QStringLiteral("could not create scroll setup command directory");
      return false;
    }
    const QString hyprctl =
        QDir(commands.path()).filePath(QStringLiteral("hyprctl"));
    const QString childPidFile =
        QDir(commands.path()).filePath(QStringLiteral("child.pid"));
    if (!writeExecutable(
            hyprctl,
            QByteArrayLiteral("#!/usr/bin/env bash\n"
                              "trap '' TERM\n"
                              "sleep 10 &\n"
                              "printf '%s\\n' \"$!\" > \"${0%/*}/child.pid\"\n"
                              "wait\n"))) {
      error = QStringLiteral("could not create delayed fake hyprctl");
      return false;
    }
    ProductionSetupTestGuard setupGuard;
    qputenv("PATH", commands.path().toUtf8() + ':' + qgetenv("PATH"));
    qputenv("HYPRLAND_INSTANCE_SIGNATURE", "omasnap-scroll-smoke");
    OutputCapture::setOpenTestDoubleForTest(QSize(320, 240), 150);

    ScrollCapturePanel productionPanel(MonitorInfo{}, nullptr, nullptr);
    productionPanel.resize(800, 600);
    productionPanel.show();
    QSignalSpy productionDismissed(&productionPanel,
                                   &ScrollCapturePanel::dismissed);
    std::vector<double> setupHeartbeatGaps;
    QElapsedTimer setupHeartbeatClock;
    QTimer setupHeartbeat;
    setupHeartbeat.setInterval(1);
    QObject::connect(&setupHeartbeat, &QTimer::timeout, [&] {
      setupHeartbeatGaps.push_back(elapsedMs(setupHeartbeatClock));
      setupHeartbeatClock.restart();
    });
    setupHeartbeatClock.start();
    setupHeartbeat.start();
    productionPanel.startCaptureJobForTest(
        {QStringLiteral("TEST"), QRect(0, 0, 64, 64), {},
         ScrollCaptureJobMode::Auto, stitch::Axis::Vertical, 32, 32, {}});
    if (!waitUntil(
            [&] {
              return productionPanel.statusForTest().contains(
                  QStringLiteral("Preparing auto-scroll"));
            },
            1000)) {
      error = QStringLiteral(
          "production scroll setup did not reach delayed hyprctl probe");
      return false;
    }
    pid_t childProcess = 0;
    if (!waitUntil([&] { return readProcessId(childPidFile, childProcess); },
                   1000)) {
      error = QStringLiteral(
          "delayed fake hyprctl did not publish its child process");
      return false;
    }
    StableProcessIdentity childIdentity;
    if (!childIdentity.open(childProcess)) {
      error = QStringLiteral(
          "could not acquire a stable identity for fake hyprctl child");
      return false;
    }
    QTest::mouseClick(&productionPanel, Qt::LeftButton, Qt::NoModifier,
                      productionPanel.cancelButtonCenterForTest());
    if (!waitUntil([&] { return !productionPanel.workerRunningForTest(); },
                   2500)) {
      error = QStringLiteral("Cancel did not drain delayed production setup");
      return false;
    }
    setupHeartbeat.stop();
    const bool childStillRunning =
        !waitUntil([&] { return childIdentity.exited(); }, 1000);
    if (childStillRunning)
      childIdentity.killForCleanup();
    if (childStillRunning) {
      error = QStringLiteral(
          "Cancel left the delayed hyprctl child process running");
      return false;
    }
    if (productionDismissed.count() != 1 || setupHeartbeatGaps.size() < 50) {
      error = QStringLiteral("delayed production setup did not exercise the "
                             "panel heartbeat (dismissed %1, samples %2)")
                  .arg(productionDismissed.count())
                  .arg(setupHeartbeatGaps.size());
      return false;
    }
    std::sort(setupHeartbeatGaps.begin(), setupHeartbeatGaps.end());
    const std::size_t setupP95Index =
        std::min(setupHeartbeatGaps.size() - 1,
                 static_cast<std::size_t>(setupHeartbeatGaps.size() * 0.95));
    const double setupMaximum = setupHeartbeatGaps.back();
    if (setupHeartbeatGaps[setupP95Index] >= 16.0 || setupMaximum >= 16.0) {
      error = QStringLiteral(
                  "production setup blocked the UI heartbeat (p95 %1 ms, "
                  "max %2 ms)")
                  .arg(setupHeartbeatGaps[setupP95Index], 0, 'f', 3)
                  .arg(setupMaximum, 0, 'f', 3);
      return false;
    }
  }

  // A helper can finish while one of its descendants remains in the owned
  // process group. Normal completion must clean that descendant before the
  // injector publishes its result, just as cancellation does.
  {
    QTemporaryDir commands;
    if (!commands.isValid()) {
      error = QStringLiteral("could not create leader-exit command directory");
      return false;
    }
    const QString hyprctl =
        QDir(commands.path()).filePath(QStringLiteral("hyprctl"));
    const QString childPidFile =
        QDir(commands.path()).filePath(QStringLiteral("child.pid"));
    const QString leaderReleaseFile =
        QDir(commands.path()).filePath(QStringLiteral("release"));
    if (!writeExecutable(
            hyprctl,
            QByteArrayLiteral("#!/usr/bin/env bash\n"
                              "trap '' TERM\n"
                              "sleep 10 </dev/null >/dev/null 2>&1 &\n"
                              "printf '%s\\n' \"$!\" > \"${0%/*}/child.pid\"\n"
                              "while [[ ! -e \"${0%/*}/release\" ]]; do\n"
                              "  sleep 0.01\n"
                              "done\n"
                              "printf '{\"bool\":false}\\n'\n"))) {
      error = QStringLiteral("could not create leader-exit fake hyprctl");
      return false;
    }
    ProductionSetupTestGuard setupGuard;
    qputenv("PATH", commands.path().toUtf8() + ':' + qgetenv("PATH"));
    qputenv("HYPRLAND_INSTANCE_SIGNATURE", "omasnap-scroll-smoke");
    OutputCapture::setOpenTestDoubleForTest(QSize(320, 240), 0);

    ScrollCapturePanel productionPanel(MonitorInfo{}, nullptr, nullptr);
    productionPanel.resize(800, 600);
    productionPanel.show();
    productionPanel.startCaptureJobForTest({QStringLiteral("TEST"),
                                            QRect(0, 0, 64, 64),
                                            {},
                                            ScrollCaptureJobMode::Auto,
                                            stitch::Axis::Vertical,
                                            32,
                                            32,
                                            {true, true}});

    pid_t childProcess = 0;
    if (!waitUntil([&] { return readProcessId(childPidFile, childProcess); },
                   1000)) {
      error = QStringLiteral(
          "leader-exit fake hyprctl did not publish its child process");
      return false;
    }
    StableProcessIdentity childIdentity;
    if (!childIdentity.open(childProcess)) {
      error = QStringLiteral(
          "could not acquire stable identity for leader-exit child");
      return false;
    }
    QFile leaderRelease(leaderReleaseFile);
    if (!leaderRelease.open(QIODevice::WriteOnly)) {
      childIdentity.killForCleanup();
      error = QStringLiteral("could not release leader-exit fake hyprctl");
      return false;
    }
    leaderRelease.close();
    if (!waitUntil(
            [&] {
              return productionPanel.statusForTest().contains(
                  QStringLiteral("Auto-scroll unavailable"));
            },
            1000)) {
      childIdentity.killForCleanup();
      error = QStringLiteral(
          "leader-exit setup did not finish through the production path");
      return false;
    }
    QTest::mouseClick(&productionPanel, Qt::LeftButton, Qt::NoModifier,
                      productionPanel.cancelButtonCenterForTest());
    if (!waitUntil([&] { return !productionPanel.workerRunningForTest(); },
                   1000)) {
      childIdentity.killForCleanup();
      error = QStringLiteral("leader-exit setup did not drain after Cancel");
      return false;
    }
    const bool childStillRunning =
        !waitUntil([&] { return childIdentity.exited(); }, 1000);
    if (childStillRunning)
      childIdentity.killForCleanup();
    if (childStillRunning) {
      error = QStringLiteral(
          "completed hyprctl left a TERM-ignoring child process running");
      return false;
    }
  }
  auto control = std::make_shared<ScrollCaptureJobControl>();
  auto sideEffects = std::make_shared<std::atomic<int>>(0);

  std::vector<double> heartbeatGaps;
  QElapsedTimer heartbeatClock;
  heartbeatClock.start();
  QTimer heartbeat;
  heartbeat.setInterval(1);
  QObject::connect(&heartbeat, &QTimer::timeout, [&] {
    heartbeatGaps.push_back(elapsedMs(heartbeatClock));
    heartbeatClock.restart();
  });
  heartbeat.start();

  QFutureWatcher<ScrollCaptureJobResult> watcher;
  watcher.setFuture(QtConcurrent::run([control, sideEffects, expected] {
    return runDelayedScrollCaptureJobForTest(control, 250, 250, expected,
                                             sideEffects);
  }));
  if (!waitForStage(control, ScrollCaptureJobStage::Capturing, 1000)) {
    error = QStringLiteral("delayed scroll setup did not become ready");
    return false;
  }
  QElapsedTimer controlRequest;
  controlRequest.start();
  control->request(ScrollCaptureJobCommand::Done);
  const double controlRequestMs = elapsedMs(controlRequest);
  if (!waitForFinished(watcher, 1000)) {
    error = QStringLiteral("delayed scroll finalization did not finish");
    return false;
  }
  heartbeat.stop();
  const ScrollCaptureJobResult result = watcher.result();
  if (controlRequestMs >= 2.0 ||
      result.completion != ScrollCaptureJobCompletion::Done ||
      result.image != expected ||
      sideEffects->load(std::memory_order_acquire) != 1) {
    error = QStringLiteral("scroll Done request/result contract failed (%1 ms)")
                .arg(controlRequestMs, 0, 'f', 3);
    return false;
  }
  if (heartbeatGaps.size() < 100) {
    error = QStringLiteral("scroll heartbeat collected too few samples");
    return false;
  }
  std::sort(heartbeatGaps.begin(), heartbeatGaps.end());
  const std::size_t p95Index =
      std::min(heartbeatGaps.size() - 1,
               static_cast<std::size_t>(heartbeatGaps.size() * 0.95));
  const double p95 = heartbeatGaps[p95Index];
  if (p95 >= 16.0) {
    error = QStringLiteral("scroll worker blocked the UI heartbeat (p95 %1 ms)")
                .arg(p95, 0, 'f', 3);
    return false;
  }
  qInfo().noquote() << QStringLiteral(
                           "scroll async smoke: done-handler=%1 ms "
                           "cancel-handler=%2 ms control-request=%3 ms "
                           "heartbeat-p95=%4 ms")
                           .arg(doneHandlerMs, 0, 'f', 3)
                           .arg(cancelHandlerMs, 0, 'f', 3)
                           .arg(controlRequestMs, 0, 'f', 3)
                           .arg(p95, 0, 'f', 3);

  // Cancel remains authoritative until finalization has actually returned.
  // Done may begin assembly, but it must not force a late result through after
  // the panel has been dismissed.
  auto finishCancelControl = std::make_shared<ScrollCaptureJobControl>();
  auto finishCancelEffects = std::make_shared<std::atomic<int>>(0);
  QFutureWatcher<ScrollCaptureJobResult> finishCancelWatcher;
  finishCancelWatcher.setFuture(
      QtConcurrent::run([finishCancelControl, finishCancelEffects, expected] {
        return runDelayedScrollCaptureJobForTest(finishCancelControl, 0, 250,
                                                 expected, finishCancelEffects);
      }));
  if (!waitForStage(finishCancelControl, ScrollCaptureJobStage::Capturing,
                    1000)) {
    error = QStringLiteral("cancel-during-finish setup did not become ready");
    return false;
  }
  finishCancelControl->request(ScrollCaptureJobCommand::Done);
  QTest::qWait(20);
  finishCancelControl->request(ScrollCaptureJobCommand::Cancel);
  if (!waitForFinished(finishCancelWatcher, 1000) ||
      finishCancelWatcher.result().completion !=
          ScrollCaptureJobCompletion::Cancelled ||
      !finishCancelWatcher.result().image.isNull()) {
    error = QStringLiteral("Cancel did not supersede Done during finalization");
    return false;
  }

  // Cancel during setup must wake the worker, discard the result, and produce
  // no ready-side effect that could correspond to a late park/nudge/scroll.
  auto cancelControl = std::make_shared<ScrollCaptureJobControl>();
  auto cancelEffects = std::make_shared<std::atomic<int>>(0);
  QFutureWatcher<ScrollCaptureJobResult> cancelWatcher;
  cancelWatcher.setFuture(
      QtConcurrent::run([cancelControl, cancelEffects, expected] {
        return runDelayedScrollCaptureJobForTest(cancelControl, 250, 0,
                                                 expected, cancelEffects);
      }));
  cancelControl->request(ScrollCaptureJobCommand::Cancel);
  cancelControl.reset(); // the future's shared owner must keep state alive
  if (!waitForFinished(cancelWatcher, 1000) ||
      cancelWatcher.result().completion !=
          ScrollCaptureJobCompletion::Cancelled ||
      cancelEffects->load(std::memory_order_acquire) != 0) {
    error = QStringLiteral("cancel-before-ready had a late scroll side effect");
    return false;
  }
  return true;
}

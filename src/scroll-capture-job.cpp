/** @fileoverview Serialized worker implementation for scroll capture. */
#include "scroll-capture-job.hpp"

#include "scroll-inject.hpp"

#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <thread>
#include <utility>

namespace {
constexpr int kGrabTimeoutMs = 400;
constexpr int kCaptureIntervalMs = 100;
constexpr int kMotionWaitMs = 700;
constexpr int kSettleGrabMs = 120;
constexpr int kMaxSettleMs = 2000;
constexpr double kTargetStepFraction = 0.45;
constexpr int kMaxNotchesPerTick = 12;

bool terminalCommand(const ScrollCaptureJobCommand command) {
  return command == ScrollCaptureJobCommand::Done ||
         command == ScrollCaptureJobCommand::Cancel ||
         command == ScrollCaptureJobCommand::Back;
}

const char *eventName(const stitch::ManualCapture::Event event) {
  using Event = stitch::ManualCapture::Event;
  switch (event) {
  case Event::Blank:
    return "Blank";
  case Event::Seeded:
    return "Seeded";
  case Event::Kept:
    return "Kept";
  case Event::Pending:
    return "Pending";
  case Event::Still:
    return "Still";
  case Event::PendingDropped:
    return "PendingDropped";
  case Event::ReSeeded:
    return "ReSeeded";
  case Event::Ambiguous:
    return "Ambiguous";
  case Event::Unmatchable:
    return "Unmatchable";
  case Event::WrongDirection:
    return "WrongDirection";
  case Event::Error:
    return "Error";
  case Event::Full:
    return "Full";
  }
  return "?";
}
} // namespace

struct ScrollCaptureJobControl::State {
  std::atomic<ScrollCaptureJobCommand> command{ScrollCaptureJobCommand::None};
  std::atomic<bool> cancelRequested{false};
  mutable std::mutex mutex;
  std::condition_variable changed;
  ScrollCaptureJobSnapshot snapshot;
  std::weak_ptr<std::atomic<bool>> activeEpisodeStop;
  bool completionCommitted = false;
};

ScrollCaptureJobControl::ScrollCaptureJobControl()
    : state_(std::make_shared<State>()) {}

ScrollCaptureJobControl::~ScrollCaptureJobControl() {
  request(ScrollCaptureJobCommand::Cancel);
}

void ScrollCaptureJobControl::request(const ScrollCaptureJobCommand requested) {
  {
    const std::lock_guard lock(state_->mutex);
    const ScrollCaptureJobCommand current =
        state_->command.load(std::memory_order_acquire);
    if (state_->completionCommitted ||
        current == ScrollCaptureJobCommand::Cancel)
      return;
    // Cancel is the one terminal command that may supersede another terminal
    // command. Holding the same mutex as episode registration makes the
    // terminal command and its stop flag one linearizable transition.
    if (requested != ScrollCaptureJobCommand::Cancel) {
      if (terminalCommand(current))
        return;
      if (requested == ScrollCaptureJobCommand::Continue &&
          current != ScrollCaptureJobCommand::None)
        return;
    } else {
      state_->cancelRequested.store(true, std::memory_order_release);
    }
    state_->command.store(requested, std::memory_order_release);
    if (terminalCommand(requested)) {
      if (const auto episodeStop = state_->activeEpisodeStop.lock())
        episodeStop->store(true, std::memory_order_release);
    }
  }
  state_->changed.notify_all();
}

ScrollCaptureJobCommand ScrollCaptureJobControl::command() const {
  return state_->command.load(std::memory_order_acquire);
}

ScrollCaptureJobSnapshot ScrollCaptureJobControl::snapshot() const {
  const std::lock_guard lock(state_->mutex);
  return state_->snapshot;
}

namespace {
void publish(const std::shared_ptr<ScrollCaptureJobControl::State> &state,
             const ScrollCaptureJobStage stage, const QString &status,
             const bool warning = false, const bool stalled = false) {
  const std::lock_guard lock(state->mutex);
  state->snapshot.stage = stage;
  state->snapshot.status = status;
  state->snapshot.warning = warning;
  state->snapshot.autoStalled = stalled;
  ++state->snapshot.revision;
  state->changed.notify_all();
}

bool waitUnlessCommand(
    const std::shared_ptr<ScrollCaptureJobControl::State> &state,
    const int ms) {
  std::unique_lock lock(state->mutex);
  return !state->changed.wait_for(lock, std::chrono::milliseconds(ms), [&] {
    return state->command.load(std::memory_order_acquire) !=
           ScrollCaptureJobCommand::None;
  });
}

bool waitForCancellation(
    const std::shared_ptr<ScrollCaptureJobControl::State> &state,
    const int ms) {
  std::unique_lock lock(state->mutex);
  return state->changed.wait_for(lock, std::chrono::milliseconds(ms), [&] {
    return state->command.load(std::memory_order_acquire) ==
           ScrollCaptureJobCommand::Cancel;
  });
}

bool clearCommand(const std::shared_ptr<ScrollCaptureJobControl::State> &state,
                  const ScrollCaptureJobCommand expected) {
  const std::lock_guard lock(state->mutex);
  if (state->command.load(std::memory_order_acquire) != expected)
    return false;
  state->command.store(ScrollCaptureJobCommand::None,
                       std::memory_order_release);
  return true;
}

bool registerEpisode(
    const std::shared_ptr<ScrollCaptureJobControl::State> &state,
    const std::shared_ptr<std::atomic<bool>> &episodeStop) {
  const std::lock_guard lock(state->mutex);
  if (state->completionCommitted ||
      state->command.load(std::memory_order_acquire) !=
          ScrollCaptureJobCommand::None) {
    episodeStop->store(true, std::memory_order_release);
    return false;
  }
  state->activeEpisodeStop = episodeStop;
  return true;
}

bool commitDone(const std::shared_ptr<ScrollCaptureJobControl::State> &state) {
  const std::lock_guard lock(state->mutex);
  if (state->command.load(std::memory_order_acquire) !=
          ScrollCaptureJobCommand::Done ||
      state->cancelRequested.load(std::memory_order_acquire))
    return false;
  // This is the completion point of no return. A later UI dismissal still
  // suppresses delivery to a dead/hidden panel, but it cannot turn already
  // committed debug publication and a Done result into Cancelled.
  state->completionCommitted = true;
  return true;
}

struct Worker {
  explicit Worker(const stitch::Axis axis) : session(axis), autoSession(axis) {}
  OutputCapture output;
  stitch::ManualCapture session;
  stitch::AutoCapture autoSession;
  QRect regionPhysical;
  int grabbed = 0;
  int consecutiveFailures = 0;
  std::uint64_t lastCycle = 0;
  QImage firstCrop;
  QImage lastCrop;
  QString debugDir;
  int notchesPerTick = 1;
  bool notchesFitted = false;

  [[nodiscard]] long long retainedDebugFrameBytes() const {
    long long bytes = firstCrop.isNull()
                          ? 0
                          : static_cast<long long>(firstCrop.sizeInBytes());
    if (!lastCrop.isNull() &&
        (firstCrop.isNull() || lastCrop.cacheKey() != firstCrop.cacheKey()))
      bytes += static_cast<long long>(lastCrop.sizeInBytes());
    return bytes;
  }

  [[nodiscard]] QImage crop(const QImage &frame) const {
    return frame.copy(regionPhysical).convertToFormat(QImage::Format_RGBA8888);
  }

  [[nodiscard]] bool
  acquireSettledFrame(const QImage &before, QImage &settled, QString &error,
                      const std::shared_ptr<ScrollCaptureJobControl> &control) {
    QElapsedTimer clock;
    clock.start();
    bool moved = before.isNull();
    QImage last;
    int failures = 0;
    while (control->command() == ScrollCaptureJobCommand::None) {
      QImage frame;
      const bool ok =
          output.grab(frame, error, moved ? kSettleGrabMs : kSettleGrabMs / 2);
      if (!ok) {
        if (output.sessionStopped())
          return false;
        if (moved && !last.isNull()) {
          settled = last;
          return true;
        }
        if (!moved && clock.elapsed() >= kMotionWaitMs) {
          settled = before;
          return true;
        }
        if (++failures >= 40)
          return false;
        continue;
      }
      failures = 0;
      const QImage current = crop(frame);
      if (!moved) {
        if (current != before) {
          moved = true;
        } else if (clock.elapsed() >= kMotionWaitMs) {
          settled = current;
          return true;
        } else {
          continue;
        }
      }
      if (!last.isNull() && current == last) {
        settled = current;
        return true;
      }
      last = current;
      if (clock.elapsed() >= kMaxSettleMs) {
        settled = current;
        return true;
      }
    }
    return false;
  }
};

struct DebugImageBatch {
  struct PendingFile {
    QString path;
    std::unique_ptr<QSaveFile> file;
  };
  std::vector<PendingFile> pending;
};

bool stageDebugImages(const Worker &worker, const QImage &assembled,
                      const std::atomic<bool> &cancelRequested,
                      DebugImageBatch &batch,
                      std::atomic<int> *encodeCountForTest = nullptr) {
  if (worker.debugDir.isEmpty())
    return true;
  const auto stage = [&](const QImage &image, const QString &name) {
    if (cancelRequested.load(std::memory_order_acquire))
      return false;
    if (image.isNull())
      return true;
    auto file = std::make_unique<QSaveFile>(worker.debugDir + name);
    if (encodeCountForTest)
      encodeCountForTest->fetch_add(1, std::memory_order_relaxed);
    if (!file->open(QIODevice::WriteOnly) || !image.save(file.get(), "PNG")) {
      qWarning().noquote() << QStringLiteral(
                                  "scroll: could not write debug image %1")
                                  .arg(file->fileName());
      file->cancelWriting();
      return true; // Debug output is optional; capture result remains valid.
    }
    if (cancelRequested.load(std::memory_order_acquire)) {
      file->cancelWriting();
      return false;
    }
    batch.pending.push_back({file->fileName(), std::move(file)});
    return true;
  };
  if (!stage(assembled, QStringLiteral("/scroll-stitched.png")) ||
      !stage(worker.firstCrop, QStringLiteral("/scroll-first-frame.png")) ||
      !stage(worker.lastCrop, QStringLiteral("/scroll-last-frame.png")) ||
      cancelRequested.load(std::memory_order_acquire))
    return false;
  return true;
}

void commitDebugImages(DebugImageBatch &batch) noexcept {
  // Completion has already crossed commitDone(). Publishing these independent
  // diagnostic paths can therefore no longer race a reported cancellation.
  for (DebugImageBatch::PendingFile &entry : batch.pending) {
    try {
      if (!entry.file->commit())
        qWarning().noquote()
            << QStringLiteral("scroll: could not publish debug image %1")
                   .arg(entry.path);
    } catch (...) {
      // Debug output is optional. More importantly, no exception may escape
      // after commitDone() has made the worker result irrevocably Done.
    }
  }
}

void runManualLoop(
    Worker &worker, const std::shared_ptr<ScrollCaptureJobControl> &control,
    const std::shared_ptr<ScrollCaptureJobControl::State> &state) {
  using Event = stitch::ManualCapture::Event;
  QString error;
  while (control->command() == ScrollCaptureJobCommand::None) {
    QImage frame;
    if (!worker.output.grab(frame, error, kGrabTimeoutMs)) {
      if (worker.output.sessionStopped()) {
        publish(state, ScrollCaptureJobStage::Capturing,
                QStringLiteral("Screen capture stopped · press Done to stitch "
                               "what was captured, or Cancel"),
                true);
        return;
      }
      waitUnlessCommand(state, kCaptureIntervalMs);
      if (++worker.consecutiveFailures == 50)
        publish(state, ScrollCaptureJobStage::Capturing,
                QStringLiteral("Screen capture is not delivering frames: %1")
                    .arg(error),
                true);
      continue;
    }
    worker.consecutiveFailures = 0;
    if (control->command() != ScrollCaptureJobCommand::None)
      return;
    ++worker.grabbed;
    const QImage cropped = worker.crop(frame);
    if (!worker.debugDir.isEmpty() && worker.grabbed % 8 == 0)
      cropped.save(worker.debugDir +
                       QStringLiteral("/grab-%1-crop.png")
                           .arg(worker.grabbed, 3, 10, QChar('0')),
                   "PNG");
    const bool wasStarted = worker.session.started();
    const stitch::ManualCapture::Outcome out = worker.session.feed(cropped);
    if (!wasStarted && worker.session.started())
      worker.firstCrop = cropped;
    worker.lastCrop = cropped;
    if (!worker.debugDir.isEmpty())
      qInfo().noquote()
          << QStringLiteral("scroll grab %1: %2 motion=%3(%4) err=%5 conf=%6 "
                            "kept=%7 pending=%8")
                 .arg(worker.grabbed)
                 .arg(QString::fromLatin1(eventName(out.event)))
                 .arg(static_cast<int>(out.estimate.motion.kind))
                 .arg(out.estimate.motion.delta)
                 .arg(out.estimate.error, 0, 'f', 2)
                 .arg(out.estimate.confidence, 0, 'f', 2)
                 .arg(out.keptFrames)
                 .arg(out.pendingDelta);
    const QString frames =
        QStringLiteral("%1 frame%2")
            .arg(out.keptFrames)
            .arg(out.keptFrames == 1 ? QString() : QStringLiteral("s"));
    switch (out.event) {
    case Event::Blank:
      if (!out.error.isEmpty())
        publish(state, ScrollCaptureJobStage::Capturing,
                QStringLiteral("Capture shows only a solid color · Cancel and "
                               "select a different region"),
                true);
      break;
    case Event::Seeded:
      publish(
          state, ScrollCaptureJobStage::Capturing,
          QStringLiteral("Capturing · 1 frame · scroll the page · Done when "
                         "finished"));
      break;
    case Event::Kept:
    case Event::Pending:
    case Event::PendingDropped:
      publish(
          state, ScrollCaptureJobStage::Capturing,
          QStringLiteral("Capturing · %1%2 · Done when finished")
              .arg(frames)
              .arg(worker.session.exceedsWidelyOpenableEdge()
                       ? QStringLiteral(" · very long: fine here, some apps "
                                        "may not open it")
                       : QString()));
      break;
    case Event::Still:
      break;
    case Event::ReSeeded:
      publish(state, ScrollCaptureJobStage::Capturing,
              QStringLiteral("Capturing · restarted from here (content changed "
                             "in place) · scroll the page"));
      break;
    case Event::Ambiguous:
      publish(
          state, ScrollCaptureJobStage::Capturing,
          QStringLiteral("Repeated content · keep scrolling to a distinctive "
                         "part"),
          true);
      break;
    case Event::Unmatchable:
      publish(
          state, ScrollCaptureJobStage::Capturing,
          QStringLiteral("Can't align · scroll back a little; moving content "
                         "(video) can't be captured"),
          true);
      break;
    case Event::WrongDirection:
      publish(
          state, ScrollCaptureJobStage::Capturing,
          QStringLiteral("Scroll the other way to continue this capture (or "
                         "Done to stitch what you have)"),
          true);
      break;
    case Event::Error:
      publish(state, ScrollCaptureJobStage::Capturing,
              QStringLiteral("Could not add frame: %1").arg(out.error), true);
      break;
    case Event::Full:
      publish(state, ScrollCaptureJobStage::Capturing,
              QStringLiteral("Capture is as long as it can get · press Done to "
                             "stitch it, or Cancel"),
              true);
      return;
    }
    waitUnlessCommand(state, kCaptureIntervalMs);
  }
}

enum class AutoLoopResult { Requested, Paused, Stopped, FallbackManual };

AutoLoopResult
runAutoLoop(Worker &worker, const ScrollCaptureJobSpec &spec,
            const std::shared_ptr<ScrollCaptureJobControl> &control,
            const std::shared_ptr<ScrollCaptureJobControl::State> &state,
            const std::shared_ptr<ScrollInjectorSession> &injector,
            const std::shared_ptr<stitch::CaptureHandshake> &handshake,
            const std::shared_ptr<std::atomic<bool>> &episodeStop) {
  using Ack = stitch::AutoCapture::Ack;
  using Event = stitch::AutoCapture::Event;
  QString error;
  while (control->command() == ScrollCaptureJobCommand::None) {
    const ScrollInjectorSession::Snapshot injectorStatus = injector->snapshot();
    if (injectorStatus.status == ScrollInjectorSession::Status::Failed) {
      episodeStop->store(true, std::memory_order_release);
      if (!worker.autoSession.started()) {
        publish(
            state, ScrollCaptureJobStage::Capturing,
            QStringLiteral("Auto-scroll unavailable · scroll manually · Done "
                           "stitches (%1)")
                .arg(injectorStatus.error),
            true);
        return AutoLoopResult::FallbackManual;
      }
      publish(state, ScrollCaptureJobStage::Stalled,
              QStringLiteral("Auto-scroll stopped: %1 · Done stitches what was "
                             "captured")
                  .arg(injectorStatus.error),
              true, false);
      return AutoLoopResult::Stopped;
    }
    const std::uint64_t cycle = handshake->readyCycle();
    if (cycle == 0 || cycle == worker.lastCycle) {
      waitUnlessCommand(state, 20);
      continue;
    }
    QImage cropped;
    if (!worker.acquireSettledFrame(worker.lastCrop, cropped, error, control)) {
      if (control->command() != ScrollCaptureJobCommand::None)
        return AutoLoopResult::Requested;
      if (worker.output.sessionStopped()) {
        publish(
            state, ScrollCaptureJobStage::Stalled,
            QStringLiteral("Screen capture stopped · Done stitches what was "
                           "captured"),
            true, false);
        episodeStop->store(true, std::memory_order_release);
        return AutoLoopResult::Stopped;
      }
      if (++worker.consecutiveFailures >= 3)
        publish(state, ScrollCaptureJobStage::Capturing,
                QStringLiteral("Screen capture is failing: %1").arg(error),
                true);
      waitUnlessCommand(state, 20);
      continue;
    }
    worker.consecutiveFailures = 0;
    ++worker.grabbed;
    if (!worker.debugDir.isEmpty())
      cropped.save(worker.debugDir +
                       QStringLiteral("/grab-%1-crop.png")
                           .arg(worker.grabbed, 3, 10, QChar('0')),
                   "PNG");
    const stitch::AutoCapture::Outcome out = worker.autoSession.feed(cropped);
    if (control->command() != ScrollCaptureJobCommand::None) {
      episodeStop->store(true, std::memory_order_release);
      injector->stopEpisode();
      return AutoLoopResult::Requested;
    }
    if (!worker.firstCrop.isNull() || out.event == Event::Seeded)
      worker.lastCrop = cropped;
    if (out.event == Event::Seeded)
      worker.firstCrop = cropped;
    if (out.event == Event::Blank) {
      waitUnlessCommand(state, 20);
      continue;
    }
    worker.lastCycle = cycle;
    double perNotch = 0.0;
    if (out.event == Event::Appended && out.estimate.motion.delta > 0)
      perNotch = static_cast<double>(out.estimate.motion.delta) /
                 worker.notchesPerTick;
    else if (out.event == Event::Committed && out.secondDelta > 0)
      perNotch = out.secondDelta / static_cast<double>(stitch::kProbeNotches);
    else if (out.event == Event::Committed && out.firstDelta > 0)
      perNotch = static_cast<double>(out.firstDelta) / worker.notchesPerTick;
    if (!worker.notchesFitted && perNotch > 0.0) {
      const int extent = spec.axis == stitch::Axis::Vertical
                             ? worker.regionPhysical.height()
                             : worker.regionPhysical.width();
      worker.notchesPerTick =
          std::clamp(static_cast<int>(std::lround(extent * kTargetStepFraction /
                                                  std::max(perNotch, 1.0))),
                     1, kMaxNotchesPerTick);
      worker.notchesFitted = true;
    }
    switch (out.ack) {
    case Ack::Normal:
      handshake->acknowledgeWithNotches(cycle, worker.notchesPerTick);
      break;
    case Ack::Probe:
      handshake->acknowledgeWithNotches(cycle, 1);
      break;
    case Ack::Hold:
      break;
    }
    switch (out.event) {
    case Event::Seeded:
    case Event::Appended:
    case Event::Committed:
      publish(
          state, ScrollCaptureJobStage::Capturing,
          QStringLiteral("Auto-scrolling · %1 frame%2%3 · Done stitches")
              .arg(worker.autoSession.keptFrames())
              .arg(worker.autoSession.keptFrames() == 1 ? QString()
                                                        : QStringLiteral("s"))
              .arg(worker.autoSession.exceedsWidelyOpenableEdge()
                       ? QStringLiteral(" · very long: fine here, some apps "
                                        "may not open it")
                       : QString()));
      break;
    case Event::ProbeStarted:
    case Event::ProbeAgain:
      publish(state, ScrollCaptureJobStage::Capturing,
              QStringLiteral("Verifying scroll alignment…"));
      break;
    case Event::StillOnce:
      publish(state, ScrollCaptureJobStage::Capturing,
              QStringLiteral("Confirming end of content…"));
      break;
    case Event::ReachedEnd:
    case Event::ReachedEndAtSeam:
      episodeStop->store(true, std::memory_order_release);
      publish(state, ScrollCaptureJobStage::Stalled,
              QStringLiteral("End reached · %1 frames · Continue carries on, "
                             "Done stitches it")
                  .arg(worker.autoSession.keptFrames()),
              false, true);
      return AutoLoopResult::Paused;
    case Event::Paused:
      episodeStop->store(true, std::memory_order_release);
      worker.autoSession.abandonPause();
      publish(state, ScrollCaptureJobStage::Stalled,
              QStringLiteral("Auto-scroll paused: capture lost alignment · "
                             "Continue tries again, Done stitches what was "
                             "verified"),
              true, true);
      return AutoLoopResult::Paused;
    case Event::Halted: {
      episodeStop->store(true, std::memory_order_release);
      QString reason;
      switch (out.haltReason) {
      case stitch::AutoCapture::HaltReason::LostAlignment:
        reason = QStringLiteral("Stopped: capture lost alignment");
        break;
      case stitch::AutoCapture::HaltReason::MovedBackward:
        reason = QStringLiteral("Stopped: content moved backward");
        break;
      case stitch::AutoCapture::HaltReason::Unmatchable:
        reason = QStringLiteral("Stopped: retry with slower scrolling");
        break;
      case stitch::AutoCapture::HaltReason::BlankFrames:
        reason = QStringLiteral("Capture shows only a solid color: retry");
        break;
      case stitch::AutoCapture::HaltReason::ReachedLimit:
        reason = QStringLiteral("Capture is as long as it can get");
        break;
      default:
        reason = QStringLiteral("Capture failed: %1").arg(out.error);
        break;
      }
      publish(state, ScrollCaptureJobStage::Stalled,
              reason + QStringLiteral(" · Done stitches what was captured"),
              true, false);
      return AutoLoopResult::Stopped;
    }
    case Event::Blank:
      break;
    }
    waitUnlessCommand(state, 20);
  }
  episodeStop->store(true, std::memory_order_release);
  return AutoLoopResult::Requested;
}

ScrollCaptureJobResult terminalResult(const ScrollCaptureJobCommand command) {
  ScrollCaptureJobResult result;
  result.completion = command == ScrollCaptureJobCommand::Back
                          ? ScrollCaptureJobCompletion::Back
                          : ScrollCaptureJobCompletion::Cancelled;
  return result;
}

std::optional<ScrollCaptureJobResult> rejectedEpisodeTerminalResult(
    const std::shared_ptr<ScrollCaptureJobControl> &control,
    const bool captureStarted) {
  const ScrollCaptureJobCommand command = control->command();
  if (command == ScrollCaptureJobCommand::Cancel ||
      command == ScrollCaptureJobCommand::Back)
    return terminalResult(command);
  if (command == ScrollCaptureJobCommand::Done && !captureStarted)
    return ScrollCaptureJobResult{
        ScrollCaptureJobCompletion::NoFrames, {}, {}, 0, 0};
  return std::nullopt;
}

template <typename Body>
ScrollCaptureJobResult guardedScrollCaptureJob(
    const std::shared_ptr<ScrollCaptureJobControl::State> &state, Body body) {
  try {
    return body();
  } catch (const std::bad_alloc &) {
    const QString error = QStringLiteral("Scroll capture ran out of memory");
    publish(state, ScrollCaptureJobStage::Completed, error, true);
    return {ScrollCaptureJobCompletion::Failed, {}, error};
  } catch (const std::exception &exception) {
    const QString error = QStringLiteral("Scroll capture failed: %1")
                              .arg(QString::fromUtf8(exception.what()));
    publish(state, ScrollCaptureJobStage::Completed, error, true);
    return {ScrollCaptureJobCompletion::Failed, {}, error};
  } catch (...) {
    const QString error = QStringLiteral("Scroll capture failed unexpectedly");
    publish(state, ScrollCaptureJobStage::Completed, error, true);
    return {ScrollCaptureJobCompletion::Failed, {}, error};
  }
}

template <typename Body>
std::jthread
makeInjectorOwnerThread(const std::shared_ptr<ScrollInjectorSession> &injector,
                        Body body) {
  return std::jthread(
      [injector, body = std::move(body)](const std::stop_token stopToken) {
        std::stop_callback stopCallback(stopToken,
                                        [injector] { injector->stop(); });
        try {
          body();
        } catch (const std::bad_alloc &) {
          injector->reportFailure(
              QStringLiteral("Scroll injection ran out of memory"));
        } catch (const std::exception &) {
          injector->reportFailure(QStringLiteral("Scroll injection failed"));
        } catch (...) {
          injector->reportFailure(
              QStringLiteral("Scroll injection failed unexpectedly"));
        }
      });
}
} // namespace

ScrollCaptureJobResult
runScrollCaptureJob(const ScrollCaptureJobSpec &spec,
                    const std::shared_ptr<ScrollCaptureJobControl> &control) {
  const std::shared_ptr<ScrollCaptureJobControl::State> state = control->state_;
  return guardedScrollCaptureJob(state, [&]() -> ScrollCaptureJobResult {
    publish(state, ScrollCaptureJobStage::Preparing,
            QStringLiteral("Preparing scroll capture…"));

    Worker worker(spec.axis);
    worker.debugDir = spec.debugDir;
    QString error;
    if (!worker.output.open(spec.outputName, error)) {
      if (terminalCommand(control->command()))
        return terminalResult(control->command());
      publish(state, ScrollCaptureJobStage::Completed,
              QStringLiteral("Output capture failed: %1").arg(error), true);
      return {ScrollCaptureJobCompletion::SetupFailed, {}, error};
    }
    if (terminalCommand(control->command()))
      return terminalResult(control->command());
    worker.regionPhysical = spec.regionPhysical.intersected(
        QRect(QPoint(), worker.output.bufferSize()));
    if (worker.regionPhysical.isEmpty()) {
      error =
          QStringLiteral("The selected scroll region is outside the output");
      publish(state, ScrollCaptureJobStage::Completed, error, true);
      return {ScrollCaptureJobCompletion::SetupFailed, {}, error};
    }
    long long externalReserve = worker.output.mappedBytes();
    if (!worker.debugDir.isEmpty()) {
      const long long width = worker.regionPhysical.width();
      const long long height = worker.regionPhysical.height();
      if (width <= 0 || height <= 0 ||
          width > (std::numeric_limits<long long>::max() - externalReserve) /
                      height / 8) {
        error = QStringLiteral("Scroll capture memory reservation overflowed");
        publish(state, ScrollCaptureJobStage::Completed, error, true);
        return {ScrollCaptureJobCompletion::SetupFailed, {}, error};
      }
      externalReserve += width * height * 8; // first and last debug crops
    }
    worker.session.setExternalBytes(externalReserve);
    worker.autoSession.setExternalBytes(externalReserve);

    ScrollCaptureJobMode effectiveMode = spec.mode;
    QString autoFallbackError;
    std::shared_ptr<ScrollInjectorSession> injector;
    std::jthread injectorThread;
    if (effectiveMode == ScrollCaptureJobMode::Auto) {
      injector = std::make_shared<ScrollInjectorSession>();
      injectorThread = makeInjectorOwnerThread(
          injector, [injector, outputName = spec.outputName,
                     validation = spec.injectorValidation] {
            runScrollInjectorSession(injector, outputName, validation);
          });
    }
    const auto stopInjector = [&] {
      if (injectorThread.joinable())
        injectorThread.request_stop();
      if (injector)
        injector->stop();
      if (injectorThread.joinable())
        injectorThread.join();
    };

    if (effectiveMode == ScrollCaptureJobMode::Auto) {
      publish(state, ScrollCaptureJobStage::Preparing,
              QStringLiteral("Preparing auto-scroll…"));
      while (control->command() == ScrollCaptureJobCommand::None) {
        const ScrollInjectorSession::Snapshot injectorSnapshot =
            injector->snapshot();
        if (injectorSnapshot.status == ScrollInjectorSession::Status::Ready)
          break;
        if (injectorSnapshot.status == ScrollInjectorSession::Status::Failed) {
          stopInjector();
          effectiveMode = ScrollCaptureJobMode::Manual;
          autoFallbackError = injectorSnapshot.error;
          break;
        }
        waitUnlessCommand(state, 10);
      }
      if (terminalCommand(control->command())) {
        stopInjector();
        return terminalResult(control->command());
      }
    }
    if (effectiveMode == ScrollCaptureJobMode::Manual &&
        !autoFallbackError.isEmpty())
      publish(state, ScrollCaptureJobStage::Capturing,
              QStringLiteral("Auto-scroll unavailable: %1 · scroll manually")
                  .arg(autoFallbackError),
              true);
    else if (effectiveMode == ScrollCaptureJobMode::Manual)
      publish(state, ScrollCaptureJobStage::Capturing,
              QStringLiteral("Scroll the page · Done stitches it"));

    while (true) {
      if (effectiveMode == ScrollCaptureJobMode::Manual) {
        runManualLoop(worker, control, state);
      } else {
        worker.autoSession.resumeFromEnd();
        worker.lastCycle = 0;
        auto handshake = std::make_shared<stitch::CaptureHandshake>();
        auto episodeStop = std::make_shared<std::atomic<bool>>(false);
        if (registerEpisode(state, episodeStop)) {
          injector->startEpisode(handshake, episodeStop, spec.parkX, spec.parkY,
                                 spec.axis);
          publish(
              state, ScrollCaptureJobStage::Capturing,
              QStringLiteral("Auto-scrolling… · keep the pointer still · Done "
                             "stitches it"));
          const AutoLoopResult loop = runAutoLoop(
              worker, spec, control, state, injector, handshake, episodeStop);
          if (loop == AutoLoopResult::FallbackManual) {
            stopInjector();
            effectiveMode = ScrollCaptureJobMode::Manual;
            continue;
          }
          if (loop == AutoLoopResult::Paused) {
            while (control->command() == ScrollCaptureJobCommand::None)
              waitUnlessCommand(state, 20);
            if (clearCommand(state, ScrollCaptureJobCommand::Continue)) {
              publish(state, ScrollCaptureJobStage::Capturing,
                      QStringLiteral("Preparing auto-scroll…"));
              continue;
            }
          } else if (loop == AutoLoopResult::Stopped) {
            while (true) {
              while (control->command() == ScrollCaptureJobCommand::None)
                waitUnlessCommand(state, 20);
              if (!clearCommand(state, ScrollCaptureJobCommand::Continue))
                break;
            }
          }
        } else if (const auto terminal = rejectedEpisodeTerminalResult(
                       control, worker.autoSession.started())) {
          stopInjector();
          publish(state, ScrollCaptureJobStage::Completed, QString());
          return *terminal;
        }
      }

      const ScrollCaptureJobCommand command = control->command();
      if (command == ScrollCaptureJobCommand::Continue) {
        clearCommand(state, command);
        continue;
      }
      if (command == ScrollCaptureJobCommand::Cancel ||
          command == ScrollCaptureJobCommand::Back) {
        stopInjector();
        publish(state, ScrollCaptureJobStage::Completed, QString());
        return terminalResult(command);
      }
      if (command != ScrollCaptureJobCommand::Done) {
        while (control->command() == ScrollCaptureJobCommand::None)
          waitUnlessCommand(state, 20);
        continue;
      }

      if (injector)
        injector->stopEpisode();
      const bool started = effectiveMode == ScrollCaptureJobMode::Auto
                               ? worker.autoSession.started()
                               : worker.session.started();
      if (!started) {
        stopInjector();
        publish(state, ScrollCaptureJobStage::Completed, QString());
        return {ScrollCaptureJobCompletion::NoFrames, {}, {}, 0, 0};
      }
      publish(state, ScrollCaptureJobStage::Capturing,
              QStringLiteral("Stitching…"));
      if (worker.debugDir.isEmpty()) {
        worker.firstCrop = {};
        worker.lastCrop = {};
      }
      // No more grabs occur after Done. Drop the mutable output SHM mapping
      // before reserving the stitched image so a compositor mode/scale change
      // cannot turn an already-admitted capture into a finish-time rejection.
      // OutputCapture is still closed on its owning worker thread.
      worker.output.close();
      const long long externalBytes = worker.retainedDebugFrameBytes();
      QImage assembled =
          effectiveMode == ScrollCaptureJobMode::Auto
              ? worker.autoSession.finish(error, &state->cancelRequested,
                                          externalBytes)
              : worker.session.finish(error, &state->cancelRequested,
                                      externalBytes);
      if (assembled.isNull()) {
        if (control->command() == ScrollCaptureJobCommand::Cancel)
          return terminalResult(ScrollCaptureJobCommand::Cancel);
        stopInjector();
        const QString failure = QStringLiteral("Stitch failed: %1").arg(error);
        publish(state, ScrollCaptureJobStage::Completed, failure, true);
        return {ScrollCaptureJobCompletion::Failed, {}, failure};
      }
      DebugImageBatch debugImages;
      if (!stageDebugImages(worker, assembled, state->cancelRequested,
                            debugImages))
        return terminalResult(ScrollCaptureJobCommand::Cancel);
      const int kept = effectiveMode == ScrollCaptureJobMode::Auto
                           ? worker.autoSession.keptFrames()
                           : worker.session.keptFrames();
      const int unverified = effectiveMode == ScrollCaptureJobMode::Auto
                                 ? worker.autoSession.unverifiedSeams()
                                 : 0;
      stopInjector();
      if (!commitDone(state))
        return terminalResult(ScrollCaptureJobCommand::Cancel);
      commitDebugImages(debugImages);
      return {ScrollCaptureJobCompletion::Done,
              std::move(assembled),
              {},
              kept,
              unverified};
    }
  });
}

ScrollCaptureJobResult runDelayedScrollCaptureJobForTest(
    const std::shared_ptr<ScrollCaptureJobControl> &control,
    const int setupDelayMs, const int finishDelayMs, const QImage &result,
    const std::shared_ptr<std::atomic<int>> &sideEffects,
    const int cancelDrainDelayMs) {
  const auto state = control->state_;
  const auto terminalAfterDrain = [&](const ScrollCaptureJobCommand command) {
    if (command == ScrollCaptureJobCommand::Cancel && cancelDrainDelayMs > 0)
      QThread::msleep(static_cast<unsigned long>(cancelDrainDelayMs));
    return terminalResult(command);
  };
  publish(state, ScrollCaptureJobStage::Preparing,
          QStringLiteral("Preparing scroll capture…"));
  if (!waitUnlessCommand(state, setupDelayMs))
    return terminalAfterDrain(control->command());
  if (control->command() != ScrollCaptureJobCommand::None)
    return terminalAfterDrain(control->command());
  sideEffects->fetch_add(1, std::memory_order_acq_rel);
  publish(state, ScrollCaptureJobStage::Capturing,
          QStringLiteral("Scroll the page · Done stitches it"));
  while (control->command() == ScrollCaptureJobCommand::None)
    waitUnlessCommand(state, 5);
  if (control->command() != ScrollCaptureJobCommand::Done)
    return terminalAfterDrain(control->command());
  publish(state, ScrollCaptureJobStage::Capturing,
          QStringLiteral("Stitching…"));
  if (waitForCancellation(state, finishDelayMs))
    return terminalAfterDrain(ScrollCaptureJobCommand::Cancel);
  if (result.isNull()) {
    const QString error = QStringLiteral("Stitch failed: simulated failure");
    publish(state, ScrollCaptureJobStage::Completed, error, true);
    return {ScrollCaptureJobCompletion::Failed, {}, error};
  }
  if (!commitDone(state))
    return terminalResult(ScrollCaptureJobCommand::Cancel);
  return {ScrollCaptureJobCompletion::Done, result, {}, 1, 0};
}

ScrollCaptureJobResult runFailingScrollCaptureJobForTest(
    const std::shared_ptr<ScrollCaptureJobControl> &control) {
  const auto state = control->state_;
  return guardedScrollCaptureJob(
      state, []() -> ScrollCaptureJobResult { throw std::bad_alloc(); });
}

bool autoInjectorOwnerUnwindsForTest() {
  std::atomic<bool> started{false};
  std::atomic<bool> exited{false};
  try {
    auto injector = std::make_shared<ScrollInjectorSession>();
    std::jthread owner = makeInjectorOwnerThread(injector, [&] {
      started.store(true, std::memory_order_release);
      while (!injector->stopped())
        std::this_thread::yield();
      exited.store(true, std::memory_order_release);
    });
    while (!started.load(std::memory_order_acquire))
      std::this_thread::yield();
    throw std::bad_alloc();
  } catch (const std::bad_alloc &) {
  }
  return exited.load(std::memory_order_acquire);
}

bool injectorOwnerExceptionBecomesFailedForTest() {
  auto injector = std::make_shared<ScrollInjectorSession>();
  std::jthread owner =
      makeInjectorOwnerThread(injector, [] { throw std::bad_alloc(); });
  owner.join();
  return injector->snapshot().status == ScrollInjectorSession::Status::Failed;
}

bool terminalCommandStopsActiveEpisodeForTest(
    const ScrollCaptureJobCommand command) {
  auto control = std::make_shared<ScrollCaptureJobControl>();
  auto episodeStop = std::make_shared<std::atomic<bool>>(false);
  {
    const std::lock_guard lock(control->state_->mutex);
    control->state_->activeEpisodeStop = episodeStop;
  }
  control->request(command);
  return episodeStop->load(std::memory_order_acquire);
}

bool terminalCommandRejectsLateEpisodeForTest(
    const ScrollCaptureJobCommand command) {
  auto control = std::make_shared<ScrollCaptureJobControl>();
  control->request(command);
  auto episodeStop = std::make_shared<std::atomic<bool>>(false);
  return !registerEpisode(control->state_, episodeStop) &&
         episodeStop->load(std::memory_order_acquire);
}

ScrollCaptureJobResult runTerminalEpisodeRegistrationRaceForTest(
    const ScrollCaptureJobCommand command) {
  auto control = std::make_shared<ScrollCaptureJobControl>();
  control->request(command);
  auto episodeStop = std::make_shared<std::atomic<bool>>(false);
  if (registerEpisode(control->state_, episodeStop))
    return {ScrollCaptureJobCompletion::Failed,
            {},
            QStringLiteral("terminal episode registration unexpectedly won")};
  const auto terminal = rejectedEpisodeTerminalResult(control, false);
  return terminal.value_or(ScrollCaptureJobResult{
      ScrollCaptureJobCompletion::Failed,
      {},
      QStringLiteral("rejected registration did not drain terminal command"),
      0,
      0});
}

bool debugImagesRemainUnpublishedOnCancelForTest() {
  QTemporaryDir directory;
  if (!directory.isValid())
    return false;
  Worker worker(stitch::Axis::Vertical);
  worker.debugDir = directory.path();
  worker.firstCrop = QImage(8, 8, QImage::Format_RGBA8888);
  worker.lastCrop = QImage(8, 8, QImage::Format_RGBA8888);
  QImage assembled(8, 16, QImage::Format_RGBA8888);
  worker.firstCrop.fill(Qt::red);
  worker.lastCrop.fill(Qt::blue);
  assembled.fill(Qt::green);

  auto control = std::make_shared<ScrollCaptureJobControl>();
  DebugImageBatch batch;
  if (!stageDebugImages(worker, assembled, control->state_->cancelRequested,
                        batch))
    return false;
  control->request(ScrollCaptureJobCommand::Done);
  control->request(ScrollCaptureJobCommand::Cancel);
  if (commitDone(control->state_))
    return false;
  const bool unpublished =
      !QFile::exists(directory.path() +
                     QStringLiteral("/scroll-stitched.png")) &&
      !QFile::exists(directory.path() +
                     QStringLiteral("/scroll-first-frame.png")) &&
      !QFile::exists(directory.path() +
                     QStringLiteral("/scroll-last-frame.png"));

  auto alreadyCancelled = std::make_shared<ScrollCaptureJobControl>();
  alreadyCancelled->request(ScrollCaptureJobCommand::Cancel);
  DebugImageBatch cancelledBatch;
  std::atomic<int> encodeCount{0};
  const bool staged = stageDebugImages(
      worker, assembled, alreadyCancelled->state_->cancelRequested,
      cancelledBatch, &encodeCount);
  return unpublished && !staged &&
         encodeCount.load(std::memory_order_relaxed) == 0;
}

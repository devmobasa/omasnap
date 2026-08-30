/** @fileoverview Lifetime-owned scroll capture job. One worker thread owns the
 * output-capture session from setup through final assembly; the UI sends only
 * plain commands and polls plain status snapshots. */
#pragma once

#include "capture.hpp"
#include "scroll-inject.hpp"
#include "stitch.hpp"

#include <QImage>
#include <QRect>
#include <QString>

#include <atomic>
#include <memory>

enum class ScrollCaptureJobMode { Manual, Auto };
enum class ScrollCaptureJobCommand { None, Continue, Done, Cancel, Back };
enum class ScrollCaptureJobStage { Preparing, Capturing, Stalled, Completed };
enum class ScrollCaptureJobCompletion {
  Done,
  Cancelled,
  Back,
  SetupFailed,
  NoFrames,
  Failed
};

struct ScrollCaptureJobSpec {
  QString outputName;
  QRect regionPhysical;
  QString debugDir;
  ScrollCaptureJobMode mode = ScrollCaptureJobMode::Manual;
  stitch::Axis axis = stitch::Axis::Vertical;
  int parkX = 0;
  int parkY = 0;
  ScrollInjectorValidationOptions injectorValidation;
};

struct ScrollCaptureJobSnapshot {
  ScrollCaptureJobStage stage = ScrollCaptureJobStage::Preparing;
  QString status;
  bool warning = false;
  bool autoStalled = false;
  std::uint64_t revision = 0;
};

struct ScrollCaptureJobResult {
  ScrollCaptureJobCompletion completion = ScrollCaptureJobCompletion::Cancelled;
  QImage image;
  QString error;
  int keptFrames = 0;
  int unverifiedSeams = 0;
};

class ScrollCaptureJobControl final {
public:
  struct State;
  ScrollCaptureJobControl();
  ~ScrollCaptureJobControl();
  ScrollCaptureJobControl(const ScrollCaptureJobControl &) = delete;
  ScrollCaptureJobControl &operator=(const ScrollCaptureJobControl &) = delete;

  /// Terminal commands win over Continue. Safe from the UI thread.
  void request(ScrollCaptureJobCommand command);
  [[nodiscard]] ScrollCaptureJobCommand command() const;
  [[nodiscard]] ScrollCaptureJobSnapshot snapshot() const;

private:
  std::shared_ptr<State> state_;
  friend ScrollCaptureJobResult
  runScrollCaptureJob(const ScrollCaptureJobSpec &,
                      const std::shared_ptr<ScrollCaptureJobControl> &);
  friend ScrollCaptureJobResult runDelayedScrollCaptureJobForTest(
      const std::shared_ptr<ScrollCaptureJobControl> &, int, int,
      const QImage &, const std::shared_ptr<std::atomic<int>> &, int);
  friend ScrollCaptureJobResult runFailingScrollCaptureJobForTest(
      const std::shared_ptr<ScrollCaptureJobControl> &);
  friend bool terminalCommandStopsActiveEpisodeForTest(ScrollCaptureJobCommand);
  friend bool terminalCommandRejectsLateEpisodeForTest(ScrollCaptureJobCommand);
  friend ScrollCaptureJobResult
      runTerminalEpisodeRegistrationRaceForTest(ScrollCaptureJobCommand);
  friend bool debugImagesRemainUnpublishedOnCancelForTest();
};

/// Blocking owner body; call with QtConcurrent::run().
[[nodiscard]] ScrollCaptureJobResult
runScrollCaptureJob(const ScrollCaptureJobSpec &spec,
                    const std::shared_ptr<ScrollCaptureJobControl> &control);

/// Deterministic coordinator used by the headless responsiveness/lifetime
/// smoke checks. `sideEffects` increments only if setup reaches its ready
/// point.
[[nodiscard]] ScrollCaptureJobResult runDelayedScrollCaptureJobForTest(
    const std::shared_ptr<ScrollCaptureJobControl> &control, int setupDelayMs,
    int finishDelayMs, const QImage &result,
    const std::shared_ptr<std::atomic<int>> &sideEffects,
    int cancelDrainDelayMs = 0);

/// Throws a real std::bad_alloc inside the production exception boundary.
[[nodiscard]] ScrollCaptureJobResult runFailingScrollCaptureJobForTest(
    const std::shared_ptr<ScrollCaptureJobControl> &control);
/// Proves exception unwinding requests stop before joining the nested injector
/// owner thread.
[[nodiscard]] bool autoInjectorOwnerUnwindsForTest();
/// Throws on the real injector-owner thread and proves the exception is
/// translated into a failed session instead of terminating the process.
[[nodiscard]] bool injectorOwnerExceptionBecomesFailedForTest();
[[nodiscard]] bool
terminalCommandStopsActiveEpisodeForTest(ScrollCaptureJobCommand command);
[[nodiscard]] bool
terminalCommandRejectsLateEpisodeForTest(ScrollCaptureJobCommand command);
[[nodiscard]] ScrollCaptureJobResult
runTerminalEpisodeRegistrationRaceForTest(ScrollCaptureJobCommand command);
[[nodiscard]] bool debugImagesRemainUnpublishedOnCancelForTest();

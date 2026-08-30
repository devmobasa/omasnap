/** @fileoverview The auto-scroll injection worker: drives the application
 *  under the capture region with real wheel input, one tick per acknowledged
 *  capture cycle. Backend cascade (validated on Hyprland): a uinput kernel
 *  mouse when the compositor's natural-scroll policy is known (its wheel
 *  events are pre-compensated for it), else the wlr virtual-pointer protocol
 *  bound to the target output. */
#pragma once

#include "auto-capture.hpp"
#include "stitch.hpp"

#include <QString>

#include <atomic>
#include <memory>

struct ScrollInjectorValidationOptions;

/// Thread-safe command channel for one auto-scroll capture. The blocking
/// `runScrollInjectorSession()` call owns every uinput and Wayland object from
/// creation through destruction on its caller's thread. Continue starts a new
/// episode on those same resources, so natural-scroll detection and backend
/// setup happen only once per capture session.
class ScrollInjectorSession final {
public:
  enum class Status { Preparing, Ready, Failed, Stopped };
  struct Snapshot {
    Status status = Status::Preparing;
    QString error;
  };

  ScrollInjectorSession();
  ~ScrollInjectorSession();
  ScrollInjectorSession(const ScrollInjectorSession &) = delete;
  ScrollInjectorSession &operator=(const ScrollInjectorSession &) = delete;

  /// Starts or resumes one lock-step injection episode. `parkX/parkY` are
  /// physical output pixels inside the capture region.
  void startEpisode(std::shared_ptr<stitch::CaptureHandshake> handshake,
                    std::shared_ptr<std::atomic<bool>> episodeStop, int parkX,
                    int parkY, stitch::Axis axis);
  /// Stops only the current episode, retaining the prepared backend for
  /// Continue.
  void stopEpisode();
  /// Stops the whole session and wakes every stop-aware wait.
  void stop();
  [[nodiscard]] bool stopped() const;
  [[nodiscard]] Snapshot snapshot() const;
  /// Records a failure caught by the native-resource owner boundary.
  void reportFailure(const QString &error);

private:
  struct State;
  std::shared_ptr<State> state_;
  friend void runScrollInjectorSession(
      const std::shared_ptr<ScrollInjectorSession> &, const QString &,
      ScrollInjectorValidationOptions);
  friend bool runtimeInjectionFailureBecomesFailedForTest();
};

/// Opt-in live-validation controls. Production uses the defaults; the manual
/// backend matrix simulates a denied device or omitted protocol without
/// changing permissions or compositor globals on the host.
struct ScrollInjectorValidationOptions {
  bool denyUinput = false;
  bool omitVirtualPointerProtocol = false;
};

/// Blocking owner body. Call on a joinable worker thread, never the UI thread.
/// It prepares the backends, services episodes until `session->stop()`, and
/// destroys all native resources before returning.
void runScrollInjectorSession(
    const std::shared_ptr<ScrollInjectorSession> &session,
    const QString &outputName,
    ScrollInjectorValidationOptions validation = {});

/// Runs the real stop-aware park sequence with a fake pointer that requests
/// cancellation after its initial absolute motion. Returns the total number of
/// emitted pointer events; cancellation must keep this at one.
[[nodiscard]] int pointerEventsAfterCancelDuringParkForTest();
/// Exercises the production episode failure transition with a fake backend
/// whose first wheel write fails.
[[nodiscard]] bool runtimeInjectionFailureBecomesFailedForTest();

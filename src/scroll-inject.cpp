/** @fileoverview Auto-scroll injection worker (see scroll-inject.hpp). */
#include "scroll-inject.hpp"

#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#include <QByteArray>
#include <QDebug>
#include <QElapsedTimer>

#include <wayland-client.h>

#include <linux/input-event-codes.h>
#include <linux/uinput.h>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern char **environ;

struct ScrollInjectorSession::State {
  struct Episode {
    std::uint64_t generation = 0;
    std::shared_ptr<stitch::CaptureHandshake> handshake;
    std::shared_ptr<std::atomic<bool>> stop;
    int parkX = 0;
    int parkY = 0;
    stitch::Axis axis = stitch::Axis::Vertical;
  };

  mutable std::mutex mutex;
  std::condition_variable changed;
  std::atomic<bool> stop{false};
  Status status = Status::Preparing;
  QString error;
  Episode episode;
};

namespace {
using stitch::Axis;
using stitch::CaptureHandshake;

/// Time between injecting a wheel group and announcing its rendered frame:
/// at 150 ms a 60 Hz client gets ~9 frames to paint while the handshake still
/// prevents the next wheel event racing the screenshot.
// The overlay waits for the page to settle itself (see scroll-capture.cpp);
// this only keeps the tick and the ready announcement from racing.
constexpr int kScrollSettleMs = 30;
constexpr int kInputRegionSettleMs = 50;
constexpr int kUinputDeviceSettleMs = 150;
constexpr int kPointerNudgeSettleMs = 20;
/// One logical wheel notch for the wlr virtual pointer.
constexpr double kNotchValue = 10.0;
constexpr int kProcessTerminateGraceMs = 100;

void sleepMs(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

/// Sleeps in short slices so a stop request interrupts the settle.
bool sleepUnlessStopped(int ms, const std::atomic<bool> &stop) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (stop.load(std::memory_order_acquire))
      return false;
    sleepMs(5);
  }
  return !stop.load(std::memory_order_acquire);
}

class OwnedProcessGroup final {
public:
  enum class LeaderState : std::uint8_t { Running, Exited, Unavailable };

  ~OwnedProcessGroup() {
    if (leader_ > 0)
      finish(false);
    closeOutput();
  }

  OwnedProcessGroup() = default;
  OwnedProcessGroup(const OwnedProcessGroup &) = delete;
  OwnedProcessGroup &operator=(const OwnedProcessGroup &) = delete;

  bool start() {
    int outputPipe[2] = {-1, -1};
    if (::pipe2(outputPipe, O_CLOEXEC) != 0)
      return false;
    if (!moveAboveStandardStreams(outputPipe[0]) ||
        !moveAboveStandardStreams(outputPipe[1])) {
      ::close(outputPipe[0]);
      ::close(outputPipe[1]);
      return false;
    }

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    bool actionsReady = false;
    bool attributesReady = false;
    int spawnError = posix_spawn_file_actions_init(&actions);
    if (spawnError == 0) {
      actionsReady = true;
      spawnError = posix_spawn_file_actions_adddup2(&actions, outputPipe[1],
                                                    STDOUT_FILENO);
    }
    if (spawnError == 0)
      spawnError = posix_spawn_file_actions_addclose(&actions, outputPipe[0]);
    if (spawnError == 0)
      spawnError = posix_spawn_file_actions_addclose(&actions, outputPipe[1]);
    if (spawnError == 0)
      spawnError = posix_spawn_file_actions_addopen(
          &actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    if (spawnError == 0) {
      spawnError = posix_spawnattr_init(&attributes);
      attributesReady = spawnError == 0;
    }
    if (spawnError == 0)
      spawnError = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    if (spawnError == 0)
      spawnError = posix_spawnattr_setpgroup(&attributes, 0);

    std::array<char, 8> executable{"hyprctl"};
    std::array<char, 10> command{"getoption"};
    std::array<char, 21> option{"input:natural_scroll"};
    std::array<char, 3> json{"-j"};
    char *const arguments[] = {executable.data(), command.data(), option.data(),
                               json.data(), nullptr};
    if (spawnError == 0)
      spawnError = ::posix_spawnp(&leader_, executable.data(), &actions,
                                  &attributes, arguments, environ);

    if (attributesReady)
      posix_spawnattr_destroy(&attributes);
    if (actionsReady)
      posix_spawn_file_actions_destroy(&actions);
    ::close(outputPipe[1]);
    if (spawnError != 0) {
      leader_ = -1;
      ::close(outputPipe[0]);
      return false;
    }
    output_ = outputPipe[0];
    return true;
  }

  LeaderState leaderState() {
    siginfo_t info{};
    int result = 0;
    do {
      result = ::waitid(P_PID, static_cast<id_t>(leader_), &info,
                        WEXITED | WNOHANG | WNOWAIT);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
      // ECHILD means some external reaper consumed the stable identity. Do not
      // signal its numeric process-group ID again because it may be reusable.
      if (errno == ECHILD)
        leader_ = -1;
      return LeaderState::Unavailable;
    }
    return info.si_pid == 0 ? LeaderState::Running : LeaderState::Exited;
  }

  int finish(const bool allowTerminateGrace) {
    if (leader_ <= 0)
      return -1;

    // The leader remains unreaped throughout group signalling. Its PID cannot
    // be reused as another process-group ID until waitpid() below releases it.
    if (allowTerminateGrace) {
      ::kill(-leader_, SIGTERM);
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(kProcessTerminateGraceMs);
      while (std::chrono::steady_clock::now() < deadline) {
        if (::kill(-leader_, 0) != 0 && errno == ESRCH)
          break;
        sleepMs(5);
      }
    }
    ::kill(-leader_, SIGKILL);

    int status = -1;
    pid_t waited = -1;
    do {
      waited = ::waitpid(leader_, &status, 0);
    } while (waited < 0 && errno == EINTR);
    leader_ = -1;
    return waited > 0 ? status : -1;
  }

  QByteArray readAllOutput() {
    QByteArray output;
    std::array<char, 4096> buffer{};
    while (output_ >= 0) {
      const ssize_t readBytes = ::read(output_, buffer.data(), buffer.size());
      if (readBytes > 0) {
        output.append(buffer.data(), readBytes);
        continue;
      }
      if (readBytes < 0 && errno == EINTR)
        continue;
      break;
    }
    closeOutput();
    return output;
  }

private:
  static bool moveAboveStandardStreams(int &descriptor) {
    if (descriptor > STDERR_FILENO)
      return true;
    const int replacement = ::fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
    if (replacement < 0)
      return false;
    ::close(descriptor);
    descriptor = replacement;
    return true;
  }

  void closeOutput() {
    if (output_ >= 0) {
      ::close(output_);
      output_ = -1;
    }
  }

  pid_t leader_ = -1;
  int output_ = -1;
};

/// The compositor applies the user's natural-scroll policy to a real kernel
/// mouse, so uinput injection must pre-compensate, and is only safe when the
/// policy is actually known.
std::optional<bool> hyprlandNaturalScroll(const std::atomic<bool> &stop) {
  if (qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE").isEmpty())
    return std::nullopt;
  OwnedProcessGroup process;
  if (!process.start())
    return std::nullopt;
  QElapsedTimer deadline;
  deadline.start();
  while (true) {
    const OwnedProcessGroup::LeaderState leaderState = process.leaderState();
    if (leaderState == OwnedProcessGroup::LeaderState::Exited)
      break;
    if (leaderState == OwnedProcessGroup::LeaderState::Unavailable)
      return std::nullopt;
    if (stop.load(std::memory_order_acquire)) {
      process.finish(true);
      return std::nullopt;
    }
    if (deadline.elapsed() >= 2000) {
      process.finish(false);
      return std::nullopt;
    }
    sleepMs(10);
  }
  const int status = process.finish(false);
  if (status < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return std::nullopt;
  const QByteArray out = process.readAllOutput();
  const qsizetype key = out.indexOf("\"bool\"");
  if (key < 0)
    return std::nullopt;
  const qsizetype colon = out.indexOf(':', key);
  if (colon < 0)
    return std::nullopt;
  const QByteArray tail = out.mid(colon + 1).trimmed();
  if (tail.startsWith("true"))
    return true;
  if (tail.startsWith("false"))
    return false;
  return std::nullopt;
}

// --- uinput kernel mouse
// ------------------------------------------------------
class UinputMouse {
public:
  bool open(QString &error, const bool denyForValidation = false) {
    if (denyForValidation) {
      error = QStringLiteral("permission denied by live validation");
      return false;
    }
    fd_ = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd_ < 0) {
      error = QStringLiteral("could not open /dev/uinput");
      return false;
    }
    ioctl(fd_, UI_SET_EVBIT, EV_REL);
    for (const int axis : {REL_X, REL_Y, REL_WHEEL, REL_HWHEEL})
      ioctl(fd_, UI_SET_RELBIT, axis);
    // libinput requires a button before udev classifies the device as a
    // mouse (so the natural-scroll policy applies); advertised, never
    // emitted.
    ioctl(fd_, UI_SET_EVBIT, EV_KEY);
    ioctl(fd_, UI_SET_KEYBIT, BTN_LEFT);
    uinput_setup setup{};
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x1d6b;
    setup.id.product = 0x0001;
    std::strncpy(setup.name, "Omasnap Auto Scroll", sizeof(setup.name) - 1);
    if (ioctl(fd_, UI_DEV_SETUP, &setup) < 0 || ioctl(fd_, UI_DEV_CREATE) < 0) {
      error = QStringLiteral("uinput device setup failed");
      close();
      return false;
    }
    return true;
  }
  bool nudge(const std::atomic<bool> &stop) {
    // ±1 px so the compositor re-hit-tests the surface under the pointer.
    if (stop.load(std::memory_order_acquire) || !emitEvent(EV_REL, REL_X, 1) ||
        !emitEvent(EV_SYN, SYN_REPORT, 0))
      return false;
    if (!sleepUnlessStopped(kPointerNudgeSettleMs, stop))
      return false;
    return emitEvent(EV_REL, REL_X, -1) && emitEvent(EV_SYN, SYN_REPORT, 0);
  }
  bool scroll(Axis axis, int notches, bool naturalScroll,
              const std::atomic<bool> &stop) {
    if (stop.load(std::memory_order_acquire))
      return false;
    // REL_WHEEL follows the physical wheel: negative = down; HWHEEL positive
    // = right. The compositor then applies natural-scroll, so pre-negate.
    const int code = axis == Axis::Vertical ? REL_WHEEL : REL_HWHEEL;
    int amount = axis == Axis::Vertical ? -notches : notches;
    if (naturalScroll)
      amount = -amount;
    return emitEvent(EV_REL, code, amount) && emitEvent(EV_SYN, SYN_REPORT, 0);
  }
  void close() {
    if (fd_ >= 0) {
      ioctl(fd_, UI_DEV_DESTROY);
      ::close(fd_);
      fd_ = -1;
    }
  }
  ~UinputMouse() { close(); }

private:
  bool emitEvent(int type, int code, int value) {
    input_event event{};
    event.type = static_cast<std::uint16_t>(type);
    event.code = static_cast<std::uint16_t>(code);
    event.value = value;
    return write(fd_, &event, sizeof(event)) == sizeof(event);
  }
  int fd_ = -1;
};

template <typename AbsoluteMotion, typename RelativeMotion>
bool runStopAwarePointerPark(const std::atomic<bool> &stop,
                             AbsoluteMotion absoluteMotion,
                             RelativeMotion relativeMotion) {
  if (stop.load(std::memory_order_acquire) || !absoluteMotion())
    return false;
  if (!sleepUnlessStopped(kPointerNudgeSettleMs, stop) ||
      stop.load(std::memory_order_acquire) || !relativeMotion(1))
    return false;
  if (!sleepUnlessStopped(kPointerNudgeSettleMs, stop) ||
      stop.load(std::memory_order_acquire) || !relativeMotion(-1))
    return false;
  return true;
}

// --- wlr virtual pointer
// ------------------------------------------------------
struct WlrPointer {
  wl_display *display = nullptr;
  wl_registry *registry = nullptr;
  wl_seat *seat = nullptr;
  zwlr_virtual_pointer_manager_v1 *manager = nullptr;
  zwlr_virtual_pointer_v1 *pointer = nullptr;
  struct Output {
    wl_output *handle = nullptr;
    std::string name;
    int width = 0;
    int height = 0;
  };
  std::vector<std::unique_ptr<Output>> outputs;
  Output *target = nullptr;
  std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();

  std::uint32_t timeMs() const {
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
  }
  bool park(int x, int y, const std::atomic<bool> &stop) {
    if (!pointer || !target)
      return false;
    const auto width = static_cast<std::uint32_t>(std::max(1, target->width));
    const auto height = static_cast<std::uint32_t>(std::max(1, target->height));
    return runStopAwarePointerPark(
        stop,
        [&] {
          zwlr_virtual_pointer_v1_motion_absolute(
              pointer, timeMs(),
              static_cast<std::uint32_t>(std::clamp(x, 0, target->width)),
              static_cast<std::uint32_t>(std::clamp(y, 0, target->height)),
              width, height);
          zwlr_virtual_pointer_v1_frame(pointer);
          return wl_display_flush(display) >= 0;
        },
        [&](const int delta) {
          zwlr_virtual_pointer_v1_motion(pointer, timeMs(),
                                         wl_fixed_from_int(delta), 0);
          zwlr_virtual_pointer_v1_frame(pointer);
          return wl_display_flush(display) >= 0;
        });
  }
  bool scroll(Axis axis, int notches, const std::atomic<bool> &stop) {
    if (!pointer || stop.load(std::memory_order_acquire))
      return false;
    // One aggregate discrete event, axis_source AFTER axis_discrete
    // (Hyprland ties the source to the most recent axis), one frame.
    const std::uint32_t wlAxis = axis == Axis::Vertical ? 0 : 1;
    zwlr_virtual_pointer_v1_axis_discrete(
        pointer, timeMs(), wlAxis, wl_fixed_from_double(kNotchValue * notches),
        notches);
    zwlr_virtual_pointer_v1_axis_source(pointer, 0 /* wheel */);
    zwlr_virtual_pointer_v1_frame(pointer);
    return wl_display_flush(display) >= 0;
  }
  ~WlrPointer() {
    if (pointer)
      zwlr_virtual_pointer_v1_destroy(pointer);
    for (const auto &output : outputs)
      if (output->handle)
        wl_output_release(output->handle);
    if (manager)
      zwlr_virtual_pointer_manager_v1_destroy(manager);
    if (seat)
      wl_seat_destroy(seat);
    if (registry)
      wl_registry_destroy(registry);
    if (display) {
      wl_display_flush(display);
      wl_display_disconnect(display);
    }
  }
};

void injectOutputName(void *data, wl_output *, const char *name) {
  static_cast<WlrPointer::Output *>(data)->name = name;
}
void injectOutputMode(void *data, wl_output *, std::uint32_t flags, int32_t w,
                      int32_t h, int32_t) {
  if (flags & WL_OUTPUT_MODE_CURRENT) {
    auto *output = static_cast<WlrPointer::Output *>(data);
    output->width = w;
    output->height = h;
  }
}
void injectOutputGeometry(void *, wl_output *, int32_t, int32_t, int32_t,
                          int32_t, int32_t, const char *, const char *,
                          int32_t) {}
void injectOutputDone(void *, wl_output *) {}
void injectOutputScale(void *, wl_output *, int32_t) {}
void injectOutputDescription(void *, wl_output *, const char *) {}
constexpr wl_output_listener kInjectOutputListener{
    injectOutputGeometry, injectOutputMode, injectOutputDone,
    injectOutputScale,    injectOutputName, injectOutputDescription};

void injectRegistryGlobal(void *data, wl_registry *registry, std::uint32_t name,
                          const char *interface, std::uint32_t version) {
  auto &state = *static_cast<WlrPointer *>(data);
  if (std::strcmp(interface, wl_seat_interface.name) == 0 && !state.seat) {
    state.seat = static_cast<wl_seat *>(wl_registry_bind(
        registry, name, &wl_seat_interface, std::min(version, 8u)));
  } else if (std::strcmp(interface, wl_output_interface.name) == 0 &&
             version >= 4) {
    auto output = std::make_unique<WlrPointer::Output>();
    output->handle = static_cast<wl_output *>(
        wl_registry_bind(registry, name, &wl_output_interface, 4));
    wl_output_add_listener(output->handle, &kInjectOutputListener,
                           output.get());
    state.outputs.push_back(std::move(output));
  } else if (std::strcmp(interface,
                         zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
    state.manager =
        static_cast<zwlr_virtual_pointer_manager_v1 *>(wl_registry_bind(
            registry, name, &zwlr_virtual_pointer_manager_v1_interface,
            std::min(version, 2u)));
  }
}
void injectRegistryRemove(void *, wl_registry *, std::uint32_t) {}
constexpr wl_registry_listener kInjectRegistryListener{injectRegistryGlobal,
                                                       injectRegistryRemove};

std::unique_ptr<WlrPointer> connectWlrPointer(const QString &outputName,
                                              QString &error,
                                              const bool omitForValidation =
                                                  false) {
  auto state = std::make_unique<WlrPointer>();
  state->display = wl_display_connect(nullptr);
  if (!state->display) {
    error = QStringLiteral("could not connect to Wayland for injection");
    return nullptr;
  }
  state->registry = wl_display_get_registry(state->display);
  wl_registry_add_listener(state->registry, &kInjectRegistryListener,
                           state.get());
  wl_display_roundtrip(state->display); // globals
  wl_display_roundtrip(state->display); // output names/modes
  if (omitForValidation && state->manager) {
    zwlr_virtual_pointer_manager_v1_destroy(state->manager);
    state->manager = nullptr;
  }
  if (!state->manager) {
    error = QStringLiteral("compositor does not expose the virtual pointer");
    return nullptr;
  }
  const std::string wanted = outputName.toStdString();
  for (const auto &output : state->outputs) {
    if (wanted.empty() || output->name == wanted) {
      state->target = output.get();
      break;
    }
  }
  if (!state->target) {
    error = QStringLiteral("output %1 not found for injection").arg(outputName);
    return nullptr;
  }
  // motion_absolute coordinates are physical pixels of the bound output; an
  // unbound pointer maps across the whole layout and misses on multi-monitor.
  if (zwlr_virtual_pointer_manager_v1_get_version(state->manager) >= 2) {
    state->pointer =
        zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
            state->manager, state->seat, state->target->handle);
  } else {
    qInfo().noquote() << QStringLiteral("scroll-inject: virtual pointer v1 "
                                        "only; warps map to the whole layout");
    state->pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
        state->manager, state->seat);
  }
  wl_display_roundtrip(state->display);
  if (!state->pointer) {
    error = QStringLiteral("could not create the virtual pointer");
    return nullptr;
  }
  return state;
}

enum class CaptureScrollLoopResult { Stopped, InjectionFailed };

/// One tick per acknowledged cycle, forever until stopped.
CaptureScrollLoopResult
runCaptureScrollLoop(const std::shared_ptr<std::atomic<bool>> &stop,
                     const std::shared_ptr<CaptureHandshake> &handshake,
                     const std::function<bool(int)> &inject) {
  std::uint64_t cycle = handshake->publishReady(); // cycle 1: unscrolled frame
  while (true) {
    const std::optional<int> notches = handshake->waitForCapture(cycle, *stop);
    if (!notches)
      break;
    if (stop->load(std::memory_order_acquire))
      break;
    // Parked once at the start and left alone after that. Re-parking every tick
    // kept the wheel on the page, but it also meant the pointer could never be
    // taken anywhere, and moving it away is how a person stops an auto scroll.
    // Left alone, moving out of the region sends the injected wheel somewhere
    // harmless, the scrolling stops, and the buttons and keys are reachable
    // again.
    if (!inject(*notches)) {
      return stop->load(std::memory_order_acquire)
                 ? CaptureScrollLoopResult::Stopped
                 : CaptureScrollLoopResult::InjectionFailed;
    }
    if (!sleepUnlessStopped(kScrollSettleMs, *stop))
      break;
    cycle = handshake->publishReady();
  }
  return CaptureScrollLoopResult::Stopped;
}
} // namespace

ScrollInjectorSession::ScrollInjectorSession()
    : state_(std::make_shared<State>()) {}

ScrollInjectorSession::~ScrollInjectorSession() { stop(); }

void ScrollInjectorSession::startEpisode(
    std::shared_ptr<stitch::CaptureHandshake> handshake,
    std::shared_ptr<std::atomic<bool>> episodeStop, int parkX, int parkY,
    stitch::Axis axis) {
  const std::lock_guard lock(state_->mutex);
  if (state_->stop.load(std::memory_order_acquire))
    return;
  if (state_->episode.stop)
    state_->episode.stop->store(true, std::memory_order_release);
  ++state_->episode.generation;
  state_->episode.handshake = std::move(handshake);
  state_->episode.stop = std::move(episodeStop);
  state_->episode.parkX = parkX;
  state_->episode.parkY = parkY;
  state_->episode.axis = axis;
  state_->changed.notify_all();
}

void ScrollInjectorSession::stopEpisode() {
  const std::lock_guard lock(state_->mutex);
  if (state_->episode.stop)
    state_->episode.stop->store(true, std::memory_order_release);
  state_->changed.notify_all();
}

void ScrollInjectorSession::stop() {
  state_->stop.store(true, std::memory_order_release);
  const std::lock_guard lock(state_->mutex);
  if (state_->episode.stop)
    state_->episode.stop->store(true, std::memory_order_release);
  state_->changed.notify_all();
}

bool ScrollInjectorSession::stopped() const {
  return state_->stop.load(std::memory_order_acquire);
}

ScrollInjectorSession::Snapshot ScrollInjectorSession::snapshot() const {
  const std::lock_guard lock(state_->mutex);
  return {state_->status, state_->error};
}

void ScrollInjectorSession::reportFailure(const QString &error) {
  const std::lock_guard lock(state_->mutex);
  state_->status = Status::Failed;
  state_->error = error;
  state_->changed.notify_all();
}

void runScrollInjectorSession(
    const std::shared_ptr<ScrollInjectorSession> &session,
    const QString &outputName,
    const ScrollInjectorValidationOptions validation) {
  const std::shared_ptr<ScrollInjectorSession::State> state = session->state_;
  const auto fail = [&](const QString &error) {
    session->reportFailure(error);
  };

  // Setup, use, and teardown all stay on this thread. The natural-scroll
  // answer is consequently cached for every Continue episode in the session.
  const std::optional<bool> naturalScroll = hyprlandNaturalScroll(state->stop);
  if (state->stop.load(std::memory_order_acquire))
    return;
  UinputMouse uinput;
  bool haveUinput = false;
  if (naturalScroll) {
    QString uinputError;
    haveUinput = uinput.open(uinputError, validation.denyUinput);
    if (!haveUinput)
      qInfo().noquote()
          << QStringLiteral("scroll-inject: uinput unavailable (%1); using the "
                            "virtual pointer")
                 .arg(uinputError);
  } else {
    qInfo().noquote() << QStringLiteral(
        "scroll-inject: natural-scroll policy unknown; using the virtual "
        "pointer");
  }
  // The wlr pointer parks the cursor for both backends (and scrolls when
  // uinput is unavailable).
  QString wlrError;
  std::unique_ptr<WlrPointer> wlr = connectWlrPointer(
      outputName, wlrError, validation.omitVirtualPointerProtocol);
  if (state->stop.load(std::memory_order_acquire))
    return;
  if (!wlr && !haveUinput) {
    fail(wlrError);
    return;
  }
  if (!wlr)
    qInfo().noquote() << QStringLiteral(
                             "scroll-inject: no virtual pointer (%1); cannot "
                             "park · scrolling wherever the cursor is")
                             .arg(wlrError);
  qInfo().noquote() << QStringLiteral(
                           "scroll-inject: backend=%1 natural=%2 on %3")
                           .arg(haveUinput ? QStringLiteral("uinput")
                                           : QStringLiteral("wlr-pointer"))
                           .arg(naturalScroll
                                    ? (*naturalScroll ? "true" : "false")
                                    : "unknown")
                           .arg(outputName);

  {
    const std::lock_guard lock(state->mutex);
    state->status = ScrollInjectorSession::Status::Ready;
    state->changed.notify_all();
  }

  std::uint64_t consumedGeneration = 0;
  while (!state->stop.load(std::memory_order_acquire)) {
    ScrollInjectorSession::State::Episode episode;
    {
      std::unique_lock lock(state->mutex);
      state->changed.wait(lock, [&] {
        return state->stop.load(std::memory_order_acquire) ||
               state->episode.generation > consumedGeneration;
      });
      if (state->stop.load(std::memory_order_acquire))
        break;
      episode = state->episode;
      consumedGeneration = episode.generation;
    }
    if (!episode.handshake || !episode.stop)
      continue;
    qInfo().noquote() << QStringLiteral("scroll-inject: episode %1 park=%2,%3")
                             .arg(episode.generation)
                             .arg(episode.parkX)
                             .arg(episode.parkY);
    if (!sleepUnlessStopped(kInputRegionSettleMs, *episode.stop))
      continue;
    if (haveUinput && !sleepUnlessStopped(kUinputDeviceSettleMs, *episode.stop))
      continue;
    if (state->stop.load(std::memory_order_acquire) ||
        episode.stop->load(std::memory_order_acquire))
      continue;
    if (wlr && !wlr->park(episode.parkX, episode.parkY, *episode.stop)) {
      if (episode.stop->load(std::memory_order_acquire))
        continue;
      fail(QStringLiteral("pointer park failed"));
      break;
    }
    if (state->stop.load(std::memory_order_acquire) ||
        episode.stop->load(std::memory_order_acquire))
      continue;
    if (haveUinput && !uinput.nudge(*episode.stop)) {
      if (episode.stop->load(std::memory_order_acquire))
        continue;
      qWarning().noquote() << QStringLiteral(
          "scroll-inject: pointer nudge failed");
      fail(QStringLiteral("pointer nudge failed"));
      break;
    }
    const std::function<bool(int)> inject = [&](const int notches) {
      if (state->stop.load(std::memory_order_acquire) ||
          episode.stop->load(std::memory_order_acquire))
        return false;
      qInfo().noquote() << QStringLiteral("scroll-inject: tick %1 notch%2")
                               .arg(notches)
                               .arg(notches == 1 ? QString()
                                                 : QStringLiteral("es"));
      if (haveUinput)
        return uinput.scroll(episode.axis, notches,
                             naturalScroll.value_or(false), *episode.stop);
      if (wlr) {
        return wlr->scroll(episode.axis, notches, *episode.stop);
      }
      return false;
    };
    if (runCaptureScrollLoop(episode.stop, episode.handshake, inject) ==
        CaptureScrollLoopResult::InjectionFailed) {
      episode.stop->store(true, std::memory_order_release);
      fail(QStringLiteral("scroll injection failed"));
      break;
    }
  }
  {
    const std::lock_guard lock(state->mutex);
    if (state->status != ScrollInjectorSession::Status::Failed)
      state->status = ScrollInjectorSession::Status::Stopped;
  }
  qInfo().noquote() << QStringLiteral("scroll-inject: worker exited");
}

int pointerEventsAfterCancelDuringParkForTest() {
  std::atomic<bool> stop{false};
  int events = 0;
  runStopAwarePointerPark(
      stop,
      [&] {
        ++events;
        stop.store(true, std::memory_order_release);
        return true;
      },
      [&](int) {
        ++events;
        return true;
      });
  return events;
}

bool runtimeInjectionFailureBecomesFailedForTest() {
  auto session = std::make_shared<ScrollInjectorSession>();
  const auto state = session->state_;
  {
    const std::lock_guard lock(state->mutex);
    state->status = ScrollInjectorSession::Status::Ready;
  }
  auto stop = std::make_shared<std::atomic<bool>>(false);
  auto handshake = std::make_shared<CaptureHandshake>();
  CaptureScrollLoopResult result = CaptureScrollLoopResult::Stopped;
  std::jthread worker([&] {
    result = runCaptureScrollLoop(stop, handshake, [](int) { return false; });
  });
  while (handshake->readyCycle() == 0)
    std::this_thread::yield();
  handshake->acknowledge(handshake->readyCycle());
  worker.join();
  if (result == CaptureScrollLoopResult::InjectionFailed)
    session->reportFailure(QStringLiteral("scroll injection failed"));
  return session->snapshot().status == ScrollInjectorSession::Status::Failed;
}

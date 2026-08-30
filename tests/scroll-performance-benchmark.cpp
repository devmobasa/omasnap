#include "scroll-capture-job.hpp"
#include "scroll-capture.hpp"
#include "scroll-inject.hpp"
#include "stitch.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QMouseEvent>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
double elapsedMs(const QElapsedTimer &timer) {
  return static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
}

double percentile(std::vector<double> values, const double fraction) {
  std::sort(values.begin(), values.end());
  const std::size_t index = std::min(
      values.size() - 1,
      static_cast<std::size_t>(fraction * static_cast<double>(values.size())));
  return values[index];
}

QRgb documentPixel(const int x, const int y) {
  std::uint32_t value = static_cast<std::uint32_t>(x) * 0x85ebca6bu;
  value ^= static_cast<std::uint32_t>(y) * 0xc2b2ae35u;
  value ^= value >> 16;
  value *= 0x7feb352du;
  value ^= value >> 15;
  return qRgba(value & 255, (value >> 8) & 255, (value >> 16) & 255, 255);
}

QImage budgetFrame(const int width, const int height, const int delta,
                   const int index) {
  QImage frame(width, height, QImage::Format_RGBA8888);
  const int origin = index * delta;
  for (int y = 0; y < height; ++y) {
    auto *row = reinterpret_cast<QRgb *>(frame.scanLine(y));
    const int documentY = origin + y;
    for (int x = 0; x < width; ++x)
      row[x] = documentPixel(x, documentY);
  }
  return frame;
}

bool exactDocumentParity(const QImage &image) {
  for (int y = 0; y < image.height(); ++y) {
    const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
    for (int x = 0; x < image.width(); ++x) {
      if (row[x] != documentPixel(x, y))
        return false;
    }
  }
  return true;
}

long long residentBytes() {
  QFile file(QStringLiteral("/proc/self/statm"));
  if (!file.open(QIODevice::ReadOnly))
    return -1;
  const QList<QByteArray> fields = file.readAll().simplified().split(' ');
  if (fields.size() < 2)
    return -1;
  bool ok = false;
  const long long residentPages = fields.at(1).toLongLong(&ok);
  const long pageSize = sysconf(_SC_PAGESIZE);
  return ok && pageSize > 0 ? residentPages * pageSize : -1;
}

long long currentCgroupPeakBytes() {
  std::ifstream cgroup("/proc/self/cgroup");
  if (!cgroup)
    return -1;
  std::string line;
  while (std::getline(cgroup, line)) {
    if (!line.starts_with("0::"))
      continue;
    std::ifstream peak("/sys/fs/cgroup" + line.substr(3) + "/memory.peak");
    if (!peak)
      return -1;
    long long value = -1;
    peak >> value;
    return value;
  }
  return -1;
}

bool runTinyDeltaFixture() {
  constexpr int width = 512;
  constexpr int height = 512;
  constexpr int delta = 16;
  constexpr int frames = 64;
  QString error;
  bool ok = false;
  QImage first = budgetFrame(width, height, delta, 0);
  stitch::StitchAccumulator accumulator(first, stitch::Axis::Vertical, ok,
                                        error);
  first = {};
  if (!ok)
    return false;
  for (int index = 1; index < frames; ++index) {
    QImage frame = budgetFrame(width, height, delta, index);
    if (!accumulator.pushForward(frame, delta, error))
      return false;
  }
  const QImage assembled = accumulator.finish(error);
  return assembled.size() == QSize(width, height + (frames - 1) * delta) &&
         exactDocumentParity(assembled);
}

int runMaximumBudgetFixture() {
  constexpr int delta = 3968;
  constexpr long long externalBytes = 2560LL * 1440 * 4;
  QString error;
  bool ok = false;
  QImage first = budgetFrame(4096, 4096, delta, 0);
  stitch::StitchAccumulator accumulator(first, stitch::Axis::Vertical, ok,
                                        error, externalBytes);
  first = {};
  if (!ok)
    return 20;
  int accepted = 0;
  for (int index = 1; index < 20; ++index) {
    QImage frame = budgetFrame(4096, 4096, delta, index);
    if (!accumulator.pushForward(frame, delta, error))
      break;
    ++accepted;
  }
  if (accepted == 0 || !error.contains(QStringLiteral("maximum length")))
    return 21;
  const stitch::StitchAccumulator::MemoryUsage usage =
      accumulator.memoryUsageForFinish(externalBytes);
  const long long remappedCaptureBytes =
      externalBytes +
      std::max(1LL, stitch::kMaxStitchWorkingBytes - usage.peakBytes + 1);
  const stitch::StitchAccumulator::MemoryUsage staleMappingUsage =
      accumulator.memoryUsageForFinish(remappedCaptureBytes);
  if (!stitch::exceedsStitchWorkingBudget(
          staleMappingUsage.retainedRgbaBytes +
              staleMappingUsage.externalBytes,
          staleMappingUsage.scoringBytes, staleMappingUsage.outputBytes,
          staleMappingUsage.conversionBytes))
    return 27;
  QElapsedTimer timer;
  timer.start();
  const QImage assembled = accumulator.finish(error, nullptr, externalBytes);
  const QSize expectedSize(4096, 4096 + accepted * delta);
  if (assembled.size() != expectedSize || !exactDocumentParity(assembled))
    return 22;
  if (!runTinyDeltaFixture())
    return 23;
  const long long cgroupPeak = currentCgroupPeakBytes();
  std::printf("maximum_fixture_status=ok accepted_bands=%d output=%dx%d "
              "retained_bytes=%lld output_bytes=%lld accounted_peak_bytes=%lld "
              "external_bytes=%lld cgroup_peak_bytes=%lld tiny_delta_parity=ok "
              "maximum_parity=ok remapped_shm_released_before_finish=ok "
              "finish_ms=%.3f\n",
              accepted, assembled.width(), assembled.height(),
              usage.retainedRgbaBytes, usage.outputBytes, usage.peakBytes,
              externalBytes, cgroupPeak, elapsedMs(timer));
  return 0;
}

int runLiveInjectionEpisode(const QStringList &arguments) {
  bool conversionsOk = false;
  const int parkX = arguments.at(3).toInt(&conversionsOk);
  if (!conversionsOk)
    return 30;
  const int parkY = arguments.at(4).toInt(&conversionsOk);
  if (!conversionsOk)
    return 30;
  const int cropX = arguments.at(5).toInt(&conversionsOk);
  if (!conversionsOk)
    return 30;
  const int cropY = arguments.at(6).toInt(&conversionsOk);
  if (!conversionsOk)
    return 30;
  const int cropWidth = arguments.at(7).toInt(&conversionsOk);
  if (!conversionsOk)
    return 30;
  const int cropHeight = arguments.at(8).toInt(&conversionsOk);
  if (!conversionsOk)
    return 30;
  const QString backend = arguments.at(9);
  ScrollInjectorValidationOptions validation;
  if (backend == QStringLiteral("virtual")) {
    validation.denyUinput = true;
  } else if (backend == QStringLiteral("no-pointer")) {
    validation.omitVirtualPointerProtocol = true;
  } else if (backend == QStringLiteral("manual-fallback")) {
    validation.denyUinput = true;
    validation.omitVirtualPointerProtocol = true;
  } else if (backend != QStringLiteral("uinput")) {
    return 30;
  }

  const QString outputName = arguments.at(2);
  if (backend == QStringLiteral("manual-fallback")) {
    auto control = std::make_shared<ScrollCaptureJobControl>();
    ScrollCaptureJobSpec spec{outputName,
                              QRect(cropX, cropY, cropWidth, cropHeight),
                              {},
                              ScrollCaptureJobMode::Auto,
                              stitch::Axis::Vertical,
                              parkX,
                              parkY,
                              {}};
    spec.injectorValidation = validation;
    ScrollCaptureJobResult result;
    std::jthread owner([&] { result = runScrollCaptureJob(spec, control); });
    QElapsedTimer fallback;
    fallback.start();
    ScrollCaptureJobSnapshot jobSnapshot;
    do {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      jobSnapshot = control->snapshot();
    } while (!jobSnapshot.status.contains(
                 QStringLiteral("Auto-scroll unavailable")) &&
             jobSnapshot.stage != ScrollCaptureJobStage::Completed &&
             fallback.elapsed() < 3000);
    const bool enteredManual =
        jobSnapshot.stage == ScrollCaptureJobStage::Capturing &&
        jobSnapshot.status.contains(QStringLiteral("scroll manually"));
    control->request(ScrollCaptureJobCommand::Cancel);
    owner.join();
    std::printf("live_backend=manual-fallback entered_manual=%s "
                "completion=%d status=%s\n",
                enteredManual ? "true" : "false",
                static_cast<int>(result.completion),
                jobSnapshot.status.toUtf8().constData());
    return enteredManual &&
                   result.completion == ScrollCaptureJobCompletion::Cancelled
               ? 0
               : 31;
  }
  auto injector = std::make_shared<ScrollInjectorSession>();
  std::jthread owner([injector, outputName, validation] {
    runScrollInjectorSession(injector, outputName, validation);
  });
  QElapsedTimer setup;
  setup.start();
  ScrollInjectorSession::Snapshot snapshot;
  do {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    snapshot = injector->snapshot();
  } while (snapshot.status == ScrollInjectorSession::Status::Preparing &&
           setup.elapsed() < 3000);
  if (snapshot.status != ScrollInjectorSession::Status::Ready) {
    injector->stop();
    owner.join();
    std::printf("live_backend=%s status=%d error=%s\n",
                backend.toUtf8().constData(), static_cast<int>(snapshot.status),
                snapshot.error.toUtf8().constData());
    return 31;
  }

  QString error;
  OutputCapture output;
  if (!output.open(outputName, error)) {
    injector->stop();
    owner.join();
    std::fprintf(stderr, "live output capture failed: %s\n",
                 error.toUtf8().constData());
    return 32;
  }
  auto handshake = std::make_shared<stitch::CaptureHandshake>();
  auto episodeStop = std::make_shared<std::atomic<bool>>(false);
  injector->startEpisode(handshake, episodeStop, parkX, parkY,
                         stitch::Axis::Vertical);
  QElapsedTimer episode;
  episode.start();
  while (handshake->readyCycle() == 0 && episode.elapsed() < 3000)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const std::uint64_t cycle = handshake->readyCycle();
  QImage before;
  if (cycle == 0 || !output.grab(before, error, 2000)) {
    injector->stop();
    owner.join();
    std::fprintf(stderr, "live pre-scroll capture failed: %s\n",
                 error.toUtf8().constData());
    return 33;
  }
  handshake->acknowledgeWithNotches(cycle, 3);
  while (handshake->readyCycle() <= cycle && episode.elapsed() < 5000)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(180));
  QImage after;
  const bool grabbedAfter = output.grab(after, error, 2000);
  episodeStop->store(true, std::memory_order_release);
  injector->stop();
  owner.join();
  output.close();
  if (!grabbedAfter) {
    std::fprintf(stderr, "live post-scroll capture failed: %s\n",
                 error.toUtf8().constData());
    return 34;
  }
  const QRect crop = QRect(cropX, cropY, cropWidth, cropHeight)
                         .intersected(QRect(QPoint(), before.size()))
                         .intersected(QRect(QPoint(), after.size()));
  if (crop.width() < 32 || crop.height() < 32)
    return 35;
  const stitch::MotionEstimate motion = stitch::classifyMotion(
      stitch::downsampleToGray(before.copy(crop), stitch::Axis::Vertical),
      stitch::downsampleToGray(after.copy(crop), stitch::Axis::Vertical),
      stitch::Axis::Vertical);
  const char *motionName = "unmatchable";
  switch (motion.motion.kind) {
  case stitch::MotionKind::Stationary:
    motionName = "stationary";
    break;
  case stitch::MotionKind::Forward:
    motionName = "forward";
    break;
  case stitch::MotionKind::Reverse:
    motionName = "reverse";
    break;
  case stitch::MotionKind::Ambiguous:
    motionName = "ambiguous";
    break;
  case stitch::MotionKind::Unmatchable:
    break;
  }
  std::printf("live_backend=%s status=ready motion=%s delta=%d error=%.3f "
              "confidence=%.3f\n",
              backend.toUtf8().constData(), motionName, motion.motion.delta,
              motion.error, motion.confidence);
  return motion.motion.kind == stitch::MotionKind::Forward ? 0 : 36;
}
} // namespace

int main(int argc, char **argv) {
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  const QStringList arguments = app.arguments();
  if (arguments.size() == 2 &&
      arguments.at(1) == QStringLiteral("--baseline-rss-bytes")) {
    const long long rss = residentBytes();
    if (rss < 0)
      return 24;
    std::printf("%lld\n", rss);
    return 0;
  }
  if (arguments.size() == 2 &&
      arguments.at(1) == QStringLiteral("--maximum-budget-fixture"))
    return runMaximumBudgetFixture();
  if (arguments.size() == 3 &&
      arguments.at(1) == QStringLiteral("--live-inject-setup")) {
    auto injector = std::make_shared<ScrollInjectorSession>();
    QElapsedTimer setup;
    setup.start();
    std::jthread owner([injector, output = arguments.at(2)] {
      runScrollInjectorSession(injector, output);
    });
    ScrollInjectorSession::Snapshot snapshot;
    do {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      snapshot = injector->snapshot();
    } while (snapshot.status == ScrollInjectorSession::Status::Preparing &&
             setup.elapsed() < 3000);
    const double setupMs = elapsedMs(setup);
    injector->stop();
    owner.join();
    std::printf("inject_setup_ms=%.3f status=%d error=%s\n", setupMs,
                static_cast<int>(snapshot.status),
                snapshot.error.toUtf8().constData());
    return snapshot.status == ScrollInjectorSession::Status::Ready ? 0 : 5;
  }
  if (arguments.size() == 10 &&
      arguments.at(1) == QStringLiteral("--live-inject-episode"))
    return runLiveInjectionEpisode(arguments);
  auto sideEffects = std::make_shared<std::atomic<int>>(0);
  QImage delayedResult(64, 64, QImage::Format_RGBA8888);
  delayedResult.fill(Qt::blue);

  std::vector<double> heartbeatGaps;
  QElapsedTimer heartbeatClock;
  QTimer heartbeat;
  heartbeat.setInterval(1);
  QObject::connect(&heartbeat, &QTimer::timeout, [&] {
    heartbeatGaps.push_back(elapsedMs(heartbeatClock));
    heartbeatClock.restart();
  });

  ScrollCapturePanel panel(MonitorInfo{}, nullptr, nullptr);
  panel.resize(800, 600);
  panel.show();
  panel.startDelayedCaptureForTest(250, 250, delayedResult, sideEffects);
  QEventLoop readyLoop;
  QTimer readyPoll;
  readyPoll.setInterval(1);
  QObject::connect(&readyPoll, &QTimer::timeout, &readyLoop, [&] {
    if (panel.capturingForTest())
      readyLoop.quit();
  });
  readyPoll.start();
  readyLoop.exec();
  QApplication::processEvents(); // The real panel has painted before a click.
  heartbeatClock.start();
  heartbeat.start();
  const QPoint center = panel.doneButtonCenterForTest();
  QMouseEvent press(QEvent::MouseButtonPress, QPointF(center), QPointF(center),
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QElapsedTimer handler;
  handler.start();
  QApplication::sendEvent(&panel, &press);
  const double handlerMs = elapsedMs(handler);
  QEventLoop finishedLoop;
  QTimer finishedPoll;
  finishedPoll.setInterval(1);
  QObject::connect(&finishedPoll, &QTimer::timeout, &finishedLoop, [&] {
    if (!panel.workerRunningForTest())
      finishedLoop.quit();
  });
  finishedPoll.start();
  finishedLoop.exec();
  heartbeat.stop();

  ScrollCapturePanel cancelPanel(MonitorInfo{}, nullptr, nullptr);
  cancelPanel.resize(800, 600);
  cancelPanel.show();
  auto cancelEffects = std::make_shared<std::atomic<int>>(0);
  cancelPanel.startDelayedCaptureForTest(0, 0, delayedResult, cancelEffects,
                                         250);
  QEventLoop cancelReadyLoop;
  QTimer cancelReadyPoll;
  cancelReadyPoll.setInterval(1);
  QObject::connect(&cancelReadyPoll, &QTimer::timeout, &cancelReadyLoop, [&] {
    if (cancelPanel.capturingForTest())
      cancelReadyLoop.quit();
  });
  cancelReadyPoll.start();
  cancelReadyLoop.exec();
  QApplication::processEvents();
  std::vector<double> cancelHeartbeatGaps;
  QElapsedTimer cancelHeartbeatClock;
  QTimer cancelHeartbeat;
  cancelHeartbeat.setInterval(1);
  QObject::connect(&cancelHeartbeat, &QTimer::timeout, [&] {
    cancelHeartbeatGaps.push_back(elapsedMs(cancelHeartbeatClock));
    cancelHeartbeatClock.restart();
  });
  cancelHeartbeatClock.start();
  cancelHeartbeat.start();
  const QPoint cancelCenter = cancelPanel.cancelButtonCenterForTest();
  QMouseEvent cancelPress(QEvent::MouseButtonPress, QPointF(cancelCenter),
                          QPointF(cancelCenter), Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
  QElapsedTimer cancelHandler;
  cancelHandler.start();
  QApplication::sendEvent(&cancelPanel, &cancelPress);
  const double cancelHandlerMs = elapsedMs(cancelHandler);
  QEventLoop cancelFinishedLoop;
  QTimer cancelFinishedPoll;
  cancelFinishedPoll.setInterval(1);
  QObject::connect(&cancelFinishedPoll, &QTimer::timeout, &cancelFinishedLoop,
                   [&] {
                     if (!cancelPanel.workerRunningForTest())
                       cancelFinishedLoop.quit();
                   });
  cancelFinishedPoll.start();
  cancelFinishedLoop.exec();
  cancelHeartbeat.stop();

  std::printf("panel_done_handler_ms=%.3f panel_cancel_handler_ms=%.3f\n",
              handlerMs, cancelHandlerMs);
  if (handlerMs >= 2.0 || cancelHandlerMs >= 2.0)
    return 25;
  std::printf("heartbeat_p95_ms=%.3f heartbeat_max_ms=%.3f\n",
              percentile(heartbeatGaps, 0.95),
              *std::max_element(heartbeatGaps.begin(), heartbeatGaps.end()));
  if (cancelHeartbeatGaps.size() < 100 ||
      percentile(cancelHeartbeatGaps, 0.95) >= 16.0)
    return 26;
  std::printf("cancel_drain_heartbeat_p95_ms=%.3f "
              "cancel_drain_heartbeat_max_ms=%.3f\n",
              percentile(cancelHeartbeatGaps, 0.95),
              *std::max_element(cancelHeartbeatGaps.begin(),
                                cancelHeartbeatGaps.end()));

  constexpr int width = 2304;
  constexpr int frameHeight = 1296;
  constexpr int delta = 480;
  constexpr int frameCount = 10;
  QImage document(width, frameHeight + (frameCount - 1) * delta,
                  QImage::Format_RGBA8888);
  for (int y = 0; y < document.height(); ++y) {
    auto *row = reinterpret_cast<QRgb *>(document.scanLine(y));
    for (int x = 0; x < document.width(); ++x)
      row[x] = qRgba((x + y) & 255, (2 * x + y) & 255, (x + 3 * y) & 255, 255);
  }
  QVector<QImage> frames;
  for (int index = 0; index < frameCount; ++index)
    frames.push_back(document.copy(0, index * delta, width, frameHeight));

  std::vector<double> finishTimes;
  stitch::StitchAccumulator::MemoryUsage usage;
  long retainedAfter = -1;
  for (int iteration = 0; iteration < 30; ++iteration) {
    QString error;
    bool ok = false;
    stitch::StitchAccumulator accumulator(frames.first(),
                                          stitch::Axis::Vertical, ok, error);
    if (!ok)
      return 2;
    for (int index = 1; index < frames.size(); ++index)
      if (!accumulator.pushForward(frames[index], delta, error))
        return 3;
    usage = accumulator.memoryUsageForFinish();
    QElapsedTimer finishTimer;
    finishTimer.start();
    const QImage assembled = accumulator.finish(error);
    finishTimes.push_back(elapsedMs(finishTimer));
    if (assembled.isNull())
      return 4;
    retainedAfter = accumulator.retainedRgbaBytes();
  }
  std::printf("finish_p50_ms=%.3f finish_p95_ms=%.3f finish_max_ms=%.3f\n",
              percentile(finishTimes, 0.50), percentile(finishTimes, 0.95),
              *std::max_element(finishTimes.begin(), finishTimes.end()));
  std::printf(
      "retained_before_bytes=%lld retained_after_bytes=%ld output_bytes=%lld "
      "conversion_bytes=%lld accounted_peak_bytes=%lld\n",
      usage.retainedRgbaBytes, retainedAfter, usage.outputBytes,
      usage.conversionBytes, usage.peakBytes);
  return 0;
}

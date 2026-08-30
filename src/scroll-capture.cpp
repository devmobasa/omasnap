/** @fileoverview Manual scroll capture overlay (see scroll-capture.hpp). */
#include "scroll-capture.hpp"

#include <LayerShellQt/Window>

#include <QtConcurrent/QtConcurrentRun>

#include <QApplication>
#include <QCursor>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QScreen>
#include <QTimer>
#include <QWindow>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kMinRegion = 32; // logical px
/// How far outside the region its grips and its draggable border reach. The
/// region itself has to stay untouched: it is what the capture crops.
constexpr int kGripBand = 16;
/// How long to let the compositor show a frame with the chrome hidden before
/// grabbing the first one. Two frames at 60 Hz, with room to spare.
constexpr int kChromeSettleMs = 60;
const QColor kDim(0, 0, 0, 150);
const QColor kAccent(10, 132, 255);
const QColor kWarn(255, 159, 10);

struct ModeButton {
  const char *label;
  bool automatic;
  stitch::Axis axis;
};
/// The Selected-phase mode choices (painted, hit-tested like the pills).
constexpr ModeButton kModeButtons[] = {
    {"Manual \u2193", false, stitch::Axis::Vertical},
    {"Auto \u2193", true, stitch::Axis::Vertical},
    {"Manual \u2192", false, stitch::Axis::Horizontal},
    {"Auto \u2192", true, stitch::Axis::Horizontal},
};
constexpr int kModeButtonCount = 4;
constexpr int kModeButtonWidth = 118;
constexpr int kModeButtonHeight = 40;
constexpr int kModeButtonGap = 10;

} // namespace

ScrollCapturePanel::ScrollCapturePanel(MonitorInfo monitor,
                                       LayerShellQt::Window *layer,
                                       QWidget *parent)
    : QWidget(parent), monitor_(std::move(monitor)), layer_(layer) {
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
  setAttribute(Qt::WA_TranslucentBackground);
  if (parent)
    setGeometry(parent->rect());
  workerPollTimer_.setInterval(8);
  connect(&workerPollTimer_, &QTimer::timeout, this,
          &ScrollCapturePanel::pollWorker);
  connect(&workerWatcher_, &QFutureWatcher<ScrollCaptureJobResult>::finished,
          this, &ScrollCapturePanel::workerFinished);
}

ScrollCapturePanel::~ScrollCapturePanel() { release(); }

void ScrollCapturePanel::release() {
  if (released_)
    return;
  released_ = true;
  requestWorker(ScrollCaptureJobCommand::Cancel);
  // Hand the surface back whole: no hole, keyboard exclusive again.
  if (QWindow *window = surfaceWindow())
    window->setMask(QRegion());
  if (layer_)
    layer_->setKeyboardInteractivity(
        LayerShellQt::Window::KeyboardInteractivityExclusive);
  hide();
}

QWindow *ScrollCapturePanel::surfaceWindow() const {
  const QWidget *top = window();
  return top ? top->windowHandle() : nullptr;
}

QRect ScrollCapturePanel::regionPhysical() const {
  // Round the edges, not the extents: rounding x and width independently can
  // shift the crop against the visible hole by a pixel at fractional scales,
  // stitching a stationary overlay-edge column into every band.
  const qreal scale = monitor_.scale > 0 ? monitor_.scale : 1.0;
  const int left = qRound(region_.x() * scale);
  const int top = qRound(region_.y() * scale);
  const int right = qRound((region_.x() + region_.width()) * scale);
  const int bottom = qRound((region_.y() + region_.height()) * scale);
  return QRect(left, top, std::max(1, right - left), std::max(1, bottom - top));
}

void ScrollCapturePanel::setStatus(const QString &status, bool warning) {
  status_ = status;
  statusWarning_ = warning;
  update();
}


QVector<QRect> ScrollCapturePanel::chromeRects() const {
  QVector<QRect> rects;
  for (const CaptureTab &tab : captureTabLayout(rect()))
    rects.push_back(tab.rect.toAlignedRect());
  if (phase_ == Phase::Selected || phase_ == Phase::Preparing) {
    for (int index = 0; index < kModeButtonCount; ++index)
      rects.push_back(modeButtonRect(index));
    rects.push_back(selectedCancelButtonRect());
    for (const auto &[grip, which] : gripRects())
      rects.push_back(grip);
    return rects;
  }
  rects.push_back(doneButtonRect());
  rects.push_back(backButtonRect());
  rects.push_back(cancelButtonRect());
  if (autoStalled_)
    rects.push_back(continueButtonRect());
  return rects;
}

void ScrollCapturePanel::applyInputRegion() {
  QWindow *window = surfaceWindow();
  if (!window)
    return;
  // The region is exposed from the moment it is chosen, so the page can be
  // scrolled into place without losing it.
  if (phase_ != Phase::Capturing && phase_ != Phase::Selected &&
      phase_ != Phase::Preparing) {
    window->setMask(QRegion()); // whole surface takes input
    return;
  }
  window->setMask(scrollOverlayInputRegion(rect(), region_, chromeRects()));
}

void ScrollCapturePanel::updateKeyboardZone(const QPoint &point) {
  // The grab pins pointer focus to this layer, which is what stops the wheel
  // reaching the page, so it is held only while the pointer is on our own
  // chrome, and only once a real event has said so. Assuming the pointer was on
  // the chrome is what broke scrolling before: a Wayland client cannot ask
  // where the pointer is, so the answer here always comes from an event.
  setKeyboardGrab(!region_.contains(point));
}

void ScrollCapturePanel::setKeyboardGrab(bool grab) {
  // Hyprland pins pointer focus to a layer that holds an exclusive keyboard
  // grab, even over an input-region hole, so while the grab is held the wheel
  // never reaches the page. Once a region exists the page has to be
  // scrollable, so the grab goes away for that whole phase instead of being
  // handed back and forth as the pointer crosses the region's edge: that
  // handoff depended on knowing where the pointer was, and a Wayland client
  // cannot ask. Restoring a region put the pointer somewhere we had never
  // seen it move, and the grab stayed on. The pills are the controls from
  // then on.
  if (!layer_ || keyboardGrabbed_ == grab)
    return;
  keyboardGrabbed_ = grab;
  layer_->setKeyboardInteractivity(
      grab ? LayerShellQt::Window::KeyboardInteractivityExclusive
           : LayerShellQt::Window::KeyboardInteractivityNone);
}

// ---- capture -----------------------------------------------------------------

void ScrollCapturePanel::startDelayedCaptureForTest(
    const int setupDelayMs, const int finishDelayMs, const QImage &result,
    const std::shared_ptr<std::atomic<int>> &sideEffects,
    const int cancelDrainDelayMs) {
  phase_ = Phase::Preparing;
  workerControl_ = std::make_shared<ScrollCaptureJobControl>();
  workerRevision_ = 0;
  workerPollTimer_.start();
  workerWatcher_.setFuture(QtConcurrent::run(
      [control = workerControl_, setupDelayMs, finishDelayMs, result,
       sideEffects, cancelDrainDelayMs] {
        return runDelayedScrollCaptureJobForTest(
            control, setupDelayMs, finishDelayMs, result, sideEffects,
            cancelDrainDelayMs);
      }));
}

void ScrollCapturePanel::startCaptureJobForTest(
    const ScrollCaptureJobSpec &spec) {
  phase_ = Phase::Preparing;
  setStatus(QStringLiteral("Preparing scroll capture…"));
  launchCaptureJob(spec);
}

void ScrollCapturePanel::launchCaptureJob(const ScrollCaptureJobSpec &spec) {
  workerControl_ = std::make_shared<ScrollCaptureJobControl>();
  workerRevision_ = 0;
  workerPollTimer_.start();
  workerWatcher_.setFuture(
      QtConcurrent::run([spec, control = workerControl_] {
        return runScrollCaptureJob(spec, control);
      }));
}

void ScrollCapturePanel::startCapture(Mode mode, stitch::Axis axis) {
  if (region_.width() < kMinRegion || region_.height() < kMinRegion)
    return;
  if (workerWatcher_.isRunning())
    return;
  mode_ = mode;
  axis_ = axis;
  autoStalled_ = false;
  pendingMode_.reset();
  phase_ = Phase::Preparing;
  applyInputRegion();
  // The move puck lives inside the region, so the frame on screen right now
  // still has it. Paint the phase change first and let it be shown: the
  // capture reads the screen back, so it must not start until the screen no
  // longer has any of our chrome on the part being captured.
  repaint();
  // Drop the exclusive keyboard grab for the whole capture. Hyprland pins
  // pointer focus to a layer that holds an exclusive grab (even over an
  // input-region hole), which stops the wheel reaching the page; with the grab
  // released the pointer is never pinned, so scrolling the exposed page always
  // works. Finish/cancel run off the on-screen buttons (mouse clicks, governed
  // by the input region, not the keyboard).
  setKeyboardGrab(false);
  setStatus(QStringLiteral("Preparing scroll capture…"));
  const QRect physical = regionPhysical();
  const auto [parkX, parkY] = autoScrollParkPoint();
  const ScrollCaptureJobSpec spec{
      monitor_.name,
      physical,
      qEnvironmentVariable("OMASNAP_SCROLL_DEBUG_DIR"),
      mode == Mode::Auto ? ScrollCaptureJobMode::Auto
                         : ScrollCaptureJobMode::Manual,
      axis,
      parkX,
      parkY,
      {},
  };
  // Wait for the chrome-free frame without blocking the event handler. The
  // job then opens, uses, and closes its Wayland session on one worker thread.
  QTimer::singleShot(kChromeSettleMs, this, [this, spec] {
    if (phase_ != Phase::Preparing || released_)
      return;
    launchCaptureJob(spec);
  });
}

void ScrollCapturePanel::requestWorker(const ScrollCaptureJobCommand command) {
  if (workerControl_)
    workerControl_->request(command);
}

void ScrollCapturePanel::pollWorker() {
  if (!workerControl_)
    return;
  const ScrollCaptureJobSnapshot snapshot = workerControl_->snapshot();
  if (snapshot.revision == workerRevision_)
    return;
  workerRevision_ = snapshot.revision;
  if (phase_ == Phase::Preparing &&
      (snapshot.stage == ScrollCaptureJobStage::Capturing ||
       snapshot.stage == ScrollCaptureJobStage::Stalled))
    phase_ = Phase::Capturing;
  const bool stalledChanged = autoStalled_ != snapshot.autoStalled;
  autoStalled_ = snapshot.autoStalled;
  if (stalledChanged)
    applyInputRegion();
  if (!snapshot.status.isEmpty() &&
      (phase_ == Phase::Preparing || phase_ == Phase::Capturing ||
       (phase_ == Phase::Finishing &&
        snapshot.status == QStringLiteral("Stitching…"))))
    setStatus(snapshot.status, snapshot.warning);
  update();
}

void ScrollCapturePanel::workerFinished() {
  workerPollTimer_.stop();
  pollWorker();
  ScrollCaptureJobResult result;
  try {
    result = workerWatcher_.result();
  } catch (...) {
    result.completion = ScrollCaptureJobCompletion::Failed;
    result.error = QStringLiteral("Scroll capture worker failed unexpectedly");
  }
  workerControl_.reset();
  if (released_ || phase_ == Phase::Finished)
    return;
  switch (result.completion) {
  case ScrollCaptureJobCompletion::Done:
    if (result.unverifiedSeams > 0)
      qWarning().noquote()
          << QStringLiteral("scroll: capture may contain %1 repeated or missing "
                            "section(s)")
                 .arg(result.unverifiedSeams);
    qInfo().noquote() << QStringLiteral("scroll: stitched %1 frames into %2x%3")
                             .arg(result.keptFrames)
                             .arg(result.image.width())
                             .arg(result.image.height());
    phase_ = Phase::Finished;
    emit stitched(result.image);
    return;
  case ScrollCaptureJobCompletion::Back:
    phase_ = Phase::Selected;
    enterSelected();
    if (pendingMode_) {
      const Mode mode = *pendingMode_;
      pendingMode_.reset();
      startCapture(mode, axis_);
    }
    return;
  case ScrollCaptureJobCompletion::SetupFailed:
    phase_ = Phase::Selected;
    applyInputRegion();
    setStatus(QStringLiteral("Output capture failed: %1").arg(result.error),
              true);
    update();
    return;
  case ScrollCaptureJobCompletion::Failed:
    phase_ = Phase::Selected;
    autoStalled_ = false;
    applyInputRegion();
    setStatus(result.error.isEmpty()
                  ? QStringLiteral("Scroll capture failed")
                  : result.error,
              true);
    update();
    return;
  case ScrollCaptureJobCompletion::NoFrames:
  case ScrollCaptureJobCompletion::Cancelled:
    phase_ = Phase::Finished;
    emit dismissed();
    return;
  }
}

void ScrollCapturePanel::finishCapture() {
  if (phase_ != Phase::Capturing)
    return;
  phase_ = Phase::Finishing;
  setStatus(QStringLiteral("Stitching…"));
  requestWorker(ScrollCaptureJobCommand::Done);
}

void ScrollCapturePanel::switchMode(Mode mode) {
  // Wrong mode is the same mistake as the wrong direction: keep the region,
  // throw the frames away, start again the other way.
  if (phase_ == Phase::Selected) {
    startCapture(mode, axis_);
    return;
  }
  if ((phase_ != Phase::Preparing && phase_ != Phase::Capturing) ||
      mode == mode_)
    return;
  pendingMode_ = mode;
  phase_ = Phase::Finishing;
  setStatus(QStringLiteral("Changing scroll mode…"));
  requestWorker(ScrollCaptureJobCommand::Back);
}

std::pair<int, int> ScrollCapturePanel::autoScrollParkPoint() const {
  // Bottom-right corner, inset 10% of the region so the point stays inside
  // it: a corner is more often page margin than a centre, which is where
  // hero content (a video, an embed) tends to sit and swallow the wheel
  // once the page has scrolled it under the parked point.
  const qreal scale = monitor_.scale > 0 ? monitor_.scale : 1.0;
  const qreal insetX = region_.width() * 0.10;
  const qreal insetY = region_.height() * 0.10;
  const int x = qRound((region_.x() + region_.width() - insetX) * scale);
  const int y = qRound((region_.y() + region_.height() - insetY) * scale);
  return {x, y};
}

void ScrollCapturePanel::continueCapture() {
  // Picks the same capture back up: the session keeps every band it already
  // has, so this carries on from the last one rather than starting a second
  // capture of the same page. The fresh injector parks the pointer back
  // inside the frame.
  if (phase_ != Phase::Capturing || !workerControl_ || mode_ != Mode::Auto ||
      !autoStalled_)
    return;
  autoStalled_ = false;
  // Pressing Continue means the pointer was on our chrome, which is where we
  // hold the keyboard, and Hyprland pins pointer focus to a layer that holds
  // it, so the injected wheel would land on us instead of the page. Let it go
  // before parking the pointer back inside the frame.
  setKeyboardGrab(false);
  setStatus(QStringLiteral("Preparing auto-scroll…"));
  update();
  requestWorker(ScrollCaptureJobCommand::Continue);
}

void ScrollCapturePanel::returnToModeChoice() {
  // Throw the frames away and go back to the mode row with the region intact.
  if (phase_ != Phase::Preparing && phase_ != Phase::Capturing)
    return;
  phase_ = Phase::Finishing;
  setStatus(QStringLiteral("Stopping scroll capture…"));
  requestWorker(ScrollCaptureJobCommand::Back);
}

void ScrollCapturePanel::cancel() {
  requestWorker(ScrollCaptureJobCommand::Cancel);
  phase_ = Phase::Finished;
  hide();
  emit dismissed();
}

// ---- chrome ------------------------------------------------------------------

int ScrollCapturePanel::capturePillCount() const {
  return autoStalled_ ? 4 : 3;
}

QRect ScrollCapturePanel::doneButtonRect() const {
  return scrollOverlayPillRect(rect(), region_, capturePillCount(), 0, 132, 40,
                               12);
}

QRect ScrollCapturePanel::continueButtonRect() const {
  if (!autoStalled_)
    return {};
  return scrollOverlayPillRect(rect(), region_, capturePillCount(), 1, 132, 40,
                               12);
}
QVector<QPair<QString, QString>> ScrollCapturePanel::legendEntries() const {
  // Once a region exists the keyboard belongs to the page, which is what makes
  // it scrollable, so nothing here may promise a key. The buttons on screen
  // are the controls, and they say so themselves.
  return {};
}

QVector<QPair<QRect, ScrollCapturePanel::Grip>>
ScrollCapturePanel::gripRects() const {
  // Every grip lives in the band *outside* the region, never over it: the
  // capture is that rectangle of the screen, so a bracket drawn inside it is a
  // bracket stitched into the result. Each corner is two arms, which is what
  // makes the bracket shape and keeps every rect clear of the region.
  if (phase_ != Phase::Selected || region_.isEmpty())
    return {};
  const int arm = 34;
  const int barLong = 62;
  const QPoint center = region_.center();
  const int outerTop = region_.top() - kGripBand;
  const int outerLeft = region_.left() - kGripBand;
  const int belowBottom = region_.bottom() + 1;
  const int pastRight = region_.right() + 1;
  const auto across = [&](int x, int y) { return QRect(x, y, arm, kGripBand); };
  const auto down = [&](int x, int y) { return QRect(x, y, kGripBand, arm); };
  return {
      {across(outerLeft, outerTop), Grip::TopLeft},
      {down(outerLeft, outerTop), Grip::TopLeft},
      {across(region_.right() - arm + kGripBand + 1, outerTop),
       Grip::TopRight},
      {down(pastRight, outerTop), Grip::TopRight},
      {across(region_.right() - arm + kGripBand + 1, belowBottom),
       Grip::BottomRight},
      {down(pastRight, region_.bottom() - arm + kGripBand + 1),
       Grip::BottomRight},
      {across(outerLeft, belowBottom), Grip::BottomLeft},
      {down(outerLeft, region_.bottom() - arm + kGripBand + 1),
       Grip::BottomLeft},
      {QRect(center.x() - barLong / 2, outerTop, barLong, kGripBand),
       Grip::Top},
      {QRect(center.x() - barLong / 2, belowBottom, barLong, kGripBand),
       Grip::Bottom},
      {QRect(outerLeft, center.y() - barLong / 2, kGripBand, barLong),
       Grip::Left},
      {QRect(pastRight, center.y() - barLong / 2, kGripBand, barLong),
       Grip::Right},
      // The puck is the one grip that has to sit inside the region, because the
      // middle is what it moves. It is drawn only while a mode is being chosen,
      // and a capture does not start until a frame without it has been painted
      // and shown, see startCapture.
      {QRect(center.x() - 22, center.y() - 22, 44, 44), Grip::Move},
  };
}

Qt::CursorShape ScrollCapturePanel::gripCursor(Grip grip) {
  switch (grip) {
  case Grip::TopLeft:
  case Grip::BottomRight:
    return Qt::SizeFDiagCursor;
  case Grip::TopRight:
  case Grip::BottomLeft:
    return Qt::SizeBDiagCursor;
  case Grip::Top:
  case Grip::Bottom:
    return Qt::SizeVerCursor;
  case Grip::Left:
  case Grip::Right:
    return Qt::SizeHorCursor;
  case Grip::Move:
    return Qt::SizeAllCursor;
  case Grip::None:
    break;
  }
  return Qt::ArrowCursor;
}

ScrollCapturePanel::Grip
ScrollCapturePanel::gripAt(const QPoint &point) const {
  if (phase_ != Phase::Selected || region_.isEmpty())
    return Grip::None;
  // Corners are listed first and sides overlap them at the ends, so the first
  // match wins: a corner is never stolen by the bar beside it.
  for (const auto &[rect, grip] : gripRects()) {
    if (rect.contains(point))
      return grip;
  }
  // The border moves the region as well: the puck is the obvious way, the
  // border is the one already under your hand after a resize.
  const QRect band =
      region_.adjusted(-kGripBand, -kGripBand, kGripBand, kGripBand);
  if (band.contains(point) && !region_.contains(point))
    return Grip::Move;
  return Grip::None;
}

void ScrollCapturePanel::applyGrip(const QPoint &point) {
  const QRect surface = rect();
  const QPoint delta = point - gripStartPoint_;
  QRect updated = gripStartRegion_;
  if (activeGrip_ == Grip::Move) {
    updated.moveTo(gripStartRegion_.topLeft() + delta);
    // Clamped whole, so dragging past an edge slides along it instead of
    // shrinking the region.
    updated.moveLeft(std::clamp(updated.left(), surface.left(),
                                surface.right() - updated.width() + 1));
    updated.moveTop(std::clamp(updated.top(), surface.top(),
                               surface.bottom() - updated.height() + 1));
    region_ = updated;
    return;
  }
  const bool movesLeft = activeGrip_ == Grip::TopLeft ||
                         activeGrip_ == Grip::Left ||
                         activeGrip_ == Grip::BottomLeft;
  const bool movesRight = activeGrip_ == Grip::TopRight ||
                          activeGrip_ == Grip::Right ||
                          activeGrip_ == Grip::BottomRight;
  const bool movesTop = activeGrip_ == Grip::TopLeft ||
                        activeGrip_ == Grip::Top ||
                        activeGrip_ == Grip::TopRight;
  const bool movesBottom = activeGrip_ == Grip::BottomLeft ||
                           activeGrip_ == Grip::Bottom ||
                           activeGrip_ == Grip::BottomRight;
  // Clamped against the edge that stays, so a region dragged through itself
  // stops at the minimum rather than turning inside out.
  if (movesLeft)
    updated.setLeft(std::clamp(point.x(), surface.left(),
                               gripStartRegion_.right() - kMinRegion));
  if (movesRight)
    updated.setRight(std::clamp(point.x(),
                                gripStartRegion_.left() + kMinRegion,
                                surface.right()));
  if (movesTop)
    updated.setTop(std::clamp(point.y(), surface.top(),
                              gripStartRegion_.bottom() - kMinRegion));
  if (movesBottom)
    updated.setBottom(std::clamp(point.y(),
                                 gripStartRegion_.top() + kMinRegion,
                                 surface.bottom()));
  region_ = updated;
}

QRect ScrollCapturePanel::backButtonRect() const {
  return scrollOverlayPillRect(rect(), region_, capturePillCount(),
                               autoStalled_ ? 2 : 1, 132, 40, 12);
}
QRect ScrollCapturePanel::cancelButtonRect() const {
  return scrollOverlayPillRect(rect(), region_, capturePillCount(),
                               autoStalled_ ? 3 : 2, 132, 40, 12);
}
QRect ScrollCapturePanel::selectedCancelButtonRect() const {
  return modeButtonRect(kModeButtonCount);
}
QRect ScrollCapturePanel::modeButtonRect(int index) const {
  // The mode pills plus an explicit Cancel: Esc alone cannot be the way out,
  // because the keyboard belongs to the page whenever the pointer is over it.
  return scrollOverlayPillRect(rect(), region_, kModeButtonCount + 1, index,
                               kModeButtonWidth, kModeButtonHeight,
                               kModeButtonGap);
}
int ScrollCapturePanel::modeButtonAt(const QPoint &point) const {
  for (int index = 0; index < kModeButtonCount; ++index)
    if (modeButtonRect(index).contains(point))
      return index;
  return -1;
}

void ScrollCapturePanel::paintEvent(QPaintEvent *) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  {
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), kDim);
    painter.fillRect(region_, Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    // Drawn first, low-opacity, no card: the live page, the region outline,
    // the pills and the tab strip all paint over it wherever they overlap.
    drawHotkeyLegend(painter, rect(), legendEntries());
    painter.setPen(QPen(statusWarning_ ? kWarn : kAccent, 2));
    painter.setBrush(Qt::NoBrush);
    // Fully outside the region so no overlay pixel lands in the capture.
    painter.drawRect(region_.adjusted(-3, -3, 3, 3));

    // Grips live in the band outside the region: that rectangle is the
    // capture, so anything drawn inside it would be captured.
    if (phase_ == Phase::Selected || phase_ == Phase::Preparing) {
      const int thickness = 4;
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(255, 255, 255, 235));
      for (const auto &[grip, which] : gripRects()) {
        if (which == Grip::Move) {
          const QPointF middle = grip.center();
          painter.setBrush(QColor(20, 20, 26, 215));
          painter.setPen(QPen(QColor(255, 255, 255, 235), 2));
          painter.drawEllipse(middle, grip.width() / 2.0 - 2,
                              grip.height() / 2.0 - 2);
          painter.drawLine(middle + QPointF(-9, 0), middle + QPointF(9, 0));
          painter.drawLine(middle + QPointF(0, -9), middle + QPointF(0, 9));
          painter.setPen(Qt::NoPen);
          painter.setBrush(QColor(255, 255, 255, 235));
          continue;
        }
        if (grip.bottom() < region_.top())
          painter.drawRect(QRect(grip.left(), grip.bottom() - thickness + 1,
                                 grip.width(), thickness));
        else if (grip.top() > region_.bottom())
          painter.drawRect(
              QRect(grip.left(), grip.top(), grip.width(), thickness));
        else if (grip.right() < region_.left())
          painter.drawRect(QRect(grip.right() - thickness + 1, grip.top(),
                                 thickness, grip.height()));
        else
          painter.drawRect(
              QRect(grip.left(), grip.top(), thickness, grip.height()));
      }
    }
    QFont buttonFont = painter.font();
    buttonFont.setPixelSize(15);
    buttonFont.setBold(true);
    painter.setFont(buttonFont);
    if (phase_ == Phase::Selected || phase_ == Phase::Preparing) {
      for (int index = 0; index < kModeButtonCount; ++index) {
        const QRect button = modeButtonRect(index);
        painter.setPen(Qt::NoPen);
        painter.setBrush(phase_ == Phase::Preparing
                             ? QColor(40, 40, 48, 160)
                             : kModeButtons[index].automatic
                                   ? kAccent
                                   : QColor(40, 40, 48, 240));
        painter.drawRoundedRect(button, 8, 8);
        painter.setPen(Qt::white);
        painter.drawText(button, Qt::AlignCenter,
                         QString::fromUtf8(kModeButtons[index].label));
      }
      const QRect cancelSlot = selectedCancelButtonRect();
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(40, 40, 48, 240));
      painter.drawRoundedRect(cancelSlot, 8, 8);
      painter.setPen(Qt::white);
      painter.drawText(cancelSlot, Qt::AlignCenter, QStringLiteral("Cancel"));
    } else {
      const QRect done = doneButtonRect();
      const QRect backRect = backButtonRect();
      const QRect cancelRect = cancelButtonRect();
      const bool finishing = phase_ == Phase::Finishing;
      painter.setPen(Qt::NoPen);
      painter.setBrush(finishing ? QColor(90, 74, 70, 120) : kAccent);
      painter.drawRoundedRect(done, 8, 8);
      painter.setBrush(finishing ? QColor(40, 40, 48, 120)
                                 : QColor(40, 40, 48, 240));
      painter.drawRoundedRect(backRect, 8, 8);
      painter.setBrush(QColor(40, 40, 48, 240));
      painter.drawRoundedRect(cancelRect, 8, 8);
      painter.setPen(finishing ? QColor(170, 170, 175) : Qt::white);
      painter.drawText(done, Qt::AlignCenter, QStringLiteral("Done · stitch"));
      painter.drawText(backRect, Qt::AlignCenter, QStringLiteral("Back"));
      painter.setPen(Qt::white);
      painter.drawText(cancelRect, Qt::AlignCenter, QStringLiteral("Cancel"));
      if (autoStalled_) {
        const QRect resume = continueButtonRect();
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(40, 40, 48, 240));
        painter.drawRoundedRect(resume, 8, 8);
        painter.setPen(Qt::white);
        painter.drawText(resume, Qt::AlignCenter, QStringLiteral("Continue"));
      }
    }
  }
  // The same tab strip every overlay wears, with this kind lit. The other
  // tabs leave for the area overlay in that mode.
  drawCaptureTabs(painter, captureTabLayout(rect()), CaptureKind::Scroll,
                  cursor_);
  drawStatusPill(painter, rect(), status_);
}

void ScrollCapturePanel::wheelEvent(QWheelEvent *event) {
  // The hole passes the wheel to the page; the overlay only sees wheel over its
  // own chrome. Log it (debug runs), during capture a wheel event arriving with
  // the pointer inside the region would mean the input-region hole is not in
  // effect.
  if (!qEnvironmentVariableIsEmpty("OMASNAP_SCROLL_DEBUG_DIR"))
    qInfo().noquote() << QStringLiteral("scroll: overlay wheel at %1,%2 phase=%3")
                             .arg(event->position().x())
                             .arg(event->position().y())
                             .arg(static_cast<int>(phase_));
  QWidget::wheelEvent(event);
}

void ScrollCapturePanel::enterEvent(QEnterEvent *event) {
  updateKeyboardZone(event->position().toPoint());
  if (!qEnvironmentVariableIsEmpty("OMASNAP_SCROLL_DEBUG_DIR"))
    qInfo().noquote() << QStringLiteral("scroll: pointer entered overlay at %1,%2")
                             .arg(event->position().x())
                             .arg(event->position().y());
  QWidget::enterEvent(event);
}

void ScrollCapturePanel::leaveEvent(QEvent *event) {
  // The pointer is off our input region entirely, over the page, or off the
  // screen. Either way it is not on our chrome, so the keyboard goes back to
  // whatever is under it and the page can be scrolled again.
  setKeyboardGrab(false);
  if (!qEnvironmentVariableIsEmpty("OMASNAP_SCROLL_DEBUG_DIR"))
    qInfo().noquote() << QStringLiteral("scroll: pointer left overlay");
  QWidget::leaveEvent(event);
}

void ScrollCapturePanel::mousePressEvent(QMouseEvent *event) {
  if (event->button() == Qt::RightButton) {
    cancel();
    return;
  }
  if (event->button() != Qt::LeftButton)
    return;
  if (const int tab = captureTabAt(captureTabLayout(rect()), event->position());
      tab >= 0) {
    const CaptureKind kind = captureTabLayout(rect()).at(tab).kind;
    if (kind != CaptureKind::Scroll) {
      requestWorker(ScrollCaptureJobCommand::Cancel);
      phase_ = Phase::Finished;
      hide();
      emit tabRequested(kind);
    }
    return;
  }
  if (phase_ == Phase::Capturing) {
    const QPoint point = event->position().toPoint();
    if (doneButtonRect().contains(point))
      finishCapture();
    else if (autoStalled_ && continueButtonRect().contains(point))
      continueCapture();
    else if (backButtonRect().contains(point))
      returnToModeChoice();
    else if (cancelButtonRect().contains(point))
      cancel();
    return; // clicks elsewhere in the chrome do nothing
  }
  if (phase_ == Phase::Finishing) {
    if (cancelButtonRect().contains(event->position().toPoint()))
      cancel();
    return;
  }
  if (phase_ == Phase::Selected || phase_ == Phase::Preparing) {
    const QPoint point = event->position().toPoint();
    if (selectedCancelButtonRect().contains(point)) {
      cancel();
      return;
    }
    if (phase_ == Phase::Preparing)
      return;
    const int mode = modeButtonAt(point);
    if (mode >= 0) {
      startCapture(kModeButtons[mode].automatic ? Mode::Auto : Mode::Manual,
                   kModeButtons[mode].axis);
      update();
      return;
    }
    if (const Grip grip = gripAt(point); grip != Grip::None) {
      activeGrip_ = grip;
      gripStartRegion_ = region_;
      gripStartPoint_ = point;
      return;
    }
    // A press inside the region belongs to the page, the input region should
    // have sent it there. If one reaches the overlay anyway (a mask that has
    // not been committed yet, or the pixel or two beneath the outline), it must
    // not be read as "start over": losing the region to a click meant for the
    // page is the one thing this phase cannot do.
    if (region_.adjusted(-6, -6, 6, 6).contains(point)) {
      applyInputRegion();
      return;
    }
    // Anywhere else, that is, anywhere on the chrome, asks for a fresh
    // region: the editor's own selection takes it from here.
    cancel();
    return;
  }
}

void ScrollCapturePanel::mouseMoveEvent(QMouseEvent *event) {
  const QPoint point = event->position().toPoint();
  cursor_ = point;
  update(); // the tab strip's hover hint follows the pointer
  updateKeyboardZone(point);
  if (activeGrip_ != Grip::None) {
    // The button is down, so motion keeps arriving even over the hole.
    applyGrip(point);
    applyInputRegion(); // the hole follows the region as it is dragged
    update();
    return;
  }
  if (phase_ == Phase::Selected)
    setCursor(gripCursor(gripAt(point)));
}

void ScrollCapturePanel::mouseReleaseEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton && activeGrip_ != Grip::None) {
    activeGrip_ = Grip::None;
    applyInputRegion();
    update();
    return;
  }
}

void ScrollCapturePanel::reserveChromeStrip() {
  // Reserve a strip of real chrome: inside the hole the buttons' clicks fall
  // through and the chrome bakes into the capture, and a full-screen region
  // would empty the input mask entirely.
  constexpr int chromeStrip = 40 + 18 + 12;
  if (region_.top() < chromeStrip && region_.bottom() > height() - chromeStrip)
    region_.setBottom(height() - chromeStrip);
}

void ScrollCapturePanel::begin(const QRect &region) {
  const QRect clamped = region.intersected(rect());
  if (clamped.width() < kMinRegion || clamped.height() < kMinRegion) {
    emit dismissed();
    return;
  }
  region_ = clamped;
  reserveChromeStrip();
  enterSelected();
}

void ScrollCapturePanel::enterSelected() {
  phase_ = Phase::Selected;
  applyInputRegion();
  setCursor(Qt::ArrowCursor); // the grips take it from here
  setKeyboardGrab(false);
  setStatus(QStringLiteral("The page inside is live · scroll it into position, "
                           "then choose a mode"));
  update();
}

void ScrollCapturePanel::keyPressEvent(QKeyEvent *event) {
  if (event->key() == Qt::Key_Escape) {
    cancel();
    return;
  }
  // Enter stitches what has been captured so far, for when the pointer is
  // already on the chrome and the buttons are a reach.
  if (phase_ == Phase::Capturing &&
      (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
    finishCapture();
    return;
  }
  // A shortcut, never the advertised way: only the pointer being on our own
  // chrome puts the keyboard here at all.
  if ((phase_ == Phase::Selected || phase_ == Phase::Capturing) &&
      (event->key() == Qt::Key_S || event->key() == Qt::Key_A)) {
    switchMode(event->key() == Qt::Key_A ? Mode::Auto : Mode::Manual);
    return;
  }
  // While capturing the layer holds no keyboard, so no key arrives here.
  QWidget::keyPressEvent(event);
}

# Threading: the main thread never blocks

Omasnap is a layer-shell overlay. The instant it stops painting — even for
one dropped frame — it looks broken, because there is nothing else on
screen to explain the freeze. So the rule is absolute: **the UI thread does
capture, paint, and input handling, and nothing else.** Anything that can
take more than a frame (disk I/O for a full-resolution image, spawning a
process, PNG encoding, network of any kind) runs off it.

## The pattern

Qt-managed background operations follow the same shape:

1. Copy the small amount of state the worker needs by value into a lambda
   (`CaptureData`, an image, a path). Qt's implicit sharing makes this
   cheap; it also means the worker never touches `this` while the UI thread
   might be mutating it.
2. `QtConcurrent::run(...)` the lambda on Qt's global thread pool.
3. A `QFutureWatcher` connected on the UI thread picks up the result via a
   queued `finished` signal and applies it — never the other way around.
4. A `busy_`-style flag (or a more specific one) blocks reentrancy while the
   watcher is in flight, and the status pill says what is happening.

`src/editor.hpp` currently has six watchers, each the entry point for
reading its corresponding worker:

| Watcher | Worker does |
|---|---|
| `captureWatcher_` | Reads window/monitor pixels via `captureMonitorPixels` |
| `ocrWatcher_` | Renders the OCR crop and runs `tesseract` |
| `finishWatcher_` | Renders the export, encodes PNG, does the clipboard round trip, moves the file |
| `snapshotWatcher_` | Writes the crash-recovery working snapshot + operation log |
| `pinWatcher_` | Renders the image for a pinned layer surface |
| `recentsWatcher_` | Lists and decodes thumbnails for the recents shelf |

`src/scroll-capture.cpp` follows the same rule with a
`QFutureWatcher<ScrollCaptureJobResult>`. One serialized job opens the native
output session, grabs and classifies frames, assembles the stitch, writes any
debug PNGs, and closes the session on the same worker thread. The UI sends
plain Done/Cancel/Continue commands through lifetime-owned shared state and
polls plain status snapshots; it never waits for the job.

Automatic injection has its own joinable owner job. That job detects the
natural-scroll policy once per capture session, creates the uinput/virtual-
pointer resources, services every Continue episode, and destroys the native
resources on the same thread. Before the worker's explicit Done-commit point,
Cancel stops setup-aware waits before any park, nudge, or wheel event and
discards assembly plus staged debug files. After that point the result and
debug publication are committed to Done; a later panel dismissal suppresses
delivery but is not reported as a cancelled worker result.

The injector is the deliberate native-owner exception to the Qt watcher
mechanism: `scroll-capture-job.cpp` starts one nested `std::jthread` because its
uinput descriptor and Wayland objects must be created, used, and destroyed on
that exact thread. Its stop token is bridged to lifetime-owned command state,
and the outer `QFutureWatcher<ScrollCaptureJobResult>` remains the only GUI
completion path. Use this exception only for a native resource whose
same-thread lifetime cannot be expressed as independent thread-pool tasks.

## What this buys, concretely

- **OCR**: whole-image or drag-region text recognition spawns `tesseract`
  and renders a full-resolution crop, both off the UI thread, with a
  scanning animation over the region so the wait reads as progress rather
  than a hang.
- **Export**: a stitched scroll capture can be 25,000 pixels tall. PNG
  encoding that image, plus the `wl-copy`/`wl-paste` verification round
  trip, is seconds of work — all in `finishWatcher_`'s worker. See
  `CaptureEditor::finish()`.
- **Scroll capture**: reading the screen back after every wheel tick, at
  whatever cadence the page's animation settles at, never stalls painting
  the overlay's own chrome.

## The one documented exception

Before any window exists — single-instance handover in `src/instance-lock.cpp`,
and the instant `--fullscreen --copy`-style quick output path in `main()`
(`quickOutput()`, called before `QGuiApplication::exec()` even runs) — there
is no live, painted surface to keep responsive, so a bounded synchronous
wait is fine. The rule is about not freezing something the user is looking
at; a CLI-style path that exits before showing anything doesn't have that
problem. Don't extend this exception to anything that runs after a window
is visible.

## Self-violations found and fixed

Two places broke this rule despite being written after the pattern was
established, which is worth remembering: the pattern has to be followed on
purpose every time, since nothing enforces it automatically.

- **`CaptureEditor::reopenRecent()`** loaded a shelved capture's full-resolution
  source with a synchronous `QImage::load()` directly in the shelf's click
  handler — for a stitched scroll capture, tens of megapixels, on the UI
  thread. Fixed to decode on the worker pool (`reopenWatcher_`), the same
  shape as every other watcher above.
- **`CaptureEditor::pinSnapshot()`** rendered the pin image on a worker but
  then PNG-encoded and wrote it to disk in the `pinWatcher_::finished` slot —
  back on the UI thread, after the watcher had already proven the async
  shape was easy to reach. Fixed to do the encode+write inside the same
  worker lambda as the render, so the slot only launches the pin process.

## Scroll worker ownership

Scroll capture deliberately uses one longer-lived result job rather than a
sequence of unrelated thread-pool lambdas. `OutputCapture` wraps live Wayland
objects, so setup, every grab, finish, and destruction stay in that job. The
injector is likewise joinable rather than detached. Do not split either
sequence into fresh `QtConcurrent::run()` calls: the global pool does not
guarantee that a later call resumes on the native object's owning thread.

## Adding new work

If you're adding an operation that touches disk, spawns a process, or does
anything non-trivial with an image, it does not go on the UI thread. Follow
the existing watchers as templates unless one worker must own a native
resource for its complete lifetime; in that case, follow the documented scroll
injector owner job and keep its completion behind the outer watcher. If a
signal needs to fire when the worker is truly
finished, remember `QFutureWatcher::finished` is a queued connection: it
does not race obtaining a "the watcher is running" check made moments
earlier in the same call.

See also [editing-model.md](editing-model.md) for what state a background
render is allowed to read, and [dependencies.md](dependencies.md) for the
processes (`tesseract`, `wl-copy`/`wl-paste`, `hyprctl`) these workers spawn.

import { Component, Show, createEffect, createSignal, onCleanup } from 'solid-js';
import * as api from '../../lib/api';
import { timeline } from '../../stores/timeline';
import './ExportVideoDialog.css';

interface ExportVideoDialogProps {
  open: () => boolean;
  onClose: () => void;
}

type Stage = 'config' | 'running' | 'done' | 'error';

export const ExportVideoDialog: Component<ExportVideoDialogProps> = (props) => {
  // Inputs prefilled from the timeline; user can override before exporting.
  // Re-syncing on each open keeps the dialog honest if the user changed the
  // timeline range while the dialog was closed.
  const [stage, setStage] = createSignal<Stage>('config');
  const [path, setPath] = createSignal('');
  const [frameStart, setFrameStart] = createSignal(1);
  const [frameEnd, setFrameEnd] = createSignal(1);
  const [fps, setFps] = createSignal(24);
  const [samples, setSamples] = createSignal(64);
  const [maxBounces, setMaxBounces] = createSignal(8);
  const [width, setWidth] = createSignal(1280);
  const [height, setHeight] = createSignal(720);
  const [codec, setCodec] = createSignal<api.VideoCodec>('h264');
  const [errorMsg, setErrorMsg] = createSignal('');
  const [progressFrame, setProgressFrame] = createSignal(0);
  const [progressTotal, setProgressTotal] = createSignal(0);
  const [elapsedMs, setElapsedMs] = createSignal(0);
  const [doneCancelled, setDoneCancelled] = createSignal(false);
  const [donePath, setDonePath] = createSignal('');

  // Reset per-open: snapshot current timeline range/fps, clear progress state,
  // and prefill resolution from the live viewport so the export matches what
  // the user is looking at unless they explicitly change it.
  createEffect(() => {
    if (!props.open()) return;
    const tl = timeline();
    setStage('config');
    setPath('');
    setFrameStart(tl.frame_start);
    setFrameEnd(tl.frame_end);
    setFps(tl.fps);
    setErrorMsg('');
    setProgressFrame(0);
    setProgressTotal(0);
    setElapsedMs(0);
    setDoneCancelled(false);
    setDonePath('');
    api.getViewportResolution()
      .then(([w, h]) => {
        // H.264 needs even dimensions; round down to the nearest even pixel.
        setWidth(Math.max(2, Math.floor(w / 2) * 2));
        setHeight(Math.max(2, Math.floor(h / 2) * 2));
      })
      .catch(() => {
        setWidth(1280);
        setHeight(720);
      });
    api.getMaxBounces()
      .then((b) => setMaxBounces(b))
      .catch(() => setMaxBounces(8));
  });

  // Hide the native overlay while the dialog is up so its click region
  // doesn't swallow the dialog inputs. Mirrors MaterialGraphEditor.
  createEffect(() => {
    const isOpen = props.open();
    api.setViewportVisible(!isOpen).catch((e) => {
      console.warn('setViewportVisible failed:', e);
    });
  });

  // Subscribe to native progress / done / error events for the lifetime of
  // the component. We filter by stage so stale events from a previous run
  // don't flicker the UI.
  const offProgress = api.listen('video_export_progress', (msg) => {
    if (stage() !== 'running') return;
    setProgressFrame(typeof msg.frame === 'number' ? msg.frame : 0);
    setProgressTotal(typeof msg.total === 'number' ? msg.total : 0);
    setElapsedMs(typeof msg.elapsed_ms === 'number' ? msg.elapsed_ms : 0);
  });
  const offDone = api.listen('video_export_done', (msg) => {
    if (stage() !== 'running') return;
    setDonePath(typeof msg.path === 'string' ? msg.path : '');
    setDoneCancelled(msg.cancelled === true);
    setStage('done');
  });
  const offError = api.listen('video_export_error', (msg) => {
    if (stage() !== 'running') return;
    setErrorMsg(typeof msg.message === 'string' ? msg.message : 'unknown error');
    setStage('error');
  });
  onCleanup(() => {
    offProgress();
    offDone();
    offError();
  });

  const browse = async () => {
    try {
      const picked = await api.saveFileDialog(
        'Export Video',
        'render.mov',
        [{ description: 'QuickTime', extensions: ['mov'] }],
      );
      if (picked) setPath(picked);
    } catch (e) {
      console.warn('saveFileDialog failed:', e);
    }
  };

  const startExport = async () => {
    if (!path()) {
      setErrorMsg('Pick an output path first');
      return;
    }
    if (frameEnd() < frameStart()) {
      setErrorMsg('frame_end must be >= frame_start');
      return;
    }
    if (samples() < 1) {
      setErrorMsg('Samples must be >= 1');
      return;
    }
    if (width() < 2 || height() < 2) {
      setErrorMsg('Width and height must be >= 2');
      return;
    }
    if ((width() & 1) || (height() & 1)) {
      setErrorMsg('Width and height must be even (H.264 requirement)');
      return;
    }
    if (maxBounces() < 0) {
      setErrorMsg('Max bounces must be >= 0');
      return;
    }
    setErrorMsg('');
    setProgressFrame(0);
    setProgressTotal(frameEnd() - frameStart() + 1);
    setElapsedMs(0);
    setStage('running');
    try {
      await api.exportVideoStart({
        path: path(),
        frame_start: frameStart(),
        frame_end: frameEnd(),
        fps: fps(),
        samples_per_frame: samples(),
        max_bounces: maxBounces(),
        width: width(),
        height: height(),
        codec: codec(),
      });
    } catch (e) {
      setErrorMsg(e instanceof Error ? e.message : String(e));
      setStage('error');
    }
  };

  const cancelExport = async () => {
    try {
      await api.exportVideoCancel();
    } catch (e) {
      console.warn('exportVideoCancel failed:', e);
    }
  };

  const matchViewport = async () => {
    try {
      const [w, h] = await api.getViewportResolution();
      setWidth(Math.max(2, Math.floor(w / 2) * 2));
      setHeight(Math.max(2, Math.floor(h / 2) * 2));
    } catch (e) {
      console.warn('getViewportResolution failed:', e);
    }
  };

  const setPreset = (w: number, h: number) => {
    setWidth(w);
    setHeight(h);
  };

  const close = () => {
    // If a run is in flight, also tell the native side to abort so the worker
    // doesn't keep churning frames after the dialog disappears.
    if (stage() === 'running') {
      api.exportVideoCancel().catch(() => {});
    }
    props.onClose();
  };

  const progressPct = () => {
    const total = progressTotal();
    if (total <= 0) return 0;
    return Math.min(100, (progressFrame() / total) * 100);
  };

  const eta = () => {
    const f = progressFrame();
    const total = progressTotal();
    if (f <= 0 || total <= 0) return '—';
    const msPerFrame = elapsedMs() / f;
    const remaining = (total - f) * msPerFrame;
    return `${(remaining / 1000).toFixed(1)}s`;
  };

  return (
    <Show when={props.open()}>
      <div class="export-video-modal" role="dialog" aria-modal="true">
        <div class="export-video-toolbar">
          <span class="export-video-title">Export Video</span>
          <button class="export-video-close" onClick={close} type="button">
            Close
          </button>
        </div>

        <div class="export-video-body">
          <Show when={stage() === 'config' || stage() === 'error'}>
            <div class="export-video-row">
              <label>Output</label>
              <div class="export-video-path-row">
                <input
                  type="text"
                  title="Output file path"
                  value={path()}
                  placeholder="/path/to/render.mov"
                  onInput={(e) => setPath(e.currentTarget.value)}
                />
                <button type="button" onClick={browse}>Browse…</button>
              </div>
            </div>

            <div class="export-video-row">
              <label>Frame range</label>
              <div class="export-video-inline">
                <input
                  type="number"
                  title="First frame"
                  value={frameStart()}
                  onInput={(e) => setFrameStart(parseInt(e.currentTarget.value, 10) || 1)}
                />
                <span class="export-video-sep">to</span>
                <input
                  type="number"
                  title="Last frame"
                  value={frameEnd()}
                  onInput={(e) => setFrameEnd(parseInt(e.currentTarget.value, 10) || 1)}
                />
              </div>
            </div>

            <div class="export-video-row">
              <label>FPS</label>
              <input
                type="number"
                title="Frames per second"
                value={fps()}
                step="0.1"
                onInput={(e) => setFps(parseFloat(e.currentTarget.value) || 24)}
              />
            </div>

            <div class="export-video-row">
              <label>Resolution</label>
              <div class="export-video-inline">
                <input
                  type="number"
                  title="Output width in pixels (must be even)"
                  value={width()}
                  min="2"
                  step="2"
                  onInput={(e) => setWidth(parseInt(e.currentTarget.value, 10) || 0)}
                />
                <span class="export-video-sep">×</span>
                <input
                  type="number"
                  title="Output height in pixels (must be even)"
                  value={height()}
                  min="2"
                  step="2"
                  onInput={(e) => setHeight(parseInt(e.currentTarget.value, 10) || 0)}
                />
                <button type="button" title="Match viewport" onClick={matchViewport}>
                  Match viewport
                </button>
              </div>
              <div class="export-video-presets">
                <button type="button" onClick={() => setPreset(1280, 720)}>720p</button>
                <button type="button" onClick={() => setPreset(1920, 1080)}>1080p</button>
                <button type="button" onClick={() => setPreset(2560, 1440)}>1440p</button>
                <button type="button" onClick={() => setPreset(3840, 2160)}>4K</button>
              </div>
            </div>

            <div class="export-video-row">
              <label>Samples / frame</label>
              <input
                type="number"
                title="Path-tracer samples accumulated per frame"
                value={samples()}
                min="1"
                onInput={(e) => setSamples(parseInt(e.currentTarget.value, 10) || 1)}
              />
            </div>

            <div class="export-video-row">
              <label>Max bounces</label>
              <input
                type="number"
                title="Maximum ray bounces per sample (0 = camera ray only)"
                value={maxBounces()}
                min="0"
                onInput={(e) => setMaxBounces(parseInt(e.currentTarget.value, 10) || 0)}
              />
            </div>

            <div class="export-video-row">
              <label>Codec</label>
              <select
                title="Video codec"
                value={codec()}
                onChange={(e) => setCodec(e.currentTarget.value as api.VideoCodec)}
              >
                <option value="h264">H.264</option>
                <option value="prores">ProRes 422</option>
              </select>
            </div>

            <Show when={errorMsg()}>
              <div class="export-video-error">{errorMsg()}</div>
            </Show>

            <div class="export-video-actions">
              <button class="export-video-primary" type="button" onClick={startExport}>
                Export
              </button>
            </div>
          </Show>

          <Show when={stage() === 'running'}>
            <div class="export-video-progress">
              <div
                class="export-video-progress-bar"
                ref={(el) => {
                  // Drive the bar fill via a CSS custom property that the
                  // stylesheet consumes, instead of an inline `style` attr
                  // (which the project linter forbids).
                  createEffect(() => {
                    el.style.setProperty('--progress-pct', `${progressPct()}%`);
                  });
                }}
              >
                <div class="export-video-progress-fill" />
              </div>
              <div class="export-video-progress-text">
                Frame {progressFrame()} / {progressTotal()}
                {' · '}
                {progressFrame() > 0
                  ? `${(elapsedMs() / Math.max(1, progressFrame())).toFixed(0)} ms/frame`
                  : 'preparing…'}
                {' · '}
                ETA {eta()}
              </div>
              <div class="export-video-actions">
                <button type="button" onClick={cancelExport}>
                  Cancel
                </button>
              </div>
            </div>
          </Show>

          <Show when={stage() === 'done'}>
            <div class="export-video-done">
              <div class="export-video-done-msg">
                {doneCancelled()
                  ? 'Export cancelled.'
                  : `Exported to ${donePath()}`}
              </div>
              <div class="export-video-actions">
                <button class="export-video-primary" type="button" onClick={close}>
                  Close
                </button>
              </div>
            </div>
          </Show>
        </div>
      </div>
    </Show>
  );
};

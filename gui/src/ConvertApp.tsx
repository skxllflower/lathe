// Lathe (Convert) — the standalone Lathe app's main window. Fork of
// WAVdesk's LatheConvertApp (the in-WAVdesk tool window); the two stay
// behaviorally aligned, with the WAVdesk-host glue (selection seeds,
// overlay drag-out, windowed dialogs) swapped for standalone
// equivalents. Three columns: persistent input list on the left,
// output options + advanced config in the middle, session history on
// the right.
//
// Mental model:
//   • Inputs are sticky — files added stay until removed (X button or
//     drag-out trash gesture). Selection drives Convert; unselected
//     inputs sit idle.
//   • Each Convert run creates new OutputItem rows tagged with the
//     format/bitrate used. Multiple outputs per input are expected
//     (re-convert same file to MP3, then FLAC, then WAV).
//   • Settings persist across window-close via localStorage. Named
//     presets save common templates ("MP3 320", "FLAC -8", etc.).
//
// Bootstrap: when the wrapper boots and discovers ffmpeg.exe missing
// (a fresh checkout on a different machine), it downloads ffmpeg from
// the official BtbN GPL build before the actual conversion starts.
// We overlay a small "Downloading ffmpeg…" status during that phase.

import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { getCurrentWindow } from '@tauri-apps/api/window';
import { getCurrentWebview } from '@tauri-apps/api/webview';
import { invoke } from '@tauri-apps/api/core';
import { listen, type UnlistenFn } from '@tauri-apps/api/event';
import { open as openDialog } from '@tauri-apps/plugin-dialog';
import {
  X, Plus, FolderOpen, Trash2, ArrowRightLeft,
  CheckCircle2, XCircle, Loader2, ChevronRight, ChevronDown, CloudDownload,
  Link2, Link2Off, RotateCcw, FolderInput, Save, CheckSquare,
  Music, Image as ImageIcon, Film, Minus, Check, Info,
} from 'lucide-react';
import { useTheme, THEME_BG } from './theme';
import { startOverlayDrag, endOverlayDrag } from './internalDragHandoff';
import { askPromptInWindow, confirmInWindow } from './dialogs';
import { openAboutWindow } from './aboutWindow';
import { WdSelect, type WdSelectOption } from './WdSelect';
import {
  isAudioPath, isImagePath, isVideoPath, isRawPath,
  AUDIO_EXTS, IMAGE_EXTS, VIDEO_EXTS, RAW_EXTS,
} from './formats';

// Target formats grouped by media kind. Lathe (ffmpeg) infers the output
// format from the output file's extension, so each entry is just an
// extension the window can target — image/video ride the exact same
// `lathe convert` path as audio (see the per-kind branches in lathe's
// convert.cpp apply_options).
type AudioFormat = 'wav' | 'mp3' | 'flac' | 'aac' | 'ogg' | 'opus' | 'm4a' | 'aiff';
type ImageFormat = 'png' | 'jpg' | 'webp' | 'avif' | 'gif' | 'bmp' | 'tiff';
type VideoFormat = 'mp4' | 'mkv' | 'webm' | 'mov';
type Format = AudioFormat | ImageFormat | VideoFormat;
type MediaKind = 'audio' | 'image' | 'video';

const AUDIO_FORMATS: AudioFormat[] = ['wav', 'mp3', 'flac', 'aac', 'ogg', 'opus', 'm4a', 'aiff'];
const IMAGE_FORMATS: ImageFormat[] = ['png', 'jpg', 'webp', 'avif', 'gif', 'bmp', 'tiff'];
const VIDEO_FORMATS: VideoFormat[] = ['mp4', 'mkv', 'webm', 'mov'];
const FORMATS: Format[] = [...AUDIO_FORMATS, ...IMAGE_FORMATS, ...VIDEO_FORMATS];

const formatKind = (f: Format): MediaKind =>
  (IMAGE_FORMATS as readonly string[]).includes(f) ? 'image'
  : (VIDEO_FORMATS as readonly string[]).includes(f) ? 'video'
  : 'audio';

const DEFAULT_FORMAT_FOR_KIND: Record<MediaKind, Format> = {
  audio: 'wav', image: 'png', video: 'mp4',
};

// Image kinds the bundled ffmpeg can't actually decode (verified against the
// BtbN build): SVG ships a demuxer but no rasterizer (no librsvg). HEIC/HEIF
// decode fine via the mov demuxer (single + multi-item verified, incl.
// through lathe); camera RAW demosaics via LibRaw (lathe 24f68d9+).
const LATHE_IMAGE_UNSUPPORTED: ReadonlyArray<string> = ['.svg'];

// Kind of an input file from its path. null = unsupported (refused at add time).
const pathKind = (p: string): MediaKind | null => {
  const lower = p.toLowerCase();
  if (LATHE_IMAGE_UNSUPPORTED.some(ext => lower.endsWith(ext))) return null;
  if (isRawPath(p)) return 'image';
  return isImagePath(p) ? 'image' : isVideoPath(p) ? 'video' : isAudioPath(p) ? 'audio' : null;
};

// Browse-dialog filter — every extension the input gate accepts.
const DIALOG_EXTENSIONS: string[] = [...AUDIO_EXTS, ...IMAGE_EXTS, ...RAW_EXTS, ...VIDEO_EXTS]
  .filter(e => !LATHE_IMAGE_UNSUPPORTED.includes(e))
  .map(e => e.slice(1));

// Valid target kinds for an input kind. Video may target video OR audio
// (audio-extract); image/audio stay in-kind.
const targetKindsFor = (k: MediaKind): MediaKind[] =>
  k === 'video' ? ['video', 'audio'] : [k];

const canConvertKind = (inputKind: MediaKind | null, targetKind: MediaKind): boolean =>
  !!inputKind && targetKindsFor(inputKind).includes(targetKind);

type OutputMode = 'subdir' | 'samedir' | 'overwrite' | 'custom';

interface LatheSettings {
  format:           Format;
  outputMode:       OutputMode;
  subdir:           string;
  customFolder:     string;
  sampleRate:       string;
  bitDepth:         string;
  bitrate:          string;
  vbrQuality:       string;
  compressionLevel: string;
  // Image/video perceptual quality 0..100 (higher = better). Only meaningful
  // for lossy image (JPG/WebP) + video targets; ignored elsewhere.
  quality:          string;
  // Video + GIF: downscale cap (height px, never upscales). Empty = source.
  maxHeight:        string;
  // Video: x264/x265 encoder preset. Empty = ffmpeg default (medium).
  vidPreset:        string;
  // GIF: frame rate (lathe defaults 15) + palette size (defaults 256).
  gifFps:           string;
  gifColors:        string;
  // Video: remux only (stream copy, no re-encode). Quality / resolution /
  // preset are meaningless while set, and lathe skips them.
  remux:            boolean;
}

interface InputItem {
  id:               string;
  path:             string;
  name:             string;
  selected:         boolean;
  // Source byte size, statted lazily after add. -1 = stat failed (sentinel
  // so the fetch effect doesn't retry forever); undefined = not yet statted.
  size?:            number;
}

interface OutputItem {
  id:               string;
  inputId:          string;
  inputName:        string;
  inputPath:        string;
  status:           'queued' | 'converting' | 'done' | 'failed' | 'cancelled';
  percent:          number;
  output?:          string;
  error?:           string;
  jobId?:           string;
  settings:         LatheSettings;
  batchId:          string;
  selected:         boolean;
  // Source size captured at queue time + output size statted on done —
  // drives the "1.2 MB (-37%)" cell. -1 sentinels mirror InputItem.size.
  srcSize?:         number;
  outSize?:         number;
}

interface Preset {
  name:     string;
  settings: LatheSettings;
}

type BootstrapStage = 'idle' | 'downloading' | 'extracting' | 'failed';

// v2: bumped when the best-quality defaults landed so existing installs pick
// up the new pre-fill instead of their old persisted '' / 80 quality values.
// One-time reset of the convert settings only — presets (PRESETS_KEY) and
// everything else are untouched.
const SETTINGS_KEY = 'wd-lathe-settings-v2';
const PRESETS_KEY  = 'wd-lathe-presets';

// Defaults pre-fill BEST QUALITY for every format — the Lathe window is the
// full-control surface, so it starts at the top and the user dials DOWN if a
// smaller file is wanted. (The quick no-window path in latheConvert.ts
// intentionally diverges for video: it defaults to a sensible CRF, not 100,
// since there's no knob to dial it down there.) These are global
// fields shared across formats; each only applies to the formats that expose
// it (bitrate → mp3/aac/ogg/opus/m4a, compression → flac, quality →
// jpg/webp/video, bit depth → wav/aiff/flac).
const DEFAULT_SETTINGS: LatheSettings = {
  format:           'wav',
  outputMode:       'subdir',
  subdir:           'lathe-out',
  customFolder:     '',
  sampleRate:       '',      // preserve source rate
  bitDepth:         '24',    // 24-bit PCM
  bitrate:          '320k',  // max CBR for lossy
  vbrQuality:       '',      // off → mp3 uses the 320k CBR above
  compressionLevel: '8',     // FLAC: lossless, near-max compression
  quality:          '100',   // jpg/webp/video: top quality
  maxHeight:        '',      // video/gif: keep source resolution
  vidPreset:        '',      // video: ffmpeg default (medium)
  gifFps:           '',      // gif: lathe default (15)
  gifColors:        '',      // gif: full palette (256)
  remux:            false,   // video: re-encode by default
};

const formatBytes = (n: number) => {
  if (!Number.isFinite(n) || n <= 0) return '';
  if (n < 1024)            return `${n} B`;
  if (n < 1024 * 1024)     return `${(n / 1024).toFixed(0)} KB`;
  if (n < 1024 ** 3)       return `${(n / (1024 * 1024)).toFixed(1)} MB`;
  return `${(n / (1024 ** 3)).toFixed(2)} GB`;
};

const uid = () => Math.random().toString(36).slice(2) + Date.now().toString(36);

// Compact format/bitrate stamp shown on each output row so the session
// log is self-documenting ("kick.mp3 — MP3 320k", "kick.flac — FLAC -8").
const formatStamp = (s: LatheSettings): string => {
  const f = s.format.toUpperCase();
  if (s.remux && formatKind(s.format) === 'video') return `${f} COPY`;
  if (s.vbrQuality)       return `${f} V${s.vbrQuality}`;
  if (s.bitrate)          return `${f} ${s.bitrate}`;
  if (s.bitDepth)         return `${f} ${s.bitDepth}${s.bitDepth === 'f32' ? '' : '-bit'}`;
  if (s.compressionLevel) return `${f} -${s.compressionLevel}`;
  if (s.quality && (s.format === 'jpg' || s.format === 'webp' || s.format === 'avif' || formatKind(s.format) === 'video')) {
    return `${f} Q${s.quality}`;
  }
  return f;
};

const buildOutputPath = (inputPath: string, s: LatheSettings): string => {
  const sep    = inputPath.includes('/') ? '/' : '\\';
  const slash  = inputPath.lastIndexOf(sep);
  const base   = slash >= 0 ? inputPath.slice(slash + 1) : inputPath;
  const dir    = slash >= 0 ? inputPath.slice(0, slash) : '';
  const dot    = base.lastIndexOf('.');
  const stem   = dot > 0 ? base.slice(0, dot) : base;
  const file   = stem + '.' + s.format;
  // samedir lands beside the source. Same expression as overwrite, but the
  // Rust side gets overwrite=false so unique_output suffixes " (2)" on a
  // name collision (same-format conversions) instead of replacing.
  if (s.outputMode === 'overwrite' || s.outputMode === 'samedir') {
    return (dir ? dir + sep : '') + file;
  }
  if (s.outputMode === 'custom' && s.customFolder.trim()) {
    const target = s.customFolder.trim();
    const tsep   = target.includes('/') ? '/' : '\\';
    return target.replace(/[\\/]+$/, '') + tsep + file;
  }
  const sub = (s.subdir.trim() || 'lathe-out');
  return (dir ? dir + sep : '') + sub + sep + file;
};

// " (n)" suffix matching the Rust side's unique_output naming. Used to
// pre-dedupe colliding target paths within one batch — unique_output only
// checks the disk, and parallel jobs would race past it.
const withCopySuffix = (path: string, n: number): string => {
  const sep   = path.includes('/') ? '/' : '\\';
  const slash = path.lastIndexOf(sep);
  const dir   = slash >= 0 ? path.slice(0, slash + 1) : '';
  const base  = slash >= 0 ? path.slice(slash + 1) : path;
  const dot   = base.lastIndexOf('.');
  const stem  = dot > 0 ? base.slice(0, dot) : base;
  const ext   = dot > 0 ? base.slice(dot) : '';
  return `${dir}${stem} (${n})${ext}`;
};

// How many conversions run at once. ffmpeg is CPU-bound, so a small pool
// cuts batch wall-clock without starving the UI or the disk.
const CONVERT_CONCURRENCY = 3;

// Folder-drop expansion cap — a dropped drive root shouldn't mount
// ten thousand rows.
const FOLDER_CAP = 2000;

const formatExposes = (f: Format) => ({
  bitDepth:        f === 'wav' || f === 'aiff' || f === 'flac',
  bitrate:         f === 'mp3' || f === 'aac' || f === 'm4a' || f === 'ogg' || f === 'opus',
  vbr:             f === 'mp3',
  compression:     f === 'flac',
  // Sample rate is an audio-only knob (ffmpeg -ar). Hidden for image/video.
  sampleRate:      formatKind(f) === 'audio',
  // Quality slider: lossy image targets (JPG/WebP/AVIF) + every video target.
  quality:         f === 'jpg' || f === 'webp' || f === 'avif' || formatKind(f) === 'video',
  // Advanced knobs: resolution cap serves video AND gif (scale stage of the
  // palettegen graph); preset + remux are video-only; fps/colors gif-only.
  maxHeight:       formatKind(f) === 'video' || f === 'gif',
  vidPreset:       formatKind(f) === 'video',
  gifFps:          f === 'gif',
  gifColors:       f === 'gif',
  remux:           formatKind(f) === 'video',
});

// Best-effort load + parse of stored settings. Falls back to defaults
// on any failure so a corrupted localStorage entry never blocks boot.
const loadStoredSettings = (): LatheSettings => {
  try {
    const raw = localStorage.getItem(SETTINGS_KEY);
    if (!raw) return { ...DEFAULT_SETTINGS };
    const parsed = JSON.parse(raw) as Partial<LatheSettings>;
    return { ...DEFAULT_SETTINGS, ...parsed };
  } catch {
    return { ...DEFAULT_SETTINGS };
  }
};

const loadStoredPresets = (): Preset[] => {
  try {
    const raw = localStorage.getItem(PRESETS_KEY);
    if (!raw) return [];
    const parsed = JSON.parse(raw) as Preset[];
    return Array.isArray(parsed) ? parsed : [];
  } catch {
    return [];
  }
};

export default function ConvertApp() {
  const { theme } = useTheme();
  const close = () => { try { void getCurrentWindow().close(); } catch { window.close(); } };

  // URL-param format preset overrides stored settings (context-menu
  // "Convert to MP3 (Lathe)" launches with ?format=mp3).
  const presetFormat = useMemo<Format | null>(() => {
    const sp = new URLSearchParams(window.location.search);
    const f = sp.get('format') as Format | null;
    return f && FORMATS.includes(f) ? f : null;
  }, []);

  // Hydrate settings from localStorage on first render. The URL-param
  // format (if any) wins over the stored format.
  const initialSettings = useMemo<LatheSettings>(() => {
    const stored = loadStoredSettings();
    return presetFormat ? { ...stored, format: presetFormat } : stored;
  }, [presetFormat]);

  const [inputs, setInputs]   = useState<InputItem[]>([]);
  const [outputs, setOutputs] = useState<OutputItem[]>([]);

  const [format, setFormat]                 = useState<Format>(initialSettings.format);
  const [outputMode, setOutputMode]         = useState<OutputMode>(initialSettings.outputMode);
  const [subdir, setSubdir]                 = useState(initialSettings.subdir);
  const [customFolder, setCustomFolder]     = useState(initialSettings.customFolder);
  const [sampleRate, setSampleRate]         = useState(initialSettings.sampleRate);
  const [bitDepth, setBitDepth]             = useState(initialSettings.bitDepth);
  const [bitrate, setBitrate]               = useState(initialSettings.bitrate);
  const [vbrQuality, setVbrQuality]         = useState(initialSettings.vbrQuality);
  const [compressionLevel, setCompression]  = useState(initialSettings.compressionLevel);
  const [quality, setQuality]               = useState(initialSettings.quality);
  const [maxHeight, setMaxHeight]           = useState(initialSettings.maxHeight);
  const [vidPreset, setVidPreset]           = useState(initialSettings.vidPreset);
  const [gifFps, setGifFps]                 = useState(initialSettings.gifFps);
  const [gifColors, setGifColors]           = useState(initialSettings.gifColors);
  const [remux, setRemux]                   = useState(initialSettings.remux);

  const [running, setRunning]   = useState(false);
  const [dragOver, setDragOver] = useState(false);

  // Transient "N unsupported files skipped" notice in the inputs status
  // cell — feedback for the add-time gate so a refused drop isn't silent.
  const [skipNotice, setSkipNotice] = useState('');
  const skipNoticeTimer = useRef<number | null>(null);
  useEffect(() => () => {
    if (skipNoticeTimer.current !== null) window.clearTimeout(skipNoticeTimer.current);
  }, []);

  // Advanced options collapsible state — non-persistent (cheap toggle).
  const [showAdvanced, setShowAdvanced] = useState(false);

  // Preset management — localStorage-backed list keyed by display name.
  const [presets, setPresets] = useState<Preset[]>(() => loadStoredPresets());
  // Most recently applied preset — cleared the moment any setting
  // drifts so the dropdown's "active" state stays truthful. Drives
  // the delete-button enabled state too.
  const [currentPreset, setCurrentPreset] = useState<string>('');

  // Standalone: no configured path — the Rust resolver walks env →
  // dev checkout → installed locations.
  const [lathePath] = useState('');

  // "Binary connected" indicator state. Re-probed via tool_binary_probe
  // whenever the configured path changes — the title-bar icon flips
  // green/red without needing a real conversion to surface the answer.
  const [binStatus, setBinStatus] = useState<{
    resolved: boolean;
    path:     string;
    source:   string;
    message:  string;
  } | null>(null);

  // Bootstrap status — tracks the wrapper auto-downloading missing ffmpeg.
  const [bootstrap, setBootstrap] = useState<{
    stage:   BootstrapStage;
    bytes?:  number;
    total?:  number;
    percent?: number;
    message?: string;
  }>({ stage: 'idle' });

  const inputsRef  = useRef(inputs);
  inputsRef.current = inputs;
  const outputsRef = useRef(outputs);
  outputsRef.current = outputs;

  // Per-input status indicator, derived from the outputs that reference
  // each input. Outputs are the source of truth. An earlier version
  // mirrored status onto a separate InputItem.lastStatus field, but that
  // mirror relied on a setState updater running synchronously (only true
  // on React's eager-bailout path) and so randomly dropped terminal
  // events mid-batch — leaving rows spinning forever after conversion.
  // Priority: an active run (converting/queued) outranks any stale
  // terminal status; among terminals the most recent output wins.
  const inputStatus = useMemo(() => {
    const rank = { done: 0, failed: 0, cancelled: 0, queued: 1, converting: 2 };
    const map = new Map<string, OutputItem['status']>();
    for (const o of outputs) {
      const cur = map.get(o.inputId);
      if (cur === undefined || rank[o.status] >= rank[cur]) map.set(o.inputId, o.status);
    }
    return map;
  }, [outputs]);

  // Cancel flips this so pool workers stop pulling queued jobs; the
  // already-running ones get lathe_cancel'd individually.
  const batchCancelRef = useRef(false);

  // Last-selected row id per panel — anchor for Shift+click range
  // selection. Inputs and outputs each keep their own anchor so a
  // shift-click in one panel never reaches across to the other.
  const lastSelectedRef        = useRef<string | null>(null);
  const lastSelectedOutputRef  = useRef<string | null>(null);
  // Which panel the user last interacted with — drives the Delete key,
  // since there's no real focus model distinguishing the two lists.
  const lastPanelRef           = useRef<'inputs' | 'outputs'>('inputs');

  // Created hidden (tauri.conf visible:false) — reveal after the first
  // paint so the user never sees the transparent shell fill in.
  useEffect(() => {
    requestAnimationFrame(() => { void getCurrentWindow().show(); });
  }, []);

  // Main-window close owns full app teardown: the pre-spawned drag
  // overlay would otherwise keep a headless app alive. An in-flight
  // batch prompts first — closing cancels it (lathe removes partials).
  useEffect(() => {
    const w = getCurrentWindow();
    const un = w.onCloseRequested(async (e) => {
      e.preventDefault();
      const active = outputsRef.current.filter(
        o => (o.status === 'converting' || o.status === 'queued'));
      if (active.length > 0) {
        const goAhead = await confirmInWindow({
          title:        `${active.length} conversion${active.length === 1 ? '' : 's'} in progress`,
          message:      'Closing cancels the active conversions. Partial outputs are cleaned up.',
          confirmLabel: 'Cancel & close',
          cancelLabel:  'Keep working',
        });
        if (!goAhead) return;
        batchCancelRef.current = true;
        for (const o of active) {
          if (o.jobId) void invoke('lathe_cancel', { jobId: o.jobId });
        }
      }
      void invoke('app_exit');
    });
    return () => { void un.then(u => u()).catch(() => {}); };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // Probe drives the title-bar connected indicator. Cheap — just a
  // path existence check on the resolution chain.
  useEffect(() => {
    void (async () => {
      try {
        const status = await invoke<typeof binStatus>('tool_binary_probe', {
          name: 'lathe',
          configured: lathePath,
        });
        setBinStatus(status);
      } catch { setBinStatus(null); }
    })();
  }, [lathePath]);

  // Snapshot current settings for write-back / Convert capture.
  const currentSettings = useMemo<LatheSettings>(() => ({
    format, outputMode, subdir, customFolder,
    sampleRate, bitDepth, bitrate, vbrQuality, compressionLevel, quality,
    maxHeight, vidPreset, gifFps, gifColors, remux,
  }), [format, outputMode, subdir, customFolder,
       sampleRate, bitDepth, bitrate, vbrQuality, compressionLevel, quality,
       maxHeight, vidPreset, gifFps, gifColors, remux]);

  // Persist settings on change. localStorage write is synchronous +
  // cheap; no debounce needed at this rate of change.
  useEffect(() => {
    try { localStorage.setItem(SETTINGS_KEY, JSON.stringify(currentSettings)); } catch {}
  }, [currentSettings]);

  // ── Kind-aware target formats ──────────────────────────────────
  // The Format dropdown only offers targets valid for the currently-
  // selected inputs: images → image formats, audio → audio, video →
  // video OR audio (audio-extract). Mixed selections union their valid
  // targets; nothing selected falls back to audio (the historical default).
  const selectedInputKinds = useMemo<Set<MediaKind>>(() => {
    const kinds = new Set<MediaKind>();
    for (const it of inputs) {
      if (!it.selected) continue;
      const k = pathKind(it.path);
      if (k) kinds.add(k);
    }
    return kinds;
  }, [inputs]);

  const availableFormats = useMemo<Format[]>(() => {
    const targets = new Set<MediaKind>();
    if (selectedInputKinds.size === 0) targets.add('audio');
    else for (const k of selectedInputKinds) for (const t of targetKindsFor(k)) targets.add(t);
    const list: Format[] = [];
    if (targets.has('audio')) list.push(...AUDIO_FORMATS);
    if (targets.has('image')) list.push(...IMAGE_FORMATS);
    if (targets.has('video')) list.push(...VIDEO_FORMATS);
    return list;
  }, [selectedInputKinds]);

  // First selected input's kind — drives the default target when the
  // current format falls out of the available set (selecting a video
  // defaults to MP4, not the audio-extract fallback).
  const primaryInputKind = useMemo<MediaKind | null>(() => {
    for (const it of inputs) {
      if (!it.selected) continue;
      const k = pathKind(it.path);
      if (k) return k;
    }
    return null;
  }, [inputs]);

  // Snap the chosen format to a valid one when the available set shifts
  // (new selection) and the current pick is no longer offered.
  useEffect(() => {
    if (availableFormats.length === 0) return;
    if (availableFormats.includes(format)) return;
    const pref = DEFAULT_FORMAT_FOR_KIND[primaryInputKind ?? 'audio'];
    setFormat(availableFormats.includes(pref) ? pref : availableFormats[0]);
  }, [availableFormats, primaryInputKind, format]);

  // Persist preset list. Same rationale as settings persistence.
  useEffect(() => {
    try { localStorage.setItem(PRESETS_KEY, JSON.stringify(presets)); } catch {}
  }, [presets]);

  // Append paths to the input list, auto-selecting newly-added rows
  // so the common drag-and-convert flow is one click after adding.
  // Dedupes by path against existing inputs (already-added paths are
  // a no-op — drop-from-Explorer of the same file shouldn't double up).
  // Unsupported kinds (RAW images, SVG, folders, projects, ...) are
  // refused at the door instead of mounting rows the convert pass
  // would silently skip; every entry point (drop, browse, seed,
  // add-main-selection) funnels through here.
  const flashNotice = useCallback((msg: string) => {
    setSkipNotice(msg);
    if (skipNoticeTimer.current !== null) window.clearTimeout(skipNoticeTimer.current);
    skipNoticeTimer.current = window.setTimeout(() => setSkipNotice(''), 5000);
  }, []);

  const addPaths = useCallback((paths: string[]) => {
    const supported = paths.filter(p => pathKind(p) !== null);
    const skipped = paths.length - supported.length;
    if (skipped > 0) {
      flashNotice(`${skipped} unsupported ${skipped === 1 ? 'file' : 'files'} skipped`);
    }
    if (supported.length === 0) return;
    setInputs(prev => {
      const existing = new Set(prev.map(it => it.path));
      const next = [...prev];
      for (const p of supported) {
        if (existing.has(p)) continue;
        const sep = p.includes('/') ? '/' : '\\';
        const slash = p.lastIndexOf(sep);
        const name = slash >= 0 ? p.slice(slash + 1) : p;
        next.push({ id: uid(), path: p, name, selected: true });
      }
      return next;
    });
  }, [flashNotice]);

  // Folder-aware drop intake. Files go straight through the gate; dropped
  // directories expand recursively on the Rust side (symlink-safe, capped)
  // and contribute only their supported files — a sample pack full of
  // project files shouldn't spam the skip notice, the user dropped a
  // folder, not those exact files.
  const addDropped = useCallback(async (paths: string[]) => {
    let dirFlags: boolean[];
    try {
      dirFlags = await Promise.all(paths.map(p =>
        invoke<boolean>('fs_is_dir', { path: p }).catch(() => false)));
    } catch {
      dirFlags = paths.map(() => false);
    }
    const files = paths.filter((_, i) => !dirFlags[i]);
    const dirs  = paths.filter((_, i) => dirFlags[i]);
    if (files.length) addPaths(files);
    if (dirs.length === 0) return;
    let expanded: string[] = [];
    let capped = false;
    for (const d of dirs) {
      try {
        const got = await invoke<string[]>('lathe_collect_dir', { dir: d, maxFiles: FOLDER_CAP });
        if (got.length >= FOLDER_CAP) capped = true;
        expanded.push(...got);
      } catch { /* unreadable dir — skip */ }
    }
    const supported = expanded.filter(p => pathKind(p) !== null);
    if (supported.length) addPaths(supported);
    if (capped) {
      flashNotice(`Folder add capped at ${FOLDER_CAP.toLocaleString()} files`);
    } else if (expanded.length > 0 && supported.length === 0 && files.length === 0) {
      flashNotice('No supported files in dropped folder');
    }
  }, [addPaths, flashNotice]);

  // Lazily stat newly-added inputs in bounded rounds (the effect re-fires
  // as sizes land until none are missing). Sizes are cosmetic — failures
  // park on the -1 sentinel and leave the cell blank.
  useEffect(() => {
    const missing = inputs.filter(it => it.size === undefined).slice(0, 64);
    if (missing.length === 0) return;
    let stale = false;
    void (async () => {
      const sizes = await Promise.all(missing.map(it =>
        invoke<number>('fs_stat', { path: it.path }).catch(() => -1)));
      if (stale) return;
      const byId = new Map(missing.map((it, i) => [it.id, sizes[i]]));
      setInputs(prev => prev.map(it =>
        byId.has(it.id) && it.size === undefined ? { ...it, size: byId.get(it.id)! } : it));
    })();
    return () => { stale = true; };
  }, [inputs]);

  // Same pattern for finished outputs — drives the size + delta cell.
  useEffect(() => {
    const missing = outputs.filter(
      it => it.status === 'done' && it.output && it.outSize === undefined).slice(0, 16);
    if (missing.length === 0) return;
    let stale = false;
    void (async () => {
      const sizes = await Promise.all(missing.map(it =>
        invoke<number>('fs_stat', { path: it.output! }).catch(() => -1)));
      if (stale) return;
      const byId = new Map(missing.map((it, i) => [it.id, sizes[i]]));
      setOutputs(prev => prev.map(it =>
        byId.has(it.id) && it.outSize === undefined ? { ...it, outSize: byId.get(it.id)! } : it));
    })();
    return () => { stale = true; };
  }, [outputs]);

  // Wrapper events → output state. Each running job's jobId maps to
  // one OutputItem; bootstrap events are window-scoped and update the
  // overlay state instead.
  useEffect(() => {
    let unlisten: UnlistenFn | null = null;
    void (async () => {
      unlisten = await listen<{
        tool: string;
        jobId: string;
        event: { type: string; [k: string]: any };
      }>('lathe-event', (e) => {
        const { jobId, event } = e.payload;
        if (event.type === 'bootstrap') {
          if (event.stage === 'download') {
            setBootstrap({
              stage:   'downloading',
              bytes:   typeof event.bytes   === 'number' ? event.bytes   : undefined,
              total:   typeof event.total   === 'number' ? event.total   : undefined,
              percent: typeof event.percent === 'number' ? event.percent : undefined,
            });
          } else if (event.stage === 'extracting') setBootstrap({ stage: 'extracting' });
          else if (event.stage === 'done')   setBootstrap({ stage: 'idle' });
          else if (event.stage === 'failed') setBootstrap({ stage: 'failed', message: event.message });
          return;
        }
        // Outputs are the source of truth; the input-row indicator is
        // derived from them (see `inputStatus`), so there's nothing to
        // mirror onto the inputs here.
        setOutputs(prev => prev.map(it => {
          if (it.jobId !== jobId) return it;
          if (event.type === 'progress')
            return { ...it, status: 'converting', percent: event.percent ?? it.percent };
          if (event.type === 'done')
            return { ...it, status: 'done', percent: 100, output: event.output };
          if (event.type === 'cancelled')
            return { ...it, status: 'cancelled' };
          if (event.type === 'error')
            return { ...it, status: 'failed', error: event.message };
          return it;
        }));
      });
    })();
    return () => { if (unlisten) unlisten(); };
  }, []);

  // Drag-and-drop INTO Lathe. Uses getCurrentWebview() (not Window) —
  // Tauri 2's drag-drop events fire on the webview channel; the window
  // channel doesn't deliver them reliably and silently drops file
  // payloads from external apps + in-app overlay-sourced drags.
  useEffect(() => {
    let unlisten: UnlistenFn | null = null;
    void (async () => {
      unlisten = await getCurrentWebview().onDragDropEvent((e) => {
        if (e.payload.type === 'over' || e.payload.type === 'enter') {
          setDragOver(true);
        } else if (e.payload.type === 'leave') {
          setDragOver(false);
        } else if (e.payload.type === 'drop') {
          setDragOver(false);
          const paths = (e.payload as { paths?: string[] }).paths ?? [];
          if (paths.length) void addDropped(paths);
        }
      });
    })();
    return () => { if (unlisten) unlisten(); };
  }, [addDropped]);

  // ─────────────────────────────────────────────────────────────────
  // Input mutations
  // ─────────────────────────────────────────────────────────────────

  const onAddClick = useCallback(async () => {
    const picked = await openDialog({
      multiple: true,
      filters: [{
        name: 'Media (audio / image / video)',
        extensions: DIALOG_EXTENSIONS,
      }, { name: 'All files', extensions: ['*'] }],
    });
    if (!picked) return;
    const paths = Array.isArray(picked) ? picked : [picked];
    addPaths(paths.filter((p): p is string => typeof p === 'string'));
  }, [addPaths]);

  const removeInput = useCallback((id: string) => {
    setInputs(prev => prev.filter(it => it.id !== id));
    if (lastSelectedRef.current === id) lastSelectedRef.current = null;
  }, []);

  const clearAllInputs = useCallback(() => {
    setInputs([]);
    lastSelectedRef.current = null;
  }, []);

  // Click semantics:
  //   • plain click       → select only this row
  //   • Ctrl/Cmd-click    → toggle this row (multi-select)
  //   • Shift-click       → range select from lastSelected → this
  const selectInput = useCallback((id: string, mode: 'single' | 'toggle' | 'range') => {
    lastPanelRef.current = 'inputs';
    setInputs(prev => {
      if (mode === 'single') {
        lastSelectedRef.current = id;
        return prev.map(it => ({ ...it, selected: it.id === id }));
      }
      if (mode === 'toggle') {
        lastSelectedRef.current = id;
        return prev.map(it => it.id === id ? { ...it, selected: !it.selected } : it);
      }
      // range
      const anchor = lastSelectedRef.current;
      const targetIdx = prev.findIndex(it => it.id === id);
      const anchorIdx = anchor ? prev.findIndex(it => it.id === anchor) : -1;
      if (targetIdx < 0 || anchorIdx < 0) {
        // No anchor (or stale anchor) — degrade to single-select.
        lastSelectedRef.current = id;
        return prev.map(it => ({ ...it, selected: it.id === id }));
      }
      const [lo, hi] = anchorIdx < targetIdx ? [anchorIdx, targetIdx] : [targetIdx, anchorIdx];
      return prev.map((it, i) => ({ ...it, selected: i >= lo && i <= hi }));
    });
  }, []);

  const selectAll = useCallback(() => {
    setInputs(prev => prev.map(it => ({ ...it, selected: true })));
  }, []);

  const clearSelection = useCallback(() => {
    setInputs(prev => prev.map(it => ({ ...it, selected: false })));
    lastSelectedRef.current = null;
  }, []);

  // Output row selection — mirrors input semantics so users build the
  // muscle memory once. Export-To honors selection; Select-All on the
  // outputs side selects every output regardless of status, so the
  // user can also batch-delete via the trash button if we add it later.
  const selectOutput = useCallback((id: string, mode: 'single' | 'toggle' | 'range') => {
    lastPanelRef.current = 'outputs';
    setOutputs(prev => {
      if (mode === 'single') {
        lastSelectedOutputRef.current = id;
        return prev.map(it => ({ ...it, selected: it.id === id }));
      }
      if (mode === 'toggle') {
        lastSelectedOutputRef.current = id;
        return prev.map(it => it.id === id ? { ...it, selected: !it.selected } : it);
      }
      const anchor    = lastSelectedOutputRef.current;
      const targetIdx = prev.findIndex(it => it.id === id);
      const anchorIdx = anchor ? prev.findIndex(it => it.id === anchor) : -1;
      if (targetIdx < 0 || anchorIdx < 0) {
        lastSelectedOutputRef.current = id;
        return prev.map(it => ({ ...it, selected: it.id === id }));
      }
      const [lo, hi] = anchorIdx < targetIdx ? [anchorIdx, targetIdx] : [targetIdx, anchorIdx];
      return prev.map((it, i) => ({ ...it, selected: i >= lo && i <= hi }));
    });
  }, []);

  const selectAllOutputs = useCallback(() => {
    setOutputs(prev => prev.map(it => ({ ...it, selected: true })));
  }, []);

  const clearOutputSelection = useCallback(() => {
    setOutputs(prev => prev.map(it => ({ ...it, selected: false })));
    lastSelectedOutputRef.current = null;
  }, []);

  // Clearing an output only drops it from the session log — the file on
  // disk is untouched. Gated to terminal rows: removing a converting /
  // queued row would strand runOne's status poll (it waits on the row by
  // id) and hang that pool worker. Cancel an in-flight row, don't clear it.
  const isClearable = (s: OutputItem['status']) =>
    s === 'done' || s === 'failed' || s === 'cancelled';

  const removeOutput = useCallback((id: string) => {
    setOutputs(prev => prev.filter(it => !(it.id === id && isClearable(it.status))));
    if (lastSelectedOutputRef.current === id) lastSelectedOutputRef.current = null;
  }, []);

  const removeSelectedOutputs = useCallback(() => {
    setOutputs(prev => prev.filter(it => !(it.selected && isClearable(it.status))));
    lastSelectedOutputRef.current = null;
  }, []);

  const removeSelectedInputs = useCallback(() => {
    setInputs(prev => prev.filter(it => !it.selected));
    lastSelectedRef.current = null;
  }, []);

  // ─────────────────────────────────────────────────────────────────
  // Convert
  // ─────────────────────────────────────────────────────────────────

  const onConvert = useCallback(async () => {
    if (running) return;
    // Only convert inputs whose kind can target the chosen format
    // (image→image, audio→audio, video→video or audio-extract). Inputs of
    // an incompatible kind stay selected but untouched this run.
    const targetKind = formatKind(currentSettings.format);
    const selected = inputsRef.current.filter(
      it => it.selected && canConvertKind(pathKind(it.path), targetKind));
    if (selected.length === 0) return;

    // Capture current settings once for the entire batch — even if the
    // user diddles the format mid-run, each output keeps the settings
    // it was queued with.
    const batchSettings = currentSettings;

    // Overwrite mode rewrites the originals in place via Lathe, which runs
    // as a separate process with no session pre-image: there is NO undo.
    // A cross-format run can also replace an unrelated same-stem sibling
    // (track.flac -> track.wav stomps an existing track.wav). Make the user
    // acknowledge the blast radius before a bulk irreversible rewrite.
    if (batchSettings.outputMode === 'overwrite') {
      const n = selected.length;
      const ok = await confirmInWindow({
        title:        `Overwrite ${n.toLocaleString()} ${n === 1 ? 'file' : 'files'} in place?`,
        message:      'Converts originals in place via Lathe. This cannot be undone, and any same-named file at the target is replaced.',
        confirmLabel: `Overwrite ${n.toLocaleString()}`,
        cancelLabel:  'Cancel',
      });
      if (!ok) return;
    }

    const batchId       = uid();

    // Create the output rows up-front so the user sees what's about
    // to happen. Status starts as 'queued' and flips to 'converting'
    // when each job spins up.
    const newOutputs: OutputItem[] = selected.map(inp => ({
      id:        uid(),
      inputId:   inp.id,
      inputName: inp.name,
      inputPath: inp.path,
      status:    'queued',
      percent:   0,
      settings:  batchSettings,
      batchId,
      selected:  false,
      srcSize:   inp.size,
    }));
    // The new 'queued' outputs make `inputStatus` reflect this run
    // immediately — active states outrank any stale terminal one.
    setOutputs(prev => [...prev, ...newOutputs]);

    // Pre-dedupe target paths within the batch — two same-stem inputs
    // (a/kick.wav + b/kick.wav → custom folder) resolve the same target,
    // and the Rust side's unique_output only checks the disk, so parallel
    // jobs would race past it and clobber each other.
    const seenTargets = new Map<string, number>();
    const targetFor   = new Map<string, string>();
    for (const out of newOutputs) {
      let target = buildOutputPath(out.inputPath, batchSettings);
      const n = (seenTargets.get(target) ?? 0) + 1;
      seenTargets.set(target, n);
      if (n > 1 && batchSettings.outputMode !== 'overwrite') {
        target = withCopySuffix(target, n);
      }
      targetFor.set(out.id, target);
    }

    const exp = formatExposes(batchSettings.format);
    // Remux sends ONLY the copy flag — lathe skips every encode knob in
    // copy mode anyway, and serde's defaults blank the rest on the Rust side.
    const copyMode = exp.remux && batchSettings.remux;
    const runOne = async (out: OutputItem) => {
      const jobId = uid();
      setOutputs(prev => prev.map(q =>
        q.id === out.id ? { ...q, jobId, status: 'converting', percent: 0 } : q));
      try {
        await invoke('lathe_convert', {
          windowLabel: 'main',
          jobId,
          binaryPath: lathePath,
          input: out.inputPath,
          output: targetFor.get(out.id)!,
          overwrite: batchSettings.outputMode === 'overwrite',
          options: copyMode ? { copy: true } : {
            sampleRate:       batchSettings.sampleRate,
            bitDepth:         batchSettings.bitDepth,
            bitrate:          batchSettings.bitrate,
            vbrQuality:       batchSettings.vbrQuality,
            compressionLevel: batchSettings.compressionLevel,
            quality:          exp.quality   ? batchSettings.quality   : '',
            maxHeight:        exp.maxHeight ? batchSettings.maxHeight : '',
            preset:           exp.vidPreset ? batchSettings.vidPreset : '',
            fps:              exp.gifFps    ? batchSettings.gifFps    : '',
            colors:           exp.gifColors ? batchSettings.gifColors : '',
          },
        });
        // Wait for the lathe-event listener to flip status to a
        // terminal state. Polling the outputsRef avoids racing with
        // the React state batcher — the listener may not have run
        // before invoke's promise resolves.
        await new Promise<void>(resolve => {
          const interval = setInterval(() => {
            const cur = outputsRef.current.find(q => q.id === out.id);
            if (cur && (cur.status === 'done' || cur.status === 'failed' || cur.status === 'cancelled')) {
              clearInterval(interval);
              resolve();
            }
          }, 100);
        });
      } catch (err: any) {
        setOutputs(prev => prev.map(q =>
          q.id === out.id ? { ...q, status: 'failed', error: String(err?.message ?? err) } : q));
      }
    };

    // Bounded worker pool. Overwrite mode stays serial: same-stem inputs
    // in one directory legitimately target the SAME path there, and two
    // concurrent ffmpegs writing one file produce garbage.
    batchCancelRef.current = false;
    const queue = [...newOutputs];
    const concurrency = batchSettings.outputMode === 'overwrite' ? 1 : CONVERT_CONCURRENCY;
    const worker = async () => {
      for (;;) {
        const out = queue.shift();
        if (!out) return;
        if (batchCancelRef.current) {
          setOutputs(prev => prev.map(q =>
            q.id === out.id ? { ...q, status: 'cancelled' } : q));
          continue;
        }
        await runOne(out);
      }
    };

    setRunning(true);
    try {
      await Promise.all(
        Array.from({ length: Math.min(concurrency, queue.length) }, () => worker()));
    } finally {
      setRunning(false);
    }
  }, [running, currentSettings, lathePath]);

  const onCancel = useCallback(() => {
    batchCancelRef.current = true;
    for (const it of outputsRef.current) {
      if (it.status === 'converting' && it.jobId) {
        void invoke('lathe_cancel', { jobId: it.jobId });
      }
    }
  }, []);

  // Re-run a single failed/cancelled output using its captured
  // settings — distinct from rerunning from inputs because the user
  // might have changed the global settings since this output was
  // produced. The captured snapshot keeps each row reproducible.
  const retryOutput = useCallback(async (id: string) => {
    const out = outputsRef.current.find(o => o.id === id);
    if (!out || running) return;
    const jobId = uid();
    const targetPath = buildOutputPath(out.inputPath, out.settings);
    setOutputs(prev => prev.map(q =>
      q.id === id ? { ...q, jobId, status: 'converting', percent: 0, error: undefined, output: undefined } : q));
    setRunning(true);
    try {
      await invoke('lathe_convert', {
        windowLabel: 'main',
        jobId,
        binaryPath: lathePath,
        input: out.inputPath,
        output: targetPath,
        overwrite: out.settings.outputMode === 'overwrite',
        options: (() => {
          const exp = formatExposes(out.settings.format);
          if (exp.remux && out.settings.remux) return { copy: true };
          return {
            sampleRate:       out.settings.sampleRate,
            bitDepth:         out.settings.bitDepth,
            bitrate:          out.settings.bitrate,
            vbrQuality:       out.settings.vbrQuality,
            compressionLevel: out.settings.compressionLevel,
            quality:          exp.quality   ? out.settings.quality           : '',
            maxHeight:        exp.maxHeight ? (out.settings.maxHeight ?? '') : '',
            preset:           exp.vidPreset ? (out.settings.vidPreset ?? '') : '',
            fps:              exp.gifFps    ? (out.settings.gifFps    ?? '') : '',
            colors:           exp.gifColors ? (out.settings.gifColors ?? '') : '',
          };
        })(),
      });
      await new Promise<void>(resolve => {
        const interval = setInterval(() => {
          const cur = outputsRef.current.find(q => q.id === id);
          if (cur && (cur.status === 'done' || cur.status === 'failed' || cur.status === 'cancelled')) {
            clearInterval(interval);
            resolve();
          }
        }, 100);
      });
    } catch (err: any) {
      setOutputs(prev => prev.map(q =>
        q.id === id ? { ...q, status: 'failed', error: String(err?.message ?? err) } : q));
    } finally {
      setRunning(false);
    }
  }, [running, lathePath]);

  // ─────────────────────────────────────────────────────────────────
  // Export — bulk-move all successful outputs to a chosen folder.
  // Useful when the user converted to subdir/overwrite but then
  // decides where they actually want the files. fs_move is the same
  // call that drag-from-Lathe-to-Explorer uses, so popularity and
  // history bookkeeping stays consistent.
  // ─────────────────────────────────────────────────────────────────

  const doneOutputs = useMemo(
    () => outputs.filter(it => it.status === 'done' && it.output),
    [outputs]
  );

  // Outputs the Export-To button will move. Honors selection when any
  // rows are selected; falls back to "all done" so the empty-selection
  // case keeps the natural "ship the whole batch" workflow.
  const exportableOutputs = useMemo(() => {
    const selected = doneOutputs.filter(it => it.selected);
    return selected.length > 0 ? selected : doneOutputs;
  }, [doneOutputs]);

  const onExportTo = useCallback(async () => {
    if (exportableOutputs.length === 0) return;
    const picked = await openDialog({ directory: true, multiple: false });
    if (!picked || typeof picked !== 'string') return;
    const paths = exportableOutputs.map(it => it.output!).filter(p => !!p);
    try {
      const res = await invoke<{ entries: Array<{ src: string; dst: string; success: boolean }> }>('fs_move', {
        srcs: paths,
        destDir: picked,
      });
      // Update the output rows to reflect the new locations so subsequent
      // drag-out / retry / export use the moved path.
      if (res && Array.isArray(res.entries)) {
        const moveMap = new Map(res.entries.filter(e => e.success).map(e => [e.src, e.dst]));
        setOutputs(prev => prev.map(it => {
          if (!it.output) return it;
          const dst = moveMap.get(it.output);
          return dst ? { ...it, output: dst } : it;
        }));
      }
    } catch (err) {
      console.warn('lathe export-to fs_move failed:', err);
    }
  }, [exportableOutputs]);

  const onBrowseCustomFolder = useCallback(async () => {
    const picked = await openDialog({ directory: true, multiple: false });
    if (typeof picked === 'string') setCustomFolder(picked);
  }, []);

  // ─────────────────────────────────────────────────────────────────
  // Presets
  // ─────────────────────────────────────────────────────────────────

  const onSavePreset = useCallback(async () => {
    const name = await askPromptInWindow({
      title:        'Save preset',
      message:      'Saves the current format and advanced options under this name.',
      placeholder:  'e.g. MP3 320 master',
      confirmLabel: 'Save',
      maxLength:    60,
    });
    if (!name) return;
    // Presets own format + advanced + quality only. Destination is a sticky
    // workspace preference, and a preset must never silently re-arm
    // Overwrite originals. Stored as defaults to keep the type shape.
    setPresets(prev => {
      const next = prev.filter(p => p.name !== name);
      next.push({ name, settings: {
        ...currentSettings,
        outputMode:   DEFAULT_SETTINGS.outputMode,
        subdir:       DEFAULT_SETTINGS.subdir,
        customFolder: DEFAULT_SETTINGS.customFolder,
      } });
      return next;
    });
    setCurrentPreset(name);
  }, [currentSettings]);

  const onApplyPreset = useCallback((name: string) => {
    const p = presets.find(p => p.name === name);
    if (!p) return;
    // Destination fields are deliberately NOT applied — presets saved
    // before the exclusion still carry them; ignoring covers legacy too.
    setFormat(p.settings.format);
    setSampleRate(p.settings.sampleRate);
    setBitDepth(p.settings.bitDepth);
    setBitrate(p.settings.bitrate);
    setVbrQuality(p.settings.vbrQuality);
    setCompression(p.settings.compressionLevel);
    setQuality(p.settings.quality ?? DEFAULT_SETTINGS.quality);
    setMaxHeight(p.settings.maxHeight ?? DEFAULT_SETTINGS.maxHeight);
    setVidPreset(p.settings.vidPreset ?? DEFAULT_SETTINGS.vidPreset);
    setGifFps(p.settings.gifFps ?? DEFAULT_SETTINGS.gifFps);
    setGifColors(p.settings.gifColors ?? DEFAULT_SETTINGS.gifColors);
    setRemux(p.settings.remux ?? DEFAULT_SETTINGS.remux);
    setCurrentPreset(p.name);
  }, [presets]);

  // Reset every knob back to the factory defaults. Wired to the
  // dropdown's "init preset" row so a quick way out of a deeply-
  // customized state is always one click away.
  const onInitPreset = useCallback(() => {
    setFormat(DEFAULT_SETTINGS.format);
    setSampleRate(DEFAULT_SETTINGS.sampleRate);
    setBitDepth(DEFAULT_SETTINGS.bitDepth);
    setBitrate(DEFAULT_SETTINGS.bitrate);
    setVbrQuality(DEFAULT_SETTINGS.vbrQuality);
    setCompression(DEFAULT_SETTINGS.compressionLevel);
    setQuality(DEFAULT_SETTINGS.quality);
    setMaxHeight(DEFAULT_SETTINGS.maxHeight);
    setVidPreset(DEFAULT_SETTINGS.vidPreset);
    setGifFps(DEFAULT_SETTINGS.gifFps);
    setGifColors(DEFAULT_SETTINGS.gifColors);
    setRemux(DEFAULT_SETTINGS.remux);
    setCurrentPreset('');
  }, []);

  // Drop the active-preset label the moment any individual setting
  // drifts from the saved snapshot. Without this the dropdown would
  // keep showing the old preset name as "loaded" even though the
  // current settings no longer match it.
  useEffect(() => {
    if (!currentPreset) return;
    const p = presets.find(pp => pp.name === currentPreset);
    if (!p) { setCurrentPreset(''); return; }
    const s = p.settings;
    // Destination fields excluded — presets don't own them.
    const drifted = (
      s.format           !== format           ||
      s.sampleRate       !== sampleRate       ||
      s.bitDepth         !== bitDepth         ||
      s.bitrate          !== bitrate          ||
      s.vbrQuality       !== vbrQuality       ||
      s.compressionLevel !== compressionLevel ||
      (s.quality   ?? DEFAULT_SETTINGS.quality)   !== quality   ||
      (s.maxHeight ?? DEFAULT_SETTINGS.maxHeight) !== maxHeight ||
      (s.vidPreset ?? DEFAULT_SETTINGS.vidPreset) !== vidPreset ||
      (s.gifFps    ?? DEFAULT_SETTINGS.gifFps)    !== gifFps    ||
      (s.gifColors ?? DEFAULT_SETTINGS.gifColors) !== gifColors ||
      (s.remux     ?? DEFAULT_SETTINGS.remux)     !== remux
    );
    if (drifted) setCurrentPreset('');
  }, [currentPreset, presets, format,
      sampleRate, bitDepth, bitrate, vbrQuality, compressionLevel, quality,
      maxHeight, vidPreset, gifFps, gifColors, remux]);

  const onDeleteCurrentPreset = useCallback(async () => {
    if (!currentPreset) return;
    const ok = await confirmInWindow({
      title:        `Delete preset "${currentPreset}"?`,
      message:      'This cannot be undone.',
      confirmLabel: 'Delete',
    });
    if (!ok) return;
    const name = currentPreset;
    setPresets(prev => prev.filter(p => p.name !== name));
    setCurrentPreset('');
  }, [currentPreset]);

  // ─────────────────────────────────────────────────────────────────
  // Derived state
  // ─────────────────────────────────────────────────────────────────

  const selectedCount = inputs.filter(it => it.selected).length;
  // Inputs whose kind can't reach the chosen format are skipped by the
  // convert pass — surface that up-front: rows dim, the button counts
  // only participants ("Convert (3 of 5)").
  const targetKind         = formatKind(format);
  const participatingCount = inputs.filter(
    it => it.selected && canConvertKind(pathKind(it.path), targetKind)).length;
  const exposes       = formatExposes(format);
  // The advanced section only hosts the dropdowns below — quality has its
  // own slider above. Formats exposing none of them (gif/png/bmp/tiff)
  // hide the toggle instead of expanding to nothing.
  const hasAdvanced   = exposes.sampleRate || exposes.bitDepth || exposes.bitrate
                     || exposes.vbr || exposes.compression
                     || exposes.maxHeight || exposes.vidPreset
                     || exposes.gifFps || exposes.gifColors || exposes.remux;
  // Remux makes the encode knobs meaningless — gray them while it's on.
  const remuxOn       = exposes.remux && remux;
  const doneCount     = outputs.filter(it => it.status === 'done').length;
  const failedCount   = outputs.filter(it => it.status === 'failed').length;

  // Status-bar source: the actively-converting output, then the
  // queued one waiting to start. Falls back to idle summary.
  const activeOutput = useMemo(
    () => outputs.find(it => it.status === 'converting') ?? outputs.find(it => it.status === 'queued'),
    [outputs]
  );
  // Batch-aware overall progress: count outputs in the most recent
  // running batch (matches activeOutput's batchId). Drops to 0 when
  // no batch is active.
  const batchInfo = useMemo(() => {
    if (!activeOutput) return null;
    const batch = outputs.filter(it => it.batchId === activeOutput.batchId);
    const finished = batch.filter(it => it.status === 'done' || it.status === 'failed' || it.status === 'cancelled').length;
    // Pool runs several jobs at once — sum every converting row's percent.
    const activePct = batch.reduce((acc, it) =>
      it.status === 'converting' ? acc + Math.max(0, Math.min(100, it.percent)) : acc, 0);
    const overall  = batch.length > 0
      ? ((finished * 100) + activePct) / batch.length
      : 0;
    return { batch, finished, total: batch.length, overall };
  }, [activeOutput, outputs]);

  // Drop a small visual divider between outputs of different batches
  // so the user can tell "first run" from "second run" at a glance.
  const outputsWithDividers = useMemo(() => {
    const result: Array<{ kind: 'output'; item: OutputItem } | { kind: 'divider'; key: string }> = [];
    let prevBatch: string | null = null;
    for (const o of outputs) {
      if (prevBatch !== null && o.batchId !== prevBatch) {
        result.push({ kind: 'divider', key: `div-${o.batchId}` });
      }
      result.push({ kind: 'output', item: o });
      prevBatch = o.batchId;
    }
    return result;
  }, [outputs]);

  // Keyboard shortcuts (inputs-only): Ctrl+A select all inputs, Esc
  // clears input selection. Outputs use their own panel-level Select-
  // all button — Ctrl+A targeting both panels would be ambiguous since
  // there's no focus model that distinguishes them.
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      // Ignore when an input element is focused (text fields etc.).
      const tag = (e.target as HTMLElement | null)?.tagName?.toLowerCase();
      if (tag === 'input' || tag === 'select' || tag === 'textarea') return;
      if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'a') {
        e.preventDefault();
        selectAll();
        return;
      }
      if (e.key === 'Delete') {
        // Clear the selected rows in whichever panel was last touched.
        // Outputs only drop terminal rows from the log (file untouched);
        // inputs drop the file from the convert queue.
        if (lastPanelRef.current === 'outputs') {
          if (outputsRef.current.some(it => it.selected && isClearable(it.status))) {
            e.preventDefault();
            removeSelectedOutputs();
          }
        } else if (inputsRef.current.some(it => it.selected)) {
          e.preventDefault();
          removeSelectedInputs();
        }
        return;
      }
      if (e.key === 'Escape') {
        clearSelection();
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [selectAll, clearSelection, removeSelectedOutputs, removeSelectedInputs]);

  return (
    <div
      className="h-screen flex flex-col font-mono select-none text-zinc-300 relative"
      style={{ background: THEME_BG[theme] ?? '#09090b' }}
    >
      {/* Title bar */}
      <div
        data-tauri-drag-region
        className="h-7 bg-zinc-950 border-b border-zinc-800 flex items-center px-2 shrink-0"
      >
        <ArrowRightLeft size={11} className="text-zinc-400 mr-1.5" />
        <span
          data-tauri-drag-region
          className="text-[0.625rem] font-bold uppercase tracking-tight text-zinc-300"
        >
          Lathe
        </span>
        {binStatus && (
          <span
            className="ml-1.5 flex items-center"
            title={
              binStatus.resolved
                ? `Connected · ${binStatus.source}\n${binStatus.path}`
                : `Not connected\n${binStatus.message}`
            }
          >
            {binStatus.resolved
              ? <Link2     size={10} className="text-emerald-400" />
              : <Link2Off  size={10} className="text-zinc-400" />}
          </span>
        )}
        <button
          onClick={() => { void getCurrentWindow().minimize().catch(() => {}); }}
          className="ml-auto text-zinc-500 hover:text-zinc-100 hover:bg-zinc-800 p-0.5 transition-none"
          title="Minimize"
        >
          <Minus size={11} />
        </button>
        <button
          onClick={close}
          className="text-zinc-500 hover:text-zinc-100 hover:bg-zinc-800 p-0.5 transition-none"
          title="Close"
        >
          <X size={11} />
        </button>
      </div>

      {/* Body */}
      <div className="flex-1 flex min-h-0">
        {/* LEFT: persistent input list */}
        <div className="flex-1 flex flex-col min-w-0 border-r border-zinc-800">
          <div className="h-7 px-2 border-b border-zinc-800 flex items-center gap-2 shrink-0">
            <span className="text-[0.5625rem] uppercase font-bold tracking-tight text-zinc-500">
              Inputs
            </span>
            <span className="text-[0.5625rem] text-zinc-600 ml-auto">
              {selectedCount > 0
                ? <><span className="text-zinc-300">{selectedCount}</span> / {inputs.length}</>
                : <>{inputs.length} {inputs.length === 1 ? 'file' : 'files'}</>
              }
            </span>
            <button
              onClick={selectAll}
              disabled={inputs.length === 0 || selectedCount === inputs.length}
              className="text-zinc-400 hover:text-zinc-100 disabled:opacity-30 disabled:hover:text-zinc-400 p-0.5 transition-none"
              title="Select all inputs"
            >
              <CheckSquare size={11} />
            </button>
            <button
              onClick={onAddClick}
              className="text-zinc-400 hover:text-zinc-100 p-0.5 transition-none"
              title="Add files"
            >
              <Plus size={11} />
            </button>
            <button
              onClick={clearAllInputs}
              disabled={inputs.length === 0}
              className="text-zinc-400 hover:text-zinc-100 disabled:opacity-30 disabled:hover:text-zinc-400 p-0.5 transition-none"
              title="Clear all inputs"
            >
              <Trash2 size={11} />
            </button>
          </div>
          <div
            className={`flex-1 min-h-0 overflow-y-auto px-1 py-1 transition-none ${
              dragOver ? 'bg-zinc-800/40' : ''
            }`}
            onClick={(e) => {
              // Click on the panel's empty space (outside any row)
              // clears selection. Rows stopPropagation their clicks.
              if (e.target === e.currentTarget) clearSelection();
            }}
          >
            {inputs.length === 0 ? (
              <div
                className="h-full flex flex-col items-center justify-center text-center px-4 gap-2 pointer-events-none"
              >
                <FolderOpen size={20} className="text-zinc-700" />
                <span className="text-[0.625rem] text-zinc-500">
                  Drop audio / image / video files here
                </span>
                <span className="text-[0.5625rem] text-zinc-600">
                  or click <span className="text-zinc-400">+</span> to browse
                </span>
              </div>
            ) : (
              inputs.map(it => {
                const kind = pathKind(it.path);
                const compatible = canConvertKind(kind, targetKind);
                return (
                <div
                  key={it.id}
                  className={`group flex items-center gap-1.5 px-1.5 py-1 text-[0.625rem] cursor-pointer transition-none ${
                    it.selected ? 'bg-zinc-800/70 text-zinc-100' : 'text-zinc-400 hover:bg-zinc-900/60'
                  } ${compatible ? '' : 'opacity-40'}`}
                  title={
                    (compatible
                      ? it.path
                      : `${it.path}\nWon't convert to ${format.toUpperCase()}: kind mismatch`)
                    + '\nDrag out of the list to remove'
                  }
                  draggable
                  onClick={(e) => {
                    e.stopPropagation();
                    const mode = e.shiftKey ? 'range' : (e.ctrlKey || e.metaKey) ? 'toggle' : 'single';
                    selectInput(it.id, mode);
                  }}
                  onDragStart={(e) => {
                    // Trash gesture — dragging an input row out of the
                    // queue removes it. preventDefault cancels the OS
                    // drag so no ghost is created.
                    e.preventDefault();
                    removeInput(it.id);
                  }}
                >
                  {/* Status indicator — derived from this input's outputs.
                      Spinner only while a run is active (converting) or
                      pending in the batch (queued); terminal runs show a
                      glyph, and inputs with no outputs show nothing. */}
                  <span className="shrink-0 w-2.5 flex items-center justify-center">
                    {(() => {
                      switch (inputStatus.get(it.id)) {
                        case 'converting': return <Loader2 size={9} className="animate-spin text-zinc-300" />;
                        case 'queued':     return <Loader2 size={9} className="animate-spin text-zinc-600" />;
                        case 'done':       return <CheckCircle2 size={9} className="text-emerald-400" />;
                        case 'failed':     return <XCircle size={9} className="text-rose-400" />;
                        case 'cancelled':  return <XCircle size={9} className="text-zinc-500" />;
                        default:           return null;
                      }
                    })()}
                  </span>
                  <span className="shrink-0 text-zinc-500 flex items-center">
                    {kind === 'audio' && <Music     size={9} />}
                    {kind === 'image' && <ImageIcon size={9} />}
                    {kind === 'video' && <Film      size={9} />}
                  </span>
                  <span className="flex-1 min-w-0 truncate">{it.name}</span>
                  {it.size !== undefined && it.size >= 0 && (
                    <span className="shrink-0 text-[0.5rem] text-zinc-600 tabular-nums">
                      {formatBytes(it.size)}
                    </span>
                  )}
                  <button
                    onClick={(e) => { e.stopPropagation(); removeInput(it.id); }}
                    className="wd-slide-action text-zinc-600 hover:text-zinc-300"
                    title="Remove from inputs"
                  >
                    <X size={10} />
                  </button>
                </div>
              ); })
            )}
          </div>
        </div>

        {/* MIDDLE: format + advanced + actions */}
        <div className="w-[210px] flex flex-col border-r border-zinc-800 shrink-0">
          <div className="h-7 px-2 border-b border-zinc-800 flex items-center gap-2 shrink-0">
            <span className="text-[0.5625rem] uppercase font-bold tracking-tight text-zinc-500">
              Configure
            </span>
          </div>
          <div className="flex-1 flex flex-col px-3 py-2 gap-2 min-h-0 overflow-y-auto">
            {/* Preset row */}
            <div className="flex flex-col gap-1">
              <label className="text-[0.5rem] uppercase tracking-widest text-zinc-500">Preset</label>
              <div className="flex items-center gap-1">
                <div className="flex-1 min-w-0">
                  <WdSelect<string>
                    value={currentPreset}
                    // Empty value = "init preset" row. Resets every
                    // knob to DEFAULT_SETTINGS. Non-empty = load that
                    // named preset.
                    onChange={(v) => { if (v) onApplyPreset(v); else onInitPreset(); }}
                    disabled={running}
                    ariaLabel="Preset"
                    options={[
                      { value: '', label: 'init preset' },
                      ...presets.map((p): WdSelectOption<string> => ({ value: p.name, label: p.name })),
                    ]}
                  />
                </div>
                <button
                  onClick={onSavePreset}
                  disabled={running}
                  className="text-zinc-400 hover:text-zinc-100 disabled:opacity-30 p-0.5 transition-none"
                  title="Save current settings as preset"
                >
                  <Save size={11} />
                </button>
                <button
                  onClick={onDeleteCurrentPreset}
                  disabled={running || !currentPreset}
                  className="text-zinc-600 hover:text-zinc-300 disabled:opacity-30 p-0.5 transition-none"
                  title={currentPreset ? `Delete preset "${currentPreset}"` : 'Load a preset first to delete it'}
                >
                  <Trash2 size={11} />
                </button>
              </div>
            </div>

            <div className="flex flex-col gap-1">
              <label className="text-[0.5rem] uppercase tracking-widest text-zinc-500">Format</label>
              <WdSelect<Format>
                value={format}
                onChange={setFormat}
                disabled={running}
                ariaLabel="Format"
                options={availableFormats.map((f): WdSelectOption<Format> => ({ value: f, label: f.toUpperCase() }))}
              />
            </div>

            {/* Quality slider — lossy image (JPG/WebP) + video targets only.
                One 0..100 perceptual scale (higher = better); lathe maps it
                per output codec (JPEG -q:v, WebP -quality, H.264/VP9 -crf). */}
            {exposes.quality && (
              <div className="flex flex-col gap-1">
                <label className="text-[0.5rem] uppercase tracking-widest text-zinc-500">
                  Quality <span className="normal-case tracking-normal text-zinc-600">{quality}</span>
                </label>
                <input
                  type="range"
                  min={0}
                  max={100}
                  step={1}
                  value={quality}
                  onChange={(e) => setQuality(e.target.value)}
                  disabled={running || remuxOn}
                  aria-label="Quality"
                  className="w-full accent-zinc-400 disabled:opacity-40"
                />
                <div className="flex justify-between text-[0.5rem] uppercase tracking-widest text-zinc-600">
                  <span>smaller</span>
                  <span>better</span>
                </div>
              </div>
            )}

            <div className="flex flex-col gap-1">
              <label className="text-[0.5rem] uppercase tracking-widest text-zinc-500">Destination</label>
              <label className="flex items-center gap-1.5 text-[0.625rem] text-zinc-300 cursor-pointer">
                <input
                  type="radio"
                  name="output-mode"
                  checked={outputMode === 'subdir'}
                  onChange={() => setOutputMode('subdir')}
                  disabled={running}
                  className="accent-zinc-400"
                />
                Subdirectory
              </label>
              {outputMode === 'subdir' && (
                <input
                  value={subdir}
                  onChange={(e) => setSubdir(e.target.value)}
                  disabled={running}
                  placeholder="folder name"
                  className="bg-zinc-900 border border-zinc-700 text-zinc-200 text-[0.625rem] px-2 h-5 ml-4 focus:outline-none focus:border-zinc-500 disabled:opacity-50"
                />
              )}
              <label
                className="flex items-center gap-1.5 text-[0.625rem] text-zinc-300 cursor-pointer"
                title="Output lands in the source file's folder. A name collision gets a (2) suffix, the original is never replaced."
              >
                <input
                  type="radio"
                  name="output-mode"
                  checked={outputMode === 'samedir'}
                  onChange={() => setOutputMode('samedir')}
                  disabled={running}
                  className="accent-zinc-400"
                />
                Next to original
              </label>
              <label className="flex items-center gap-1.5 text-[0.625rem] text-zinc-300 cursor-pointer">
                <input
                  type="radio"
                  name="output-mode"
                  checked={outputMode === 'overwrite'}
                  onChange={() => setOutputMode('overwrite')}
                  disabled={running}
                  className="accent-zinc-400"
                />
                Overwrite originals
              </label>
              <label className="flex items-center gap-1.5 text-[0.625rem] text-zinc-300 cursor-pointer">
                <input
                  type="radio"
                  name="output-mode"
                  checked={outputMode === 'custom'}
                  onChange={() => setOutputMode('custom')}
                  disabled={running}
                  className="accent-zinc-400"
                />
                Custom folder
              </label>
              {outputMode === 'custom' && (
                <div className="flex items-center gap-1 ml-4">
                  <input
                    value={customFolder}
                    readOnly
                    placeholder="— pick folder —"
                    title={customFolder || 'No folder chosen'}
                    className="flex-1 min-w-0 bg-zinc-900 border border-zinc-700 text-zinc-200 text-[0.625rem] px-2 h-5 truncate focus:outline-none disabled:opacity-50"
                  />
                  <button
                    onClick={onBrowseCustomFolder}
                    disabled={running}
                    className="text-zinc-400 hover:text-zinc-100 disabled:opacity-30 p-0.5 transition-none"
                    title="Browse for folder"
                  >
                    <FolderOpen size={10} />
                  </button>
                </div>
              )}
            </div>

            {/* Advanced (collapsible) — hidden entirely for formats with
                no advanced knobs so the expansion never opens onto nothing. */}
            {hasAdvanced && (
              <button
                onClick={() => setShowAdvanced(v => !v)}
                className="flex items-center gap-1 text-[0.5rem] uppercase tracking-widest text-zinc-500 hover:text-zinc-300 transition-none mt-1"
              >
                {showAdvanced ? <ChevronDown size={9} /> : <ChevronRight size={9} />}
                Advanced
              </button>
            )}
            {hasAdvanced && showAdvanced && (
              <div className="flex flex-col gap-1.5 pl-2 border-l border-zinc-800">
                {exposes.sampleRate && (
                  <div className="flex flex-col gap-0.5">
                    <label className="text-[0.5rem] uppercase tracking-widest text-zinc-600">Sample rate</label>
                    <WdSelect<string>
                      value={sampleRate}
                      onChange={setSampleRate}
                      disabled={running}
                      ariaLabel="Sample rate"
                      options={[
                        { value: '',       label: '— preserve —' },
                        { value: '22050',  label: '22 050 Hz' },
                        { value: '44100',  label: '44 100 Hz' },
                        { value: '48000',  label: '48 000 Hz' },
                        { value: '88200',  label: '88 200 Hz' },
                        { value: '96000',  label: '96 000 Hz' },
                        { value: '192000', label: '192 000 Hz' },
                      ]}
                    />
                  </div>
                )}
                {exposes.bitDepth && (
                  <div className="flex flex-col gap-0.5">
                    <label className="text-[0.5rem] uppercase tracking-widest text-zinc-600">Bit depth</label>
                    <WdSelect<string>
                      value={bitDepth}
                      onChange={setBitDepth}
                      disabled={running}
                      ariaLabel="Bit depth"
                      options={[
                        { value: '',   label: '— preserve —' },
                        { value: '16', label: '16-bit PCM' },
                        { value: '24', label: '24-bit PCM' },
                        { value: '32', label: '32-bit PCM' },
                        ...((format === 'wav' || format === 'aiff')
                          ? [{ value: 'f32', label: '32-bit float' } as WdSelectOption<string>]
                          : []),
                      ]}
                    />
                  </div>
                )}
                {exposes.bitrate && (
                  <div className="flex flex-col gap-0.5">
                    <label className="text-[0.5rem] uppercase tracking-widest text-zinc-600">Bitrate (CBR)</label>
                    <WdSelect<string>
                      value={bitrate}
                      onChange={setBitrate}
                      disabled={running}
                      ariaLabel="Bitrate"
                      options={[
                        { value: '',     label: '— ffmpeg default —' },
                        { value: '96k',  label: '96 kbps' },
                        { value: '128k', label: '128 kbps' },
                        { value: '160k', label: '160 kbps' },
                        { value: '192k', label: '192 kbps' },
                        { value: '256k', label: '256 kbps' },
                        { value: '320k', label: '320 kbps' },
                      ]}
                    />
                  </div>
                )}
                {exposes.vbr && (
                  <div className="flex flex-col gap-0.5">
                    <label className="text-[0.5rem] uppercase tracking-widest text-zinc-600">VBR quality (overrides CBR)</label>
                    <WdSelect<string>
                      value={vbrQuality}
                      onChange={setVbrQuality}
                      disabled={running}
                      ariaLabel="VBR quality"
                      options={[
                        { value: '',  label: '— off —' },
                        { value: '0', label: 'V0 (best, ~245k)' },
                        { value: '2', label: 'V2 (~190k)' },
                        { value: '4', label: 'V4 (~165k)' },
                        { value: '6', label: 'V6 (~130k)' },
                      ]}
                    />
                  </div>
                )}
                {exposes.compression && (
                  <div className="flex flex-col gap-0.5">
                    <label className="text-[0.5rem] uppercase tracking-widest text-zinc-600">FLAC compression</label>
                    <WdSelect<string>
                      value={compressionLevel}
                      onChange={setCompression}
                      disabled={running}
                      ariaLabel="FLAC compression"
                      options={[
                        { value: '',   label: '— default (5) —' },
                        { value: '0',  label: '0 (fastest)' },
                        { value: '3',  label: '3' },
                        { value: '5',  label: '5' },
                        { value: '8',  label: '8' },
                        { value: '12', label: '12 (max)' },
                      ]}
                    />
                  </div>
                )}
                {exposes.remux && (
                  <label
                    className="flex items-center gap-1.5 text-[0.625rem] text-zinc-300 cursor-pointer"
                    title="Repackage the source streams into the new container without re-encoding. Near-instant and lossless; fails when the codecs don't fit the target container."
                  >
                    <input
                      type="checkbox"
                      checked={remux}
                      onChange={(e) => setRemux(e.target.checked)}
                      disabled={running}
                      className="accent-zinc-400"
                    />
                    Remux (copy streams)
                  </label>
                )}
                {exposes.gifFps && (
                  <div className="flex flex-col gap-0.5">
                    <label className="text-[0.5rem] uppercase tracking-widest text-zinc-600">Frame rate</label>
                    <WdSelect<string>
                      value={gifFps}
                      onChange={setGifFps}
                      disabled={running}
                      ariaLabel="GIF frame rate"
                      options={[
                        { value: '',   label: '— default (15) —' },
                        { value: '24', label: '24 fps' },
                        { value: '15', label: '15 fps' },
                        { value: '12', label: '12 fps' },
                        { value: '10', label: '10 fps' },
                        { value: '8',  label: '8 fps' },
                      ]}
                    />
                  </div>
                )}
                {exposes.gifColors && (
                  <div className="flex flex-col gap-0.5">
                    <label className="text-[0.5rem] uppercase tracking-widest text-zinc-600">Palette colors</label>
                    <WdSelect<string>
                      value={gifColors}
                      onChange={setGifColors}
                      disabled={running}
                      ariaLabel="GIF palette colors"
                      options={[
                        { value: '',    label: '— max (256) —' },
                        { value: '128', label: '128' },
                        { value: '64',  label: '64' },
                        { value: '32',  label: '32' },
                      ]}
                    />
                  </div>
                )}
                {exposes.maxHeight && (
                  <div className="flex flex-col gap-0.5">
                    <label className="text-[0.5rem] uppercase tracking-widest text-zinc-600">Resolution cap</label>
                    <WdSelect<string>
                      value={maxHeight}
                      onChange={setMaxHeight}
                      disabled={running || remuxOn}
                      ariaLabel="Resolution cap"
                      options={[
                        { value: '',     label: '— source —' },
                        { value: '2160', label: '2160p (4K)' },
                        { value: '1440', label: '1440p' },
                        { value: '1080', label: '1080p' },
                        { value: '720',  label: '720p' },
                        { value: '480',  label: '480p' },
                      ]}
                    />
                  </div>
                )}
                {exposes.vidPreset && (
                  <div className="flex flex-col gap-0.5">
                    <label className="text-[0.5rem] uppercase tracking-widest text-zinc-600">Encoder preset</label>
                    <WdSelect<string>
                      value={vidPreset}
                      onChange={setVidPreset}
                      disabled={running || remuxOn}
                      ariaLabel="Encoder preset"
                      options={[
                        { value: '',          label: '— default (medium) —' },
                        { value: 'ultrafast', label: 'ultrafast (biggest file)' },
                        { value: 'veryfast',  label: 'veryfast' },
                        { value: 'fast',      label: 'fast' },
                        { value: 'medium',    label: 'medium' },
                        { value: 'slow',      label: 'slow' },
                        { value: 'veryslow',  label: 'veryslow (smallest file)' },
                      ]}
                    />
                  </div>
                )}
              </div>
            )}

            <div className="border-t border-zinc-800 mt-1" />

            {running ? (
              <button
                onClick={onCancel}
                className="px-4 h-7 bg-zinc-900/40 hover:bg-zinc-900/60 text-zinc-200 border border-zinc-800 text-[0.5625rem] uppercase font-bold transition-none"
              >
                Cancel
              </button>
            ) : (
              <button
                onClick={onConvert}
                disabled={participatingCount === 0 || (outputMode === 'custom' && !customFolder.trim())}
                className="px-4 h-7 bg-zinc-700 hover:bg-zinc-600 text-zinc-100 border border-zinc-600 hover:border-zinc-500 disabled:opacity-30 disabled:hover:bg-zinc-700 text-[0.5625rem] uppercase font-bold transition-none"
                title={
                  outputMode === 'custom' && !customFolder.trim()
                    ? 'Choose a custom folder first'
                    : selectedCount === 0
                      ? 'Select inputs to convert'
                      : participatingCount === 0
                        ? `No selected input converts to ${format.toUpperCase()}`
                        : participatingCount < selectedCount
                          ? `${participatingCount} of ${selectedCount} selected inputs convert to ${format.toUpperCase()}; the rest are skipped`
                          : `Convert ${selectedCount} selected input${selectedCount === 1 ? '' : 's'}`
                }
              >
                {participatingCount > 0 && participatingCount < selectedCount
                  ? `Convert (${participatingCount} of ${selectedCount})`
                  : selectedCount > 0 ? `Convert (${selectedCount})` : 'Convert'}
              </button>
            )}
          </div>
        </div>

        {/* RIGHT: session history of outputs */}
        <div className="flex-1 flex flex-col min-w-0">
          <div className="h-7 px-2 border-b border-zinc-800 flex items-center gap-2 shrink-0">
            <span className="text-[0.5625rem] uppercase font-bold tracking-tight text-zinc-500">
              Outputs
            </span>
            <span className="text-[0.5625rem] text-zinc-600 ml-auto">
              {doneCount > 0 && <span className="text-emerald-400">{doneCount} ok</span>}
              {doneCount > 0 && failedCount > 0 && <span className="text-zinc-700"> / </span>}
              {failedCount > 0 && <span className="text-zinc-400">{failedCount} fail</span>}
            </span>
            <button
              onClick={selectAllOutputs}
              disabled={outputs.length === 0 || outputs.every(it => it.selected)}
              className="text-zinc-400 hover:text-zinc-100 disabled:opacity-30 disabled:hover:text-zinc-400 p-0.5 transition-none"
              title="Select all outputs"
            >
              <CheckSquare size={11} />
            </button>
            <button
              onClick={() => setOutputs([])}
              disabled={outputs.length === 0 || running}
              className="text-zinc-400 hover:text-zinc-100 disabled:opacity-30 disabled:hover:text-zinc-400 p-0.5 transition-none"
              title="Clear session log (does not delete files)"
            >
              <Trash2 size={11} />
            </button>
          </div>
          <div
            className="flex-1 min-h-0 overflow-y-auto px-1 py-1"
            onClick={(e) => {
              // Click on the panel's empty space (outside any row)
              // clears the output selection. Rows stopPropagation.
              if (e.target === e.currentTarget) clearOutputSelection();
            }}
          >
            {outputs.length === 0 ? (
              <div className="h-full flex items-center justify-center text-[0.5625rem] text-zinc-700 px-4 text-center pointer-events-none">
                Processed files appear here
              </div>
            ) : (
              outputsWithDividers.map(entry => {
                if (entry.kind === 'divider') {
                  return (
                    <div key={entry.key} className="h-px bg-zinc-800/60 my-1 mx-1.5" />
                  );
                }
                const it = entry.item;
                const stamp = formatStamp(it.settings);
                const dragOK = it.status === 'done' && !!it.output;
                const sizeCell = it.status === 'done' && it.outSize !== undefined && it.outSize >= 0
                  ? formatBytes(it.outSize) + (it.srcSize && it.srcSize > 0
                      ? ` ${it.outSize >= it.srcSize ? '+' : '-'}${Math.abs(Math.round(((it.outSize - it.srcSize) / it.srcSize) * 100))}%`
                      : '')
                  : '';
                return (
                  <div
                    key={it.id}
                    className={`group flex items-center gap-1.5 px-1.5 py-1 text-[0.625rem] ${
                      it.selected
                        ? 'bg-zinc-800/70 text-zinc-100'
                        : 'hover:bg-zinc-900/60'
                    } ${dragOK ? 'cursor-grab active:cursor-grabbing' : 'cursor-default'}`}
                    title={it.error ?? it.output ?? it.inputPath}
                    onClick={(e) => {
                      e.stopPropagation();
                      const mode = e.shiftKey ? 'range' : (e.ctrlKey || e.metaKey) ? 'toggle' : 'single';
                      selectOutput(it.id, mode);
                    }}
                    draggable={dragOK}
                    onDragStart={(e) => {
                      if (!dragOK) return;
                      e.preventDefault();
                      // Dragging a selected row carries every selected done
                      // output; an unselected row drags alone. Dedupe covers
                      // overwrite-mode re-runs landing on the same path.
                      const dragPaths = it.selected
                        ? Array.from(new Set(outputsRef.current
                            .filter(o => o.selected && o.status === 'done' && o.output)
                            .map(o => o.output!)))
                        : [it.output!];
                      const name = it.output!.split(/[\\/]/).pop() ?? it.inputName;
                      void (async () => {
                        await startOverlayDrag({
                          paths:       dragPaths,
                          fileName:    name,
                          isDirectory: false,
                          count:       dragPaths.length,
                        });
                        try {
                          await invoke('start_os_file_drag', {
                            paths:       dragPaths,
                            previewPng:  null,
                            transparent: true,
                          });
                        } catch (err) {
                          console.warn('start_os_file_drag (lathe) failed:', err);
                          await endOverlayDrag();
                        }
                      })();
                    }}
                  >
                    {it.status === 'done'      && <CheckCircle2 size={10} className="text-emerald-400 shrink-0" />}
                    {it.status === 'failed'    && <XCircle      size={10} className="text-rose-400 shrink-0" />}
                    {it.status === 'cancelled' && <XCircle      size={10} className="text-zinc-500 shrink-0" />}
                    {it.status === 'converting'&& <Loader2      size={10} className="text-zinc-300 shrink-0 animate-spin" />}
                    {it.status === 'queued'    && <Loader2      size={10} className="text-zinc-600 shrink-0" />}
                    <span className="flex-1 min-w-0 truncate">
                      {it.status === 'done' && it.output
                        ? (it.output.split(/[\\/]/).pop() ?? it.inputName)
                        : it.inputName}
                    </span>
                    {sizeCell && (
                      <span
                        className="text-zinc-600 text-[0.5rem] tabular-nums shrink-0"
                        title={it.srcSize && it.srcSize > 0 ? `Source: ${formatBytes(it.srcSize)}` : undefined}
                      >
                        {sizeCell}
                      </span>
                    )}
                    <span className="text-zinc-600 text-[0.5rem] uppercase tracking-wider shrink-0">{stamp}</span>
                    {it.status === 'converting' && (
                      <span className="text-zinc-500 text-[0.5625rem]">{it.percent.toFixed(0)}%</span>
                    )}
                    {(it.status === 'failed' || it.status === 'cancelled') && (
                      <button
                        onClick={() => retryOutput(it.id)}
                        disabled={running}
                        className="text-zinc-500 hover:text-zinc-200 disabled:opacity-30 transition-none shrink-0 cursor-pointer"
                        title={it.status === 'failed' ? `Retry: ${it.error ?? 'failed'}` : 'Retry cancelled item'}
                      >
                        <RotateCcw size={9} />
                      </button>
                    )}
                    {it.status === 'done' && it.output && (
                      <button
                        onClick={(e) => {
                          e.stopPropagation();
                          void invoke('os_reveal_path', { path: it.output! });
                        }}
                        className="text-zinc-600 hover:text-zinc-300 transition-none shrink-0 cursor-pointer"
                        title="Show in folder"
                      >
                        <FolderOpen size={9} />
                      </button>
                    )}
                    {/* Clear-from-log X — slides open on row hover, pushing
                        the folder/retry glyph aside. Terminal rows only;
                        clearing a converting row would strand runOne. */}
                    {(it.status === 'done' || it.status === 'failed' || it.status === 'cancelled') && (
                      <button
                        onClick={(e) => { e.stopPropagation(); removeOutput(it.id); }}
                        className="wd-slide-action text-zinc-600 hover:text-zinc-300 cursor-pointer"
                        title="Clear from log (does not delete the file)"
                      >
                        <X size={9} />
                      </button>
                    )}
                  </div>
                );
              })
            )}
          </div>
        </div>
      </div>

      {/* Status bar — three cells aligned with the columns above so each
          panel's status info sits directly under it. Inputs counts on
          the left, binary chip + batch-progress in the middle, outputs
          counts + Export-To on the right. A 1px progress fill spans the
          full width below and tracks current-batch completion. */}
      <div className="shrink-0 border-t border-zinc-800 bg-zinc-950">
        <div className="h-5 flex items-stretch text-[0.5625rem] tabular-nums">
          {/* INPUTS CELL */}
          <div className="flex-1 min-w-0 flex items-center px-2 gap-2 border-r border-zinc-800 text-zinc-600">
            {skipNotice ? (
              <span className="truncate text-[color:var(--theme-warn-fg)]">{skipNotice}</span>
            ) : inputs.length === 0 ? (
              <span className="truncate">Drop files to begin</span>
            ) : selectedCount === 0 ? (
              <span className="truncate">{inputs.length} input{inputs.length === 1 ? '' : 's'} · select to convert</span>
            ) : (
              <span className="truncate">
                <span className="text-zinc-300">{selectedCount}</span>
                {' / '}{inputs.length} selected
              </span>
            )}
          </div>

          {/* CONFIGURE CELL */}
          <div className="w-[210px] shrink-0 flex items-center px-2 gap-2 border-r border-zinc-800">
            {binStatus && (
              <span
                className="flex items-center gap-1 shrink-0"
                title={
                  binStatus.resolved
                    ? `Connected · ${binStatus.source}\n${binStatus.path}`
                    : binStatus.message
                }
              >
                {binStatus.resolved
                  ? <Link2     size={9} className="text-emerald-400" />
                  : <Link2Off  size={9} className="text-zinc-400" />}
                <span className="text-zinc-500 uppercase tracking-wider">
                  {binStatus.resolved ? 'lathe.exe' : 'no binary'}
                </span>
              </span>
            )}
            {running && batchInfo && (
              <span className="ml-auto text-zinc-500 shrink-0 flex items-center gap-1">
                <Loader2 size={8} className="animate-spin text-zinc-400" />
                {batchInfo.finished}/{batchInfo.total} · {batchInfo.overall.toFixed(0)}%
              </span>
            )}
            {/* About — info glyph opposite the lathe.exe chip; hover
                reveals the "ABOUT" label, click opens the About window. */}
            <button
              onClick={() => { void openAboutWindow(); }}
              className="wd-about ml-auto flex items-center gap-1 text-zinc-600 hover:text-zinc-300 shrink-0 cursor-pointer"
              title="About Lathe"
            >
              <span className="wd-about-label uppercase tracking-wider text-[0.5rem]">About</span>
              <Info size={10} className="shrink-0" />
            </button>
          </div>

          {/* OUTPUTS CELL */}
          <div className="flex-1 min-w-0 flex items-center px-2 gap-2">
            <span className="text-zinc-600 truncate">
              {outputs.length === 0 ? (
                'No outputs yet'
              ) : (
                <>
                  {doneCount > 0 && <span className="text-emerald-400">{doneCount} done</span>}
                  {doneCount > 0 && failedCount > 0 && <span className="text-zinc-700"> · </span>}
                  {failedCount > 0 && <span className="text-zinc-400">{failedCount} failed</span>}
                </>
              )}
            </span>
            <button
              onClick={onExportTo}
              disabled={exportableOutputs.length === 0 || running}
              className="ml-auto flex items-center gap-1 text-[0.5rem] uppercase tracking-wider text-zinc-400 hover:text-zinc-100 disabled:opacity-30 disabled:hover:text-zinc-400 px-1.5 h-4 border border-zinc-700 hover:border-zinc-500 disabled:hover:border-zinc-700 transition-none shrink-0"
              title={
                exportableOutputs.length === 0
                  ? 'No completed outputs to export'
                  : `Move ${exportableOutputs.length} ${doneOutputs.some(o => o.selected) ? 'selected' : 'converted'} file${exportableOutputs.length === 1 ? '' : 's'} to a chosen folder`
              }
            >
              <FolderInput size={9} />
              Export to…
            </button>
          </div>
        </div>
        <div className="h-0.5 w-full bg-zinc-900 relative overflow-hidden">
          <div
            className="absolute inset-y-0 left-0 bg-zinc-300 transition-[width] duration-150 ease-linear"
            style={{
              width: running && batchInfo
                ? `${Math.min(100, Math.max(0, batchInfo.overall))}%`
                : '0%',
            }}
          />
        </div>
      </div>

      {/* Bootstrap overlay — covers the body during first-run download. */}
      {bootstrap.stage !== 'idle' && (
        <div className="absolute inset-0 top-7 bg-zinc-950/90 backdrop-blur-sm flex flex-col items-center justify-center gap-2 text-center px-6">
          <CloudDownload size={22} className="text-zinc-300" />
          <span className="text-[0.625rem] uppercase tracking-widest text-zinc-200 font-bold">
            {bootstrap.stage === 'downloading' && 'Downloading ffmpeg…'}
            {bootstrap.stage === 'extracting'  && 'Extracting ffmpeg…'}
            {bootstrap.stage === 'failed'      && 'Bootstrap failed'}
          </span>
          {bootstrap.stage === 'downloading' && bootstrap.total ? (
            <>
              <div className="w-[220px] h-1 bg-zinc-800 overflow-hidden">
                <div
                  className="h-full bg-zinc-300 transition-[width] duration-150 ease-linear"
                  style={{ width: `${Math.min(100, bootstrap.percent ?? 0)}%` }}
                />
              </div>
              <span className="text-[0.5rem] tabular-nums text-zinc-500">
                {(bootstrap.percent ?? 0).toFixed(0)}% · {formatBytes(bootstrap.bytes ?? 0)} / {formatBytes(bootstrap.total)}
              </span>
            </>
          ) : (
            <span className="text-[0.5rem] text-zinc-500 max-w-[260px]">
              {bootstrap.stage === 'failed'
                ? bootstrap.message ?? 'Check Settings → Processing for the lathe.exe path.'
                : 'First-run setup. From the official BtbN GPL build. Cached next to lathe.exe.'}
            </span>
          )}
        </div>
      )}

    </div>
  );
}

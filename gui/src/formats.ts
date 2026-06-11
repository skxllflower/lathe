// Extension sets + kind predicates for Lathe's input gate. Trimmed fork
// of WAVdesk's audioFormats.ts (the explorer-classifier half stays
// there). Everything listed decodes through ffmpeg, except RAW which
// demosaics through LibRaw inside lathe.

export const AUDIO_EXTS: ReadonlyArray<string> = [
  '.wav', '.wave',
  '.aif', '.aiff',
  '.flac',
  '.mp3',
  '.ogg', '.oga',
  '.m4a',
  '.aac',
  '.opus',
  '.wma',
  '.ape',
  '.mp2', '.mp1',
  '.mpc',
  '.ac3',
  '.dts',
  '.amr',
  '.spx',
  '.ra',
  '.wv',
  '.tta',
  '.tak',
  '.caf',
];

export const IMAGE_EXTS: ReadonlyArray<string> = [
  '.jpg', '.jpeg', '.png', '.gif', '.webp', '.bmp',
  '.tiff', '.tif', '.heic', '.heif', '.avif', '.svg', '.ico',
];

export const VIDEO_EXTS: ReadonlyArray<string> = [
  '.mp4', '.m4v', '.mov', '.mkv', '.webm', '.avi', '.wmv',
  '.flv', '.mpg', '.mpeg', '.ogv', '.mts', '.m2ts', '.3gp',
];

export const RAW_EXTS: ReadonlyArray<string> = [
  '.cr2', '.cr3', '.crw',
  '.nef', '.nrw',
  '.arw', '.srf', '.sr2',
  '.dng',
  '.raf',
  '.orf',
  '.rw2',
  '.pef',
  '.srw',
  '.dcr', '.kdc',
  '.erf',
  '.mef',
  '.mrw',
  '.mos',
  '.x3f',
  '.3fr', '.fff',
  '.iiq',
  '.rwl',
  '.gpr',
];

function endsWithAny(lower: string, exts: ReadonlyArray<string>): boolean {
  for (const ext of exts) {
    if (lower.endsWith(ext)) return true;
  }
  return false;
}

export function isAudioPath(path: string | undefined | null): boolean {
  if (!path) return false;
  return endsWithAny(path.toLowerCase(), AUDIO_EXTS);
}

export function isImagePath(path: string | undefined | null): boolean {
  if (!path) return false;
  return endsWithAny(path.toLowerCase(), IMAGE_EXTS);
}

export function isVideoPath(path: string | undefined | null): boolean {
  if (!path) return false;
  return endsWithAny(path.toLowerCase(), VIDEO_EXTS);
}

export function isRawPath(path: string | undefined | null): boolean {
  if (!path) return false;
  return endsWithAny(path.toLowerCase(), RAW_EXTS);
}

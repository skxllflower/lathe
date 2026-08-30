// About window body (route ?wd=about). Static app identity plus the
// third-party license notices Lathe is obliged to surface (FFmpeg, LibRaw).
// Opened from the status-bar info button (aboutWindow.ts). Closed via the
// titlebar X or Esc. URLs render as selectable text — there's no URL-opener
// plugin wired up, and copy-to-clipboard is enough for a notices panel.

import React, { useEffect, useState } from 'react';
import { invoke } from '@tauri-apps/api/core';
import { getCurrentWindow } from '@tauri-apps/api/window';
import { getVersion } from '@tauri-apps/api/app';
import { X } from 'lucide-react';
import appIcon from './assets/app-icon.svg';

const close = () => { void getCurrentWindow().close().catch(() => {}); };

function Notice({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <div className="flex flex-col gap-1">
      <span className="text-[0.5625rem] font-bold uppercase tracking-wider text-zinc-300">{title}</span>
      <p className="text-[0.625rem] leading-relaxed text-zinc-500">{children}</p>
    </div>
  );
}

function Url({ href }: { href: string }) {
  return (
    <span
      onClick={() => { void invoke('os_open_url', { url: href }); }}
      className="text-emerald-500/80 hover:text-emerald-300 hover:underline cursor-pointer break-all"
    >
      {href}
    </span>
  );
}

export default function AboutApp() {
  // Runtime version off tauri.conf.json, never a hardcoded constant.
  const [version, setVersion] = useState('');
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') { e.preventDefault(); close(); }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, []);
  useEffect(() => { let live = true; getVersion().then(v => { if (live) setVersion(v); }).catch(() => {}); return () => { live = false; }; }, []);

  return (
    <div className="h-screen flex flex-col font-mono select-none text-zinc-300 bg-[#09090b] overflow-hidden">
      {/* Titlebar */}
      <div
        data-tauri-drag-region
        className="h-7 bg-zinc-950 border-b border-zinc-800 flex items-center px-2 shrink-0"
      >
        <span
          data-tauri-drag-region
          className="text-[0.625rem] font-bold uppercase tracking-tight text-zinc-400"
        >
          About
        </span>
        <button
          onClick={close}
          className="ml-auto text-zinc-500 hover:text-zinc-100 hover:bg-zinc-800 p-0.5 transition-none cursor-pointer"
          title="Close"
        >
          <X size={11} />
        </button>
      </div>

      {/* Identity */}
      <div className="flex flex-col items-center gap-0.5 pt-5 pb-4 shrink-0 border-b border-zinc-900">
        <img src={appIcon} alt="" draggable={false} className="w-14 h-14 mb-2" />
        <span className="text-[1.125rem] font-bold tracking-[0.3em] text-zinc-100 pl-[0.3em]">LATHE</span>
        <span className="text-[0.5625rem] uppercase tracking-wider text-zinc-500">by Vacant Systems</span>
        <span className="text-[0.5625rem] tabular-nums text-zinc-600">{version ? `Version ${version}` : ' '}</span>
      </div>

      {/* Notices — scrollable */}
      <div className="flex-1 min-h-0 overflow-y-auto px-4 py-3 flex flex-col gap-3">
        <p className="text-[0.625rem] leading-relaxed text-zinc-500">
          Lathe is a standalone media converter. It builds on open-source
          software, used and distributed under the terms below.
        </p>

        {/* Two DIFFERENT FFmpegs, and only the first carries a source
            obligation on us: the libav shared libraries are LINKED into the
            core and ship inside the app, while the converter program is
            downloaded at runtime and merely invoked (aggregation). The single
            notice this replaced described only the second, so the component
            that actually needs its source offered was the one going
            unmentioned. Version + archive URL are in lockstep with
            tools/build-ffmpeg-lgpl-mac.sh (FFMPEG_VERSION) and
            THIRD_PARTY_NOTICES.txt: bump all three together. */}
        <Notice title="FFmpeg (bundled libraries)">
          Lathe links the FFmpeg libraries avformat, avcodec, avutil, swscale
          and swresample, which ship as shared libraries beside the Lathe core.
          They are an unmodified LGPL v2.1 build of FFmpeg 8.1.2, configured
          without GPL or nonfree components. The corresponding source is the
          official release archive:{' '}
          <Url href="https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz" />
          {' '}The LGPL text and the exact configure arguments used ship with
          the app, in its licenses folder; applying those arguments to that
          archive reproduces the libraries as shipped. Being dynamically
          linked, they are user-replaceable: you may substitute your own
          compatible builds beside the Lathe core executable.
        </Notice>

        <Notice title="FFmpeg (converter program)">
          Separately, Lathe downloads the stand-alone FFmpeg program from the
          project's official builds on first run and invokes it as a separate
          program. That build is licensed under the GPL and is not linked into
          Lathe. FFmpeg is a trademark of Fabrice Bellard. Project and full
          license texts: <Url href="https://ffmpeg.org" />
        </Notice>

        <Notice title="LibRaw">
          RAW image decoding is provided by the LibRaw library, included
          under the Common Development and Distribution License (CDDL)
          v1.0. LibRaw source is available at <Url href="https://www.libraw.org" />
        </Notice>

        <Notice title="Other components">
          Lathe also includes software distributed under permissive licenses
          (MIT, Apache-2.0, BSD), including the Tauri framework, the Rust
          crate ecosystem, and React. Full license texts are available on
          request.
        </Notice>

        <div className="border-t border-zinc-900 pt-3 mt-1">
          <p className="text-[0.5625rem] leading-relaxed text-zinc-600">
            © 2026 Vacant Systems. All rights reserved. Lathe is proprietary
            software.
          </p>
        </div>
      </div>
    </div>
  );
}

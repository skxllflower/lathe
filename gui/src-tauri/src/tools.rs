// Lathe spawn + stream plumbing — fork of WAVdesk's external_tools.rs
// (the lathe half). The two stay behaviorally aligned: NDJSON events on
// `lathe-event`, kill-on-close job objects on every spawn, no-clobber
// unique_output on non-overwrite converts. When this logic changes in
// either home, port the change to the other (shared-crate promotion is
// the planned fix for the duplication).

use serde_json::Value;
use std::collections::HashMap;
use std::io::{BufRead, BufReader};
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex, OnceLock};
use tauri::{AppHandle, Emitter};

#[cfg(target_os = "windows")]
use std::os::windows::process::CommandExt;

#[cfg(target_os = "windows")]
const CREATE_NO_WINDOW: u32 = 0x0800_0000;

type JobMap = Mutex<HashMap<String, Arc<Mutex<Child>>>>;

static LATHE_JOBS: OnceLock<JobMap> = OnceLock::new();

fn lathe_jobs() -> &'static JobMap {
    LATHE_JOBS.get_or_init(|| Mutex::new(HashMap::new()))
}

// Resolution order:
//   1) {NAME}_EXE env override (shell-launched testing)
//   2) sibling dev checkout at %USERPROFILE%\Dev\{name}\build\Debug
//   3) installed locations (exe-relative siblings, then the default
//      vendor spaces — Program Files\Vacant Systems on Windows)
// The standalone app has no Settings field yet, so `configured` is
// normally empty; the parameter stays for parity with WAVdesk's IPC.
// Release first: a Debug-built lathe converts/decodes far below
// realtime — when both configurations exist the Release build wins.
fn dev_tool_fallbacks(name: &str) -> Vec<PathBuf> {
    let Some(home) = std::env::var_os("USERPROFILE") else {
        return Vec::new();
    };
    let base = PathBuf::from(home).join("Dev").join(name).join("build");
    vec![
        base.join("Release").join(format!("{}.exe", name)),
        base.join("Debug").join(format!("{}.exe", name)),
    ]
}

fn tool_dir_name(name: &str) -> String {
    let mut chars = name.chars();
    match chars.next() {
        Some(f) => f.to_uppercase().collect::<String>() + chars.as_str(),
        None => String::new(),
    }
}

fn installed_tool_fallbacks(name: &str) -> Vec<PathBuf> {
    let exe_name = if cfg!(windows) {
        format!("{}.exe", name)
    } else {
        name.to_string()
    };
    let app_dir = tool_dir_name(name);
    let mut out = Vec::new();
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            // Bundled core: tauri resources install into <install>/coredist/
            // next to the GUI exe (NSIS resource_dir == install root).
            out.push(dir.join("coredist").join(&exe_name));
            // Installed layout: the CLI ships right next to this GUI exe.
            out.push(dir.join(&exe_name));
            if let Some(vendor) = dir.parent() {
                out.push(vendor.join(&app_dir).join(&exe_name));
            }
        }
    }
    #[cfg(windows)]
    {
        let pf = std::env::var_os("ProgramFiles")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from(r"C:\Program Files"));
        let vendor = pf.join("Vacant Systems");
        out.push(vendor.join(&app_dir).join(&exe_name));
        out.push(vendor.join(&exe_name));
    }
    #[cfg(target_os = "macos")]
    {
        let vendor = PathBuf::from("/Library/Application Support/Vacant Systems");
        out.push(vendor.join(&app_dir).join(&exe_name));
        if let Some(home) = std::env::var_os("HOME") {
            out.push(
                PathBuf::from(home)
                    .join("Library/Application Support/Vacant Systems")
                    .join(&app_dir)
                    .join(&exe_name),
            );
        }
    }
    out
}

fn find_tool_binary(name: &str, configured: &str) -> Result<PathBuf, String> {
    if !configured.trim().is_empty() {
        let pb = PathBuf::from(configured.trim());
        if pb.exists() {
            return Ok(pb);
        }
        return Err(format!(
            "{}: configured path does not exist: {}",
            name,
            pb.display()
        ));
    }
    let env_var = format!("{}_EXE", name.to_uppercase());
    if let Ok(p) = std::env::var(&env_var) {
        let pb = PathBuf::from(&p);
        if pb.exists() {
            return Ok(pb);
        }
    }
    // Dev builds prefer a locally-built binary; a RELEASE/installed build must
    // NOT — a stray %USERPROFILE%\Dev\<tool>\build checkout (possibly a stale,
    // pre-feature core) would otherwise SHADOW the freshly-installed coredist
    // binary and run old/missing commands.
    if cfg!(debug_assertions) {
        for cand in dev_tool_fallbacks(name) {
            if cand.exists() {
                return Ok(cand);
            }
        }
    }
    for cand in installed_tool_fallbacks(name) {
        if cand.exists() {
            return Ok(cand);
        }
    }
    Err(format!(
        "{}.exe not found. Set {} env, reinstall {}, or build at {}.",
        name,
        env_var,
        tool_dir_name(name),
        format!(r"%USERPROFILE%\Dev\{}\build", name),
    ))
}

/// Resolve this app's own CLI core (lathe.exe) for self-registration into the
/// shared discovery manifest. Returns None if it can't be found.
pub(crate) fn resolve_self_core() -> Option<PathBuf> {
    find_tool_binary("lathe", "").ok()
}

fn spawn_tool(
    binary: PathBuf,
    args: Vec<String>,
) -> Result<(Child, std::process::ChildStdout), String> {
    let mut cmd = Command::new(&binary);
    cmd.args(&args).stdout(Stdio::piped()).stderr(Stdio::piped());

    #[cfg(target_os = "windows")]
    {
        cmd.creation_flags(CREATE_NO_WINDOW);
    }

    let child = cmd
        .spawn()
        .map_err(|e| format!("failed to spawn {}: {}", binary.display(), e))?;
    let mut child = child;
    crate::job_object::assign_child(&child);
    let stdout = child.stdout.take().ok_or_else(|| "no stdout".to_string())?;
    Ok((child, stdout))
}

fn run_reader(
    tool: &'static str,
    job_id: String,
    window_label: String,
    app: AppHandle,
    stdout: std::process::ChildStdout,
    child_arc: Arc<Mutex<Child>>,
    jobs: &'static JobMap,
) {
    std::thread::spawn(move || {
        let event_name = format!("{}-event", tool);
        let reader = BufReader::new(stdout);
        for line in reader.lines() {
            let Ok(line) = line else { break };
            if line.is_empty() {
                continue;
            }
            let payload = match serde_json::from_str::<Value>(&line) {
                Ok(v) => serde_json::json!({
                    "tool": tool,
                    "jobId": job_id,
                    "event": v,
                }),
                Err(_) => serde_json::json!({
                    "tool": tool,
                    "jobId": job_id,
                    "event": { "type": "raw", "line": line },
                }),
            };
            let _ = app.emit_to(window_label.as_str(), event_name.as_str(), payload);
        }

        let exit_code: i32 = {
            let mut guard = child_arc.lock().unwrap();
            match guard.wait() {
                Ok(status) => status.code().unwrap_or(-1),
                Err(_) => -1,
            }
        };

        let final_payload = serde_json::json!({
            "tool": tool,
            "jobId": job_id,
            "event": { "type": "exit", "code": exit_code },
        });
        let _ = app.emit_to(window_label.as_str(), event_name.as_str(), final_payload);

        // Overwrite-in-place: on a CLEAN exit, remove the original source a
        // format change left beside the new file. Never on a non-zero exit —
        // the original is the user's only copy if the convert failed/cancelled.
        if let Ok(mut m) = lathe_overwrite_originals().lock() {
            if let Some(orig) = m.remove(&job_id) {
                if exit_code == 0 {
                    let _ = std::fs::remove_file(&orig);
                }
            }
        }

        if let Ok(mut map) = jobs.lock() {
            map.remove(&job_id);
        }
        if let Ok(mut m) = lathe_outputs().lock() {
            m.remove(&job_id);
        }
    });
}

#[derive(serde::Deserialize, Default, Debug)]
#[serde(rename_all = "camelCase", default)]
pub struct LatheOptions {
    pub sample_rate: String,
    pub bit_depth: String,
    pub bitrate: String,
    pub vbr_quality: String,
    pub compression_level: String,
    pub quality: String,
    pub max_height: String,
    pub preset: String,
    pub fps: String,
    pub colors: String,
    pub copy: bool,
}

fn push_lathe_options(args: &mut Vec<String>, o: &LatheOptions) {
    if !o.sample_rate.is_empty()       { args.push(format!("--sample-rate={}", o.sample_rate)); }
    if !o.bit_depth.is_empty()         { args.push(format!("--bit-depth={}", o.bit_depth)); }
    if !o.bitrate.is_empty()           { args.push(format!("--bitrate={}", o.bitrate)); }
    if !o.vbr_quality.is_empty()       { args.push(format!("--vbr-quality={}", o.vbr_quality)); }
    if !o.compression_level.is_empty() { args.push(format!("--compression-level={}", o.compression_level)); }
    if !o.quality.is_empty()           { args.push(format!("--quality={}", o.quality)); }
    if !o.max_height.is_empty()        { args.push(format!("--max-height={}", o.max_height)); }
    if !o.preset.is_empty()            { args.push(format!("--preset={}", o.preset)); }
    if !o.fps.is_empty()               { args.push(format!("--fps={}", o.fps)); }
    if !o.colors.is_empty()            { args.push(format!("--colors={}", o.colors)); }
    if o.copy                          { args.push("--copy".to_string()); }
}

// Output path per in-flight job, so a cancel can delete the partial the
// hard kill leaves behind. Non-overwrite jobs only.
fn lathe_outputs() -> &'static Mutex<HashMap<String, String>> {
    static OUTPUTS: OnceLock<Mutex<HashMap<String, String>>> = OnceLock::new();
    OUTPUTS.get_or_init(|| Mutex::new(HashMap::new()))
}

// Original source path per overwrite-in-place job, removed on SUCCESS only. A
// format change writes <stem>.<newext> beside the source, so "overwrite in
// place" must delete the differently-named original — but never on
// failure/cancel (it's the user's only copy).
fn lathe_overwrite_originals() -> &'static Mutex<HashMap<String, String>> {
    static ORIG: OnceLock<Mutex<HashMap<String, String>>> = OnceLock::new();
    ORIG.get_or_init(|| Mutex::new(HashMap::new()))
}

// Pick a non-colliding output path: returns `output` if free, else
// "<stem> (2).<ext>", "<stem> (3).<ext>", … in the same directory.
fn unique_output(output: &str) -> String {
    let p = std::path::Path::new(output);
    if !p.exists() {
        return output.to_string();
    }
    let dir = p.parent().unwrap_or_else(|| std::path::Path::new("."));
    let stem = p.file_stem().and_then(|s| s.to_str()).unwrap_or("output");
    let ext = p
        .extension()
        .and_then(|e| e.to_str())
        .map(|e| format!(".{e}"))
        .unwrap_or_default();
    for n in 2..=9999u32 {
        let cand = dir.join(format!("{stem} ({n}){ext}"));
        if !cand.exists() {
            return cand.to_string_lossy().into_owned();
        }
    }
    output.to_string()
}

#[tauri::command]
pub async fn lathe_convert(
    app: AppHandle,
    window_label: String,
    job_id: String,
    binary_path: String,
    input: String,
    output: String,
    options: LatheOptions,
    overwrite: Option<bool>,
) -> Result<(), String> {
    let bin = find_tool_binary("lathe", &binary_path)?;

    let is_overwrite = overwrite.unwrap_or(false);
    let output = if is_overwrite {
        output
    } else {
        unique_output(&output)
    };

    if !is_overwrite {
        if let Ok(mut m) = lathe_outputs().lock() {
            m.insert(job_id.clone(), output.clone());
        }
    }

    // Overwrite-in-place across a format change leaves the differently-named
    // original beside the new file — record it so run_reader removes it on a
    // clean exit (same-path same-format already clobbered via ffmpeg -y, so the
    // path compare keeps us from deleting the file we just wrote).
    if is_overwrite && !input.eq_ignore_ascii_case(output.as_str()) {
        if let Ok(mut m) = lathe_overwrite_originals().lock() {
            m.insert(job_id.clone(), input.clone());
        }
    }

    let mut args = vec!["convert".to_string(), input, output];
    push_lathe_options(&mut args, &options);
    let (child, stdout) = match spawn_tool(bin, args) {
        Ok(v) => v,
        Err(e) => {
            if let Ok(mut m) = lathe_outputs().lock() {
                m.remove(&job_id);
            }
            return Err(e);
        }
    };
    let child_arc = Arc::new(Mutex::new(child));

    if let Ok(mut map) = lathe_jobs().lock() {
        map.insert(job_id.clone(), child_arc.clone());
    }

    run_reader("lathe", job_id, window_label, app, stdout, child_arc, lathe_jobs());
    Ok(())
}

#[tauri::command]
pub fn lathe_cancel(job_id: String) -> Result<(), String> {
    // Kill only a still-running child — a cancel racing the job's finish
    // must not delete a COMPLETED output, only an interrupted partial.
    let mut killed = false;
    if let Ok(map) = lathe_jobs().lock() {
        if let Some(child_arc) = map.get(&job_id) {
            if let Ok(mut child) = child_arc.lock() {
                if matches!(child.try_wait(), Ok(None) | Err(_)) {
                    let _ = child.kill();
                    killed = true;
                }
            }
        }
    }
    if let Ok(mut m) = lathe_outputs().lock() {
        if let Some(out) = m.remove(&job_id) {
            if killed {
                let _ = std::fs::remove_file(out);
            }
        }
    }
    Ok(())
}

#[tauri::command]
pub async fn lathe_bootstrap(
    app: AppHandle,
    window_label: String,
    job_id: String,
    binary_path: String,
) -> Result<(), String> {
    let bin = find_tool_binary("lathe", &binary_path)?;
    let args = vec!["bootstrap".to_string()];
    let (child, stdout) = spawn_tool(bin, args)?;
    let child_arc = Arc::new(Mutex::new(child));
    if let Ok(mut map) = lathe_jobs().lock() {
        map.insert(job_id.clone(), child_arc.clone());
    }
    run_reader("lathe", job_id, window_label, app, stdout, child_arc, lathe_jobs());
    Ok(())
}

/// Recursive file collection for folder drops. Files only, sorted,
/// capped; never follows symlinks/junctions. Kind filtering stays in
/// the GUI — this just walks.
#[tauri::command]
pub async fn lathe_collect_dir(dir: String, max_files: usize) -> Result<Vec<String>, String> {
    let cap = max_files.clamp(1, 10_000);
    let res = tauri::async_runtime::spawn_blocking(move || {
        let mut out: Vec<String> = Vec::new();
        let mut stack = vec![std::path::PathBuf::from(&dir)];
        while let Some(d) = stack.pop() {
            let Ok(rd) = std::fs::read_dir(&d) else { continue };
            for entry in rd.flatten() {
                let Ok(ft) = entry.file_type() else { continue };
                if ft.is_symlink() {
                    continue;
                }
                if ft.is_dir() {
                    stack.push(entry.path());
                } else if ft.is_file() {
                    out.push(entry.path().to_string_lossy().into_owned());
                    if out.len() >= cap {
                        out.sort();
                        return out;
                    }
                }
            }
        }
        out.sort();
        out
    })
    .await
    .map_err(|e| format!("lathe_collect_dir join: {e}"))?;
    Ok(res)
}

#[derive(serde::Serialize)]
pub struct ToolBinaryStatus {
    pub resolved: bool,
    pub path:     String,
    pub source:   String, // "configured" | "env" | "dev" | "installed" | "missing"
    pub message:  String,
}

// ASYNC + spawn_blocking: the probe stats several candidate paths (.exists()
// on the configured path, env fallback, dev + installed locations). As a sync
// command Tauri v2 ran those filesystem hits INLINE on the MAIN thread inside
// WebMessageReceived, and it fires at boot via the tool-status init — a stalled
// or network drive would starve the pump (wavdesk converted its same-named
// command for exactly this reason). spawn_blocking keeps main flowing.
#[tauri::command]
pub async fn tool_binary_probe(name: String, configured: String) -> ToolBinaryStatus {
    tauri::async_runtime::spawn_blocking(move || tool_binary_probe_impl(name, configured))
        .await
        .unwrap_or_else(|e| ToolBinaryStatus {
            resolved: false,
            path:     String::new(),
            source:   "missing".into(),
            message:  format!("probe join error: {e}"),
        })
}

fn tool_binary_probe_impl(name: String, configured: String) -> ToolBinaryStatus {
    let trimmed = configured.trim();
    if !trimmed.is_empty() {
        let pb = PathBuf::from(trimmed);
        if pb.exists() {
            return ToolBinaryStatus {
                resolved: true,
                path:     pb.display().to_string(),
                source:   "configured".into(),
                message:  String::new(),
            };
        }
        return ToolBinaryStatus {
            resolved: false,
            path:     pb.display().to_string(),
            source:   "missing".into(),
            message:  format!("configured path does not exist: {}", pb.display()),
        };
    }
    let env_var = format!("{}_EXE", name.to_uppercase());
    if let Ok(p) = std::env::var(&env_var) {
        let pb = PathBuf::from(&p);
        if pb.exists() {
            return ToolBinaryStatus {
                resolved: true,
                path:     pb.display().to_string(),
                source:   "env".into(),
                message:  format!("from {}", env_var),
            };
        }
    }
    for cand in dev_tool_fallbacks(&name) {
        if cand.exists() {
            return ToolBinaryStatus {
                resolved: true,
                path:     cand.display().to_string(),
                source:   "dev".into(),
                message:  "dev fallback".into(),
            };
        }
    }
    for cand in installed_tool_fallbacks(&name) {
        if cand.exists() {
            return ToolBinaryStatus {
                resolved: true,
                path:     cand.display().to_string(),
                source:   "installed".into(),
                message:  "default install location".into(),
            };
        }
    }
    ToolBinaryStatus {
        resolved: false,
        path:     String::new(),
        source:   "missing".into(),
        message:  format!(
            "{}.exe not found. Set {} env, reinstall, or build at {}.",
            name,
            env_var,
            format!(r"%USERPROFILE%\Dev\{}\build", name),
        ),
    }
}

/// Tear the whole app down: kill every tracked child + exit. The main
/// window's close flow calls this so the pre-spawned drag overlay never
/// keeps a headless app alive, and no ffmpeg tree outlives the GUI.
/// lathe_cancel already removed any partial outputs for cancelled jobs.
#[tauri::command]
pub fn app_exit(app: AppHandle) {
    if let Ok(mut map) = lathe_jobs().lock() {
        for (_, child) in map.drain() {
            if let Ok(mut c) = child.lock() {
                let _ = c.kill();
            }
        }
    }
    app.exit(0);
}

#[tauri::command]
pub async fn fs_stat(path: String) -> Result<u64, String> {
    std::fs::metadata(&path)
        .map(|m| m.len())
        .map_err(|e| format!("fs_stat {}: {}", path, e))
}

#[tauri::command]
pub async fn fs_is_dir(path: String) -> Result<bool, String> {
    Ok(std::fs::metadata(&path).map(|m| m.is_dir()).unwrap_or(false))
}

/// Reveal a file in the OS file manager. Fork of WAVdesk's os_reveal_path
/// — see that function for the explorer.exe raw_arg quoting rationale.
#[tauri::command]
pub fn os_reveal_path(path: String) -> Result<(), String> {
    if path.is_empty() {
        return Err("os_reveal_path: empty path".to_string());
    }
    #[cfg(target_os = "windows")]
    {
        // explorer parses its own command line: the comma must sit OUTSIDE
        // the quoted region, which Rust's default arg-quoting can't produce.
        let normalized = path.replace('/', "\\");
        let raw = format!("/select,\"{}\"", normalized);
        Command::new("explorer")
            .raw_arg(&raw)
            .spawn()
            .map(|_| ())
            .map_err(|e| format!("explorer /select spawn: {}", e))
    }
    #[cfg(target_os = "macos")]
    {
        Command::new("open")
            .args(["-R", &path])
            .spawn()
            .map(|_| ())
            .map_err(|e| format!("open -R spawn: {}", e))
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        let parent = std::path::Path::new(&path)
            .parent()
            .ok_or_else(|| "os_reveal_path: no parent directory".to_string())?;
        Command::new("xdg-open")
            .arg(parent)
            .spawn()
            .map(|_| ())
            .map_err(|e| format!("xdg-open spawn: {}", e))
    }
}

/// Open an http/https URL in the user's default browser. Used by the About
/// window's license links. Scheme-gated so a stray value can't launch an
/// arbitrary protocol handler.
#[tauri::command]
pub fn os_open_url(url: String) -> Result<(), String> {
    if !(url.starts_with("http://") || url.starts_with("https://")) {
        return Err("os_open_url: only http/https URLs are allowed".to_string());
    }
    #[cfg(target_os = "windows")]
    {
        Command::new("explorer")
            .arg(&url)
            .spawn()
            .map(|_| ())
            .map_err(|e| format!("explorer url spawn: {}", e))
    }
    #[cfg(target_os = "macos")]
    {
        Command::new("open")
            .arg(&url)
            .spawn()
            .map(|_| ())
            .map_err(|e| format!("open url spawn: {}", e))
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        Command::new("xdg-open")
            .arg(&url)
            .spawn()
            .map(|_| ())
            .map_err(|e| format!("xdg-open url spawn: {}", e))
    }
}

#[derive(serde::Serialize)]
pub struct MoveEntry {
    pub src:     String,
    pub dst:     String,
    pub success: bool,
}

#[derive(serde::Serialize)]
pub struct MoveResult {
    pub entries: Vec<MoveEntry>,
}

/// Move files into `dest_dir` (the Export To… flow). No-clobber: an
/// existing name lands on a " (2)" sibling. rename first, copy+delete
/// fallback for cross-volume moves. Same response shape as WAVdesk's
/// fs_move so the ported frontend needs no changes.
#[tauri::command]
pub async fn fs_move(srcs: Vec<String>, dest_dir: String) -> Result<MoveResult, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let dest = std::path::PathBuf::from(&dest_dir);
        let mut entries = Vec::new();
        for src in srcs {
            let name = std::path::Path::new(&src)
                .file_name()
                .map(|n| n.to_string_lossy().into_owned())
                .unwrap_or_default();
            if name.is_empty() {
                entries.push(MoveEntry { src, dst: String::new(), success: false });
                continue;
            }
            let dst = unique_output(&dest.join(&name).to_string_lossy());
            let ok = std::fs::rename(&src, &dst).is_ok()
                || (std::fs::copy(&src, &dst).is_ok() && std::fs::remove_file(&src).is_ok());
            entries.push(MoveEntry { src, dst, success: ok });
        }
        Ok(MoveResult { entries })
    })
    .await
    .map_err(|e| format!("fs_move join: {e}"))?
}

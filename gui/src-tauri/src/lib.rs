#[cfg(target_os = "windows")]
mod chip_bitmap_server;
mod drag_overlay;
#[cfg(target_os = "windows")]
mod explorer_folder;
mod job_object;
#[cfg(target_os = "windows")]
mod native_drag_chip;
mod os_drag;
mod tools;

use tauri::{WebviewUrl, WebviewWindowBuilder};

pub fn run() {
    // Kill-on-close job from the very start, so lathe.exe + its ffmpeg
    // children (and the WebView2 tree) can never outlive a crash.
    job_object::assign_self();

    tauri::Builder::default()
        .plugin(tauri_plugin_single_instance::init(|app, _argv, _cwd| {
            use tauri::Manager;
            if let Some(w) = app.get_webview_window("main") {
                let _ = w.set_focus();
            }
        }))
        .plugin(tauri_plugin_dialog::init())
        .setup(|app| {
            // Pre-spawn the drag-overlay window hidden — it's both the OS
            // drag SOURCE (see os_drag.rs) and the chip's render surface.
            // Recipe mirrors WAVdesk's: transparent, AOT, passthrough,
            // 1000x200 covers both chip variants, shadow off so the OS
            // never outlines the transparent surface.
            let overlay_url = WebviewUrl::App("/?wd=drag-overlay".into());
            let overlay_builder = WebviewWindowBuilder::new(app, "drag-overlay", overlay_url)
                .title("Lathe Drag")
                .inner_size(1000.0, 200.0)
                .position(0.0, 0.0)
                .decorations(false)
                .transparent(true)
                .background_color(tauri::utils::config::Color(0, 0, 0, 0))
                .shadow(false)
                .always_on_top(true)
                .skip_taskbar(true)
                .focused(false)
                .visible(false)
                .resizable(false)
                .closable(false)
                .minimizable(false)
                .maximizable(false);
            match overlay_builder.build() {
                Ok(overlay) => {
                    if let Err(e) = overlay.set_ignore_cursor_events(true) {
                        eprintln!("drag-overlay set_ignore_cursor_events failed: {e}");
                    }
                }
                Err(e) => eprintln!("drag-overlay creation failed: {e}"),
            }
            #[cfg(target_os = "windows")]
            chip_bitmap_server::start();
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            tools::lathe_convert,
            tools::lathe_cancel,
            tools::lathe_collect_dir,
            tools::lathe_bootstrap,
            tools::tool_binary_probe,
            tools::fs_stat,
            tools::fs_is_dir,
            tools::os_reveal_path,
            tools::fs_move,
            tools::app_exit,
            drag_overlay::drag_overlay_start,
            drag_overlay::drag_overlay_stop,
            drag_overlay::drag_overlay_last_cursor,
            drag_overlay::drag_overlay_modifier_state,
            drag_overlay::drag_chip_set_bitmap,
            drag_overlay::get_chip_bitmap_endpoint,
            os_drag::start_os_file_drag,
        ])
        .run(tauri::generate_context!())
        .expect("error while running Lathe");
}

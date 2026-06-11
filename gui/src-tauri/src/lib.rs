mod job_object;
mod tools;

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
        ])
        .run(tauri::generate_context!())
        .expect("error while running Lathe");
}

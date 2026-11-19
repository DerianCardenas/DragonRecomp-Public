// DragonRecomp Studio — Tauri v2 UI Prototype (stub, not yet wired to C++ backend)
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    tauri::Builder::default()
        .run(tauri::generate_context!())
        .expect("error while running DragonRecomp Studio Tauri prototype");
}

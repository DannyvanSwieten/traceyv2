use tauri::{
    menu::{Menu, MenuItem, Submenu},
    App, Emitter,
};

pub fn setup(app: &mut App) -> tauri::Result<()> {
    let handle = app.handle();

    // Create menu items using the simpler MenuItem::with_id
    let import_item = MenuItem::with_id(handle, "import", "Import...", true, Some("CmdOrCtrl+I"))?;
    let export_item = MenuItem::with_id(handle, "export", "Export Image...", true, Some("CmdOrCtrl+E"))?;

    // Create File submenu
    let file_menu = Submenu::with_id_and_items(
        handle,
        "file",
        "File",
        true,
        &[&import_item, &export_item],
    )?;

    // Build the full menu
    let menu = Menu::with_items(handle, &[&file_menu])?;

    // Set menu on the app
    app.set_menu(menu)?;

    // Set up event handlers
    app.on_menu_event(move |app_handle, event| {
        println!("Menu event received: {:?}", event.id());
        let id = event.id().as_ref();
        match id {
            "import" => {
                println!("Import menu clicked!");
                // Emit to all windows
                if let Err(e) = app_handle.emit_to(tauri::EventTarget::any(), "menu-import", ()) {
                    eprintln!("Failed to emit menu-import: {}", e);
                } else {
                    println!("menu-import event emitted successfully");
                }
            }
            "export" => {
                println!("Export menu clicked!");
                if let Err(e) = app_handle.emit_to(tauri::EventTarget::any(), "menu-export", ()) {
                    eprintln!("Failed to emit menu-export: {}", e);
                } else {
                    println!("menu-export event emitted successfully");
                }
            }
            _ => {
                println!("Unknown menu event: {}", id);
            }
        }
    });

    Ok(())
}

mod bash;
mod niri_windows;
mod niri_workspaces;

use std::thread;
use std::time::Duration;

fn main() {
    loop {
        match niri_windows::get_floating_windows() {
            Ok(windows) => match niri_workspaces::get_current_workspace() {
                Ok(workspace_id) => {
                    for w in windows {
                        if let Err(e) = niri_windows::move_window_to_workspace(w.id, workspace_id) {
                            eprintln!("Failed to move window {}: {}", w.id, e);
                        }
                    }
                }
                Err(e) => eprintln!("Failed to get current workspace: {e}"),
            },
            Err(e) => eprintln!("Failed to get floating windows: {e}"),
        }

        thread::sleep(Duration::from_millis(200));
    }
}

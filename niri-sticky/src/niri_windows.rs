use crate::bash;
use regex::Regex;

#[derive(Debug)]
pub struct Window {
    pub id: u32,
    pub title: String,
    pub app_id: String,
    pub is_floating: bool,
    pub pid: u32,
    pub workspace_id: u32,
}

pub fn get_floating_windows() -> Result<Vec<Window>, String> {
    let output = bash::run_command("niri msg windows")
        .map_err(|e| format!("Failed to list windows: {e}"))?;
    parse_windows(&output).map(|ws| ws.into_iter().filter(|w| w.is_floating).collect())
}

pub fn move_window_to_workspace(window_id: u32, workspace_id: u32) -> Result<(), String> {
    let cmd =
        format!("niri msg action move-window-to-workspace --window-id {window_id} {workspace_id}");
    bash::run_command(&cmd)
        .map(|_| ())
        .map_err(|e| e.to_string())
}

fn parse_windows(input: &str) -> Result<Vec<Window>, String> {
    let mut windows = Vec::new();
    let lines: Vec<&str> = input.lines().collect();
    let mut current: Option<Window> = None;

    let id_re = Regex::new(r"Window ID (\d+)(: \(focused\))?").unwrap();
    let title_re = Regex::new(r#"Title: "(.*)""#).unwrap();
    let app_id_re = Regex::new(r#"App ID: "(.*)""#).unwrap();
    let float_re = Regex::new(r"Is floating: (yes|no)").unwrap();
    let pid_re = Regex::new(r"PID: (\d+)").unwrap();
    let workspace_re = Regex::new(r"Workspace ID: (\d+)").unwrap();

    for line in lines {
        let line = line.trim();
        if line.is_empty() {
            continue;
        }

        if let Some(cap) = id_re.captures(line) {
            if let Some(w) = current.take() {
                windows.push(w);
            }

            current = Some(Window {
                id: cap[1].parse().unwrap(),
                title: String::new(),
                app_id: String::new(),
                is_floating: false,
                pid: 0,
                workspace_id: 0,
            });
        } else if let Some(ref mut w) = current {
            if let Some(cap) = title_re.captures(line) {
                w.title = cap[1].to_string();
            } else if let Some(cap) = app_id_re.captures(line) {
                w.app_id = cap[1].to_string();
            } else if let Some(cap) = float_re.captures(line) {
                w.is_floating = &cap[1] == "yes";
            } else if let Some(cap) = pid_re.captures(line) {
                w.pid = cap[1].parse().unwrap_or(0);
            } else if let Some(cap) = workspace_re.captures(line) {
                w.workspace_id = cap[1].parse().unwrap_or(0);
            }
        }
    }

    if let Some(w) = current {
        windows.push(w);
    }

    Ok(windows)
}

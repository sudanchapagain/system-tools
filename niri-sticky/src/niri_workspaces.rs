use crate::bash;
use regex::Regex;

pub fn get_current_workspace() -> Result<u32, String> {
    let output = bash::run_command("niri msg workspaces")
        .map_err(|e| format!("Failed to run command: {e}"))?;

    let re = Regex::new(r"\*\s*(\d+)").unwrap();
    for line in output.lines() {
        if let Some(cap) = re.captures(line) {
            if let Ok(id) = cap[1].parse::<u32>() {
                return Ok(id);
            }
        }
    }

    Err("Current workspace not found".into())
}

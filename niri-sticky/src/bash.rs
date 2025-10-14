use std::io;
use std::process::{Command, Stdio};

pub fn run_command(command: &str) -> io::Result<String> {
    let output = Command::new("sh")
        .arg("-c")
        .arg(command)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .output()?;

    if output.status.success() {
        Ok(String::from_utf8_lossy(&output.stdout).to_string())
    } else {
        Err(io::Error::other(String::from_utf8_lossy(&output.stderr)))
    }
}

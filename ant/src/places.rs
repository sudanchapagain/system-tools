#![allow(dead_code)]

use fspp::*;
use piglog::prelude::*;
use piglog::*;
use std::io;

// The setup function for the directories
pub fn setup() -> Result<(), io::Error> {
    let directories = [base(), gens()];

    for i in directories.iter() {
        match directory::create(i) {
            Ok(_o) => info!("Created directory: {}", i.to_string()),
            Err(e) => {
                error!("Failed to create directory: {}", i.to_string());
                return Err(e);
            }
        };
    }

    Ok(())
}

/// The base directory of operations for ant (Legacy)
pub fn base_legacy() -> Path {
    location::home().unwrap().add_str(".ant-base")
}

/// The base directory of operations for ant
pub fn base() -> Path {
    location::state().unwrap().add_str("ant")
}

/// The directory of generations
pub fn gens() -> Path {
    base().add_str("generations")
}

/// User's ant config directory
pub fn base_user() -> Path {
    location::config().unwrap().add_str("ant")
}

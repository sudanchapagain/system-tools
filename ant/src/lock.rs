use colored::Colorize;
use fspp::*;
use piglog::prelude::*;
use std::io;

use crate::places;
use crate::proc;

#[derive(Debug, Clone, Copy, Eq, PartialEq, Ord, PartialOrd, Hash)]
/// State of the lock (on (owned), on (not owned), off)
pub enum LockState {
    /// The lock is off
    Off,
    /// The lock is on, and the owner is this ant process
    OnOwned,
    /// The lock is on, but another ant process is the owner
    OnNotOwned,
}

// Lock other ant sessions from running.
pub fn lock_on() -> Result<(), io::Error> {
    if !is_lock_on() {
        match file::write("", &lock_file_path()) {
            Ok(_) => (),
            Err(e) => {
                piglog::error!("Failed to create locking file! ({e})");

                return Err(e);
            }
        };

        match file::write(&proc::get_proc_id(), &lock_file_owner_path()) {
            Ok(_) => (),
            Err(e) => {
                piglog::error!("Failed to create and write locking owner file! ({e})");

                return Err(e);
            }
        };
    }

    Ok(())
}

#[inline(always)]
pub fn lock_off() -> Result<(), io::Error> {
    lock_off_core(false)
}

#[inline(always)]
pub fn lock_off_force() -> Result<(), io::Error> {
    lock_off_core(true)
}

// Unlock other ant sessions from running.
pub fn lock_off_core(force: bool) -> Result<(), io::Error> {
    if !force && lock_state()? != LockState::OnOwned {
        return Ok(());
    }

    if is_lock_on_without_owner() {
        match fs_action::delete(&lock_file_path()) {
            Ok(_) => (),
            Err(e) => {
                piglog::error!("Failed to delete locking file! ({e})");

                return Err(e);
            }
        };

        match fs_action::delete(&lock_file_owner_path()) {
            Ok(_) => (),
            Err(e) => {
                piglog::error!("Failed to delete locking owner file! ({e})");

                return Err(e);
            }
        };
    }

    Ok(())
}

#[inline(always)]
pub fn lock_state() -> Result<LockState, io::Error> {
    Ok(match (is_lock_on(), is_lock_on_without_owner()) {
        (false, false) => LockState::Off,
        (false, true) => LockState::OnOwned,
        (true, true) => LockState::OnNotOwned,
        _ => unreachable!(),
    })
}

#[inline(always)]
pub fn is_lock_on() -> bool {
    is_lock_on_core(true)
}

#[inline(always)]
fn is_lock_on_without_owner() -> bool {
    is_lock_on_core(false)
}

// Check if the lock is active or not.
fn is_lock_on_core(include_owner: bool) -> bool {
    if lock_file_path().exists() {
        if !include_owner {
            return true;
        }

        let owner = match file::read(&lock_file_owner_path()) {
            Ok(o) => o,
            Err(_) => return true,
        };
        let owner = owner.trim();

        owner != proc::get_proc_id().trim()
    } else {
        false
    }
}

// Abort the program if the lock is on.
pub fn abort_if_locked() {
    if is_lock_on() {
        piglog::warning!("ant process lock detected... aborting...");
        piglog::note!(
            "If there are no other ant processes running, you can force unlock with the following command:"
        );
        piglog::note!(
            "{} {}",
            "$:".bright_cyan().bold(),
            "ant force-unlock".bright_magenta()
        );

        std::process::exit(1);
    }
}

fn lock_file_path() -> Path {
    places::base().add_str(".block_others_DO_NOT_DELETE_MANUALLY")
}

fn lock_file_owner_path() -> Path {
    places::base().add_str(".owner_of_blocking_DO_NOT_DELETE_MANUALLY")
}

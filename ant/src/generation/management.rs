use piglog::prelude::*;
use std::io;

use crate::lock;

pub fn tidy_up() -> Result<(), io::Error> {
    lock::abort_if_locked();

    let deleted = clean_dups(false)?;
    let aligned = align(false)?;

    piglog::success!("Deleted {deleted} duplicate generations!");
    piglog::success!("Aligned {aligned} generations!");

    Ok(())
}

pub fn clean_dups(verbose: bool) -> Result<usize, io::Error> {
    lock::abort_if_locked();

    let mut deleted: usize = 0; // The amount of deleted generations.

    let mut gen_nums = super::list_gen_nums()?;
    gen_nums.sort();

    let mut comparison: Option<super::Generation> = None;
    let mut comp_num: Option<usize> = None;

    for i in gen_nums {
        let generation = super::get_gen_from_usize(i)?;

        if let Some(ref s) = comparison {
            if generation == *s {
                let current = super::is_current(i)?;
                let built = super::is_built(i)?;

                if verbose {
                    piglog::info!("Deleting generation: {i}");
                }

                if current {
                    match super::set_current(comp_num.unwrap(), verbose) {
                        Ok(_) => (),
                        Err(e) => {
                            piglog::fatal!("Failed to set 'current' generation!");

                            return Err(e);
                        }
                    };
                }

                if built {
                    match super::set_built(comp_num.unwrap(), verbose) {
                        Ok(_) => (),
                        Err(e) => {
                            piglog::fatal!("Failed to set 'built' generation!");

                            return Err(e);
                        }
                    };
                }

                match super::delete(i, verbose) {
                    Ok(_) => (),
                    Err(e) => {
                        piglog::fatal!("Failed to delete generation: {i}");

                        return Err(e);
                    }
                };

                deleted += 1;
            } else {
                comparison = Some(generation);
                comp_num = Some(i);
            }
        } else {
            comparison = Some(generation);
            comp_num = Some(i);
        }
    }

    Ok(deleted)
}

pub fn align(verbose: bool) -> Result<usize, io::Error> {
    lock::abort_if_locked();

    let mut moved: usize = 0; // The amount of moved generations.

    let mut gen_nums = super::list_gen_nums()?;
    gen_nums.sort();

    for (comparison, i) in gen_nums.into_iter().enumerate() {
        let new_number = comparison + 1;

        if i > new_number {
            if verbose {
                piglog::info!("Moving: {i} -> {new_number}");
            }

            match super::move_gen(i, new_number, verbose) {
                Ok(_) => (),
                Err(e) => {
                    piglog::fatal!("Failed to move generation {i} to {new_number}!");

                    return Err(e);
                }
            };

            moved += 1;
        }
    }

    Ok(moved)
}

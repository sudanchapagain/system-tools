#![allow(dead_code)]

pub mod management;

use colored::Colorize;
use fspp::*;
use hashbrown::HashMap;
use piglog::prelude::*;
use piglog::*;
use serde::{Deserialize, Serialize};
use std::io;

use crate::config::config_for;
use crate::config::{Config, ConfigSide};
use crate::hook::run_hook_and_return_if_err;
use crate::library;
use crate::library::*;
use crate::lock::*;
use crate::management::load_manager;
use crate::places;
use crate::system;

trait Migrate<T> {
    fn migrate(self) -> T;
}

#[derive(PartialEq, Serialize, Deserialize, Debug, Default)]
#[serde(deny_unknown_fields, default)]
pub struct ManagerOrder {
    pub begin: Vec<String>,
    pub end: Vec<String>,
}

#[derive(PartialEq, Serialize, Deserialize, Debug, Default)]
#[serde(deny_unknown_fields, default)]
pub struct Items {
    pub items: Vec<String>,
}

pub mod legacy_1 {
    use serde::{Deserialize, Serialize};

    use super::Migrate;

    #[derive(PartialEq, Serialize, Deserialize, Debug, Default)]
    #[serde(deny_unknown_fields, default)]
    pub struct Generation {
        pub imports: Vec<String>,
        pub pkgs: Vec<String>,
        pub flatpaks: Vec<String>,
        pub crates: Vec<String>,
    }

    impl Migrate<super::legacy_2::Generation> for Generation {
        fn migrate(self) -> super::legacy_2::Generation {
            let mut generation = super::legacy_2::Generation::default();

            generation.pkg_managers.insert(
                "system".to_string(),
                super::legacy_2::Packages { pkgs: self.pkgs },
            );
            generation.pkg_managers.insert(
                "flatpak".to_string(),
                super::legacy_2::Packages {
                    pkgs: self.flatpaks,
                },
            );
            generation.pkg_managers.insert(
                "cargo".to_string(),
                super::legacy_2::Packages { pkgs: self.crates },
            );

            generation
        }
    }

    impl Migrate<super::Generation> for Generation {
        fn migrate(self) -> super::Generation {
            let mut generation = super::Generation::default();

            generation
                .managers
                .insert("system".to_string(), super::Items { items: self.pkgs });
            generation.managers.insert(
                "flatpak".to_string(),
                super::Items {
                    items: self.flatpaks,
                },
            );
            generation
                .managers
                .insert("cargo".to_string(), super::Items { items: self.crates });

            generation
        }
    }
}

pub mod legacy_2 {
    use hashbrown::HashMap;
    use serde::{Deserialize, Serialize};

    use super::Migrate;

    #[derive(PartialEq, Serialize, Deserialize, Debug, Default)]
    #[serde(deny_unknown_fields, default)]
    pub struct Packages {
        pub pkgs: Vec<String>,
    }

    #[derive(PartialEq, Serialize, Deserialize, Debug)]
    #[serde(deny_unknown_fields, default)]
    pub struct Generation {
        pub imports: Vec<String>,
        pub pkg_managers: HashMap<String, Packages>,
    }

    impl Migrate<super::Generation> for Generation {
        fn migrate(self) -> super::Generation {
            let mut generation = super::Generation::default();

            for (key, value) in self.pkg_managers.into_iter() {
                generation
                    .managers
                    .insert(key, super::Items { items: value.pkgs });
            }

            generation
        }
    }

    impl Default for Generation {
        fn default() -> Generation {
            Generation {
                imports: Vec::new(),
                pkg_managers: HashMap::new(),
            }
        }
    }
}

#[derive(PartialEq, Serialize, Deserialize, Debug)]
#[serde(deny_unknown_fields, default)]
pub struct Generation {
    pub imports: Vec<String>,
    pub managers: HashMap<String, Items>,
}

impl Default for Generation {
    fn default() -> Generation {
        Generation {
            imports: Vec::new(),
            managers: HashMap::new(),
        }
    }
}

impl GenerationUtils for Generation {
    fn extend(&mut self, other_gen: Generation) {
        self.imports.extend(other_gen.imports);

        for i in other_gen.managers.keys() {
            match self.managers.get_mut(i) {
                Some(s) => s
                    .items
                    .extend(other_gen.managers.get(i).unwrap().items.clone()),
                None => {
                    self.managers
                        .insert(i.to_string(), Items { items: Vec::new() });
                    self.managers
                        .get_mut(i)
                        .unwrap()
                        .items
                        .extend(other_gen.managers.get(i).unwrap().items.clone());
                }
            };
        }
    }
}

pub trait GenerationUtils {
    /// Extend all of the fields from one Generation object to another, another being the caller
    fn extend(&mut self, other_gen: Generation);
}

// Return generation structure for...
pub fn generate(side: ConfigSide) -> Result<Generation, io::Error> {
    let mut generation = read_to_gen(&config_for(Config::Generation, side))?;
    let system_hostname = system::hostname()?;

    if side == ConfigSide::User {
        generation.extend(read_to_gen(
            &places::base_user()
                .add_str("machines")
                .add_str(&system_hostname)
                .add_str("gen.toml"),
        )?);
    }

    while !generation.imports.is_empty() {
        let gen_imports = generation.imports.clone();

        for i in gen_imports.iter() {
            let i_gen = read_to_gen(
                &places::base_user()
                    .add_str("imports")
                    .add_str(&format!("{i}.toml")),
            )?;

            generation.extend(i_gen);
        }

        let after_gen_imports = generation.imports.clone();

        for (i, import) in after_gen_imports.iter().enumerate() {
            if gen_imports.contains(import) {
                generation.imports[i] = String::new();
            }
        }

        generation.imports.retain(|x| *x != String::new());
    }

    Ok(generation)
}

macro_rules! deserialize_legacy {
    ($generation: ident, $string: expr, $gen_type: ty, $version: expr) => {
        let mut should_try = false;

        match $generation {
            None => should_try = true,
            Some(ref s) => {
                match s {
                    Err(_) => should_try = true,
                    _ => (),
                };
            }
        };

        if should_try {
            match toml::from_str::<$gen_type>($string) {
                Ok(o) => {
                    success!("Deserialized generation in legacy mode {}.x.x!", $version);

                    $generation = Some(Ok(o.migrate()));
                }
                Err(e) => {
                    error!(
                        "Failed to deserialize generation in legacy mode {}.x.x!",
                        $version
                    );

                    $generation = Some(Err(e));
                }
            };
        }
    };
}

// Read a file and return a Generation object.
fn read_to_gen(path: &Path) -> Result<Generation, io::Error> {
    let gen_string = match file::read(path) {
        Ok(o) => o,
        Err(e) => {
            error!("Failed to read generation TOML file!");
            return Err(e);
        }
    };

    Ok(match toml::from_str(&gen_string) {
        Ok(o) => o,
        Err(e) => {
            warning!(
                "Failed to deserialize generation, attempting legacy modes... ('{}')",
                path.to_string()
            );

            let mut generation: Option<Result<Generation, toml::de::Error>> = None;

            deserialize_legacy!(generation, &gen_string, legacy_1::Generation, 1);
            deserialize_legacy!(generation, &gen_string, legacy_2::Generation, 2);

            match generation {
                Some(s) => match s {
                    Ok(o) => o,
                    Err(_) => {
                        error!(
                            "Failed to deserialize in legacy modes! Regular deserialization error:"
                        );
                        error!("{e:#?}");
                        error!("Path: '{}'", path.to_string());

                        return Err(custom_error("Failed to deserialize generation!"));
                    }
                },
                None => unreachable!(),
            }
        }
    })
}

// Does the generation specified exist?
pub fn gen_exists(gen_id: usize) -> bool {
    let path = places::gens()
        .add_str(&gen_id.to_string())
        .add_str("gen.toml");

    path.exists()
}

// Get generation for the id
pub fn get_gen_from_usize(gen_id: usize) -> Result<Generation, io::Error> {
    let generation = read_to_gen(
        &places::gens()
            .add_str(&gen_id.to_string())
            .add_str("gen.toml"),
    )?;

    Ok(generation)
}

// Get generation commit for the id
pub fn get_gen_commit_from_usize(gen_id: usize) -> Result<String, io::Error> {
    let gen_commit = file::read(
        &places::gens()
            .add_str(&gen_id.to_string())
            .add_str("commit"),
    )?;

    Ok(gen_commit)
}

pub fn latest_number() -> Result<usize, io::Error> {
    let generation_numbers = list_gen_nums()?;

    if generation_numbers.is_empty() {
        return Ok(0);
    }

    let latest_num = match generation_numbers.into_iter().max() {
        Some(s) => s,
        None => {
            error!("Failed to get max number in generation numbers list!");
            return Err(custom_error(
                "Failed to get max number in generation number list!",
            ));
        }
    };

    Ok(latest_num)
}

// Create a new system generation based on the user generation.
pub fn commit(msg: &str) -> Result<(), io::Error> {
    abort_if_locked();

    let generation_number = latest_number()? + 1;

    let gen_dir = places::gens().add_str(&generation_number.to_string());

    let user_gen = generate(ConfigSide::User)?;
    let user_gen_string = match toml::to_string(&user_gen) {
        Ok(o) => o,
        Err(_e) => {
            error!("Failed to convert user generation to string!");
            return Err(custom_error("Failed to convert user generation to string!"));
        }
    };

    match directory::create(&gen_dir) {
        Ok(_) => info!("Created generation directory."),
        Err(e) => {
            error!("Failed to create generation directory!");
            return Err(e);
        }
    };

    let files = [
        (msg, gen_dir.add_str("commit")),
        (user_gen_string.as_str(), gen_dir.add_str("gen.toml")),
    ];

    for i in files.iter() {
        match file::write(i.0, &i.1) {
            Ok(_o) => info!("Created file: {}", i.1.basename()),
            Err(e) => {
                error!("Failed to create file: {}", i.1.basename());

                match fs_action::delete(&gen_dir) {
                    Ok(_) => (),
                    Err(e) => {
                        error!("Failed to delete generation directory!");
                        return Err(e);
                    }
                };

                return Err(e);
            }
        };
    }

    match set_current(generation_number, true) {
        Ok(_o) => {}
        Err(e) => return Err(e),
    };

    Ok(())
}

fn get_order(generation: &Generation) -> Result<Vec<String>, io::Error> {
    let return_order = {
        let path = places::base_user().add_str("manager_order.toml");

        if path.exists() {
            info!("Reading order rules from manager_order.toml...");

            let order_obj: ManagerOrder = match toml::from_str(&file::read(&path)?) {
                Ok(o) => o,
                Err(e) => {
                    error!("Failed to deserialize manager_order.toml!");
                    error!("TOML Error: {e:#?}");

                    return Err(custom_error("Failed to deserialize manager_order.toml!"));
                }
            };

            let mut order: Vec<String> = order_obj.begin.clone();

            for key in generation.managers.keys() {
                if order_obj.begin.contains(key) || order_obj.end.contains(key) {
                    continue;
                }
                order.push(key.to_string());
            }

            order.extend(order_obj.end);

            let mut dup_track: HashMap<String, usize> = HashMap::new();

            for o in order.iter() {
                if dup_track.get(o).is_none() {
                    dup_track.insert(o.to_string(), 1);
                    continue;
                }

                *dup_track.get_mut(o).unwrap() += 1;
            }

            for (key, value) in dup_track.into_iter() {
                if value == 1 {
                    continue;
                }

                warning!("Duplicates in manager_order.toml! (Found {value} of: '{key}')");
            }

            order
                .into_iter()
                .filter(|x| generation.managers.contains_key(x))
                .collect()
        } else {
            generation.managers.keys().map(|x| x.to_string()).collect()
        }
    };

    Ok(return_order)
}

// Build the 'current' system generation.
pub fn build() -> Result<(), io::Error> {
    abort_if_locked();

    run_hook_and_return_if_err!("pre_build");

    let current_num = get_current()?;
    let curr_gen = generate(ConfigSide::System)?;

    match file::read(&places::gens().add_str("built")) {
        Ok(o) => {
            let built_gen = read_to_gen(&places::gens().add_str(o.trim()).add_str("gen.toml"))?;

            let mut summary_entries: HashMap<String, Vec<History>> = HashMap::new();

            let curr_order: Vec<String> = get_order(&curr_gen)?;

            // Add new items, remove old items.
            for i in curr_order.iter() {
                let man = load_manager(i)?;

                let curr_items = curr_gen.managers.get(i).unwrap();

                match built_gen.managers.get(i) {
                    Some(built_items) => {
                        let diffs = history(&built_items.items, &curr_items.items);

                        let mut to_install: Vec<String> = Vec::new();
                        let mut to_remove: Vec<String> = Vec::new();

                        for j in diffs.iter() {
                            match j.mode {
                                HistoryMode::Add => to_install.push(j.line.to_string()),
                                HistoryMode::Remove => to_remove.push(j.line.to_string()),
                            };
                        }

                        man.add(&to_install)?;
                        man.remove(&to_remove)?;

                        summary_entries.insert(i.to_string(), diffs);
                    }
                    None => {
                        man.add(&curr_items.items)?;

                        summary_entries.insert(
                            i.to_string(),
                            curr_items
                                .items
                                .iter()
                                .map(|x| History {
                                    mode: HistoryMode::Add,
                                    line: x.to_string(),
                                })
                                .collect(),
                        );
                    }
                }
            }

            let built_order: Vec<String> = get_order(&built_gen)?;

            // Remove items from managers that were removed from the generation.
            for i in built_order.iter() {
                let built_items = built_gen.managers.get(i).unwrap();

                match curr_gen.managers.get(i) {
                    Some(_) => (),
                    None => {
                        let man = load_manager(i)?;

                        man.remove(&built_items.items)?;

                        summary_entries.insert(
                            i.to_string(),
                            built_items
                                .items
                                .iter()
                                .map(|x| History {
                                    mode: HistoryMode::Remove,
                                    line: x.to_string(),
                                })
                                .collect(),
                        );
                    }
                };
            }

            info!("#################");
            info!("#    SUMMARY    #");
            info!("#################");

            library::print_history_gen(&summary_entries);
        }
        Err(_) => {
            let curr_order = get_order(&curr_gen)?;

            for i in curr_order.iter() {
                let curr_items = curr_gen.managers.get(i).unwrap();

                let man = load_manager(i)?;

                man.add(&curr_items.items)?;
            }

            note!("There is no summary. (First time building.)");
        }
    };

    match set_built(current_num, true) {
        Ok(_o) => {}
        Err(e) => return Err(e),
    };

    run_hook_and_return_if_err!("post_build");

    Ok(())
}

// Set the 'current' generation to another older generation.
pub fn rollback(by: isize, verbose: bool) -> Result<(), io::Error> {
    abort_if_locked();

    let current_num = get_current()?;

    let new_current = (current_num as isize) - by;

    match set_current(new_current as usize, verbose) {
        Ok(_o) => {}
        Err(e) => return Err(e),
    };

    Ok(())
}

// Set the 'current' generation to the latest generation.
pub fn latest(verbose: bool) -> Result<(), io::Error> {
    abort_if_locked();

    match set_current(latest_number()?, verbose) {
        Ok(_o) => {}
        Err(e) => return Err(e),
    };

    Ok(())
}

// Set the 'current' generation to a specific generation.
pub fn set_current(to: usize, verbose: bool) -> Result<(), io::Error> {
    abort_if_locked();

    if to > latest_number()? {
        error!("Out of range! Too high!");
        return Err(custom_error("Out of range!"));
    }

    if to < 1 {
        error!("Out of range! Too low!");
        return Err(custom_error("Out of range!"));
    }

    match file::write(to.to_string().trim(), &places::gens().add_str("current")) {
        Ok(_) => {
            if verbose {
                info!("Set 'current' to: {}", to);
            }

            Ok(())
        }
        Err(e) => {
            error!("Failed to create/write 'current' tracking file!");
            Err(e)
        }
    }
}

// Set the 'built' generation to a specific generation.
pub fn set_built(to: usize, verbose: bool) -> Result<(), io::Error> {
    abort_if_locked();

    if to > latest_number()? {
        error!("Out of range! Too high!");
        return Err(custom_error("Out of range!"));
    }

    if to < 1 {
        error!("Out of range! Too low!");
        return Err(custom_error("Out of range!"));
    }

    match file::write(to.to_string().trim(), &places::gens().add_str("built")) {
        Ok(_o) => {
            if verbose {
                info!("Set 'built' to: {}", to);
            }

            Ok(())
        }
        Err(e) => {
            error!("Failed to create/write 'built' tracking file!");
            Err(e)
        }
    }
}

// Get the 'current' generation number.
pub fn get_current() -> Result<usize, io::Error> {
    let contents = match file::read(&places::gens().add_str("current")) {
        Ok(o) => o,
        Err(e) => {
            error!("Failed to read 'current' file!");
            return Err(e);
        }
    };

    let generation: usize = match contents.trim().parse() {
        Ok(o) => o,
        Err(_e) => {
            error!(
                "Failed to parse number from 'current' file! (Maybe 'current' file is corrupted?)"
            );
            return Err(custom_error(
                "Failed to parse number out of 'current' file!",
            ));
        }
    };

    Ok(generation)
}

// Get the currently built generation number. (With output.)
pub fn get_built() -> Result<usize, io::Error> {
    get_built_core(true)
}

// Get the currently built generation number. (Without output.)
pub fn get_built_no_output() -> Result<usize, io::Error> {
    get_built_core(false)
}

// Get the currently built generation number. (CORE)
pub fn get_built_core(output: bool) -> Result<usize, io::Error> {
    let contents = match file::read(&places::gens().add_str("built")) {
        Ok(o) => o,
        Err(e) => {
            if output {
                error!("Failed to read 'built' file!");
            }

            return Err(e);
        }
    };

    let generation: usize = match contents.trim().parse() {
        Ok(o) => o,
        Err(_e) => {
            if output {
                error!(
                    "Failed to parse number from 'built' file! (Maybe 'built' file is corrupted?)"
                );
            }

            return Err(custom_error("Failed to parse number out of 'built' file!"));
        }
    };

    Ok(generation)
}

// Has a generation been built yet?
pub fn been_built() -> bool {
    places::gens().add_str("built").exists()
}

// Delete old generations.
pub fn delete_old(how_many: usize, verbose: bool) -> Result<(), io::Error> {
    abort_if_locked();

    let offset = get_oldest()?;

    let latest_gen = latest_number()?;

    for i in offset..(how_many + offset) {
        if i > latest_gen {
            break;
        }

        match delete(i, verbose) {
            Ok(_) => (), // This is a rare instance where the matched function actually did the info!() itself!
            Err(e) => return Err(e),
        };
    }

    Ok(())
}

// Delete a specific generation.
pub fn delete(generation: usize, verbose: bool) -> Result<(), io::Error> {
    abort_if_locked();

    if is_current(generation)? {
        warning!(
            "Could not delete generation {}, because it is the 'current' generation, and is protected!",
            generation
        );
        return Ok(());
    }

    if been_built() && !is_built(generation)? {
        warning!(
            "Could not delete generation {}, because it is the currently built generation, and is protected!",
            generation
        );
        return Ok(());
    }

    if !exists(generation)? {
        error!("Generation {} does not exist!", generation);
        return Err(custom_error("Generation does not exist!"));
    }

    match fs_action::delete(&places::gens().add_str(&generation.to_string())) {
        Ok(_) => {
            if verbose {
                info!("Deleted generation: {}", generation);
            }
        }
        Err(e) => {
            error!("Failed to delete generation: {}", generation);
            return Err(e);
        }
    };

    Ok(())
}

// Move a generation to another spot. (Number -> Number)
pub fn move_gen(from: usize, to: usize, verbose: bool) -> Result<(), io::Error> {
    abort_if_locked();

    let current = is_current(from)?;
    let built = is_built(from)?;

    let from_path = places::gens().add_str(&from.to_string());
    let to_path = places::gens().add_str(&to.to_string());

    fs_action::mv(&from_path, &to_path)?;

    if verbose {
        info!("Moved generation: {from} -> {to}");
    }

    if current {
        set_current(to, verbose)?;
    }

    if built {
        set_built(to, verbose)?;
    }

    Ok(())
}

// See if a generation exists.
pub fn exists(generation: usize) -> Result<bool, io::Error> {
    let gen_nums = list_gen_nums()?;
    Ok(gen_nums.contains(&generation))
}

// List generation numbers.
pub fn list_gen_nums() -> Result<Vec<usize>, io::Error> {
    let gen_list = list_with_no_calls()?;
    let mut gen_nums: Vec<usize> = Vec::new();

    for i in gen_list.iter() {
        gen_nums.push(usize_from_gen_name(i.0.as_str())?);
    }

    Ok(gen_nums)
}

// Parse generation name to usize.
pub fn usize_from_gen_name(name: &str) -> Result<usize, io::Error> {
    Ok(match name.trim().parse() {
        Ok(o) => o,
        Err(_e) => {
            error!("Failed to parse invalid generation name! ({})", name.trim());
            return Err(custom_error("Failed to parse invalid generation name!"));
        }
    })
}

// Return true or false based on if the given generation is the 'current' generation.
pub fn is_current(generation: usize) -> Result<bool, io::Error> {
    if generation == get_current()? {
        Ok(true)
    } else {
        Ok(false)
    }
}

// Return true or false based on if the given generation is the built generation.
pub fn is_built(generation: usize) -> Result<bool, io::Error> {
    if generation == get_built()? {
        Ok(true)
    } else {
        Ok(false)
    }
}

// List all generations. (NORMAL)
pub fn list() -> Result<Vec<(String, String, bool, bool)>, io::Error> {
    list_core(true)
}

// List all generations. (ISOLATED MODE | For avoiding errors with called un-needed functions!)
pub fn list_with_no_calls() -> Result<Vec<(String, String, bool, bool)>, io::Error> {
    list_core(false)
}

// List all generations. (CORE)
fn list_core(calls: bool) -> Result<Vec<(String, String, bool, bool)>, io::Error> {
    let gen_listed = match directory::list_items(&places::gens()) {
        Ok(o) => o,
        Err(e) => {
            error!(
                "Failed to list the generations directory! ({})",
                places::gens().to_string()
            );
            return Err(e);
        }
    };

    let mut generations: Vec<Path> = Vec::new();

    for i in gen_listed.into_iter() {
        match i.path_type() {
            PathType::File => {}
            PathType::Directory => generations.push(i),
            PathType::Invalid => {
                error!("Found invalid path! (Listing generations.)");
                return Err(custom_error("Found invalid path."));
            }
        };
    }

    let mut gens_with_info: Vec<(String, String, bool, bool)> = Vec::new();

    for i in generations.iter() {
        let generation_name = i.basename();
        let commit_msg = file::read(&i.add_str("commit"))
            .unwrap_or(String::from("<< COMMIT MESSAGE MISSING >>"));

        let current_number: usize;
        let built_number: usize;

        if calls {
            current_number = get_current()?;
            built_number = get_built_no_output()?;
        } else {
            current_number = 0;
            built_number = 0;
        }

        let is_current = generation_name == current_number.to_string();
        let is_built = generation_name == built_number.to_string();

        gens_with_info.push((generation_name, commit_msg, is_current, is_built));
    }

    Ok(gens_with_info)
}

// Print out the list of generations.
pub fn list_print() -> Result<(), io::Error> {
    let list_items = list()?;
    let list_items_sorted = sort_list_vector(&list_items)?;
    let mut max_digits: usize = 0;

    if !list_items_sorted.is_empty() {
        max_digits = list_items_sorted[list_items_sorted.len() - 1]
            .0
            .to_string()
            .trim()
            .len();
    }

    for i in list_items_sorted.iter() {
        let mut misc_text = String::new();

        if i.2 {
            misc_text.push_str(
                format!(
                    " {}{}{}",
                    "[".bright_black().bold(),
                    "CURRENT".bright_green().bold(),
                    "]".bright_black().bold()
                )
                .as_str(),
            );
        }

        if i.3 {
            misc_text.push_str(
                format!(
                    " {}{}{}",
                    "[".bright_black().bold(),
                    "BUILT".bright_yellow().bold(),
                    "]".bright_black().bold()
                )
                .as_str(),
            );
        }

        let mut tabbed = String::new();

        for _j in 0..(max_digits - i.0.trim().len()) {
            tabbed.push(' ');
        }

        generic!("{}{} ... ({}){}", tabbed, i.0, i.1, misc_text);
    }

    Ok(())
}

// Get only list vector generation names.
fn get_list_vector_names(list_vec: &[(String, String, bool, bool)]) -> Vec<String> {
    let mut new_vec: Vec<String> = Vec::new();

    for i in list_vec.iter() {
        new_vec.push(i.0.to_string());
    }

    new_vec
}

// Sort list vector.
fn sort_list_vector(
    list_vec: &[(String, String, bool, bool)],
) -> Result<Vec<(String, String, bool, bool)>, io::Error> {
    if list_vec.is_empty() {
        return Ok(list_vec.to_vec());
    }

    let list_vec_names = get_list_vector_names(list_vec);

    let mut list_vec_nums: Vec<usize> = Vec::new();

    for i in list_vec_names.iter() {
        list_vec_nums.push(usize_from_gen_name(i)?);
    }

    list_vec_nums.sort();

    let mut new_vec: Vec<(String, String, bool, bool)> = Vec::new();

    for i in list_vec_nums.iter() {
        for j in list_vec.iter() {
            let j_num: usize = usize_from_gen_name(j.0.as_str())?;
            if &j_num == i {
                new_vec.push(j.clone());
                break;
            }
        }
    }

    Ok(new_vec)
}

// Get oldest generation name.
pub fn get_oldest() -> Result<usize, io::Error> {
    let gen_names = get_list_vector_names(&sort_list_vector(&list_with_no_calls()?)?);

    if gen_names.is_empty() {
        error!("Tried to call generation::get_oldest(), when there are no generations!");
        return Err(custom_error("Not enough generations!"));
    }

    let oldest_name = gen_names[0].to_string();

    let oldest_number: usize = usize_from_gen_name(oldest_name.as_str())?;

    Ok(oldest_number)
}

// Get the 'current' generation TOML file.
pub fn current_gen() -> Result<Path, io::Error> {
    let current = get_current()?;

    Ok(places::gens()
        .add_str(&current.to_string())
        .add_str("gen.toml"))
}

#![allow(dead_code)]

use crate::generation::Generation;
use crate::obj_print_boilerplate::macros::*;
use piglog::prelude::*;

pub fn generation(generation: &Generation) {
    for i in generation.managers.keys() {
        print_entry!(i, generation.managers.get(i).unwrap().items);
    }
}

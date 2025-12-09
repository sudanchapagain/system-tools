#![allow(dead_code)]

pub mod macros {
    macro_rules! print_entry {
        (
            $entry_name: expr,
            $gen_field_name: expr
        ) => {
            piglog::info!("{}:", $entry_name);

            for i in $gen_field_name.iter() {
                piglog::generic!("{}", i);
            }

            println!("");
        };
    }

    pub(crate) use print_entry;
}

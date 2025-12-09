#![allow(unused_imports)]

macro_rules! run_hook_and_return_if_err {
    (
        $hook_name: expr
    ) => {
        let macro_hook_path = crate::places::base_user()
            .add_str("hooks")
            .add_str(&format!("{}", $hook_name));

        let hook_name = format!("{}", $hook_name);

        if macro_hook_path.exists() {
            crate::info!("Running hook: {}", hook_name);

            match library::run_command(&macro_hook_path.to_string()) {
                true => crate::info!("Successfully ran hook: {}", hook_name),
                false => {
                    crate::error!("Failed to run hook: {}", hook_name);

                    return Err(library::custom_error("Failed to run hook!"));
                }
            };
        }

        std::mem::drop(macro_hook_path);
    };
}

pub(crate) use run_hook_and_return_if_err;

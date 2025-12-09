const ID_ENV_NAME: &str = "__ANT_PROC_ID_THIS_ENV_VAR_SHOULD_NOT_BE_OVERWRITTEN__";

pub fn init_proc_id() {
    let proc_id = format!("{}_{}", nanoid::nanoid!(), nanoid::nanoid!());

    unsafe {
        std::env::set_var(ID_ENV_NAME, proc_id);
    }
}

pub fn get_proc_id() -> String {
    std::env::var(ID_ENV_NAME).unwrap()
}

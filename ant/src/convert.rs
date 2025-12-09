#![allow(dead_code)]

pub fn str_to_string_vec(the_str: &str, split_at: &str) -> Vec<String> {
    let mut return_vec: Vec<String> = Vec::new();

    for i in the_str.split(split_at) {
        return_vec.push(i.to_string());
    }

    return_vec
}

pub fn string_vec_to_string(vector: &[String], filler: &str) -> String {
    let mut phrase = String::new();

    for i in 0..vector.len() {
        phrase.push_str(vector[i].as_str());

        if i < vector.len() - 1 {
            phrase.push_str(filler);
        }
    }

    phrase
}

#!/usr/bin/fish

set got

function test_match_nth --argument-names input search with_nth_format match_nth_format
    set got (echo -e "$input" |
                 $FUZZEL_TEST_BIN --dmenu \
                                  --search="$search" \
                                  --auto-select \
                                  --nth-delim ' ' \
                                  --with-nth="$with_nth_format" \
                                  --match-nth="$match_nth_format")
end

test_match_nth "apple banana cherry" "banana" "{1}" "{2}"
@test "--match-nth with valid formatters" "$got" = "apple banana cherry"

test_match_nth "apple banana cherry\nbanana apple cherry" "apple" "{1}" "{2}"
@test "--match-nth with valid formatters" "$got" = "banana apple cherry"

test_match_nth "apple banana cherry" "cherry" "{1}" "{2..3}"
@test "--match-nth with index range" "$got" = "apple banana cherry"

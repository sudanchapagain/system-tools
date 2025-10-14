#!/usr/bin/fish

set got

function test_accept_nth --argument-names input search nth_format
    set got (echo -e "$input" |
                 $FUZZEL_TEST_BIN --dmenu \
                                  --search="$search" \
                                  --auto-select \
                                  --accept-nth="$nth_format")
end

test_accept_nth "apple\tbanana\tcherry" "apple" "{3} - {2}: {1}"
@test "--accept-nth with valid formatters" "$got" = "cherry - banana: apple"

test_accept_nth "apple\tbanana\tcherry" "apple" "{0} - {4}"
@test "--accept-nth with invalid column indices" "$got" = "{0} - {4}"

test_accept_nth "apple\tbanana\tcherry" "apple" "{{1}"
@test "--accept-nth with unclosed curly bracket" "$got" = "{apple"

test_accept_nth "apple\tbanana\tcherry" "apple" "{1a}"
@test "--accept-nth with non-numerical index" "$got" = "{1a}"

test_accept_nth "apple\tbanana\tcherry" "apple" "{1..3}"
@test "--accept-nth with index range" "$got" = "apple banana cherry"

test_accept_nth "apple\tbanana\tcherry" "apple" "{3..1}"
@test "--accept-nth with reversed index range" "$got" = "{3..1}"

test_accept_nth "apple\tbanana\tcherry" "apple" "{1.3} {1...3}"
@test "--accept-nth with invalid index range" "$got" = "{1.3} {1...3}"

test_accept_nth "apple\tbanana\tcherry" "apple" "{2..}"
@test "--accept-nth with open index range" "$got" = "banana cherry"

test_accept_nth "apple\tbanana\tcherry" "apple" "{2.} {2...}"
@test "--accept-nth with invalid open index range" "$got" = "{2.} {2...}"

test_accept_nth "apple\tbanana\tcherry" "apple" "{..2}"
@test "--accept-nth with invalid open index range" "$got" = "{..2}"

test_accept_nth "first\t\tthird" first "{3}"
@test "--accept-nth with empty column" "$got" = third

test_accept_nth "first\tsecond" first 0
@test "--accept-nth=0 " "$got" = "first	second"

test_accept_nth "first\tsecond" first ''
@test "--accept-nth= should work like 0" "$got" = "first	second"

#!/usr/bin/fish

set got

# Given lines of input as $input and a search string as $search,
# run Fuzzel with auto-select enabled and set the returned string in $got
function test_auto_select --argument-names input search
    rm -f out.txt

    echo -e "$input" \
        | $FUZZEL_TEST_BIN --dmenu --auto-select --search "$search" >out.txt &
    # Wait for fuzzel to auto-select and exit
    sleep .3
    set got (cat out.txt)
end

# Test that auto-select works when only one match remains
test_auto_select "apple\nbanana\ncherry" app
@test "auto-select should execute when only one match remains" "$got" = "apple"

# Test that auto-select works with partial matches
test_auto_select "application\napple\nbanana" applic
@test "auto-select should work with partial matches" "$got" = "application"

# Test that auto-select doesn't trigger with multiple matches
function test_no_auto_select --argument-names input search
    rm -f out.txt

    echo -e "$input" \
        | $FUZZEL_TEST_BIN --dmenu --auto-select --search "$search" >out.txt &
    # Wait briefly to see if fuzzel auto-selects (it shouldn't)
    sleep .1
    # Force exit with Escape
    wtype -k Escape
    sleep .1
    set got (cat out.txt)
end

test_no_auto_select "apple\napricot\nbanana" ap
@test "auto-select should not trigger with multiple matches" "$got" = ""

# Test without auto-select flag (should not auto-select)
function test_normal_mode --argument-names input search
    rm -f out.txt

    echo -e "$input" \
        | $FUZZEL_TEST_BIN --dmenu --search "$search" >out.txt &
    # Wait briefly 
    sleep .1
    # Force exit with Escape since it won't auto-select
    wtype -k Escape
    sleep .1
    set got (cat out.txt)
end

test_normal_mode unique uniqu
@test "normal mode should not auto-select even with one match" "$got" = ""

# Test --auto-select with --search option
function test_auto_select_with_search --argument-names input search
    rm -f out.txt

    echo -e "$input" \
        | $FUZZEL_TEST_BIN --dmenu --auto-select --search "$search" >out.txt &
    # Wait for fuzzel to auto-select and exit
    sleep .3
    set got (cat out.txt)
end

# Test that auto-select works with --search when only one match remains
test_auto_select_with_search "application\napple\nbanana" "applic"
@test "auto-select with --search should work when only one match remains" "$got" = "application"

# Test that auto-select works with --search for exact matches
test_auto_select_with_search "calculator\ncalendar\ncamera" "calculator"
@test "auto-select with --search should work for exact matches" "$got" = "calculator"

# Test that auto-select with --search doesn't trigger with multiple matches
function test_auto_select_with_search_multiple --argument-names input search
    rm -f out.txt

    echo -e "$input" \
        | $FUZZEL_TEST_BIN --dmenu --auto-select --search "$search" >out.txt &
    # Wait briefly to see if fuzzel auto-selects (it shouldn't)
    sleep .1
    # Force exit with Escape
    wtype -k Escape
    sleep .1
    set got (cat out.txt)
end

test_auto_select_with_search_multiple "apple\napricot\nbanana" "ap"
@test "auto-select with --search should not trigger with multiple matches" "$got" = ""

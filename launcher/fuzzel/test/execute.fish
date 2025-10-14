#!/usr/bin/fish

set got

# Test that execute action (Enter) selects the input text when there are no matches in dmenu mode
function test_execute_no_matches --argument-names input search
    rm -f out.txt

    echo -e "$input" \
        | $FUZZEL_TEST_BIN --dmenu --search "$search" >out.txt &
    # Wait for fuzzel to launch
    sleep .1
    # Press Enter to execute (select the input)
    wtype -k Return
    # Wait for fuzzel to exit
    sleep .1
    set got (cat out.txt)
end

# Test with input that doesn't match any of the provided options
test_execute_no_matches "apple\nbanana\ncherry" "orange"
@test "execute should select the input text when no matches exist" "$got" = "orange"

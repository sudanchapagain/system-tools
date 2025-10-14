#!/usr/bin/fish

set got

# Given lines of input as $input and a search string as $search,
# run Fuzzel and set the returned string in $got
function test_fzf_mode --argument-names input search
    rm -f out.txt

    echo -e "$input" \
        | $FUZZEL_TEST_BIN --dmenu --match-mode=fzf --search "$search" >out.txt &
    # Wait for fuzzel to launch
    sleep .1
    wtype -k Return
    # wait for fuzzel to exit
    sleep .1
    set got (cat out.txt)
end

test_fzf_mode "hamburger\nflag: bulgaria\nbug\njunk\ntrunk" bug
@test "exact matches should rank higher than fuzzy matches" $got = bug

test_fzf_mode "longer entry\nlonger\nlong" long
@test "shorter matches should match first" $got = long

test_fzf_mode "\nbcdef\nabcd\n" cd
@test "matched sub-string is closer to the beginning is sorted first." $got = bcdef

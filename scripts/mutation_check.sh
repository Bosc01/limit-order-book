#!/bin/bash
# Breaks the final engine on purpose and checks that the property and
# differential tests notice. Each mutant is one edit to a copy of
# include/lob/order_book.hpp; the two suites are rebuilt against it and run.
# For every mutant the script prints the first failing assertion of each
# suite, and it fails if any mutant passes both suites, or if the unmutated
# copy fails either.
#   scripts/mutation_check.sh
set -eu
DIR="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Copy the tracked tree, including uncommitted edits to tracked files.
(cd "$DIR" && git ls-files | tar -cf - -T -) | tar -xf - -C "$TMP"
[ ! -d "$DIR/third_party" ] || ln -s "$DIR/third_party" "$TMP/third_party"
cat >> "$TMP/CMakeLists.txt" <<'EOF'

add_executable(mutant_tests tests/property_test.cpp tests/differential_test.cpp)
target_include_directories(mutant_tests PRIVATE include bench .)
target_compile_options(mutant_tests PRIVATE -O1)
target_link_libraries(mutant_tests PRIVATE GTest::gtest_main)
EOF
cmake -S "$TMP" -B "$TMP/build" -DCMAKE_BUILD_TYPE=Release > /dev/null
HDR="$TMP/include/lob/order_book.hpp"
cp "$HDR" "$TMP/pristine.hpp"

# name | perl substitution applied with -0 (whole file); it must match once.
MUTANTS=(
"skip-fill|s/\n\s*o->qty\s+-= remaining;\n\s*lvl\.total_qty -= remaining;//"
"cancel-leaves-order|s/(orders_\.erase\(id\);)\n\s*remove_from_book\(o\);\n\s*pool_\.destroy\(o\);/\$1/"
"newest-first-at-level|s/(Level& lvl = opp\.at\(best\);\s*Order\* o = )lvl\.head;/\$1lvl.tail;/"
"rest-at-head-of-level|s/\.next  = nullptr,(.*?)\.prev  = lvl\.tail,(.*?)if \(lvl\.tail\) lvl\.tail->next = o;\s*else\s+lvl\.head = o;\s*lvl\.tail = o;/.next  = lvl.head,\$1.prev  = nullptr,\$2if (lvl.head) lvl.head->prev = o; else lvl.tail = o; lvl.head = o;/s"
"reject-incoming-cancels-resting|s/if \(stp_ == Stp::RejectIncoming\) \{(\s*m\.stp_stopped = true;)/if (false) {\$1/"
"reject-incoming-rests-remainder|s/r\.rejected = \(m\.filled == 0\);\s*return r;/r.rejected = false;/"
)

# Prints "caught: file:line message" or "passed" for one suite.
run_suite() {
    local out
    if out="$("$TMP/build/mutant_tests" --gtest_filter="$1" 2>&1)"; then
        echo "passed"
        return 1
    fi
    echo "caught: $(echo "$out" | grep -m1 -A4 ': Failure$' | tr '\n' ' ' \
        | sed -e "s#$TMP/##g" -e 's/  */ /g' | cut -c1-200)"
}

status=0
for m in "baseline|" "${MUTANTS[@]}"; do
    name="${m%%|*}"; expr="${m#*|}"
    cp "$TMP/pristine.hpp" "$HDR"
    if [ -n "$expr" ]; then
        perl -0 -i -pe "\$n += $expr; END { exit(\$n == 1 ? 0 : 1) }" "$HDR" \
            || { echo "$name: the edit did not match exactly once"; exit 1; }
    fi
    cmake --build "$TMP/build" --target mutant_tests -j 4 > "$TMP/build.log" 2>&1 \
        || { cat "$TMP/build.log"; echo "$name: build failed"; exit 1; }
    echo "== $name"
    p="$(run_suite 'StpPolicy/Property.*')" && pc=1 || pc=0
    d="$(run_suite 'Differential.*')" && dc=1 || dc=0
    echo "   property:     $p"
    echo "   differential: $d"
    if [ "$name" = baseline ]; then
        [ $pc -eq 0 ] && [ $dc -eq 0 ] || { echo "   the unmutated engine fails"; status=1; }
    elif [ $pc -eq 0 ] && [ $dc -eq 0 ]; then
        echo "   NOT CAUGHT"; status=1
    fi
done
exit $status

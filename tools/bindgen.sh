#!/bin/sh
# Makes the bindings again from the C API: native/<area>/fbl_<area>.h to luau/c/fbl_<area>.fb, named
# as the header is so it never shares a module name with the package's own files or std's, and
# removes bindings whose header is gone. fb-bindgen makes every declaration Public, without a doc;
# the bindings are the package's alone, so this makes them Package, which a program can't name and
# `fb doc --check` doesn't ask docs of, and its two comments the module's doc.
# `bindgen.sh --check` changes nothing, and fails if any binding isn't what its header makes.
set -eu
cd "$(dirname "$0")/.."
check=false
if [ "${1:-}" = "--check" ]; then
    check=true
fi
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
lib="luau_native"
bindings="luau/c"

# Whether two binding files say the same, leaving out their first lines, which name the target
# fb-bindgen ran for and so differ by machine, and any CR a checkout put before a line's end.
same_bindings() {
    [ -e "$2" ] || return 1
    tail -n +2 "$1" | tr -d '\r' > "$scratch/made"
    tail -n +2 "$2" | tr -d '\r' > "$scratch/kept"
    cmp -s "$scratch/made" "$scratch/kept"
}

stale=0
mkdir -p "$scratch/bindings"
for header in native/*/fbl_*.h; do
    [ -e "$header" ] || continue
    name="$(basename "$header" .h)"
    fb-bindgen "$header" -o "$scratch/raw.fb" --lib "$lib" 2> "$scratch/log" ||
        { cat "$scratch/log" >&2; exit 1; }
    tr -d '\r' < "$scratch/raw.fb" |
        sed -e '1,2s|^// |//! |' -e 's/^Public /Package /' -e 's/^\(@[A-Za-z]* \)Public /\1Package /' \
        > "$scratch/bindings/$name.fb"
done
for made in "$scratch/bindings"/*.fb; do
    [ -e "$made" ] || continue
    target="$bindings/$(basename "$made")"
    if ! same_bindings "$made" "$target"; then
        stale=1
        if $check; then
            echo "$target isn't what its header makes; run tools/bindgen.sh" >&2
        else
            mkdir -p "$bindings"
            cp "$made" "$target"
            echo "wrote $target"
        fi
    fi
done
for kept in "$bindings"/*.fb; do
    [ -e "$kept" ] || continue
    if [ ! -e "$scratch/bindings/$(basename "$kept")" ]; then
        stale=1
        if $check; then
            echo "$kept has no header any more; run tools/bindgen.sh" >&2
        else
            rm "$kept"
            echo "removed $kept"
        fi
    fi
done
if $check && [ "$stale" = 1 ]; then
    exit 1
fi

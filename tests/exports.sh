#!/bin/bash
# The shared library exports html2pdf's own code and nothing of the modules it links: a copy of another
# module's code exported from here wins over that module's own library in a process that loads both
# (PHP ran sc-pdflite 4.6.3's reader from libsc-html2pdf instead of libsc-pdflite 4.7.0's, 2026-11-16).
# usage: exports.sh <shared library>
library="$1"
if [ ! -f "$library" ]; then
    echo "no such library: $library" >&2
    exit 2
fi
if [ "$(uname)" = Darwin ]; then
    symbols=$(nm -gU "$library")
else
    symbols=$(nm -D --defined-only "$library")
fi
# Strong code symbols only (T): weak ones (W, V) are inline functions and template instances every user
# of a header has its own copy of, which is harmless.
foreign=$(printf '%s\n' "$symbols" | awk '$2 == "T" { print $3 }' | c++filt |
    grep -E '^(sc::(pdflite|pdf|web|qr|graphics|mupdf)::|sc::(pdf|canvas|drawing|font|text|paragraph|rect_|color|params|config)\b|litehtml::|gumbo)' |
    grep -v 'html2pdf' || true)
if [ -n "$foreign" ]; then
    echo "$library exports code that belongs to other modules:" >&2
    printf '%s\n' "$foreign" | head -20 >&2
    echo "($(printf '%s\n' "$foreign" | wc -l | tr -d ' ') in all)" >&2
    exit 1
fi
echo "$library: only html2pdf's own code is exported"

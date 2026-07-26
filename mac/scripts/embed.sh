#!/bin/sh
# Emit a C header embedding a file as a byte array:  embed.sh <symbol> <file>
# Uses od(1) only, so it works on stock macOS and Linux alike.
set -eu

sym="$1"
infile="$2"

printf 'unsigned char %s[] = {\n' "$sym"
od -v -An -tx1 "$infile" | tr ' ' '\n' | grep . | sed 's/^/0x/;s/$/,/'
printf '};\nunsigned int %s_len = sizeof(%s);\n' "$sym" "$sym"

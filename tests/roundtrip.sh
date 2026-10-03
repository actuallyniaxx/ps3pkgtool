#!/usr/bin/env bash
# Round-trip test: create -> extract, custom root, multi-pkg, raw split/join.
set -euo pipefail
BIN="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT; cd "$T"

"$BIN" selftest
"$BIN" version | grep -Eq '^ps3pkgtool [0-9]+\.[0-9]+\.[0-9]+$'

mkdir -p src/USRDIR/sub/empty
# minimal PARAM.SFO carrying TITLE_ID=TEST00000
printf '\0PSF\1\1\0\0\44\0\0\0\60\0\0\0\1\0\0\0' > src/PARAM.SFO
printf '\0\0\4\2\12\0\0\0\14\0\0\0\0\0\0\0' >> src/PARAM.SFO
printf 'TITLE_ID\0\0\0\0TEST00000\0\0\0' >> src/PARAM.SFO
head -c 5000003 /dev/urandom > src/USRDIR/EBOOT.BIN
head -c 3000001 /dev/urandom > src/USRDIR/sub/big.bin
head -c 12345 /dev/urandom > src/ICON0.PNG
printf 'hola' > src/USRDIR/sub/a.txt
: > src/USRDIR/sub/zero

echo "== create / extract"
"$BIN" create src out.pkg
"$BIN" info out.pkg | grep -q 'UP0001-TEST00000_00-0000000000000000'
"$BIN" list out.pkg
"$BIN" extract out.pkg ex
diff -r src ex

# no leading slash on -r: Git Bash on Windows would rewrite /dev_hdd0 into C:/Program Files/Git/dev_hdd0
echo "== overwrite protection"
if "$BIN" extract out.pkg ex </dev/null 2>/dev/null; then echo "extract overwrote without -f"; exit 1; fi
if "$BIN" create src out.pkg </dev/null 2>/dev/null; then echo "create overwrote without -f"; exit 1; fi
"$BIN" extract out.pkg ex -f
"$BIN" create src out.pkg -f
diff -r src ex

echo "== custom install root"
"$BIN" create src root.pkg -r dev_hdd0/GAMES/TEST00000
"$BIN" list root.pkg | grep -q '\.\./\.\./\.\./dev_hdd0/GAMES/TEST00000/USRDIR/EBOOT.BIN'
"$BIN" extract root.pkg rx
diff -r src rx/dev_hdd0/GAMES/TEST00000

echo "== multi-pkg"
"$BIN" create src multi.pkg -r dev_hdd0/GAMES/TEST00000 -s 6M
test -f multi_1p.pkg && test -f multi_2p.pkg
for f in multi_*p.pkg; do "$BIN" extract "$f" mx; done
diff -r src mx/dev_hdd0/GAMES/TEST00000

echo "== multi-pkg must refuse files that do not fit"
if "$BIN" create src bad.pkg -s 2M 2>/dev/null; then echo "should have failed"; exit 1; fi
if ls bad_*p.pkg >/dev/null 2>&1; then echo "wrote partial output"; exit 1; fi

echo "== raw split / join"
cp out.pkg orig.pkg
"$BIN" split out.pkg -s 3M
rm out.pkg
"$BIN" join out.pkg.66600
cmp out.pkg orig.pkg

echo "ALL OK"

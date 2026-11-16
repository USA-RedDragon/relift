#!/bin/sh
set -eu
if [ $# -ne 1 ]; then
    echo "usage: cpus/mips/gen/fetch-sail.sh DEST" >&2
    exit 2
fi
dest=$1
sail_cheri_mips=13a9c3fb0c3f8d320d4f4650a63d23d3a8b12724
sail=c44944f11b71e5c9bd4b30cdbf79ea32f6dcf456
mkdir -p "$dest/lib" "$dest/model"
while read -r repo path sum; do
    case $repo in
        sail) url="https://raw.githubusercontent.com/rems-project/sail/$sail/$path"; out="$dest/lib/${path#lib/}" ;;
        sail-cheri-mips) url="https://raw.githubusercontent.com/CTSRD-CHERI/sail-cheri-mips/$sail_cheri_mips/$path"; out="$dest/model/${path#mips/}" ;;
        *) echo "unknown repository $repo" >&2; exit 1 ;;
    esac
    curl -fsSL --retry 3 -o "$out" "$url"
    echo "$sum  $out" | sha256sum -c --quiet -
done < cpus/mips/gen/sources.txt
echo "$dest/lib $dest/model"

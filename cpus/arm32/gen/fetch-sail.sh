#!/bin/sh
set -eu
if [ $# -ne 1 ]; then
    echo "usage: cpus/arm32/gen/fetch-sail.sh DEST" >&2
    exit 2
fi
dest=$1
sail_arm=5416ef6b43e9527c38dc71fb752259c1d01a47e1
sail=c44944f11b71e5c9bd4b30cdbf79ea32f6dcf456
mkdir -p "$dest/lib" "$dest/model"
while read -r repo path sum; do
    case $repo in
        sail) url="https://raw.githubusercontent.com/rems-project/sail/$sail/$path"; out="$dest/lib/${path#lib/}" ;;
        sail-arm) url="https://raw.githubusercontent.com/rems-project/sail-arm/$sail_arm/$path"; out="$dest/model/${path#arm-v9.4-a/src/}" ;;
        *) echo "unknown repository $repo" >&2; exit 1 ;;
    esac
    curl -fsSL --retry 3 -o "$out" "$url"
    echo "$sum  $out" | sha256sum -c --quiet -
done < cpus/arm32/gen/sources.txt
echo "$dest/lib $dest/model"

#!/bin/sh
set -eu

# Host mksquashfs for the hisi appfs image. The camera kernel reads squashfs
# 4.0 with xz, so only gzip (the tools' default) and xz support are built.
tools_root=${1:-build/deps/squashfs-tools}
tools_url=https://github.com/plougher/squashfs-tools.git
tools_commit=708c59ae80853b0845017c33b42e56061cc546cd

for command in git make cc mkdir mktemp mv rm; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "Missing dependency tool: $command" >&2
        exit 1
    }
done

if [ -x "$tools_root/bin/mksquashfs" ] && [ -f "$tools_root/commit" ]; then
    [ "$(cat "$tools_root/commit")" = "$tools_commit" ] && exit 0
    echo "Rebuilding squashfs-tools in $tools_root for $tools_commit" >&2
    rm -rf "$tools_root"
elif [ -e "$tools_root" ]; then
    echo "$tools_root exists but is not a pinned squashfs-tools build" >&2
    exit 1
fi

mkdir -p "$(dirname "$tools_root")"
temporary=$(mktemp -d "$(dirname "$tools_root")/.squashfs-tools.XXXXXX")
trap 'rm -rf "$temporary"' EXIT
trap 'exit 1' HUP INT TERM
git init -q "$temporary/source"
git -C "$temporary/source" fetch -q --depth 1 "$tools_url" "$tools_commit"
git -C "$temporary/source" checkout -q --detach FETCH_HEAD
make -C "$temporary/source/squashfs-tools" XZ_SUPPORT=1 LZ4_SUPPORT=0 \
    LZO_SUPPORT=0 ZSTD_SUPPORT=0 LZMA_XZ_SUPPORT=0 mksquashfs
mkdir -p "$temporary/out/bin"
mv "$temporary/source/squashfs-tools/mksquashfs" "$temporary/out/bin/mksquashfs"
echo "$tools_commit" > "$temporary/out/commit"
mv "$temporary/out" "$tools_root"
rm -rf "$temporary"
trap - EXIT HUP INT TERM
echo "mksquashfs ready in $tools_root/bin"

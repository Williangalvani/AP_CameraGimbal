#!/bin/sh
# Builds onvif-check on HOST (a Linux machine with cargo) and runs it there.
#   run.sh HOST requests             writes MCM's requests to ../fixtures/onvif/
#   run.sh HOST check DIR [KIND...]  checks the replies saved as DIR/<KIND>.xml
set -eu
here=$(cd "$(dirname "$0")" && pwd)
host=$1
mode=$2
rsync -a --delete --exclude target --exclude replies --exclude requests "$here/" "$host:onvif-check/"
ssh "$host" 'cd onvif-check && PATH="$HOME/.cargo/bin:$PATH" cargo build --release -q'
rsync -a "$host:onvif-check/Cargo.lock" "$here/Cargo.lock"
case "$mode" in
requests)
    ssh "$host" 'rm -rf onvif-check/requests && mkdir onvif-check/requests && cd onvif-check/requests && ../target/release/onvif-check requests'
    mkdir -p "$here/../fixtures/onvif"
    rsync -a "$host:onvif-check/requests/" "$here/../fixtures/onvif/"
    ;;
check)
    dir=$3
    shift 3
    kinds=${*:-probe device-information services profiles stream-uri not-authorized}
    rsync -a --delete "$dir/" "$host:onvif-check/replies/"
    ssh "$host" "cd onvif-check && rc=0; for kind in $kinds; do echo \"== \$kind\"; ./target/release/onvif-check \$kind replies/\$kind.xml || rc=1; done; exit \$rc"
    ;;
*)
    echo "usage: $0 HOST requests | $0 HOST check DIR [KIND...]" >&2
    exit 2
    ;;
esac

#!/usr/bin/env bash
# Network outage test: a private sshd on 127.0.0.1:2299 (its own host key and login
# key, nothing else touched) is killed in the middle of a copy and started again.
#   1. a 20 s outage: the run waits, reconnects, resumes the big file, finishes;
#   2. --network-wait=8 with a 25 s outage: the run stops (exit 3), the next resumes.
# Usage: tests/network-outage.sh [gcc|clang]   (needs /usr/sbin/sshd; no root)
set -u
CLI=$(cd "$(dirname "$0")/.." && pwd)/out/${1:-clang}/treesync-cli
T=$HOME/.cache/treesync-outage
pass=0; fail=0
check() { if eval "$2"; then echo "PASS  $1"; pass=$((pass+1)); else echo "FAIL  $1"; fail=$((fail+1)); fi; }
desc()  { for c in $(pgrep -P "$1"); do echo "$c"; desc "$c"; done; }
start() { /usr/sbin/sshd -f "$T/sshd/sshd_config" -E "$T/sshd/log"; }
stop()  { local m; m=$(cat "$T/sshd/sshd.pid"); local k; k=$(desc "$m"); kill -9 "$m" $k 2>/dev/null; }
outage(){ sleep "$1"; stop; echo "      (sshd down)"; sleep "$2"; start; echo "      (sshd up)"; }
same()  { [ "$(cd "$T/src" && find . -type f | sort | xargs sha256sum)" = "$(cd "$T/dst" && find . -type f | sort | xargs sha256sum)" ]; }

[ -x "$CLI" ] || { echo "build first: $CLI"; exit 1; }
rm -rf "$T"; mkdir -p "$T/src/small" "$T/sshd"
ssh-keygen -q -t ed25519 -N "" -f "$T/sshd/host_key"
ssh-keygen -q -t ed25519 -N "" -f "$T/sshd/user_key"
cp "$T/sshd/user_key.pub" "$T/sshd/authorized_keys"
cat > "$T/sshd/sshd_config" <<CFG
Port 2299
ListenAddress 127.0.0.1
HostKey $T/sshd/host_key
PidFile $T/sshd/sshd.pid
AuthorizedKeysFile $T/sshd/authorized_keys
PasswordAuthentication no
KbdInteractiveAuthentication no
UsePAM no
StrictModes no
Subsystem sftp internal-sftp
CFG
echo "[127.0.0.1]:2299 $(cat "$T/sshd/host_key.pub")" > "$T/sshd/known_hosts"
head -c 200M /dev/urandom > "$T/src/big.bin"
for i in $(seq 1 60); do head -c 20000 /dev/urandom > "$T/src/small/f$i"; done
start || { echo "cannot start the test sshd (is port 2299 in use?)"; exit 1; }
sleep 1

A=(--db="$T/db" --source="$T/src" --target-host=127.0.0.1 --target-port=2299 --target-key="$T/sshd/user_key"
   --known-hosts="$T/sshd/known_hosts" --target="$T/dst" --mirror --no-dry-run --threads=2 --bwlimit=20480 --chunk-mib=8)
run()   { "$CLI" "${A[@]}" "$@" 2>&1 | tr '\r' '\n' | grep -E "^network|resuming|FAILED|^finished|^stopped" > "$T/out"; rc=${PIPESTATUS[0]}; sed 's/^/      /' "$T/out"; }

echo "== 1. 20 s outage while copying"
outage 3 20 & run; wait
check "finished (exit $rc)" '[ $rc = 0 ]'
check "said the connection was lost" 'grep -q "lost the connection" $T/out'
check "said it was back" 'grep -q "is back after" $T/out'
check "big file resumed at its chunk" 'grep -q "resuming big.bin at chunk" $T/out'
check "copy identical to the source" same

echo "== 2. --network-wait=8, 25 s outage"
rm -rf "$T/dst" "$T/dst.old" "$T"/db*
outage 3 25 & run --network-wait=8; wait
check "stopped (exit $rc)" '[ $rc = 3 ]'
check "said it gave up" 'grep -q "did not come back" $T/out'
run
check "next run resumed and finished (exit $rc)" '[ $rc = 0 ] && grep -q "resuming big.bin" $T/out'
check "copy identical to the source" same

stop; rm -rf "$T"
echo "== $pass passed, $fail failed"
[ $fail = 0 ]

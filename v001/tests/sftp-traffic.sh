#!/usr/bin/env bash
# SFTP traffic test: copies 128 MiB to a private sshd on 127.0.0.1:2298 and counts
# what crossed the connections. A verified copy should send about the file's size
# and receive about the same again (the read-back), not several times more.
# Usage: tests/sftp-traffic.sh [gcc|clang]   (needs /usr/sbin/sshd; no root)
set -u
CLI=$(cd "$(dirname "$0")/.." && pwd)/out/${1:-clang}/treesync-cli
T=$HOME/.cache/treesync-traffic
rm -rf "$T"; mkdir -p "$T/src" "$T/sshd"
ssh-keygen -q -t ed25519 -N "" -f "$T/sshd/host_key"; ssh-keygen -q -t ed25519 -N "" -f "$T/sshd/user_key"
cp "$T/sshd/user_key.pub" "$T/sshd/authorized_keys"
cat > "$T/sshd/sshd_config" <<CFG
Port 2298
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
echo "[127.0.0.1]:2298 $(cat "$T/sshd/host_key.pub")" > "$T/sshd/known_hosts"
/usr/sbin/sshd -f "$T/sshd/sshd_config" -E "$T/sshd/log" || { echo "cannot start the test sshd"; exit 1; }
head -c 128M /dev/urandom > "$T/src/file.bin"
sleep 0.5

# Copies with treesync-cli and records the largest byte counts each connection
# reached, sampled while it runs; sets rc, sent, recv.
measure() {
  "$CLI" --db="$T/db" --known-hosts="$T/sshd/known_hosts" --copy --no-dry-run --threads=1 "$@" >/dev/null 2>&1 &
  local pid=$!
  while kill -0 $pid 2>/dev/null; do
    ss -tinH state established "( dport = :2298 )" | awk '/^[0-9]/{a=$3} /bytes_/{for(i=1;i<=NF;i++){if($i~/^bytes_acked:/){split($i,x,":");s=x[2]} if($i~/^bytes_received:/){split($i,x,":");r=x[2]}} print a, s, r}'
    sleep 0.1
  done > "$T/samples"
  wait $pid; rc=$?
  read sent recv < <(awk '{if($2>s[$1])s[$1]=$2; if($3>r[$1])r[$1]=$3} END{for(k in s){S+=s[k];R+=r[k]} print S+0, R+0}' "$T/samples")
}
mib() { echo "scale=1; $1 / 1048576" | bc; }
fail=0
limit=$((134217728 * 5 / 4))

measure --source="$T/src" --target-host=127.0.0.1 --target-port=2298 --target-key="$T/sshd/user_key" --target="$T/dst"
echo "local -> remote, exit $rc: sent $(mib $sent) MiB, received $(mib $recv) MiB (about 128 each way expected)"
[ $rc = 0 ] && [ $sent -lt $limit ] && [ $recv -lt $limit ] && cmp -s "$T/src/file.bin" "$T/dst/file.bin" || { echo "FAIL"; fail=1; }

measure --source-host=127.0.0.1 --source-port=2298 --source-key="$T/sshd/user_key" --source="$T/dst" --target="$T/back"
echo "remote -> local, exit $rc: sent $(mib $sent) MiB, received $(mib $recv) MiB (about 128 received expected)"
[ $rc = 0 ] && [ $recv -lt $limit ] && cmp -s "$T/src/file.bin" "$T/back/file.bin" || { echo "FAIL"; fail=1; }

kill "$(cat "$T/sshd/sshd.pid")"
rm -rf "$T"
[ $fail = 0 ] && echo PASS
exit $fail

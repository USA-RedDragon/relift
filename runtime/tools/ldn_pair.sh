#!/usr/bin/env bash
# Local wireless play between two processes on a private network: a user and
# network namespace (no root) holds a veth pair whose ends sit in two more
# namespaces, 10.13.0.1/24 and 10.13.0.2/24. One process hosts a network
# through ldn:u with net.mode lan, the other finds it, joins it and leaves.
# Nothing reaches the host's own network. Linux only (unshare, ip).
#
#   tools/ldn_pair.sh [switch_tests binary] [work dir]
set -euo pipefail

if [ "${1:-}" != --inside ]; then
  tests=$(realpath "${1:-build/host/switch_tests}")
  work=$(realpath -m "${2:-build/host/ldn-pair}")
  rm -rf "$work"
  mkdir -p "$work"
  exec unshare -rn --kill-child bash "$0" --inside "$tests" "$work"
fi

tests=$2
work=$3
# One namespace per member, each waiting until its end of the link is moved in.
member() {
  local name=$1 dev=$2 addr=$3 role=$4
  exec unshare -n bash -c '
    while [ ! -e "$0/$1.go" ]; do sleep 0.05; done
    ip link set lo up
    ip addr add "$3" dev "$2"
    ip link set "$2" up
    RELIFT_LDN_PAIR="$4" RELIFT_NET_IF="$2" "$5" ldn_pair_hosts_or_joins >"$0/$1.log" 2>&1
  ' "$work" "$name" "$dev" "$addr" "$role" "$tests" &
}

ip link add va type veth peer name vb
member host va 10.13.0.1/24 host
host_pid=$!
member station vb 10.13.0.2/24 station
station_pid=$!
# Each member's process is its namespace once unshare has run.
sleep 0.3
ip link set va netns "$host_pid"
ip link set vb netns "$station_pid"
touch "$work/host.go"
sleep 0.3
touch "$work/station.go"
rc=0
wait "$host_pid" || rc=1
wait "$station_pid" || rc=1
for m in host station; do
  echo "== $m"
  grep -E '^(ok|FAIL)|ldn pair|check|failed' "$work/$m.log" || cat "$work/$m.log"
done
exit $rc

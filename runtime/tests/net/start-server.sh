#!/bin/sh
# Start the networking fixture server for ctest (FIXTURES_SETUP net-server) and
# wait until it has written servers.json. The server runs detached, with every
# descriptor redirected, so ctest does not wait on it; net-fixture-stop ends it.
#
#   start-server.sh <node> <server.mjs> <state-dir>
node=$1
server=$2
dir=$3

if [ -f "$dir/server.pid" ]; then
  kill "$(cat "$dir/server.pid")" 2>/dev/null
fi
rm -rf "$dir"
mkdir -p "$dir"

nohup "$node" "$server" --dir "$dir" >"$dir/server.log" 2>&1 </dev/null &
pid=$!

# Up to a minute: the server generates the media fixtures with ffmpeg first.
ticks=0
while [ "$ticks" -lt 600 ]; do
  if [ -f "$dir/servers.json" ]; then
    cat "$dir/servers.json"
    exit 0
  fi
  kill -0 "$pid" 2>/dev/null || break
  sleep 0.1
  ticks=$((ticks + 1))
done
echo "start-server: the fixture server did not come up" >&2
cat "$dir/server.log" >&2
exit 1

#!/bin/sh
# Stop the networking fixture server (FIXTURES_CLEANUP net-server).
#   stop-server.sh <state-dir>
dir=$1
if [ -f "$dir/server.pid" ]; then
  kill "$(cat "$dir/server.pid")" 2>/dev/null
  rm -f "$dir/server.pid"
fi
rm -f "$dir/servers.json"
exit 0

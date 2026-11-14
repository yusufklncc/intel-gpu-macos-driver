#!/bin/bash
# SPDX-License-Identifier: MIT
# gui.sh <tag> [VAR=val...] -- <program> [args]: run <program> in the console user's GUI session (a launchd agent, since
# ssh is a Background session), output to /tmp/d3/<tag>.txt; guarded (java / geekbench_avx2), stopped after 30 s.
T=$1; shift; H=$(cd "$(dirname "$0")" && pwd); ENV=""
while [ $# -gt 0 ] && [ "$1" != "--" ]; do ENV="$ENV<key>${1%%=*}</key><string>${1#*=}</string>"; shift; done; shift
"$H/g.sh" /usr/bin/true >/dev/null || { "$H/g.sh" /usr/bin/true; exit 3; }
mkdir -p /tmp/d3; O=/tmp/d3/$T.txt; P=/tmp/d3/$T.plist; L=org.hackintosh.gui.$T; rm -f "$O"
ARGS=""; for a in "$@"; do ARGS="$ARGS<string>$a</string>"; done
cat > "$P" <<EOP
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>Label</key><string>$L</string><key>ProgramArguments</key><array>$ARGS</array>
<key>EnvironmentVariables</key><dict>$ENV</dict><key>StandardOutPath</key><string>$O</string><key>StandardErrorPath</key><string>$O</string>
<key>ProcessType</key><string>Interactive</string><key>LimitLoadToSessionType</key><string>Aqua</string><key>AbandonProcessGroup</key><true/></dict></plist>
EOP
launchctl bootout gui/$(id -u)/$L 2>/dev/null
launchctl bootstrap gui/$(id -u) "$P" && launchctl kickstart gui/$(id -u)/$L || { echo "launch failed"; exit 1; }
for i in $(seq 1 60); do sleep 0.5; launchctl print gui/$(id -u)/$L 2>/dev/null | grep -q "state = running" || break; done
launchctl bootout gui/$(id -u)/$L 2>/dev/null
cat "$O"

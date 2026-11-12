#!/bin/sh
# SPDX-License-Identifier: MIT
# unit-inside.sh: unit-test.sh's checks, run inside a container with the repository at /src (read-only).
set -u
LIB=/src/tools/probe-stick/rootfs/usr/lib/xe-stick
CL=/src/tools/report/collect-linux.sh
FAILS=0
ok() { printf '  ok   %s\n' "$1"; }
no() { printf '  FAIL %s\n' "$1"; FAILS=$((FAILS + 1)); }
t() { if eval "$2"; then ok "$1"; else no "$1"; fi; }
W=$(mktemp -d)

echo "== trace-filter.awk"
cat > "$W/raw" <<'T'
VERSION 20070824
PCIDEV 0300	8086e20b	10	fb000000	0	4000000000	0	0	0	0	1000000	0	400000000	0	0	0	0	xe
MAP 1.000000 1 0xfb000000 0xffffc90000000000 0x1000000 0x0 0
MAP 1.000100 2 0x4000000000 0xffffc90100000000 0x400000000 0x0 0
MARK 1.000200 xe-stick: bind xe 0000:03:00.0
R 4 1.000300 1 0xfb138100 0x00000001 0x0 0
W 4 1.000400 1 0xfbffffff 0x12345678 0x0 0
W 4 1.000500 2 0x4000000000 0x00000000 0x0 0
W 4 1.000600 2 0x43ffffffff 0xdeadbeef 0x0 0
R 4 1.000700 1 0xfc000000 0x0 0x0 0
UNMAP 1.000800 2 0xffffc90100000000 0
T
awk -v ranges="0x0000004000000000 0x00000043ffffffff" -v stats="$W/stats" -f "$LIB/trace-filter.awk" "$W/raw" > "$W/kept"
t "the two VRAM BAR accesses are dropped (64-bit addresses)" '[ "$(grep -c "^W 4 1.00050\|^W 4 1.00060" "$W/kept")" = 0 ]'
t "register accesses, inside and just past BAR0, are kept" '[ "$(grep -c "^[RW] " "$W/kept")" = 3 ]'
t "VERSION, PCIDEV, MAP, MARK and UNMAP lines are kept" '[ "$(grep -cE "^(VERSION|PCIDEV|MAP|MARK|UNMAP) " "$W/kept")" = 6 ]'
t "the counts: 11 lines, 9 kept, 5 accesses, 2 dropped" '[ "$(tr "\n" " " < "$W/stats")" = "lines 11 kept 9 accesses 5 dropped 2 " ]'
awk -v ranges="" -v stats="$W/stats2" -f "$LIB/trace-filter.awk" "$W/raw" > "$W/kept2"
t "no ranges (xestick.trace=full): every line kept" 'cmp -s "$W/raw" "$W/kept2"'

echo "== collect-linux.sh --redact (the stick's redaction of traces and logs)"
# test values, spelled in parts so that no address or key shape sits in the repository
MAC1=$(printf '%s:' 52 54 00 12 34)56 MAC2=$(printf '%s:' 02 11 22 33 44)55
KEY1=$(printf '%s-' ABCDE ABCDE ABCDE ABCDE)ABCDE KEY2=$(printf '%s-' GHJKL ABCDE 12345 ABCDE)ABCDE
mkdir -p "$W/r/trace" "$W/r/stick"
printf 'serial\tXSSERIAL123\nhost\tmy-box\nmac\t%s\n' "$MAC1" > "$W/lit"
cp "$W/raw" "$W/r/trace/a.mmio"
printf 'MARK 2.0 board XSSERIAL123 on my-box\n' >> "$W/r/trace/a.mmio"
printf 'usb 1-1: SerialNumber: ABCDEF12345\neth0 %s at /home/someone/x key %s\n' "$MAC2" "$KEY1" > "$W/r/stick/dmesg.txt"
sh "$CL" --redact "$W/lit" "$W/r" > "$W/out" 2>&1
t "exit 0 when everything is removed" '[ $? = 0 ]'
t "the planted values are gone from the trace" '! grep -rqE "XSSERIAL123|my-box" "$W/r"'
t "   ... replaced by their marks" 'grep -q "MARK 2.0 board <serial-redacted> on <host-redacted>" "$W/r/trace/a.mmio"'
t "the rest of the trace is byte-identical" '[ "$(head -n 11 "$W/r/trace/a.mmio" | cksum)" = "$(cksum < "$W/raw")" ]'
t "the fixed rules apply: serial line, MAC, home path, product key" 'grep -q "SerialNumber: <redacted>" "$W/r/stick/dmesg.txt" && grep -q "xx:xx:xx:xx:xx:xx at /home/USER/x key XXXXX-XXXXX" "$W/r/stick/dmesg.txt"'
printf 'x XSSERIAL123\n' > "$W/r/trace/vbt.bin"
sh "$CL" --redact "$W/lit" "$W/r" > "$W/out" 2>&1
t "a value inside a .bin file (left as it is) fails the privacy check: exit 3" '[ $? = 3 ] && grep -q "a serial value is still present in .*vbt.bin" "$W/out"'
rm -f "$W/r/trace/vbt.bin"
printf '%s\n' "$KEY2" > "$W/r/trace/key.bin"
sh "$CL" --redact "$W/lit" "$W/r" > "$W/out" 2>&1
t "a product key in a .bin file fails the privacy check: exit 3" '[ $? = 3 ]'
rm -f "$W/r/trace/key.bin"
sh "$CL" --redact "$W/lit" > "$W/out" 2>&1
t "--redact without a PATH is refused: exit 2" '[ $? = 2 ]'

echo "== collect-linux.sh as a standalone script"
mkdir -p "$W/o"
sh "$CL" --help > "$W/help" 2>&1
t "--help prints the usage" 'grep -q -- "--literals-out FILE" "$W/help"'
sh "$CL" --out "$W/o" --id XR-0123ABCD --time 30 --acpi --literals-out "$W/l1" > "$W/run" 2>&1
t "runs as root and writes its report" '[ -f "$W/o/xe-report-XR-0123ABCD.tar.gz" ]'
t "   ... --literals-out holds the values, mode 600" '[ -f "$W/l1" ] && [ "$(stat -c %a "$W/l1")" = 600 ]'
(cd "$W/o" && tar -xzf xe-report-XR-0123ABCD.tar.gz)
t "   ... the archive holds summary.txt and manifest.txt" '[ -f "$W/o/xe-report-XR-0123ABCD/summary.txt" ] && [ -f "$W/o/xe-report-XR-0123ABCD/manifest.txt" ]'
t "   ... no learned value is left in it" '! cut -f2 "$W/l1" | grep -qF -f - -r "$W/o/xe-report-XR-0123ABCD"'
t "   ... and the summary's own words are not redacted" '! grep -q "<redacted>" "$W/o/xe-report-XR-0123ABCD/summary.txt"'
sh "$CL" --id XR-1 > /dev/null 2>&1
t "a malformed --id is refused: exit 2" '[ $? = 2 ]'
if command -v su >/dev/null 2>&1 && id nobody >/dev/null 2>&1; then
	chmod 755 "$W" && mkdir -p "$W/u" && chmod 777 "$W/u"
	su -s /bin/sh nobody -c "cd '$W/u' && sh '$CL' --time 20" > "$W/urun" 2>&1
	t "runs as an ordinary user and writes its report" 'ls "$W/u"/xe-report-XR-*.tar.gz >/dev/null 2>&1'
fi
rm -rf "$W"
echo
if [ $FAILS = 0 ]; then echo "ALL PASSED"; else echo "$FAILS FAILED"; fi
exit $((FAILS > 0))

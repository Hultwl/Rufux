#!/bin/bash
# Windows ISO download against a local stand-in for Microsoft's service (mock_msdl.py). Checks
# the whole protocol order, language and architecture selection, resume, error handling and
# that nothing is fetched over plain HTTP. The real service cannot be reached from CI.
# Usage: test_msdl.sh /path/to/rufux      (exit 77 = skipped)
R=$1; HERE=$(cd "$(dirname "$0")" && pwd)
for t in curl python3 sha256sum; do command -v $t >/dev/null || { echo "skip: $t missing"; exit 77; }; done
T=$(mktemp -d); MOCKPID=
stop() { [ -n "$MOCKPID" ] && kill $MOCKPID 2>/dev/null; wait $MOCKPID 2>/dev/null; MOCKPID=; }
trap 'stop; rm -rf $T' EXIT
fail=0; bad() { echo "FAIL $*"; fail=1; }
start() { stop; rm -f $T/port $T/req.log; python3 $HERE/mock_msdl.py $T/port $T/req.log "${1:-ok}" & MOCKPID=$!
  for i in $(seq 1 50); do [ -s $T/port ] && break; sleep 0.1; done; export RUFUX_MSDL_MOCK=http://127.0.0.1:$(cat $T/port); }
EXPECT=$(python3 -c "import hashlib;print(hashlib.sha256((hashlib.sha256(b'rufux-iso').digest()*100000)[:3*1024*1024+123]).hexdigest())")
dw() { "$R" download-windows "$@"; }

start ok
dw --list | grep -q "Windows 11.*25H2" || bad "--list shows no Windows 11 release"
dw --list-langs > $T/langs 2>&1 || bad "list-langs failed: $(cat $T/langs)"
[ "$(wc -l < $T/langs)" = 5 ] || bad "languages from two sessions not merged into 5 entries: $(cat $T/langs)"
grep -qx "English|English (United States)" $T/langs || bad "language display names lost"
# protocol order: tags -> mdt.js -> ov-df -> SKU lookup, per session, and the cookie set by /tags is sent back
python3 - $T/req.log <<'PY' || bad "request order or cookies wrong"
import sys,re
lines=[l for l in open(sys.argv[1]).read().splitlines()]
paths=[l.split()[1].split("?")[0] for l in lines]
want=["/vlscppe/tags","/ov-df/mdt.js","/ov-df/","/www/software-download-connector/api/getskuinformationbyproductedition"]
i=0
for p in paths:
    if i<len(want) and p==want[i]: i+=1
assert i==len(want), paths
assert paths.count("/vlscppe/tags")==2, "Windows 11 needs one session per architecture id"
PY
echo "ok list"

# links
url=$(dw --url-only) || bad "url-only failed"; echo "$url" | grep -q "x64.iso?t=1" || bad "default architecture is not x64: $url"
dw --url-only --arch ARM64 | grep -q "Arm64.iso" || bad "--arch ARM64 not honoured"
dw --arch x86 >$T/o 2>&1; [ $? = 2 ] && grep -q "Available: x64 ARM64\|Available: ARM64 x64" $T/o || bad "missing arch not explained: $(cat $T/o)"
grep -q "ref=https://www.microsoft.com/software-download/windows11" $T/req.log || bad "Referer not sent on the link request"
grep -q "ua=Mozilla" $T/req.log || bad "no browser user agent"
# languages
dw --url-only --lang German | grep -q German || bad "exact language"
dw --url-only --lang "English International" | grep -q EnglishInternational || bad "exact language with a space"
dw --url-only --lang "french c" | grep -q FrenchCanadian || bad "prefix match, case-insensitive"
dw --url-only --lang Fre >$T/o 2>&1; [ $? = 2 ] && grep -q "matches several" $T/o || bad "ambiguous prefix accepted: $(cat $T/o)"
dw --url-only --lang Klingon >$T/o 2>&1; [ $? = 2 ] && grep -q "Available:" $T/o || bad "unknown language: $(cat $T/o)"
LANG=fr_FR.UTF-8 LC_ALL= dw --url-only | grep -q "French" || bad "system locale not used for the default language"
LANG=C LC_ALL= dw --url-only | grep -q "English" || bad "default language is not English"
# other editions / Windows 10 (a single edition id, so a single session)
dw --version 10 --url-only >/dev/null || bad "windows 10 lookup"; n=$(grep -c "/vlscppe/tags" $T/req.log)
dw --version 10 --edition 1 --url-only >/dev/null || bad "windows 10 China edition"
dw --version 12 >/dev/null 2>&1; [ $? = 2 ] || bad "unknown version accepted"
dw --edition 9 >/dev/null 2>&1; [ $? != 0 ] || bad "unknown edition accepted"
echo "ok links"

# download
mkdir -p $T/out; rm -f $T/req.log
dw --lang English --out $T/out > $T/o 2>$T/e || bad "download failed: $(cat $T/e)"
iso=$(sed -n 's/^ISO: //p' $T/o); [ -f "$iso" ] || bad "no ISO reported: $(cat $T/o)"
[ "$(sha256sum "$iso" | cut -d' ' -f1)" = "$EXPECT" ] || bad "downloaded file differs from the served one"
grep -q "^SHA-256: $EXPECT" $T/o || bad "SHA-256 not printed / wrong"
[ ! -e "$iso.part" ] || bad ".part left behind"
grep -q "^100%" $T/e || bad "no progress lines for the GUI: $(tail -2 $T/e)"
name=$(basename "$iso"); case $name in Win11_25H2_English_x64.iso) ;; *) bad "unexpected file name '$name' (query string leaked into it?)";; esac
[ -z "$(ls /tmp/rufux-msdl-* 2>/dev/null)" ] || bad "cookie file left in /tmp"
# refuses to overwrite
dw --lang English --out $T/out >$T/o 2>&1; [ $? = 3 ] && grep -q "already exists" $T/o || bad "overwrote an existing ISO: $(cat $T/o)"
echo "ok download"

# resume: a partial file is continued with a Range request, not restarted
rm -f "$iso"; head -c 1048576 <(python3 -c "import sys,hashlib;sys.stdout.buffer.write((hashlib.sha256(b'rufux-iso').digest()*100000)[:3*1024*1024+123])") > "$iso.part"
rm -f $T/req.log; dw --lang English --out $T/out >$T/o 2>&1 || bad "resume failed: $(cat $T/o)"
grep -q "range=bytes=1048576-" $T/req.log || bad "resume did not send a Range request"
[ "$(sha256sum "$iso" | cut -d' ' -f1)" = "$EXPECT" ] || bad "resumed file is corrupt"
echo "ok resume"

# service says the IP is banned
start banned; dw --out $T/out --lang German >$T/o 2>&1; rc=$?
[ $rc = 3 ] && grep -q "715-123130" $T/o || bad "banned IP not explained (rc=$rc): $(cat $T/o)"
# transient error on the language lookup is retried
start flaky; dw --list-langs >$T/o 2>&1 || bad "flaky service not retried: $(cat $T/o)"
# a link that is not HTTPS is never followed
start httplink; dw --url-only >$T/o 2>&1; rc=$?; [ $rc = 3 ] && grep -q "no ISO download links" $T/o || bad "http link accepted (rc=$rc): $(cat $T/o)"
# an HTTP error status is reported with the status, not as garbage
start http503; dw --list-langs >$T/o 2>&1; rc=$?; [ $rc = 3 ] && grep -q "answered HTTP 503" $T/o || bad "HTTP 503 not reported (rc=$rc): $(cat $T/o)"
# server gone
stop; RUFUX_MSDL_MOCK=http://127.0.0.1:9 dw --list-langs >$T/o 2>&1; rc=$?; [ $rc = 3 ] && grep -q "network error" $T/o || bad "network failure not reported (rc=$rc): $(cat $T/o)"
# the mock hook is loopback only: anything else is ignored (and then talks to the real Microsoft, so just check it does not use it)
RUFUX_MSDL_MOCK=http://evil.example:80 timeout 20 "$R" download-windows --list-langs >$T/o 2>&1; grep -q "evil.example" $T/o && bad "non-loopback mock host honoured"
echo "ok errors"
exit $fail

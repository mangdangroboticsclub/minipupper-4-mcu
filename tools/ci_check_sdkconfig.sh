#!/usr/bin/env bash
# Stage 0 verification: can CI build with no sdkconfig at all?
#
# sdkconfig is no longer in version control, so CI gets only sdkconfig.defaults.
# If the defaults are missing something the committed sdkconfig had, CI builds a
# different firmware than the one on the robots. This configures a scratch build
# tree from the defaults alone and diffs the result against the committed file.
set -uo pipefail

cd /workspaces/md-mpx-dog || exit 1
source /opt/esp/idf/export.sh >/dev/null 2>&1

mkdir -p build/ci_check
cp sdkconfig build/ci_check/sdkconfig.committed || exit 1
rm -f sdkconfig

idf.py -B build/ci_check/tree -DIDF_TARGET=esp32s3 -DMPX_BUILD_PWA=OFF reconfigure \
    > build/ci_check/reconfigure.log 2>&1
rc=$?
echo "reconfigure rc=$rc"

if [ -f sdkconfig ]; then
    cp sdkconfig build/ci_check/sdkconfig.from_defaults
else
    echo "NO sdkconfig PRODUCED"
fi

# Always put the developer's sdkconfig back, whatever happened above.
mv build/ci_check/sdkconfig.committed sdkconfig

if [ "$rc" -ne 0 ]; then
    tail -40 build/ci_check/reconfigure.log
    exit "$rc"
fi

diff -u build/ci_check/sdkconfig.from_defaults sdkconfig > build/ci_check/sdkconfig.diff
echo "diff lines: $(wc -l < build/ci_check/sdkconfig.diff)"
echo "--- symbols the committed sdkconfig has that the defaults do not:"
grep -E '^[-+]CONFIG_' build/ci_check/sdkconfig.diff | grep -v '^---' | grep -v '^+++' | wc -l

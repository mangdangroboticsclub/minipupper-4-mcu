#!/usr/bin/env bash
# Rebuild components/micro_ros (libmicroros.a + include/) for THIS project.
#
# Linux or WSL2 only, with ESP-IDF 5.5 exported (. $IDF_PATH/export.sh). Needed
# after an ESP-IDF update, a big sdkconfig change, or a change to
# app-colcon.meta (more publishers, a bigger MTU, ...). Takes ~10 minutes.
#
#   tools/uros/rebuild_microros.sh
#
# How: micro_ros_espidf_component builds the libraries while CMake configures
# a project that contains it. So this puts it in components/ for one
# configure in a scratch build folder, keeps what it produced, and takes it
# out again.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
UROS_REF=${UROS_REF:-jazzy}
WORK="$ROOT/build_microros"
COMP="$ROOT/components/micro_ros_espidf_component"
OUT="$ROOT/components/micro_ros"

[ -n "${IDF_PATH:-}" ] || { echo "export ESP-IDF first: . \$IDF_PATH/export.sh"; exit 1; }
python -m pip install -q catkin_pkg lark-parser colcon-common-extensions "empy==3.3.4"

rm -rf "$COMP" "$WORK"
git clone -q -b "$UROS_REF" --depth 1 \
    https://github.com/micro-ROS/micro_ros_espidf_component.git "$COMP"
rm -rf "$COMP/.git" "$COMP/examples" "$COMP/idf_component.yml"
# Our own Wi-Fi brings the network up; the component must not.
sed -i 's/option(BUILD_WLAN_INTERFACE "Build wlan interface support" ON)/option(BUILD_WLAN_INTERFACE "Build wlan interface support" OFF)/' "$COMP/CMakeLists.txt"
sed -i 's/REQUIRES nvs_flash esp_wifi esp_eth lwip)/REQUIRES lwip)/' "$COMP/CMakeLists.txt"

# Only one of the two may be in the build.
mv "$OUT" "$ROOT/micro_ros.keep"
restore() {
    rm -rf "$COMP"
    if [ -d "$ROOT/micro_ros.keep" ]; then
        rm -rf "$OUT"
        mv "$ROOT/micro_ros.keep" "$OUT"
    fi
}
trap restore EXIT

( cd "$ROOT" && idf.py -B "$WORK" reconfigure )

[ -f "$COMP/libmicroros.a" ] || { echo "no libmicroros.a was produced"; exit 1; }
rm -rf "$ROOT/micro_ros.keep/include" "$ROOT/micro_ros.keep/libmicroros.a"
cp -r "$COMP/include" "$ROOT/micro_ros.keep/include"
cp "$COMP/libmicroros.a" "$ROOT/micro_ros.keep/libmicroros.a"
cp "$COMP/LICENSE" "$COMP/3rd-party-licenses.txt" "$ROOT/micro_ros.keep/"
rm -rf "$WORK"
echo "components/micro_ros rebuilt ($UROS_REF). Now: idf.py fullclean build"

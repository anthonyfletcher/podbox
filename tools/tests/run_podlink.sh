#!/bin/sh
# Offline protocol, serial and screen checks; no device access.
set -eu
cd "$(dirname "$0")/../.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM
cc=${CC:-cc}
for test in podlink_firmware podlink_wire podlink_screen; do
    case "$test" in
        podlink_screen) includes='-Itools/tests/podlink_stubs'; codec='' ;;
        *) includes='-Itools/tests/podlink_stubs'
           codec='firmware/usbstack/podlink_wire.c' ;;
    esac
    "$cc" -std=gnu11 -Wall -Wextra -Werror -g \
        -fsanitize=address,undefined -fno-pie -no-pie \
        -ffunction-sections -fdata-sections -Wl,--gc-sections \
        $includes -Ifirmware/export -Ifirmware/usbstack \
        "tools/tests/${test}_test.c" $codec -o "$test_dir/$test"
    "$test_dir/$test"
done
"${PYTHON:-python3}" -m unittest discover -s tools/tests -p test_podlink.py -v

#!/usr/bin/env bash
# Copyright: 2021 Twinleaf LLC
# License: Proprietary

os=$(uname)
devs=""
multi=""


if [ "${os}" == "Darwin" ]; then
    devs=$(ioreg -k kUSBVendorString -p IOUSB | tr -d '|"' |
	       grep -e '^ *\(kUSBVendorString\|USB Serial Number\)' |
	       awk '($1 == "USB"){serial = $5}\
		    ($3 == "Twinleaf"){print "/dev/cu.usbmodem"serial"1"}')
elif [ "${os}" == "Linux" ]; then
    devs=$(for devfile in $(find /dev -name 'ttyACM*'); do
               (eval "$(udevadm info -q property --export -p \
                        $(udevadm info -q path -n ${devfile}))"
                if [ "$ID_VENDOR" == "Twinleaf_LLC" ]; then
                    echo "${devfile}"
                fi)
           done)
fi

devs=($devs)
if [ "${#devs[@]}" -ne "1" ]; then
    multi="-h"
fi
cmd="${0//autoproxy.sh/proxy} ${multi} ${@} ${devs[@]}"
cmd=($cmd)
cmd="${cmd[@]}"
echo "Running: $cmd"
eval "$cmd"

#!/bin/bash -e

prefix=${4:-"FDATA"}

grep -e "^# ${prefix}:" < "$1" | sed -E "s/# ${prefix}: //g" > "$3"
mapfile -t symbols < <(nm --defined-only "$2")

for line in "${symbols[@]}"; do
    val=$(echo $line | cut -d' ' -f1)
    symname=$(echo $line | awk '{ $1=$2=""; print $0 }' | sed 's/^[ \t]*//')
    if [ -z "$symname" ]; then
        continue
    fi
    if [ -z "${val##*[!0-9a-fA-F]*}" ]; then
        continue
    fi
    sed -i -e "s/\#${symname}\#/$val/g" $3
done

#!/bin/bash
idx=`cat index 2>/dev/null`
idx=$(($idx+0))
name="$1"
if [ -z "$name" ]; then name=Untitled; fi
nn=`printf "%04d" "$idx"`
name="$nn-$name.kryoflux"

curdir=`pwd`
mkdir -p "$name" || exit 1
cd "$name" || exit 1

dtc -r3 -g2 '-ftrack' -i0 || exit 1

cd "$curdir" || exit 1

idx=$(($idx+1))
echo -n "$idx" >index


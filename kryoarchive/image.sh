#!/bin/bash
idx=`cat index 2>/dev/null`
idx=$(($idx+0))
name="$1"
if [ -z "$name" ]; then name=Untitled; fi
nn=`printf "%04d" "$idx"`
name="$nn-$name.kryoflux"

curdir=`pwd`

echo "About to read disk"
sleep 3

for i in 1 2; do
	mkdir -p "$curdir/$name/$i" || exit 1
	cd "$curdir/$name/$i" || exit 1
	dtc -r5 -g2 '-ftrack' -i0 || exit 1

	echo "Pause"
	sleep 3
done

cd "$curdir" || exit 1

idx=$(($idx+1))
echo -n "$idx" >index


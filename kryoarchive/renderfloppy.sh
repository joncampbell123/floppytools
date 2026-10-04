#!/bin/bash
top=`pwd`
for k in *.kryoflux; do
	if [ -d "$top/$k" ]; then
		cd "$top/$k" || exit 1
		list=.
		for i in 1 2 3 4 5 6 7 8 9; do
			if [ -d "$i" ]; then
				list="$list $i"
			fi
		done
		echo "Reading $list in $k in `pwd`"
		"$top/../kryo_ibm" $list || exit 1
	fi
done


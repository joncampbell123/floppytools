#!/bin/bash
mkdir -p __DONE__ || exit 1
for k in *.kryoflux; do
	echo "Packing $k" || exit 1
	tar -cf "$k.tar" "$k" || exit 1
	mv -vn "$k" __DONE__/ || exit 1
	xz -6e -v --threads=0 "$k.tar" || exit 1
done


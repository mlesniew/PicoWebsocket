#!/usr/bin/env bash

if [ "$#" -ne 1 ]
then
	echo "Usage: $0 <pio-board-name>"
	exit 1
fi

cd "$(dirname "$0")"

mkdir -p examples.build
ROOT_DIR=$PWD

BOARD="$1"

find examples -mindepth 1 -type d | cut -d/ -f2 | sort | while read -r EXAMPLE
do
	mkdir -p examples.build/$BOARD/$EXAMPLE
	pushd examples.build/$BOARD/$EXAMPLE
	if [ ! -f platformio.ini ]
	then
		pio init --board=$BOARD
	fi
	ln -s -f -t src/ "$ROOT_DIR/examples/$EXAMPLE/"*
	ln -s -f -t lib/ "$ROOT_DIR"
	if [ -e "$ROOT_DIR/config.h" ]
	then
		ln -s -f -t src/ "$ROOT_DIR/config.h"
	fi
	pio run
	popd
done

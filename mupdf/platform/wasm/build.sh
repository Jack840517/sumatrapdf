#!/bin/bash

make -j4 -C ../.. generate

source /opt/emsdk/emsdk_env.sh

echo Building generated files:
make -j4 -C ../.. generate

echo Building library:
make -j4 -C ../.. \
	OS=wasm build=release \
	XCFLAGS="-DTOFU -DTOFU_CJK -DFZ_ENABLE_SVG=0 -DFZ_ENABLE_HTML=0 -DFZ_ENABLE_EPUB=0 -DFZ_ENABLE_JS=0" \
	libs

echo
echo Linking WebAssembly:
emcc -Wall -Os -g1 -o libmupdf.js \
	-s WASM=1 \
	-s VERBOSE=0 \
	-s ASSERTIONS=1 \
	-s ABORTING_MALLOC=0 \
	-s ALLOW_MEMORY_GROWTH=1 \
	-s EXTRA_EXPORTED_RUNTIME_METHODS='["ccall","cwrap"]' \
	-I ../../include \
	--pre-js wrap.js \
	wrap.c \
	../../build/wasm/release/libmupdf.a \
	../../build/wasm/release/libmupdf-third.a

echo Done.

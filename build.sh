#!/bin/bash

rm -rf build
cmake -B build \
  -DBUILD_VECTOR=ON \
  -DSKIP_LIBRARY=ON \
  -DBUILD_STATIC=ON \
  -DBUILD_SHARED=OFF \
  -DUSE_ZLIB=OFF \
  -DUSE_PNG=OFF \
  -DUSE_FFMPEG=OFF \
  -DUSE_LUA=OFF \
  -DUSE_FREETYPE=OFF \
  -DUSE_LIBZIP=OFF \
  -DUSE_MINIZIP=OFF \
  -DUSE_SQLITE3=OFF \
  -DUSE_JSON_C=OFF \
  -DUSE_LZMA=OFF \
  -DUSE_DISCORD_RPC=OFF \
  -DUSE_EPOXY=OFF \
  -DENABLE_SCRIPTING=OFF \
  -DBUILD_SDL=OFF \
  -DBUILD_QT=OFF \
  -DBUILD_GL=OFF \
  -DBUILD_GLES2=OFF \
  -DBUILD_GLES3=OFF \
  -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
  -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY

make -C build

if [ -z "$ROBOT_IP" ]; then
    read -rp "What's the IP of your vector? " ROBOT_IP
    export ROBOT_IP
fi

scp -i ~/ssh_root_key build/vector/mgba-vector root@"$ROBOT_IP":/root/mgba

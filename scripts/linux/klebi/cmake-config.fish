#!/usr/bin/env fish

# Configures the cmake build for the release and debug builds

mkdir -p /tmp/build-cmake-debug   && ln -sfn /tmp/build-cmake-debug   || exit 1
mkdir -p /tmp/build-cmake-release && ln -sfn /tmp/build-cmake-release || exit 1

echo '*' > ./build-cmake-debug/.gitignore
echo '*' > ./build-cmake-release/.gitignore

cmake -H. -B build-cmake-debug -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake -H. -B build-cmake-release -GNinja -DCMAKE_BUILD_TYPE:STRING=Release
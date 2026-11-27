#!/usr/bin/env fish

# Configures the cmake build for the release and debug builds

cmake -H. -Bbuild-cmake-release -GNinja -DCMAKE_BUILD_TYPE:STRING=Release;
cmake -H. -Bbuild-cmake-debug -GNinja
#!/bin/bash
#echo "Installing necessary libraries using apt"
#sudo apt install libjpeg-turbo8-dev libjpeg-dev cmake
git clone https://chromium.googlesource.com/libyuv/libyuv
cd libyuv
mkdir out
cd out
cmake -DCMAKE_INSTALL_PREFIX="/usr/lib" -DCMAKE_BUILD_TYPE="Release" -DCMAKE_POSITION_INDEPENDENT_CODE=true ..
cmake --build . --config Release

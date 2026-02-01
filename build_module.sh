#!/bin/bash
set -e

echo "Building VCam Project for Pixel 4a..."

# Check for NDK
if [ -z "$ANDROID_NDK_HOME" ]; then
    echo "Error: ANDROID_NDK_HOME not set."
    exit 1
fi

# Build Native
echo "Building Native..."
mkdir -p build
cd build
cmake -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI=arm64-v8a \
      -DANDROID_PLATFORM=android-30 \
      ..
cmake --build .
cp src/cameraserver_proxy ..
cp src/libvcam_wrapper.so ..
cd ..

# Build APK
echo "Building APK..."
cd VCamController
chmod +x gradlew
./gradlew assembleRelease
cp app/build/outputs/apk/release/app-release-unsigned.apk ../VCamController.apk
cd ..

# Zip
echo "Packetizing Magisk Module..."
mkdir -p module_out/system/bin
mkdir -p module_out/system/lib64/hw
cp module.prop module_out/
cp service.sh module_out/
cp sepolicy.rule module_out/
cp cameraserver_proxy module_out/system/bin/
cp libvcam_wrapper.so module_out/system/lib64/hw/

# APK usually goes to data, but for module we can put it in /system/app or just let user install it.
# Let's verify instructions. Usually user installs APK manually.

cd module_out
zip -r ../pixel4a_vcam_v1.zip .
cd ..

echo "Done: pixel4a_vcam_v1.zip"

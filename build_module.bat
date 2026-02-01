@echo off
echo Building VCam Project for Pixel 4a...

REM Check for NDK
if "%ANDROID_NDK_HOME%"=="" (
    echo Error: ANDROID_NDK_HOME environment variable not set.
    echo Please set it to your NDK installation path.
    exit /b 1
)

REM Build C++ Components (Daemon + HAL Wrapper)
echo Building Native Components...
mkdir build
cd build
cmake -DCMAKE_TOOLCHAIN_FILE="%ANDROID_NDK_HOME%\build\cmake\android.toolchain.cmake" ^
      -DANDROID_ABI=arm64-v8a ^
      -DANDROID_PLATFORM=android-30 ^
      ..
if %errorlevel% neq 0 exit /b %errorlevel%

cmake --build .
if %errorlevel% neq 0 exit /b %errorlevel%

copy src\cameraserver_proxy ..\
copy src\libvcam_wrapper.so ..\
cd ..

REM Build Companion App (APK)
echo Building Companion App...
cd VCamController
call keytool -genkey -v -keystore release.keystore -alias vcam -keyalg RSA -keysize 2048 -validity 10000 -storepass vcam123 -keypass vcam123 -dname "CN=VCam, OU=Dev, O=Antigravity, L=Unknown, S=Unknown, C=US"
call gradlew.bat assembleRelease
if %errorlevel% neq 0 (
    echo Gradle Build Failed. Skipping APK copy.
) else (
    copy app\build\outputs\apk\release\app-release-unsigned.apk ..\VCamController.apk
)
cd ..

REM Create Magisk Module Zip
echo Creating Magisk Module Zip...
mkdir module_out
copy module.prop module_out\
copy service.sh module_out\
copy sepolicy.rule module_out\
copy cameraserver_proxy module_out\
mkdir module_out\system\lib64\hw
copy libvcam_wrapper.so module_out\system\lib64\hw\
mkdir module_out\system\bin
copy cameraserver_proxy module_out\system\bin\

echo Zipping...
powershell Compress-Archive -Path module_out\* -DestinationPath pixel4a_vcam_v1.zip -Force

echo Done! Output: pixel4a_vcam_v1.zip
pause

# Pixel 4a (Sunfish) Virtual Camera System

This project provides a complete Virtual Camera ecosystem for the Google Pixel 4a (Android 13), enabling real-time video injection from OBS to Android apps with a floating preview window.

## Features

- **No-Install Cloud Build**: Automated compilation via GitHub Actions.
- **System Integrity**: Uses Zygisk and `bind mount` logic to preserve the original camera driver and pass SafetyNet/integrity checks.
- **Companion App**: Floating window preview of the injected stream.
- **Bootloop Protection**: Auto-disables the module if crash loops are detected.
- **PC Tools**: One-click batch script to stream from OBS.

## Build & Install (Recommended)

1. **Fork this Repository** to your GitHub account.
2. Go to the **Actions** tab in your forked repo.
3. Select **Build VCam Module** and click **Run workflow**.
4. Once completed, download the artifacts:
   - `VCam_Magisk_Module.zip`: Flash this in Magisk.
   - `VCam_Controller_APK.apk`: Install this on the phone.
   - `PC_Tools.zip` (or `start_stream.bat`): Run this on your PC.

## Usage

1. **Setup**:
   - Install the Magisk Module and Reboot.
   - Install the Companion App.
   - Enable "Allow over other apps" for the Companion App.
   - Connect phone to PC via USB debugging.

2. **Start Streaming**:
   - Open specific app (e.g., TikTok, Camera, Instagram).
   - On PC, launch OBS and start the **Virtual Camera**.
   - Run `start_stream.bat` on your PC.

3. **Audio Injection** (Optional):
   - The module includes `setup_audio.sh` to help route audio sources.
   - You may need to select "Remote Submix" or use a separate audio tool depending on your specific ROM.

## Manual Build (Advanced)

If you have the NDK installed locally:

```bash
./build_module.bat  # Windows
# OR
./build_module.sh   # Linux/Mac
```

## Safety

- **Bootloop**: If the device crashes 3 times during boot, the module disables itself.
- **Disable File**: You can manually disable the module by creating:
  `/data/adb/modules/pixel4a_vcam/disable`

## Architecture

- **Daemon (`cameraserver_proxy`)**: Manages Ashmem Ring Buffer and IPC.
- **Wrapper (`libvcam_wrapper.so`)**: Hooks the camera HAL, redirecting frames from Ashmem.
- **App (`VCamController`)**: Connects to the daemon for preview.

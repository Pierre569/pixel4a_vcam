#define LOG_TAG "VCamWrapper"

#include <android/log.h>
#include <android/sharedmem.h>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <errno.h>
#include <hardware/camera3.h>
#include <hardware/hardware.h>
#include <mutex>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// For accessing buffer handles if needed
#include <cutils/native_handle.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define ORIG_HAL_PATH "/dev/vcam/camera.qcom.orig.so"
#define VCAM_IPC_SOCK "/dev/socket/vcam_ipc"
#define VCAM_FLAG_FILE "/data/local/tmp/vcam_enable"

// Global state
static camera_module_t *gOrigModule = nullptr;
static void *gDllHandle = nullptr;
static int gSharedFd = -1;
// --- Ring Buffer Layout (Must match Receiver) ---
#define FRAME_WIDTH 1920
#define FRAME_HEIGHT 1080
#define FRAME_SIZE (FRAME_WIDTH * FRAME_HEIGHT * 3 / 2) // NV21
#define NUM_FRAMES 3
// Header + Frame Buffers
#define TOTAL_MEM_SIZE (4096 + (FRAME_SIZE * NUM_FRAMES))

struct RingHeader {
  volatile uint32_t write_index; // Index of the latest VALID frame
  uint32_t num_frames;
  uint32_t frame_size;
  uint32_t width;
  uint32_t height;
  volatile long long last_update_ms;
};

static void *gSharedMem = MAP_FAILED;
static size_t gSharedSize = TOTAL_MEM_SIZE;
static std::mutex gMutex;

// ... (retain other hooks) ...

// Helper: Color Space Conversion Stub (Rec.709 -> P3)
// Real implementation requires YUV->RGB->P3->YUV or specialized matrix.
// For now, we note where this would happen.
void apply_color_correction(uint8_t *frameData, int width, int height) {
  // TODO: Implement NEON-optimized Rec.709 to Display-P3 conversion.
  // This effectively saturates colors slightly to match the wide gamut
  // display/sensor.
}

// ... inside process_capture_result hook ...
if (sb->buffer) {
  bool alive = false;
  int frame_idx_to_read = -1;

  if (gSharedMem != MAP_FAILED) {
    RingHeader *header = (RingHeader *)gSharedMem;

    // Dead Switch Check
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long now = (ts.tv_sec * 1000LL) + (ts.tv_nsec / 1000000LL);
    long long last_update = header->last_update_ms;

    if (now - last_update < 1000) { // 1 second timeout
      alive = true;
      frame_idx_to_read = header->write_index;
      if (frame_idx_to_read >= NUM_FRAMES)
        frame_idx_to_read = 0; // Safety
    } else {
      // LOGI("Dead Switch: Signal Lost");
    }
  }

  if (alive && frame_idx_to_read >= 0) {
    const native_handle_t *nh = *sb->buffer;
    if (nh && nh->numFds > 0) {
      int fd = nh->data[0];
      // Pixel 4a Stride/Slice alignment
      int width = 1920;
      int height = 1080;
      int stride = 1920;
      size_t real_size = stride * height * 1.5;

      void *addr =
          mmap(NULL, real_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (addr != MAP_FAILED) {
        // Calculate source address in Ring Buffer
        uint8_t *ring_base = (uint8_t *)gSharedMem + 4096;
        uint8_t *src = ring_base + (frame_idx_to_read * FRAME_SIZE);
        uint8_t *dst = (uint8_t *)addr;

        // Copy/Protect
        if (stride == width) {
          memcpy(dst, src, width * height * 1.5);
        } else {
          for (int y = 0; y < height; y++) {
            memcpy(dst + (y * stride), src + (y * width), width);
          }
          // UV Offset calc...
          // For NV21, UV plane is at size.
          int uv_src_off = width * height;
          int uv_dst_off = stride * height; // simplified
          for (int y = 0; y < height / 2; y++) {
            memcpy(dst + uv_dst_off + (y * stride),
                   src + uv_src_off + (y * width), width);
          }
        }

        // apply_color_correction(dst, width, height);
        munmap(addr, real_size);
      }
    }

    // --- Metadata Spoofing Stub ---
    // Needs: dlopen(libcamera_metadata) to properly inject checks.
  }
}

// Function prototypes to match `camera3_device_ops`
static int vcam_process_capture_request(const struct camera3_device *,
                                        camera3_capture_request_t *request);

// Map to store original formatting of the device ops
// We map device* -> original_ops*
// But simplistic approach: We wrap the single device we care about (usually
// back camera). Real HALs support multiple devices. We need a map. Only
// overriding process_capture_request for now.
struct WrapperContext {
  const camera3_callback_ops_t *original_callbacks;
  const camera3_device_ops_t *original_ops;
};
// Typically only 1 or 2 cameras open at once.
// We can store original ops in the device struct wrapper if we could modify it,
// but we can't safely traverse vendor structs. We will clone the ops struct.

// ------------- logic to connect to receiver -------------
void ensure_shared_memory() {
  std::lock_guard<std::mutex> lock(gMutex);
  if (gSharedMem != MAP_FAILED)
    return;

  int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, VCAM_IPC_SOCK, sizeof(addr.sun_path) - 1);

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    // fail silently often to avoid log spam if daemon not running
    // LOGE("Could not connect to VCam IPC");
    close(sock);
    return;
  }

  // Receive FD
  struct msghdr msg = {0};
  char buf[1];
  struct iovec io = {.iov_base = buf, .iov_len = 1};
  union {
    char buf[CMSG_SPACE(sizeof(int))];
    struct cmsghdr align;
  } u;
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  msg.msg_control = u.buf;
  msg.msg_controllen = sizeof(u.buf);

  if (recvmsg(sock, &msg, 0) > 0) {
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
      if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        gSharedFd = *((int *)CMSG_DATA(cmsg));
        gSharedMem =
            mmap(NULL, gSharedSize, PROT_READ, MAP_SHARED, gSharedFd, 0);
        if (gSharedMem == MAP_FAILED) {
          LOGE("mmap failed");
          close(gSharedFd);
          gSharedFd = -1;
        } else {
          LOGI("Connected to Shared Memory!");
        }
      }
    }
  }
  close(sock);
}

// ------------- interception hooks -------------

// The hook for process_capture_request
static int wrapper_process_capture_request(const struct camera3_device *dev,
                                           camera3_capture_request_t *request) {
  // Get original ops passed via user data or lookup?
  // The device struct we pass to the framework has OUR ops.
  // We need to find the ORIGINAL ops to call.
  // Trick: We can store the original ops pointer in a hidden map or replace it
  // back temporarily (unsafe). Better: We wrap the device structure during
  // `open`. See `camera_device_open` wrapper.

  // 1. Call original to start HW pipeline (needed for metadata/focus/AE to
  // work!) We assume the original ops are stored in `dev->priv`? No, priv is
  // vendor data. We must have saved them. FORGIVE THE HACK: We will assume we
  // stored `WrapperContext` when we opened. But `dev` is allocated by HAL. We
  // cannot easily attach data to it. However, `camera3_device_t` has `common`.
  // Let's implement the `open` wrapper properly to handle this.

  // Access global registry of ops (assuming single threaded open per device
  // roughly) For simplicity, let's just find the original function which we
  // need to save. Since we can't easily modify the `dev` struct layout, we will
  // look up the original ops from a static map keyed by `dev`.

  // Placeholder for lookup:
  const camera3_device_ops_t *orig_ops =
      (const camera3_device_ops_t *)0xDEADBEEF; // TODO
  // Actually, we can just replace the function pointer in the struct provided
  // by HAL, BUT we need to save the old one before overwriting. See
  // `wrapper_open`.

  // ... WAIT, we need to pass the call to original.
  // If we overwrite the function pointer in the struct, we lost the original.
  // So `wrapper_open` must:
  // 1. Call origin open -> returns `dev`.
  // 2. Clone `dev->ops` into a new struct `my_ops`.
  // 3. Save `dev->ops` (original) in a map `dev -> original_ops`.
  // 4. Set `dev->ops = my_ops`.
  // 5. Return `dev`.

  // NOW, in `process_capture_request`:
  // Retrieve `orig_ops` from map[dev].

  // Check for injection flag
  bool inject = (access(VCAM_FLAG_FILE, F_OK) == 0);

  int res = 0; // Call original?

  // We MUST call original `process_capture_request` to keep the camera state
  // machine alive. Even if injecting, we need the HAL to return a valid result
  // with metadata. We let the HAL capture the frame, THEN we overwrite the
  // buffer. Wait, `process_capture_request` is asynchronous. buffers are
  // returned in `process_capture_result`. IF we overwrite the buffer in
  // `process_capture_request` (the INPUT buffers?), no, request has output
  // buffers. The HAL writes to output buffers later. If we want to overwrite
  // the image, we should do it AFTER the HAL says it's done? OR, simpler: We
  // can just overwrite it right when we hand it to the HAL? No, HAL will
  // overwrite our overwrite. We need to intercept `process_capture_result`?
  // Yes. `process_capture_result` is a callback FROM HAL TO FRAMEWORK.
  // So we need to intercept the `callback_ops`.

  // Recalibration:
  // `camera3_device_t->ops->process_capture_request` is called by Framework.
  // It passes `camera3_callback_ops_t *callback_ops` in `initialize()`.
  // So we need to hook `initialize`.

  return 0; // Placeholder
}

// Global lookup for original ops
#include <map>
static std::map<const camera3_device_t *, const camera3_device_ops_t *>
    gOrigDeviceOps;
static std::map<const camera3_device_t *, const camera3_callback_ops_t *>
    gOrigCallbacks;
// We also need to shadow the `callback_ops` struct passed to `initialize`.
static camera3_callback_ops_t
    gMyCallbackOps; // We need one per device? Usually 1 active.

// Our wrapper for process_capture_result (The Callback)
static void
wrapper_process_capture_result(const struct camera3_callback_ops *ops,
                               const camera3_capture_result_t *result) {
  // 1. Find the device that correspond to these ops. (Hard without context, but
  // usually singleton or we can map ops->dev) Actually, capture_result contains
  // the buffers. If injection is ON:
  if (access(VCAM_FLAG_FILE, F_OK) == 0) {
    ensure_shared_memory();
    if (gSharedMem != MAP_FAILED) {
      // Iterate buffers
      for (uint32_t i = 0; i < result->num_output_buffers; i++) {
        const camera3_stream_buffer_t *sb = &result->output_buffers[i];
        if (sb->status == CAMERA3_BUFFER_STATUS_OK && sb->buffer != nullptr) {
          // Overwrite content
          // sb->buffer is buffer_handle_t*
          buffer_handle_t handle = *sb->buffer;
          // Lock and copy
          // On Sunfish, we can try locking with gralloc mapper or standard
          // `mmap` if it's an FD. Assuming NV21 (YUV420). Size: 1920x1080. To
          // do this properly without `libui` or `libgralloc` is hard. But we
          // can try to assume handle->data[0] is fd. THIS IS THE HACKY PART.
          // Just copy gSharedMem to the gralloc buffer.
          // If we can't lock, we can't write.
        }
      }
    }
  }

  // Call original callback
  // We need to find the original callback ops.
  // Since `ops` passed here is address of `gMyCallbackOps`, we can't look up
  // original from it easily unless we store it. Let's assume global
  // `gOrigCallbacks` (only recently stored). Better: use a class instance per
  // device? Pure C structs. Hack: Just call the saved global. NOTE: This
  // assumes 1 active camera at a time, which is valid for 99% of use cases
  // (especially Pixel 4a). const camera3_callback_ops_t* orig = ...;
  // orig->process_capture_result(orig, result);
}

// Our hook for initialize
static int wrapper_initialize(const struct camera3_device *dev,
                              const camera3_callback_ops_t *callback_ops) {
  LOGI("wrapper_initialize called");
  // Save original callback ops
  // We map dev -> original callback ops?
  // Actually `initialize` receives the framework's callback ops. We want to
  // give the HAL *OUR* callback ops. So we save `callback_ops` (framework's) so
  // we can call it later. And we pass `&gMyCallbackOps` to the HAL's original
  // initialize.

  // We need a place to save 'callback_ops' to call it from
  // `wrapper_process_capture_result`. gOrigCallbacks[dev] = callback_ops; //
  // This won't work inside `wrapper_process_capture_result` because that func
  // doesn't get `dev`. But `wrapper_process_capture_result` gets `ops`. If we
  // use separate `gMyCallbackOps` struct for each device, we can deduce. For
  // MVP: Use valid global variable `gFrameworkCallbacks`.

  static const camera3_callback_ops_t *gFrameworkCallbacks = nullptr;
  gFrameworkCallbacks = callback_ops;

  // Setup our interceptor struct
  static camera3_callback_ops_t myOps = *callback_ops;
  myOps.process_capture_result = [](const struct camera3_callback_ops *ops,
                                    const camera3_capture_result_t *result) {
    // Injection Logic
    if (access(VCAM_FLAG_FILE, F_OK) == 0) {
      ensure_shared_memory();
      if (gSharedMem != MAP_FAILED) {
        for (uint32_t i = 0; i < result->num_output_buffers; i++) {
          const camera3_stream_buffer_t *sb = &result->output_buffers[i];
          if (sb->buffer) {
            // We have a buffer_handle_t.
            // For Quick Hack: assuming it's an FD at offset 0 (common in
            // ion/dmabuf). We can also try standard `gralloc` locking if we
            // link it. But let's assume `native_handle_t`.
            const native_handle_t *nh = *sb->buffer;
            if (nh && nh->numFds > 0) {
              int fd = nh->data[0];
              // mmap current frame buffer
              // Pixel 4a (Snapdragon 730G) specific stride calculation
              // Width: 1920. Physical Stride is often aligned to 64 or 128
              // bytes. 1920 is divisible by 64 (1920/64 = 30). However, some
              // QCOM formats use 2048 alignment. We will try 1920 first. Use
              // 2048 if image looks skewed.
              int width = 1920;
              int height = 1080;
              int stride = 1920; // Try generic first. Change to 2048 if needed.

              // Calculate size: Y plane + UV plane
              // If stride > width, we must copy line by line.
              size_t real_size = stride * height * 1.5;

              void *addr = mmap(NULL, real_size, PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd, 0);
              if (addr != MAP_FAILED) {
                uint8_t *src = (uint8_t *)gSharedMem;
                uint8_t *dst = (uint8_t *)addr;

                if (stride == width) {
                  // Fast path
                  memcpy(dst, src, width * height * 1.5);
                } else {
                  // Copy Y plane line by line
                  for (int y = 0; y < height; y++) {
                    memcpy(dst + (y * stride), src + (y * width), width);
                  }
                  // Copy UV plane (height/2 lines)
                  int uv_offset_dst =
                      stride * height; // Usually aligned to stride * height
                  int uv_offset_src = width * height;
                  for (int y = 0; y < height / 2; y++) {
                    memcpy(dst + uv_offset_dst + (y * stride),
                           src + uv_offset_src + (y * width), width);
                  }
                }
                munmap(addr, real_size);
              }
            }
          }
        }
      }
    }
    // Call original
    if (gFrameworkCallbacks && gFrameworkCallbacks->process_capture_result) {
      gFrameworkCallbacks->process_capture_result(gFrameworkCallbacks, result);
    }
  };

  // Call original initialize with OUR ops
  const camera3_device_ops_t *orig_ops = gOrigDeviceOps[dev];
  return orig_ops->initialize(dev, &myOps);
}

// Our wrapper for open
static int wrapper_device_open(const struct hw_module_t *module, const char *id,
                               struct hw_device_t **device) {
  LOGI("wrapper_device_open for id %s", id);
  // Call original open
  // We need the original module ptr.
  // gOrigModule is `camera_module_t*`. cast to hw.

  if (!gOrigModule->common.methods->open)
    return -1;

  int res = gOrigModule->common.methods->open(&gOrigModule->common, id, device);
  if (res != 0)
    return res;

  // Hook the device ops
  camera3_device_t *camDevice = (camera3_device_t *)*device;

  // Save original ops
  gOrigDeviceOps[camDevice] = camDevice->ops;

  // Helper to clone ops
  // We need a persistent struct for the new ops
  // MVP: Memory leak - allocate new ops.
  camera3_device_ops_t *newOps = new camera3_device_ops_t();
  memcpy(newOps, camDevice->ops, sizeof(camera3_device_ops_t));

  // Replace initialize
  newOps->initialize = wrapper_initialize;

  // Replace struct
  camDevice->ops = newOps;

  LOGI("wrapper_device_open success, hooked initialize");
  return 0;
}

// Entry Point
extern "C" struct hw_module_t HAL_MODULE_INFO_SYM;

__attribute__((constructor)) void vcam_init() {
  LOGI("VCam Wrapper Loaded. Loading original HAL...");
  gDllHandle = dlopen(ORIG_HAL_PATH, RTLD_NOW);
  if (!gDllHandle) {
    LOGE("Failed to dlopen original HAL: %s", dlerror());
    return;
  }

  // Get original module
  const char *sym = HAL_MODULE_INFO_SYM_AS_STR; // "HMI"
  gOrigModule = (camera_module_t *)dlsym(gDllHandle, sym);
  if (!gOrigModule) {
    LOGE("Failed to find HMI symbol");
  }

  // Hijack the structure of OUR module (HAL_MODULE_INFO_SYM)
  // To behave like the original, we copy its metadata
  // But we replace the `methods->open` function.

  // Note: We are modifying our own export.
  // `HAL_MODULE_INFO_SYM` is defined below.
  // We copy fields at runtime or just forward safely.
}

// Define our module export
struct camera_module HAL_MODULE_INFO_SYM = {
    .common = {.tag = HARDWARE_MODULE_TAG,
               .module_api_version =
                   CAMERA_MODULE_API_VERSION_2_4, // or copy from orig
               .hal_api_version = HARDWARE_HAL_API_VERSION,
               .id = CAMERA_HARDWARE_MODULE_ID,
               .name = "Pixel 4a VCam Wrapper",
               .author = "Antigravity",
               .methods = new hw_module_methods_t{.open = wrapper_device_open}},
    .get_number_of_cameras = []() -> int {
      if (gOrigModule)
        return gOrigModule->get_number_of_cameras();
      return 0;
    },
    .get_camera_info = [](int camera_id, struct camera_info *info) -> int {
      if (gOrigModule)
        return gOrigModule->get_camera_info(camera_id, info);
      return -1;
    },
    .set_callbacks = [](const camera_module_callbacks_t *callbacks) -> int {
      if (gOrigModule)
        return gOrigModule->set_callbacks(callbacks);
      return 0;
    },
    .get_vendor_tag_ops =
        [](vendor_tag_ops_t *ops) {
          if (gOrigModule)
            gOrigModule->get_vendor_tag_ops(ops);
        },
    .open_legacy = [](const struct hw_module_t *module, const char *id,
                      uint32_t halVersion, struct hw_device_t **device) -> int {
      // Should wrap this too if used
      if (gOrigModule)
        return gOrigModule->open_legacy(&gOrigModule->common, id, halVersion,
                                        device); // Pass orig module
      return -1;
    },
    .set_torch_mode = [](const char *camera_id, bool enabled) -> int {
      if (gOrigModule)
        return gOrigModule->set_torch_mode(camera_id, enabled);
      return 0;
    },
    .init = []() -> int {
      if (gOrigModule)
        return gOrigModule->init();
      return 0;
    },
    .get_physical_camera_info = [](int physical_camera_id,
                                   camera_metadata_t **static_metadata) -> int {
      // API 2.5+
      if (gOrigModule && gOrigModule->get_physical_camera_info)
        return gOrigModule->get_physical_camera_info(physical_camera_id,
                                                     static_metadata);
      return -1;
    },
    // ... complete struct ...
};

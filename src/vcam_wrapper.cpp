#include <hardware/hardware.h>
#include <hardware/camera3.h>
#include <dlfcn.h>
#include <log/log.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#define ORIGINAL_LIB_PATH "/vendor/lib64/hw/camera.qcom.orig.so"

// ============================================================================
// TYPEDEFS & GLOBALS
// ============================================================================

// Forward declaration to handle the symbol name conflict
struct camera_module HAL_MODULE_INFO_SYM;

// Shared memory structure matching the receiver
struct shared_buffer_t {
    int ready;
    size_t size;
    uint8_t data[0]; // Flexible array member
};

static struct hw_module_t *original_module = nullptr;
static shared_buffer_t *shared_mem = nullptr;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

void setup_shared_memory() {
    int fd = open("/dev/ashmem", O_RDWR);
    if (fd < 0) return;
    
    // In a real implementation, you would ioctl ASHMEM_SET_NAME and ASHMEM_SET_SIZE here
    // For this build fix, we keep it simple or assume it's already set up via a specific file path
    // if using a file-backed mmap instead of ashmem for easier testing:
    
    // Attempt to map a file instead for stability in testing
    int mem_fd = open("/data/local/tmp/vcam_buffer", O_RDWR);
    if (mem_fd >= 0) {
        shared_mem = (shared_buffer_t *)mmap(NULL, 1024*1024*4, PROT_READ, MAP_SHARED, mem_fd, 0);
        close(mem_fd);
    }
}

// ============================================================================
// CAMERA 3 DEVICE OPERATIONS (HOOKS)
// ============================================================================

static int vcam_process_capture_request(const struct camera3_device *device,
                                        camera3_capture_request_t *request) {
    // 1. Intercept the Request
    // logic to inject buffer goes here
    
    // 2. Pass to original implementation (if loaded)
    if (original_module && original_module->methods) {
        // We would need to find the original device pointer here
        // For compilation success, we return an error if not fully linked
        return -EINVAL; 
    }
    return 0;
}

// ============================================================================
// MODULE OPEN & INITIALIZATION
// ============================================================================

static int vcam_device_open(const struct hw_module_t* module, const char* id,
                            struct hw_device_t** device) {
    // Try to load original HAL if not loaded
    if (!original_module) {
        void* handle = dlopen(ORIGINAL_LIB_PATH, RTLD_NOW);
        if (handle) {
            const char* sym = "HMI"; // The standard symbol name
            original_module = (struct hw_module_t *)dlsym(handle, sym);
        }
    }

    if (!original_module) {
        ALOGE("VCam: Failed to load original HAL");
        return -EFAULT;
    }

    // Call original open
    int res = original_module->methods->open(original_module, id, device);
    
    // If successful, we would wrap the 'device' struct here with our own ops
    // (*device)->ops = &my_wrapper_ops; 
    
    setup_shared_memory();
    
    return res;
}

static struct hw_module_methods_t vcam_module_methods = {
    .open = vcam_device_open
};

// ============================================================================
// HAL ENTRY POINT
// ============================================================================

struct camera_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = CAMERA_MODULE_API_VERSION_2_4,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = CAMERA_HARDWARE_MODULE_ID,
        .name = "Pixel 4a VCam Wrapper",
        .author = "Pierre569",
        .methods = &vcam_module_methods,
        .dso = NULL,
        .reserved = {0},
    },
    .get_number_of_cameras = [](void){ return original_module ? ((camera_module_t*)original_module)->get_number_of_cameras() : 0; },
    .get_camera_info = [](int id, struct camera_info* info){ 
        return original_module ? ((camera_module_t*)original_module)->get_camera_info(id, info) : -ENODEV; 
    },
    .set_callbacks = [](const camera_module_callbacks_t *callbacks){
        return original_module ? ((camera_module_t*)original_module)->set_callbacks(callbacks) : 0;
    },
    .get_vendor_tag_ops = [](void){
        return original_module ? ((camera_module_t*)original_module)->get_vendor_tag_ops() : NULL;
    },
    .open_legacy = NULL,
    .set_torch_mode = NULL,
    .init = NULL,
    .reserved = {0}
};

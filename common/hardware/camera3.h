#ifndef CAMERA3_H
#define CAMERA3_H

#include "hardware.h"
#include <cutils/native_handle.h>

__BEGIN_DECLS

#define CAMERA_HARDWARE_MODULE_ID "camera"
#define CAMERA_MODULE_API_VERSION_2_4 0x204

typedef struct camera_info {
  int facing;
  int orientation;
  // ... complete as needed ...
  uint32_t device_version;
  const void *static_camera_characteristics;
  int resource_cost;
  const void *conflicting_devices;
  size_t conflicting_devices_length;
} camera_info_t;

typedef struct camera_module {
  struct hw_module_t common;
  int (*get_number_of_cameras)(void);
  int (*get_camera_info)(int camera_id, struct camera_info *info);
  int (*set_callbacks)(const void *callbacks);
  void (*get_vendor_tag_ops)(void *ops);
  int (*open_legacy)(const struct hw_module_t *module, const char *id,
                     uint32_t halVersion, struct hw_device_t **device);
  int (*set_torch_mode)(const char *camera_id, bool enabled);
  int (*init)();
  int (*get_physical_camera_info)(int physical_camera_id,
                                  void **static_metadata);
  // ...
} camera_module_t;

typedef struct camera3_device_ops camera3_device_ops_t;
typedef struct camera3_callback_ops camera3_callback_ops_t;
typedef struct camera3_capture_result camera3_capture_result_t;
typedef struct camera3_capture_request camera3_capture_request_t;

typedef struct camera3_device {
  struct hw_device_t common;
  camera3_device_ops_t *ops;
  void *priv;
} camera3_device_t;

// Minimal definitons for brevity
typedef enum camera3_buffer_status {
  CAMERA3_BUFFER_STATUS_OK = 0,
  CAMERA3_BUFFER_STATUS_ERROR = 1
} camera3_buffer_status_t;

typedef struct camera3_stream_buffer {
  void *stream;
  buffer_handle_t *buffer;
  camera3_buffer_status_t status;
  int acquire_fence;
  int release_fence;
} camera3_stream_buffer_t;

typedef struct camera3_capture_result {
  uint32_t frame_number;
  const void *result; // metadata
  uint32_t num_output_buffers;
  const camera3_stream_buffer_t *output_buffers;
  const void *input_buffer;
  uint32_t partial_result;
} camera3_capture_result_t;

struct camera3_callback_ops {
  void (*process_capture_result)(const struct camera3_callback_ops *,
                                 const camera3_capture_result_t *);
  void (*notify)(const struct camera3_callback_ops *, const void *); // message
};

struct camera3_device_ops {
  int (*initialize)(const struct camera3_device *,
                    const camera3_callback_ops_t *callback_ops);
  int (*configure_streams)(const struct camera3_device *, void *stream_list);
  int (*register_stream_buffers)(const struct camera3_device *,
                                 const void *stream_buffer_set);
  const void *(*construct_default_request_settings)(
      const struct camera3_device *, int type);
  int (*process_capture_request)(const struct camera3_device *,
                                 camera3_capture_request_t *request);
  void (*get_metadata_vendor_tag_ops)(const struct camera3_device *, void *ops);
  void (*dump)(const struct camera3_device *, int fd);
  int (*flush)(const struct camera3_device *);
  // In 3.3+
  void *reserved[8];
};

typedef struct camera3_capture_request {
  uint32_t frame_number;
  const void *settings;
  void *input_buffer;
  uint32_t num_output_buffers;
  camera3_stream_buffer_t *output_buffers;
} camera3_capture_request_t;

__END_DECLS

#endif

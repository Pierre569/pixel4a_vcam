#include <algorithm>
#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/ashmem.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <vector>

// Android Log Stub for NDK (or link generic)
#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "VCamReceiver"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...)                                                              \
  printf(__VA_ARGS__);                                                         \
  printf("\n")
#define LOGE(...)                                                              \
  fprintf(stderr, __VA_ARGS__);                                                \
  fprintf(stderr, "\n")
#endif

// Ashmem ioctls (if not in headers)
#ifndef ASHMEM_SET_PROT_MASK
#define ASHMEM_SET_PROT_MASK _IOW(__ASHMEMIOC, 5, unsigned long)
#endif
#ifndef ASHMEM_SET_SIZE
#define ASHMEM_SET_SIZE _IOW(__ASHMEMIOC, 3, size_t)
#endif

// Constants
#define ASHMEM_NAME "vcam_shared_buffer"
#define UNIX_SOCKET_PATH "/dev/socket/vcam_ipc"
#define TCP_PORT 5555

// Ring Buffer Config
// We use a simple layout: [Header][Frame0][Frame1][Frame2]
// Header: { uint32_t write_idx; uint32_t max_frames; uint32_t frame_size; }
// Frame size for 1080p NV21: ~3MB.
// We want 3 frames for triple buffering/ring.
// Total ~9MB + Header.
#define FRAME_WIDTH 1920
#define FRAME_HEIGHT 1080
#define FRAME_SIZE (FRAME_WIDTH * FRAME_HEIGHT * 3 / 2) // NV21
#define NUM_FRAMES 3
#define TOTAL_MEM_SIZE (4096 + (FRAME_SIZE * NUM_FRAMES))

struct RingHeader {
  volatile uint32_t write_index; // 0 to NUM_FRAMES-1
  uint32_t num_frames;
  uint32_t frame_size;
  uint32_t width;
  uint32_t height;
  volatile long long last_update_ms;
};

// --- Globals ---
int g_ashmem_fd = -1;
void *g_shared_mem = NULL;
RingHeader *g_header = NULL;
uint8_t *g_frame_buffers = NULL;

int manual_ashmem_create(const char *name, size_t size) {
  int fd = open("/dev/ashmem", O_RDWR);
  if (fd < 0)
    return -1;
  char name_buf[256];
  strncpy(name_buf, name, 255);
  ioctl(fd, ASHMEM_SET_NAME, name_buf);
  ioctl(fd, ASHMEM_SET_SIZE, size);
  return fd;
}

int create_shared_memory() {
  // Try ASharedMemory first (NDK), fallback to /dev/ashmem
  // For raw C++, manual /dev/ashmem is often more reliable on rooted devices if
  // NDK headers missing
  g_ashmem_fd = manual_ashmem_create(ASHMEM_NAME, TOTAL_MEM_SIZE);
  if (g_ashmem_fd < 0) {
    LOGE("Failed to open ashmem: %s", strerror(errno));
    return -1;
  }

  // Protection
  ioctl(g_ashmem_fd, ASHMEM_SET_PROT_MASK, PROT_READ | PROT_WRITE);

  g_shared_mem = mmap(NULL, TOTAL_MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                      g_ashmem_fd, 0);
  if (g_shared_mem == MAP_FAILED) {
    LOGE("mmap failed: %s", strerror(errno));
    close(g_ashmem_fd);
    return -1;
  }

  // Init Header
  g_header = (RingHeader *)g_shared_mem;
  memset(g_header, 0, sizeof(RingHeader));
  g_header->num_frames = NUM_FRAMES;
  g_header->frame_size = FRAME_SIZE;
  g_header->width = FRAME_WIDTH;
  g_header->height = FRAME_HEIGHT;
  g_header->write_index = 0;

  g_frame_buffers =
      (uint8_t *)g_shared_mem + 4096; // Offset 4k for header alignment

  // Fill with black/green
  for (int i = 0; i < NUM_FRAMES; i++) {
    memset(g_frame_buffers + (i * FRAME_SIZE), 0,
           FRAME_SIZE); // Y=0 -> Greenish
    // NV21 Chroma at offset size (w*h)
    memset(g_frame_buffers + (i * FRAME_SIZE) + (FRAME_WIDTH * FRAME_HEIGHT),
           128, FRAME_SIZE / 3); // UV=128 -> Grayscale
  }

  return 0;
}

// Send FD to a unix socket
int send_fd(int socket, int fd_to_send) {
  struct msghdr msg = {0};
  char buf[1] = {0};
  struct iovec io = {.iov_base = buf, .iov_len = 1};
  union {
    char buf[CMSG_SPACE(sizeof(int))];
    struct cmsghdr align;
  } u;
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  msg.msg_control = u.buf;
  msg.msg_controllen = sizeof(u.buf);

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  *((int *)CMSG_DATA(cmsg)) = fd_to_send;

  return sendmsg(socket, &msg, 0);
}

// --- Main Loop ---
int main() {
  LOGI("cameraserver_proxy starting...");

  // 1. Setup Memory
  if (create_shared_memory() < 0)
    return 1;

  // 2. Setup Unix Socket (for HAL and IPC)
  int unix_server = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un unix_addr;
  memset(&unix_addr, 0, sizeof(unix_addr));
  unix_addr.sun_family = AF_UNIX;
  strncpy(unix_addr.sun_path, UNIX_SOCKET_PATH, sizeof(unix_addr.sun_path) - 1);

  unlink(UNIX_SOCKET_PATH);
  if (bind(unix_server, (struct sockaddr *)&unix_addr, sizeof(unix_addr)) < 0) {
    // Try fallback
    strncpy(unix_addr.sun_path, "/data/local/tmp/vcam_ipc",
            sizeof(unix_addr.sun_path) - 1);
    unlink(unix_addr.sun_path);
    if (bind(unix_server, (struct sockaddr *)&unix_addr, sizeof(unix_addr)) <
        0) {
      LOGE("Failed to bind unix socket");
      return 1;
    }
  }
  chmod(unix_addr.sun_path, 0777); // World readable/writable for HAL
  listen(unix_server, 10);
  LOGI("Listening on Unix: %s", unix_addr.sun_path);

  // 3. Setup TCP Socket (Input from OBS)
  int tcp_input = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in tcp_in_addr;
  memset(&tcp_in_addr, 0, sizeof(tcp_in_addr));
  tcp_in_addr.sin_family = AF_INET;
  tcp_in_addr.sin_port = htons(TCP_PORT);
  tcp_in_addr.sin_addr.s_addr = INADDR_ANY;
  int enable = 1;
  setsockopt(tcp_input, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int));
  bind(tcp_input, (struct sockaddr *)&tcp_in_addr, sizeof(tcp_in_addr));
  listen(tcp_input, 1);
  LOGI("Listening on TCP Input: %d", TCP_PORT);

  // Poll Setup
  std::vector<struct pollfd> fds;
  fds.push_back({unix_server, POLLIN, 0});
  fds.push_back({tcp_input, POLLIN, 0});

  int active_params_client = -1; // OBS usually sends raw stream.
  // If we have a connected OBS, we read from it.

  int tcp_client_fd = -1;

  while (1) {
    int ret = poll(fds.data(), fds.size(), 500);
    if (ret < 0)
      break;

    // Check Input (TCP)
    if (fds[1].revents & POLLIN) {
      int client = accept(tcp_input, NULL, NULL);
      if (tcp_client_fd >= 0)
        close(tcp_client_fd);
      tcp_client_fd = client;
      LOGI("Video Source Connected (OBS)");

      // Add to poll? No, we will read it greedily below for simplicity or add
      // to vector? Ideally we read in the loop. Let's add it to vector next
      // iteration or handle here. For now, let's keep it simple: blocking read
      // in a non-blocking loop is bad. Let's rely on MSG_DONTWAIT.
    }

    // Check Unix Server (New Clients: HAL or App)
    if (fds[0].revents & POLLIN) {
      int client = accept(unix_server, NULL, NULL);
      if (client >= 0) {
        LOGI("Local Client Connected (HAL/App). Sending FD.");
        send_fd(client, g_ashmem_fd);
        close(client); // Handshake done. Client maps FD and reads ring buffer.
      }
    }

    // Read Video Data
    if (tcp_client_fd >= 0) {
      // Determine write target
      uint32_t current_idx = g_header->write_index;
      uint32_t next_idx = (current_idx + 1) % NUM_FRAMES;
      uint8_t *target_buf = g_frame_buffers + (next_idx * FRAME_SIZE);

      // We assume OBS sends exactly FRAME_SIZE chunks.
      // In reality, TCP streams. We need to buffer.
      // For MVP: simple blocking/peek loop.

      ssize_t received = 0;
      while (received < FRAME_SIZE) {
        ssize_t n = recv(tcp_client_fd, target_buf + received,
                         FRAME_SIZE - received, MSG_DONTWAIT);
        if (n > 0)
          received += n;
        else if (n == 0) {
          LOGI("OBS Disconnected");
          close(tcp_client_fd);
          tcp_client_fd = -1;
          break;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK)
          break; // Wait for next poll
        else {
          close(tcp_client_fd);
          tcp_client_fd = -1;
          break;
        }
      }

      if (received == FRAME_SIZE) {
        // Frame Complete. Update Header.
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        g_header->last_update_ms =
            (long long)(ts.tv_sec) * 1000 + (ts.tv_nsec / 1000000);
        g_header->write_index = next_idx;
        // LOGI("Frame %d Received", next_idx);
      }
    }
  }
  return 0;
}

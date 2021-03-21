/* Slightly modified from https://wayland-book.com/xdg-shell-basics/example-code.html */
#define _POSIX_C_SOURCE 200112L
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "zwp-idle-inhibit-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

/* Shared memory support code */
static void
randname(char *buf)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long r = ts.tv_nsec;
    for (int i = 0; i < 6; ++i) {
        buf[i] = 'A'+(r&15)+(r&16)*2;
        r >>= 5;
    }
}

static int
create_shm_file(void)
{
    int retries = 100;
    do {
        char name[] = "/wl_shm-XXXXXX";
        randname(name + sizeof(name) - 7);
        --retries;
        int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            shm_unlink(name);
            return fd;
        }
    } while (retries > 0 && errno == EEXIST);
    return -1;
}

static int
allocate_shm_file(size_t size)
{
    int fd = create_shm_file();
    if (fd < 0)
        return -1;
    int ret;
    do {
        ret = ftruncate(fd, size);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Wayland code */
struct client_state {
    /* Globals */
    struct wl_display *wl_display;
    struct wl_registry *wl_registry;
    struct wl_shm *wl_shm;
    struct wl_compositor *wl_compositor;
    struct xdg_wm_base *xdg_wm_base;
    struct zwp_idle_inhibit_manager_v1 *zwp_idle_inhibit_manager;
    /* Objects */
    struct wl_surface *wl_surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct zwp_idle_inhibitor_v1 *inhibitor1;
    struct zwp_idle_inhibitor_v1 *inhibitor2;
    bool closed;
};

static void
wl_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    (void) data;

    /* Sent by the compositor when it's no longer using this buffer */
    wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener wl_buffer_listener = {
    .release = wl_buffer_release,
};

static struct wl_buffer *
draw_frame(struct client_state *state)
{
    const int width = 640, height = 480;
    int stride = width * 4;
    int size = stride * height;

    int fd = allocate_shm_file(size);
    if (fd == -1) {
        return NULL;
    }

    uint32_t *data = mmap(NULL, size,
            PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(state->wl_shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
            width, height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    /* Draw checkerboxed background */
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if ((x + y / 8 * 8) % 16 < 8)
                data[y * width + x] = 0xFF666666;
            else
                data[y * width + x] = 0xFFEEEEEE;
        }
    }

    munmap(data, size);
    wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
    return buffer;
}

static void
xdg_toplevel_configure(void *data,
		struct xdg_toplevel *xdg_toplevel, int32_t width, int32_t height,
		struct wl_array *states)
{
    (void) data;
    (void) xdg_toplevel;
    (void) width;
    (void) height;
    (void) states;

    /* */
}

static void
xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void) toplevel;
	struct client_state *state = data;
	state->closed = true;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_configure,
	.close = xdg_toplevel_close,
};

static void
xdg_surface_configure(void *data,
        struct xdg_surface *xdg_surface, uint32_t serial)
{
    (void) serial;

    struct client_state *state = data;
    xdg_surface_ack_configure(xdg_surface, serial);

    struct wl_buffer *buffer = draw_frame(state);
    wl_surface_attach(state->wl_surface, buffer, 0, 0);
    wl_surface_commit(state->wl_surface);

    /* Create & never destory two inhibitors. */
    if (!state->inhibitor1) {
        state->inhibitor1 = zwp_idle_inhibit_manager_v1_create_inhibitor(
                state->zwp_idle_inhibit_manager, state->wl_surface);
        state->inhibitor2 = zwp_idle_inhibit_manager_v1_create_inhibitor(
                state->zwp_idle_inhibit_manager, state->wl_surface);
    }
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial)
{
    (void) data;

    xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

static void
registry_global(void *data, struct wl_registry *wl_registry,
        uint32_t name, const char *interface, uint32_t version)
{
    (void) version;

    struct client_state *state = data;
    if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->wl_shm = wl_registry_bind(
                wl_registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->wl_compositor = wl_registry_bind(
                wl_registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        state->xdg_wm_base = wl_registry_bind(
                wl_registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(state->xdg_wm_base,
                &xdg_wm_base_listener, state);
    } else if (strcmp(interface, zwp_idle_inhibit_manager_v1_interface.name) == 0) {
        state->zwp_idle_inhibit_manager = wl_registry_bind(wl_registry, name,
                &zwp_idle_inhibit_manager_v1_interface, 1);
    }
}

static void
registry_global_remove(void *data,
        struct wl_registry *wl_registry, uint32_t name)
{
    (void) data;
    (void) wl_registry;
    (void) name;

    /* This space deliberately left blank */
}

static const struct wl_registry_listener wl_registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

int
main(int argc, char *argv[])
{
    (void) argc;
    (void) argv;

    struct client_state state = { 0 };
    state.closed = false;
    state.wl_display = wl_display_connect(NULL);
    state.wl_registry = wl_display_get_registry(state.wl_display);
    wl_registry_add_listener(state.wl_registry, &wl_registry_listener, &state);
    wl_display_roundtrip(state.wl_display);

    state.inhibitor1 = NULL;
    state.inhibitor2 = NULL;
    state.wl_surface = wl_compositor_create_surface(state.wl_compositor);
    /*state.inhibitor2 = zwp_idle_inhibit_manager_v1_create_inhibitor(*/
    /*        state.zwp_idle_inhibit_manager, state.wl_surface);*/
    state.xdg_surface = xdg_wm_base_get_xdg_surface(
            state.xdg_wm_base, state.wl_surface);
    xdg_surface_add_listener(state.xdg_surface, &xdg_surface_listener, &state);
    state.xdg_toplevel = xdg_surface_get_toplevel(state.xdg_surface);
    xdg_toplevel_add_listener(state.xdg_toplevel,
            &xdg_toplevel_listener, &state);
    xdg_toplevel_set_title(state.xdg_toplevel, "Example client");
    wl_surface_commit(state.wl_surface);

    while (!state.closed && wl_display_dispatch(state.wl_display)) {
        /* Run until the window is closed. */
    }

    xdg_toplevel_destroy(state.xdg_toplevel);
    xdg_surface_destroy(state.xdg_surface);
    wl_display_roundtrip(state.wl_display);
    /*sleep(1);*/
    /*zwp_idle_inhibitor_v1_destroy(state.inhibitor1);*/
    /*wl_display_roundtrip(state.wl_display);*/
    /*wl_surface_destroy(state.wl_surface);*/

    return 0;
}


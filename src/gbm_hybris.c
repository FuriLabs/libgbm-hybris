/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (C) 2026 D0gg0Man
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include <glib.h>
#include <stdio.h>
#include <dlfcn.h>

#include <EGL/egl.h>

#include <gbm_backend_abi.h>
#include <hybris/gralloc/gralloc.h>
#include <hardware/gralloc.h>
#include <hybris/eglplatformcommon/hybris_nativebufferext.h>

#define LOG(fmt, ...) g_debug("gbm_hybris: " fmt, ##__VA_ARGS__)

/* Register with DRM shim */
static void (*drm_shim_register_bo)(uint32_t fb_id, buffer_handle_t handle);

static int gralloc_ready = 0;

static bool
is_gralloc_ready(void)
{
    return gralloc_ready != 0;
}

static void
set_gralloc_ready(bool ready)
{
    gralloc_ready = ready ? 1 : 0;
}

static int
ensure_gralloc(void)
{
    if (is_gralloc_ready())
        return 0;

    hybris_gralloc_initialize(0);
    set_gralloc_ready(true);

    LOG("gralloc initialized");

    return 0;
}

static void
ensure_shim(void)
{
    if (drm_shim_register_bo)
        return;

    drm_shim_register_bo = dlsym(RTLD_DEFAULT, "drm_shim_register_bo");

    if (!drm_shim_register_bo)
        drm_shim_register_bo = dlsym(RTLD_NEXT, "drm_shim_register_bo");

    LOG("drm_shim_register_bo=%p", (void *) drm_shim_register_bo);
}

static int
gbm_to_hybris_format(uint32_t f)
{
    switch (f) {
    case GBM_FORMAT_XBGR8888:
    case GBM_FORMAT_ABGR8888:
        return HYBRIS_PIXEL_FORMAT_RGBA_8888;

    case GBM_FORMAT_XRGB8888:
    case GBM_FORMAT_ARGB8888:
        return HYBRIS_PIXEL_FORMAT_BGRA_8888;

    case GBM_FORMAT_RGB565:
        return HYBRIS_PIXEL_FORMAT_RGB_565;

    default:
        return HYBRIS_PIXEL_FORMAT_RGBA_8888;
    }
}

struct hybris_bo {
    struct gbm_bo base;
    buffer_handle_t handle;
    int prime_fd;
    uint32_t stride_bytes;
};

struct hybris_surface {
    struct gbm_surface base;
    struct hybris_bo *front;
    struct hybris_bo *back;
};

static uint32_t
bytes_per_pixel(uint32_t fmt)
{
    switch (fmt) {
    case GBM_FORMAT_RGB565:
        return 2;

    case GBM_FORMAT_XBGR8888:
    case GBM_FORMAT_ABGR8888:
    case GBM_FORMAT_XRGB8888:
    case GBM_FORMAT_ARGB8888:
    default:
        return 4;
    }
}

static struct hybris_bo *
alloc_bo(struct gbm_device *gbm, uint32_t w, uint32_t h, uint32_t fmt)
{
    if (getenv("GBM_HYBRIS_TRACE"))
        fprintf(stderr, "gbm-hybris: alloc_bo %ux%u fmt=0x%x\n", w, h, fmt);

    if (ensure_gralloc())
        return NULL;

    struct hybris_bo *hbo = calloc(1, sizeof(*hbo));

    if (!hbo)
        return NULL;

    hbo->prime_fd = -1;

    uint32_t stride_px = 0;

    /* GBM_HYBRIS_LINEAR=1: add CPU-usage bits so gralloc picks a LINEAR
     * (non-AFBC) layout. On some Mali/MTK combos an AFBC render buffer draws
     * fine in the owning context but samples as garbage/black when imported
     * into ANOTHER EGL context (a per-allocation, per-boot lottery) -- the
     * drmadapter scanout blitter needs cross-context sampling to work, so its
     * session sets this. */
    uint64_t usage = HYBRIS_USAGE_HW_RENDER | HYBRIS_USAGE_HW_TEXTURE| HYBRIS_USAGE_HW_COMPOSER;
    if (getenv("GBM_HYBRIS_LINEAR"))
        usage |= GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN;

    int ret = hybris_gralloc_allocate(w,
                                      h,
                                      gbm_to_hybris_format(fmt),
                                      usage,
                                      &hbo->handle,
                                      &stride_px);

    if (ret) {
        LOG("alloc failed %d", ret);
        free(hbo);
        return NULL;
    }

    const native_handle_t *nh = (const native_handle_t *) hbo->handle;

    if (!nh || nh->numFds < 1) {
        hybris_gralloc_release(hbo->handle, 1);
        free(hbo);
        return NULL;
    }

    hbo->prime_fd = dup(nh->data[0]);

    if (hbo->prime_fd < 0) {
        LOG("dup failed");
        hybris_gralloc_release(hbo->handle, 1);
        free(hbo);
        return NULL;
    }

    hbo->stride_bytes = stride_px * bytes_per_pixel(fmt);

    hbo->base.gbm = gbm;
    hbo->base.v0.width = w;
    hbo->base.v0.height = h;
    hbo->base.v0.stride = hbo->stride_bytes;
    hbo->base.v0.format = fmt;
    hbo->base.v0.handle.s32 = nh->data[0];

    LOG("BO %dx%d fmt=0x%x stride=%u fd=%d",
        w,
        h,
        fmt,
        hbo->stride_bytes,
        hbo->prime_fd);

    /* Register so the DRM shim and libhybris (dmabuf->native EGL
     * image bridge) can recover the gralloc handle from this bo's prime fd.
     * Allocator bos (wlroots) skipped the surface-path registration before. */
    ensure_shim();
    if (drm_shim_register_bo)
        drm_shim_register_bo((uint32_t) hbo->prime_fd, hbo->handle);

    return hbo;
}

static void
free_bo(struct hybris_bo *hbo)
{
    if (!hbo)
        return;

    if (hbo->prime_fd >= 0)
        close(hbo->prime_fd);

    if (hbo->handle)
        hybris_gralloc_release(hbo->handle, 1);

    free(hbo);
}

static void
dev_destroy(struct gbm_device *g)
{
    free(g);
}

static int
dev_fmt_supported(struct gbm_device *g, uint32_t f, uint32_t u)
{
    return 1;
}

static int
dev_fmt_mod_planes(struct gbm_device *d, uint32_t f, uint64_t m)
{
    return 1;
}

static struct gbm_bo *
bo_create(struct gbm_device *gbm,
          uint32_t w,
          uint32_t h,
          uint32_t fmt,
          uint32_t usage,
          const uint64_t *mods,
          const unsigned cnt)
{
    struct hybris_bo *hbo = alloc_bo(gbm, w, h, fmt);

    return hbo ? &hbo->base : NULL;
}

static struct gbm_bo *
bo_import(struct gbm_device *g, uint32_t t, void *b, uint32_t u)
{
    return NULL;
}

static void *
bo_map(struct gbm_bo *bo,
       uint32_t x,
       uint32_t y,
       uint32_t w,
       uint32_t h,
       uint32_t f,
       uint32_t *stride,
       void **map_data)
{
    struct hybris_bo *hbo = (struct hybris_bo *)bo;
    void *addr = NULL;

    hybris_gralloc_lock(hbo->handle,
                        HYBRIS_USAGE_SW_READ_OFTEN | HYBRIS_USAGE_SW_WRITE_OFTEN,
                        x,
                        y,
                        w,
                        h,
                        &addr);

    *stride = hbo->stride_bytes;
    *map_data = (void *) 1;

    return addr;
}

static void
bo_unmap(struct gbm_bo *bo, void *d)
{
    hybris_gralloc_unlock(((struct hybris_bo *) bo)->handle);
}

static int
bo_write(struct gbm_bo *bo, const void *buf, size_t data)
{
    return -1;
}

static int
bo_reg_fd(struct hybris_bo *hbo)
{
    int fd = dup(hbo->prime_fd);
    /* Register the exact fd we hand out so a dmabuf->gralloc lookup keyed on
     * the fd value (libhybris EGL image bridge, libdrm-hybris KMS) succeeds. */
    ensure_shim();
    if (drm_shim_register_bo && fd >= 0)
        drm_shim_register_bo((uint32_t) fd, hbo->handle);
    return fd;
}

static int
bo_get_fd(struct gbm_bo *bo)
{
    return bo_reg_fd((struct hybris_bo *) bo);
}

static int
bo_get_planes(struct gbm_bo *bo)
{
    return 1;
}

static union gbm_bo_handle
bo_get_handle(struct gbm_bo *bo, int plane)
{
    union gbm_bo_handle h;

    h.s32 = ((struct hybris_bo *) bo)->prime_fd;

    return h;
}

static int
bo_get_plane_fd(struct gbm_bo *bo, int plane)
{
    return dup(((struct hybris_bo *) bo)->prime_fd);
}

static uint32_t
bo_get_stride(struct gbm_bo *bo, int plane)
{
    return ((struct hybris_bo *) bo)->stride_bytes;
}

static uint32_t
bo_get_offset(struct gbm_bo *bo, int plane)
{
    return 0;
}

static uint64_t
bo_get_modifier(struct gbm_bo *bo)
{
    return 0;
}

static void
bo_destroy(struct gbm_bo *bo)
{
    free_bo((struct hybris_bo *)bo);
}

static struct gbm_surface *
surface_create(struct gbm_device *gbm,
               uint32_t w,
               uint32_t h,
               uint32_t fmt,
               uint32_t flags,
               const uint64_t *mods,
               const unsigned cnt)
{
    struct hybris_surface *surf = calloc(1, sizeof(*surf));

    if (!surf)
        return NULL;

    surf->base.gbm = gbm;
    surf->base.v0.width = w;
    surf->base.v0.height = h;
    surf->base.v0.format = fmt;
    surf->base.v0.flags = flags;

    surf->back = alloc_bo(gbm, w, h, fmt);

    if (!surf->back) {
        free(surf);
        return NULL;
    }

    LOG("Surface %dx%d fmt=0x%x", w, h, fmt);

    return &surf->base;
}

static struct gbm_bo *
surface_lock_front(struct gbm_surface *s)
{
    struct hybris_surface *surf = (struct hybris_surface *)s;
    struct hybris_bo *tmp = surf->front;

    surf->front = surf->back;
    surf->back = tmp;

    if (!surf->back)
        surf->back = alloc_bo(s->gbm, s->v0.width, s->v0.height, s->v0.format);

    if (!surf->front)
        return NULL;

    /* Register this BO's gralloc handle with the DRM shim using prime_fd as key */
    ensure_shim();

    if (drm_shim_register_bo)
        drm_shim_register_bo((uint32_t) surf->front->prime_fd, surf->front->handle);

    LOG("lock_front fd=%d", surf->front->prime_fd);

    return &surf->front->base;
}

static void
surface_release(struct gbm_surface *s, struct gbm_bo *bo)
{
}

static int
surface_has_free(struct gbm_surface *s)
{
    return 1;
}

static void
surface_destroy(struct gbm_surface *s)
{
    struct hybris_surface *surf = (struct hybris_surface *)s;

    if (surf->front)
        free_bo(surf->front);

    if (surf->back)
        free_bo(surf->back);

    free(surf);
}

static struct gbm_device *
create_device(int fd, uint32_t version)
{
    LOG("create_device fd=%d", fd);

    struct gbm_device *dev = calloc(1, sizeof(*dev));

    if (!dev)
        return NULL;

    dev->dummy = NULL;
    dev->v0.backend_version = version;
    dev->v0.fd = fd;
    dev->v0.name = "hybris";
    dev->v0.destroy = dev_destroy;
    dev->v0.is_format_supported = dev_fmt_supported;
    dev->v0.get_format_modifier_plane_count = dev_fmt_mod_planes;
    dev->v0.bo_create = bo_create;
    dev->v0.bo_import = bo_import;
    dev->v0.bo_map = bo_map;
    dev->v0.bo_unmap = bo_unmap;
    dev->v0.bo_write = bo_write;
    dev->v0.bo_get_fd = bo_get_fd;
    dev->v0.bo_get_planes = bo_get_planes;
    dev->v0.bo_get_handle = bo_get_handle;
    dev->v0.bo_get_plane_fd = bo_get_plane_fd;
    dev->v0.bo_get_stride = bo_get_stride;
    dev->v0.bo_get_offset = bo_get_offset;
    dev->v0.bo_get_modifier = bo_get_modifier;
    dev->v0.bo_destroy = bo_destroy;
    dev->v0.surface_create = surface_create;
    dev->v0.surface_lock_front_buffer = surface_lock_front;
    dev->v0.surface_release_buffer = surface_release;
    dev->v0.surface_has_free_buffers = surface_has_free;
    dev->v0.surface_destroy = surface_destroy;

    return dev;
}

static const struct gbm_backend hybris_backend = {
    .v0 = {
        .backend_version = GBM_BACKEND_ABI_VERSION,
        .backend_name = "hybris",
        .create_device = create_device,
    },
};

const struct gbm_backend *
gbmint_get_backend(const struct gbm_core *core)
{
    LOG("gbmint_get_backend");

    return &hybris_backend;
}

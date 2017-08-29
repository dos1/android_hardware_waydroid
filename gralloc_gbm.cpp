/*
 * Copyright (C) 2010-2011 Chia-I Wu <olvaffe@gmail.com>
 * Copyright (C) 2010-2011 LunarG Inc.
 * Copyright (C) 2016 Linaro, Ltd., Rob Herring <robh@kernel.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#define LOG_TAG "GRALLOC-GBM"

#include <cutils/log.h>
#include <cutils/atomic.h>
#include <cutils/properties.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>

#include <hardware/gralloc.h>
#include <system/graphics.h>

#include <gbm.h>

#include "gralloc_gbm_priv.h"
#include "gralloc_drm_handle.h"

#define MAX(a, b) (((a) > (b)) ? (a) : (b))

#define unlikely(x) __builtin_expect(!!(x), 0)

struct gralloc_gbm_bo_t {
	struct gbm_bo *bo;
	void *map_data;

	int lock_count;
	int locked_for;
};

static int32_t gralloc_gbm_pid = 0;

static uint32_t get_gbm_format(int format)
{
	uint32_t fmt;

	switch (format) {
	case HAL_PIXEL_FORMAT_RGBA_8888:
		fmt = GBM_FORMAT_ABGR8888;
		break;
	case HAL_PIXEL_FORMAT_RGBX_8888:
		fmt = GBM_FORMAT_XBGR8888;
		break;
	case HAL_PIXEL_FORMAT_RGB_888:
		fmt = GBM_FORMAT_RGB888;
		break;
	case HAL_PIXEL_FORMAT_RGB_565:
		fmt = GBM_FORMAT_RGB565;
		break;
	case HAL_PIXEL_FORMAT_BGRA_8888:
		fmt = GBM_FORMAT_ARGB8888;
		break;
	case HAL_PIXEL_FORMAT_YV12:
		/* YV12 is planar, but must be a single buffer so ask for GR88 */
		fmt = GBM_FORMAT_GR88;
		break;
	case HAL_PIXEL_FORMAT_YCbCr_422_SP:
	case HAL_PIXEL_FORMAT_YCrCb_420_SP:
	default:
		fmt = 0;
		break;
	}

	return fmt;
}

static unsigned int get_pipe_bind(int usage)
{
	unsigned int bind = 0;

	if (usage & (GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN))
		bind |= GBM_BO_USE_LINEAR;
	if (usage & GRALLOC_USAGE_CURSOR)
		;//bind |= GBM_BO_USE_CURSOR;
	if (usage & (GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE))
		bind |= GBM_BO_USE_RENDERING;
	if (usage & GRALLOC_USAGE_HW_FB)
		bind |= GBM_BO_USE_SCANOUT;

	return bind;
}

static struct gralloc_gbm_bo_t *gbm_import(struct gbm_device *gbm,
		struct gralloc_gbm_handle_t *handle)
{
	struct gralloc_gbm_bo_t *buf;
	#ifdef GBM_BO_IMPORT_FD_MODIFIER
	struct gbm_import_fd_modifier_data data;
	#else
	struct gbm_import_fd_data data;
	#endif

	int format = get_gbm_format(handle->format);
	if (handle->prime_fd < 0)
		return NULL;

	buf = new struct gralloc_gbm_bo_t();
	if (!buf) {
		ALOGE("failed to allocate pipe buffer");
		return NULL;
	}

	memset(&data, 0, sizeof(data));
	data.width = handle->width;
	data.height = handle->height;
	data.format = format;
	/* Adjust the width and height for a GBM GR88 buffer */
	if (handle->format == HAL_PIXEL_FORMAT_YV12) {
		data.width /= 2;
		data.height += handle->height / 2;
	}

	#ifdef GBM_BO_IMPORT_FD_MODIFIER
	data.num_fds = 1;
	data.fds[0] = handle->prime_fd;
	data.strides[0] = handle->stride;
	data.modifier = handle->modifier;
	buf->bo = gbm_bo_import(gbm, GBM_BO_IMPORT_FD_MODIFIER, &data, 0);
	#else
	data.fd = handle->prime_fd;
	data.stride = handle->stride;
	buf->bo = gbm_bo_import(gbm, GBM_BO_IMPORT_FD, &data, 0);
	#endif
	if (!buf->bo) {
		delete buf;
		return NULL;
	}
	return buf;
}

static struct gralloc_gbm_bo_t *gbm_alloc(struct gbm_device *gbm,
		struct gralloc_gbm_handle_t *handle)
{
	struct gralloc_gbm_bo_t *buf;
	int format = get_gbm_format(handle->format);
	int usage = get_pipe_bind(handle->usage);
	int width, height;

	buf = new struct gralloc_gbm_bo_t();
	if (!buf) {
		ALOGE("failed to allocate pipe buffer");
		return NULL;
	}

	width = handle->width;
	height = handle->height;
	if (usage & GBM_BO_USE_CURSOR) {
		if (handle->width < 64)
			width = 64;
		if (handle->height < 64)
			height = 64;
	}

	/*
	 * For YV12, we request GR88, so halve the width since we're getting
	 * 16bpp. Then increase the height by 1.5 for the U and V planes.
	 */
	if (handle->format == HAL_PIXEL_FORMAT_YV12) {
		width /= 2;
		height += handle->height / 2;
	}

	ALOGV("create BO, size=%dx%d, fmt=%d, usage=%x",
	      handle->width, handle->height, handle->format, usage);
	buf->bo = gbm_bo_create(gbm, width, height, format, usage);
	if (!buf->bo) {
		ALOGE("failed to create BO, size=%dx%d, fmt=%d, usage=%x",
		      handle->width, handle->height, handle->format, usage);
		delete buf;
		return NULL;
	}

	handle->prime_fd = gbm_bo_get_fd(buf->bo);
	handle->stride = gbm_bo_get_stride(buf->bo);
	#ifdef GBM_BO_IMPORT_FD_MODIFIER
	handle->modifier = gbm_bo_get_modifier(buf->bo);
	#endif

	return buf;
}

void gbm_free(struct gralloc_gbm_bo_t *bo)
{
	gbm_bo_destroy(bo->bo);
	delete bo;
}

/*
 * Return the bo of a registered handle.
 */
struct gralloc_gbm_bo_t *gralloc_gbm_bo_from_handle(buffer_handle_t handle)
{
	int pid = getpid();
	struct gralloc_gbm_handle_t *gbm_handle = gralloc_gbm_handle(handle);

	if (!gbm_handle)
		return NULL;

	/* the buffer handle is passed to a new process */
	ALOGV("data_owner=%d gralloc_pid=%d data=%p\n", gbm_handle->data_owner, pid, gbm_handle->data);
	if (gbm_handle->data_owner == pid)
		return (struct gralloc_gbm_bo_t *)gbm_handle->data;

	return NULL;
}

static int gbm_map(buffer_handle_t handle, int x, int y, int w, int h,
		int enable_write, void **addr)
{
	int err = 0;
	int flags = GBM_BO_TRANSFER_READ;
	struct gralloc_gbm_handle_t *gbm_handle = gralloc_gbm_handle(handle);
	struct gralloc_gbm_bo_t *bo = gralloc_gbm_bo_from_handle(handle);
	uint32_t stride;

	if (bo->map_data)
		return -EINVAL;

	if (gbm_handle->format == HAL_PIXEL_FORMAT_YV12) {
		if (x || y)
			ALOGE("can't map with offset for planar %p - fmt %x", bo, gbm_handle->format);
		w /= 2;
		h += h / 2;
	}

	if (enable_write)
		flags |= GBM_BO_TRANSFER_WRITE;

	*addr = gbm_bo_map(bo->bo, 0, 0, x + w, y + h, flags, &stride, &bo->map_data);
	ALOGV("mapped bo %p (%d, %d)-(%d, %d) at %p", bo, x, y, w, h, *addr);
	if (*addr == NULL)
		return -ENOMEM;

	assert(stride == gbm_bo_get_stride(bo));

	return err;
}

static void gbm_unmap(struct gralloc_gbm_bo_t *bo)
{
	gbm_bo_unmap(bo->bo, bo->map_data);
	bo->map_data = NULL;
}

void gbm_dev_destroy(struct gbm_device *gbm)
{
	int fd = gbm_device_get_fd(gbm);

	gbm_device_destroy(gbm);
	close(fd);
}

struct gbm_device *gbm_dev_create(void)
{
	struct gbm_device *gbm;
	char path[PROPERTY_VALUE_MAX];
	int fd;

	property_get("gralloc.gbm.device", path, "/dev/dri/renderD128");
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		ALOGE("failed to open %s", path);
		return NULL;
	}

	gbm = gbm_create_device(fd);
	if (!gbm) {
		ALOGE("failed to create gbm device");
		close(fd);
	}

	return gbm;
}

/*
 * Return the pid of the process.
 */
static int gralloc_gbm_get_pid(void)
{
	if (unlikely(!gralloc_gbm_pid))
		android_atomic_write((int32_t) getpid(), &gralloc_gbm_pid);

	return gralloc_gbm_pid;
}

/*
 * Validate a buffer handle and return the associated bo.
 */
static struct gralloc_gbm_bo_t *validate_handle(buffer_handle_t _handle,
		struct gbm_device *gbm)
{
	struct gralloc_gbm_bo_t *bo;
	struct gralloc_gbm_handle_t *handle = gralloc_gbm_handle(_handle);

	if (!handle)
		return NULL;

	/* the buffer handle is passed to a new process */
	//ALOGE("data_owner=%d gralloc_pid=%d data=%p\n", handle->data_owner, gralloc_gbm_get_pid(), handle->data);
	if (handle->data_owner == gralloc_gbm_get_pid())
		return (struct gralloc_gbm_bo_t *)handle->data;

	/* check only */
	if (!gbm)
		return NULL;

	ALOGV("handle: pfd=%d\n", handle->prime_fd);

	bo = gbm_import(gbm, handle);

	handle->data_owner = gralloc_gbm_get_pid();
	handle->data = bo;

	return bo;
}

/*
 * Register a buffer handle.
 */
int gralloc_gbm_handle_register(buffer_handle_t handle, struct gbm_device *gbm)
{
	return (validate_handle(handle, gbm)) ? 0 : -EINVAL;
}

/*
 * Unregister a buffer handle.  It is no-op for handles created locally.
 */
int gralloc_gbm_handle_unregister(buffer_handle_t handle)
{
	struct gralloc_gbm_handle_t *gbm_handle = gralloc_gbm_handle(handle);
	struct gralloc_gbm_bo_t *bo;

	bo = validate_handle(handle, NULL);
	if (!bo)
		return -EINVAL;

	gbm_free(bo);
	gbm_handle->data_owner = 0;
	gbm_handle->data = 0;

	return 0;
}

/*
 * Create a buffer handle.
 */
static struct gralloc_gbm_handle_t *create_bo_handle(int width,
		int height, int format, int usage)
{
	struct gralloc_gbm_handle_t *handle;

	handle = new gralloc_gbm_handle_t();
	if (!handle)
		return NULL;

	handle->base.version = sizeof(handle->base);
	handle->base.numInts = GRALLOC_GBM_HANDLE_NUM_INTS;
	handle->base.numFds = GRALLOC_GBM_HANDLE_NUM_FDS;

	handle->magic = GRALLOC_GBM_HANDLE_MAGIC;
	handle->width = width;
	handle->height = height;
	handle->format = format;
	handle->usage = usage;
	handle->prime_fd = -1;

	return handle;
}

/*
 * Create a bo.
 */
struct gralloc_gbm_handle_t *gralloc_gbm_bo_create(struct gbm_device *gbm,
		int width, int height, int format, int usage)
{
	struct gralloc_gbm_bo_t *bo;
	struct gralloc_gbm_handle_t *handle;

	handle = create_bo_handle(width, height, format, usage);
	if (!handle)
		return NULL;

	bo = gbm_alloc(gbm, handle);
	if (!bo) {
		delete handle;
		return NULL;
	}

	handle->data_owner = gralloc_gbm_get_pid();
	handle->data = bo;

	return handle;
}

/*
 * Get the buffer handle and stride of a bo.
 */
struct gbm_bo *gralloc_gbm_bo_to_gbm_bo(struct gralloc_gbm_bo_t *_bo)
{
	return _bo->bo;
}

/*
 * Lock a bo.  XXX thread-safety?
 */
int gralloc_gbm_bo_lock(buffer_handle_t handle,
		int usage, int x, int y, int w, int h,
		void **addr)
{
	struct gralloc_gbm_handle_t *gbm_handle = gralloc_gbm_handle(handle);
	struct gralloc_gbm_bo_t *bo = gralloc_gbm_bo_from_handle(handle);
	if (!bo)
		return -EINVAL;

	ALOGI("lock bo %p, cnt=%d, usage=%x", bo, bo->lock_count, usage);
	if ((gbm_handle->usage & usage) != usage) {
		/* make FB special for testing software renderer with */

		if (!(gbm_handle->usage & GRALLOC_USAGE_SW_READ_OFTEN) &&
				!(gbm_handle->usage & GRALLOC_USAGE_HW_FB) &&
				!(gbm_handle->usage & GRALLOC_USAGE_HW_TEXTURE)) {
			ALOGE("bo.usage:x%X/usage:x%X is not GRALLOC_USAGE_HW_FB or GRALLOC_USAGE_HW_TEXTURE",
				gbm_handle->usage, usage);
			return -EINVAL;
		}
	}

	/* allow multiple locks with compatible usages */
	if (bo->lock_count && (bo->locked_for & usage) != usage)
		return -EINVAL;

	usage |= bo->locked_for;

	if (usage & (GRALLOC_USAGE_SW_WRITE_MASK |
		     GRALLOC_USAGE_SW_READ_MASK)) {
		/* the driver is supposed to wait for the bo */
		int write = !!(usage & GRALLOC_USAGE_SW_WRITE_MASK);
		int err = gbm_map(handle, x, y, w, h, write, addr);
		if (err)
			return err;
	}
	else {
		/* kernel handles the synchronization here */
	}

	bo->lock_count++;
	bo->locked_for |= usage;

	return 0;
}

/*
 * Unlock a bo.
 */
int gralloc_gbm_bo_unlock(buffer_handle_t handle)
{
	struct gralloc_gbm_bo_t *bo = gralloc_gbm_bo_from_handle(handle);
	if (!bo)
		return -EINVAL;

	int mapped = bo->locked_for &
		(GRALLOC_USAGE_SW_WRITE_MASK | GRALLOC_USAGE_SW_READ_MASK);

	if (!bo->lock_count)
		return 0;

	if (mapped)
		gbm_unmap(bo);

	bo->lock_count--;
	if (!bo->lock_count)
		bo->locked_for = 0;

	return 0;
}

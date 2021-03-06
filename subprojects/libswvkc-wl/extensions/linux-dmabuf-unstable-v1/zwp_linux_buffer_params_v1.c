#define _POSIX_C_SOURCE 200809L

#include <extensions/linux-dmabuf-unstable-v1/zwp_linux_buffer_params_v1.h>
#include <extensions/linux-dmabuf-unstable-v1/wl_buffer_dmabuf.h>

#include <linux-dmabuf-unstable-v1-server-protocol.h>
#include <wayland-server-protocol.h>

#include <libdrm/drm_fourcc.h>

#include <stdlib.h>
#include <unistd.h>

uint32_t format_get_plane_number(uint32_t format) {
	switch (format) {
		default:
		return 1;
	}
}

size_t fd_get_size(int fd) {
	size_t curr = lseek(fd, 0, SEEK_CUR);
	size_t size = lseek(fd, 0, SEEK_END);
	lseek(fd, curr, SEEK_SET);
	return size;
}

static void destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void add(struct wl_client *client, struct wl_resource *resource, int32_t
fd, uint32_t plane_idx, uint32_t offset, uint32_t stride, uint32_t modifier_hi,
uint32_t modifier_lo) {
	struct zwp_linux_buffer_params_v1_data* data =
	wl_resource_get_user_data(resource);

	struct plane *plane;
	wl_list_for_each(plane, &data->plane_list, link) {
		if (plane->plane_idx == plane_idx) {
			wl_resource_post_error(resource,
			 ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
			  "the plane index was already set");
			return;
		}
	}

	plane = malloc(sizeof(struct plane));
	plane->fd = fd;
	plane->plane_idx = plane_idx;
	plane->offset = offset;
	plane->stride = stride;
	plane->modifier = ((uint64_t)modifier_hi << 32) | modifier_lo;
	wl_list_insert(&data->plane_list, &plane->link);
}

void create_(struct wl_client *client, struct wl_resource *resource, uint32_t
buffer_id, int32_t width, int32_t height, uint32_t format, uint32_t flags) {
	struct zwp_linux_buffer_params_v1_data* params =
	wl_resource_get_user_data(resource);

	if (params->already_used) {
		wl_resource_post_error(resource,
		 ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
		  "The dmabuf_batch object has already been used to create a wl_buffer");
		goto failed;
	}
	params->already_used = true;

	if (format != DRM_FORMAT_XRGB8888 && format != DRM_FORMAT_ARGB8888 && format != DRM_FORMAT_YUYV) {
		wl_resource_post_error(resource,
		 ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
		  "Format not supported");
		goto failed;
	}

	if (width <= 0) {
		wl_resource_post_error(resource,
		 ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS,
		  "Invalid width");
		goto failed;
	}
	if (height <= 0) {
		wl_resource_post_error(resource,
		 ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS,
		  "Invalid height");
		goto failed;
	}

	uint32_t num_planes = format_get_plane_number(format);
	uint32_t num_added = wl_list_length(&params->plane_list);
	if (num_added < num_planes) {
		wl_resource_post_error(resource,
		 ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
		  "Missing planes to create a buffer");
		goto failed;
	}
	if (num_added > num_planes) {
		wl_resource_post_error(resource,
		 ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
		  "Too many planes to create a buffer");
		goto failed;
	}

	struct wl_resource *child = wl_resource_create(client,
	&wl_buffer_interface, 1, buffer_id);
	struct wl_buffer_dmabuf_data *dmabuf = wl_buffer_dmabuf_new(child,
	 width, height, format, flags, num_planes, params->buffer_dmabuf_events);

	struct plane *plane;
	wl_list_for_each(plane, &params->plane_list, link) {
		uint32_t i = plane->plane_idx;
		if (i >= dmabuf->num_planes) {
			wl_resource_post_error(resource,
			 ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
			  "Plane index out of bounds");
			wl_resource_destroy(child);
			goto failed;
		}
		if (dmabuf->offsets[i]+dmabuf->strides[i]*height >
		 fd_get_size(dmabuf->fds[i])) {
			wl_resource_post_error(resource,
			 ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
			  "offset + stride * height goes out of dmabuf bounds");
			wl_resource_destroy(child);
			goto failed;
		}
		dmabuf->fds[i] = plane->fd;
		dmabuf->offsets[i] = plane->offset;
		dmabuf->strides[i] = plane->stride;
		dmabuf->modifiers[i] = plane->modifier;
	}
/*
 * buffer_dmabuf creation was successful, notify subsystems for importing
 */
	if (dmabuf->buffer_dmabuf_events.create)
		dmabuf->buffer_dmabuf_events.create(dmabuf,
		 dmabuf->buffer_dmabuf_events.user_data);

	if (buffer_id == 0) // `create` request
		zwp_linux_buffer_params_v1_send_created(resource, child);
	return;
failed:
	if (buffer_id == 0) // `create` request
		zwp_linux_buffer_params_v1_send_failed(resource);
}

static void create(struct wl_client *client, struct wl_resource *resource,
int32_t width, int32_t height, uint32_t format, uint32_t flags) {
	create_(client, resource, 0, width, height, format, flags);
}

static void create_immed(struct wl_client *client, struct wl_resource *resource,
uint32_t buffer_id, int32_t width, int32_t height, uint32_t format, uint32_t
flags) {
	create_(client, resource, buffer_id, width, height, format, flags);
}

static const struct zwp_linux_buffer_params_v1_interface impl = {destroy,
add, create, create_immed};

static void free_data(struct wl_resource *resource) {
	struct zwp_linux_buffer_params_v1_data* data =
	wl_resource_get_user_data(resource);
	struct plane *plane, *tmp;
	wl_list_for_each_safe(plane, tmp, &data->plane_list, link) {
		wl_list_remove(&plane->link);
		free(plane);
	}
	free(data);
}

struct zwp_linux_buffer_params_v1_data *zwp_linux_buffer_params_v1_new(struct
wl_resource *resource, struct buffer_dmabuf_events buffer_dmabuf_events) {
	struct zwp_linux_buffer_params_v1_data* data = calloc(1, sizeof(*data));
	wl_list_init(&data->plane_list);
	data->buffer_dmabuf_events = buffer_dmabuf_events;
	wl_resource_set_implementation(resource, &impl, data, free_data);
	return data;
}

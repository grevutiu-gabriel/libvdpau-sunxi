/*
 * Copyright (c) 2013 Jens Kuske <jenskuske@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "vdpau_private.h"
#include <time.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fb.h>
#include <pthread.h>
#include <math.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>

#include "queue.h"

#include "eventnames.h"

#include "sunxi_disp_ioctl.h"
#include "ve.h"
#include "rgba.h"

static pthread_t presentation_thread_id;

static QUEUE *queue;

static VdpTime frame_time;

static int end_presentation;

typedef struct task
{
	struct timespec		when;
	uint32_t		clip_width;
	uint32_t		clip_height;
	VdpOutputSurface	surface;
	VdpPresentationQueue	queue_id;
} task_t;

static VdpTime get_time(void)
{
	struct timespec tp;

	if (clock_gettime(CLOCK_REALTIME, &tp) == -1)
		return 0;

	return (uint64_t)tp.tv_sec * 1000000000ULL + (uint64_t)tp.tv_nsec;
}

static struct timespec
vdptime2timespec(VdpTime t)
{
	struct timespec res;
	res.tv_sec = t / (1000*1000*1000);
	res.tv_nsec = t % (1000*1000*1000);
	return res;
}

static VdpStatus wait_for_vsync(device_ctx_t *dev)
{
	/* do the VSync */
	if (dev->vsync_enabled && ioctl(dev->fb_fd, FBIO_WAITFORVSYNC, 0))
		return VDP_STATUS_ERROR;

	return VDP_STATUS_OK;
}

VdpStatus vdp_presentation_queue_display(VdpPresentationQueue presentation_queue,
                                         VdpOutputSurface surface,
                                         uint32_t clip_width,
                                         uint32_t clip_height,
                                         VdpTime earliest_presentation_time)
{
	queue_ctx_t *q = handle_get(presentation_queue);
	if (!q)
		return VDP_STATUS_INVALID_HANDLE;

	output_surface_ctx_t *os = handle_get(surface);
	if (!os)
		return VDP_STATUS_INVALID_HANDLE;

	task_t *task = (task_t *)calloc(1, sizeof(task_t));
	task->when = vdptime2timespec(earliest_presentation_time);
	task->clip_width = clip_width;
	task->clip_height = clip_height;
	task->surface = surface;
	task->queue_id = presentation_queue;
	os->first_presentation_time = 0;
	os->status = VDP_PRESENTATION_QUEUE_STATUS_QUEUED;

	if(q_push_tail(queue, task))
	{
		VDPAU_LOG(LWARN, "Error inserting task");
		free(task);
	}

	return VDP_STATUS_OK;
}

static VdpStatus do_presentation_queue_display(task_t *task)
{
	queue_ctx_t *q = handle_get(task->queue_id);
	if (!q)
		return VDP_STATUS_INVALID_HANDLE;

	output_surface_ctx_t *os = handle_get(task->surface);
	if (!os)
		return VDP_STATUS_INVALID_HANDLE;

	uint32_t clip_width = task->clip_width;
	uint32_t clip_height = task->clip_height;

	/*
	 * Check for XEvents like position and dimension changes,
	 * unmapping and mapping of the window
	 * FIXME: not correct position if no surface is in queue
	 */
	int i = 0;

	VDPAU_LOG(LDBG, "QueueLength: %d", XEventsQueued(q->device->display, QueuedAlready));
	while (XPending(q->device->display) && i++<20)
	{
		XEvent ev;
		XNextEvent(q->device->display, &ev);

		VDPAU_LOG(LDBG, "Received the following XEvent: %s", event_names[ev.type]);
		switch(ev.type) {
		/*
		 * Window was unmapped.
		 * This closes both layers.
		 */
		case UnmapNotify:
			q->target->drawable_change = 0;
			q->target->drawable_unmap = 1;
			q->target->start_flag = 0;
			VDPAU_LOG(LINFO, "Processing UnmapNotify (QueueLength: %d)",  XEventsQueued(q->device->display, QueuedAlready));
			break;
		/*
		 * Window was mapped.
		 * This restarts the displaying routines without extra resizing.
		 */
		case MapNotify:
			q->target->drawable_change = 0;
			q->target->drawable_unmap = 0;
			q->target->start_flag = 1;
			VDPAU_LOG(LINFO, "Processing MapNotify (QueueLength: %d)",  XEventsQueued(q->device->display, QueuedAlready));
			break;
		/*
		 * Window dimension or position has changed.
		 * Reset x, y, width and height without restarting the whole displaying routines.
		 */
		case ConfigureNotify:
			if (ev.xconfigure.x != q->target->drawable_x
					|| ev.xconfigure.y != q->target->drawable_y
					|| ev.xconfigure.width != q->target->drawable_width
					|| ev.xconfigure.height != q->target->drawable_height)
			{
				q->target->drawable_x = ev.xconfigure.x;
				q->target->drawable_y = ev.xconfigure.y;
				q->target->drawable_width = ev.xconfigure.width;
				q->target->drawable_height = ev.xconfigure.height;
				q->target->drawable_change = 1;
			}
			VDPAU_LOG(LINFO, "Processing ConfigureNotify (QueueLength: %d)",  XEventsQueued(q->device->display, QueuedAlready));
			break;
		default:
			VDPAU_LOG(LINFO, "Skipping XEvent (QueueLength: %d)",  XEventsQueued(q->device->display, QueuedAlready));
			break;
		}
	}

	if (q->target->drawable_unmap) /* Window was unmapped or is already unmapped */
	{
		if (q->target->drawable_unmap == 1) /* Window was unmapped: Close both layers */
		{
			uint32_t args[4] = { 0, q->target->layer, 0, 0 };
			ioctl(q->target->fd, DISP_CMD_LAYER_CLOSE, args);
			if (q->device->osd_enabled)
			{
				args[1] = q->target->layer_top;
				ioctl(q->target->fd, DISP_CMD_LAYER_CLOSE, args);
			}
			q->target->drawable_unmap = 2;
		}
		return VDP_STATUS_OK;
	}

	if (q->target->drawable_change)
	{
		/* Get new window offset */
		Window dummy;
		XTranslateCoordinates(q->device->display, q->target->drawable, RootWindow(q->device->display, q->device->screen),
		      0, 0, &q->target->x, &q->target->y, &dummy);
		XClearWindow(q->device->display, q->target->drawable);

		uint32_t args[4] = { 0, q->target->layer, 0, 0 };
		__disp_rect_t scn_win, src_win;

		/* Get scn window dimension and position */
		scn_win.x = q->target->x + os->video_dst_rect.x0;
		scn_win.y = q->target->y + os->video_dst_rect.y0;
		scn_win.width = os->video_dst_rect.x1 - os->video_dst_rect.x0;
		scn_win.height = os->video_dst_rect.y1 - os->video_dst_rect.y0;

		/* Get src window dimension and position */
		src_win.x = os->video_src_rect.x0;
		src_win.y = os->video_src_rect.y0;
		src_win.width = os->video_src_rect.x1 - os->video_src_rect.x0;
		src_win.height = os->video_src_rect.y1 - os->video_src_rect.y0;

		/* Do the y cutoff (due to a bug in sunxi disp driver) */
		if (scn_win.y < 0)
		{
			int cutoff = -scn_win.y;
			src_win.y += cutoff;
			src_win.height -= cutoff;
			scn_win.y = 0;
			scn_win.height -= cutoff;
		}

		/* Reset window dimension and position */
		args[2] = (unsigned long)(&scn_win);
		ioctl(q->target->fd, DISP_CMD_LAYER_SET_SCN_WINDOW, args);
		args[2] = (unsigned long)(&src_win);
		ioctl(q->target->fd, DISP_CMD_LAYER_SET_SRC_WINDOW, args);

		q->target->drawable_change = 0;
	}

	/*
	 * Display the VIDEO layer
	 */
	if (os->vs)
	{
		static int last_id;
		uint32_t args[4] = { 0, q->target->layer, 0, 0 };

		if (os->vs->start_flag == 1 || q->target->start_flag == 1)
		{
			last_id = -1; /* Reset the video.id */

			__disp_layer_info_t layer_info;
			memset(&layer_info, 0, sizeof(layer_info));

			args[2] = (unsigned long)(&layer_info);
			ioctl(q->target->fd, DISP_CMD_LAYER_GET_PARA, args);

			layer_info.pipe = q->device->osd_enabled ? 0 : 1;
			layer_info.mode = DISP_LAYER_WORK_MODE_SCALER;
			layer_info.fb.format = DISP_FORMAT_YUV420;
			layer_info.fb.seq = DISP_SEQ_UVUV;
			switch (os->vs->source_format) {
			case VDP_YCBCR_FORMAT_YUYV:
				layer_info.fb.mode = DISP_MOD_INTERLEAVED;
				layer_info.fb.format = DISP_FORMAT_YUV422;
				layer_info.fb.seq = DISP_SEQ_YUYV;
				break;
			case VDP_YCBCR_FORMAT_UYVY:
				layer_info.fb.mode = DISP_MOD_INTERLEAVED;
				layer_info.fb.format = DISP_FORMAT_YUV422;
				layer_info.fb.seq = DISP_SEQ_UYVY;
				break;
			case VDP_YCBCR_FORMAT_NV12:
				layer_info.fb.mode = DISP_MOD_NON_MB_UV_COMBINED;
				break;
			case VDP_YCBCR_FORMAT_YV12:
				layer_info.fb.mode = DISP_MOD_NON_MB_PLANAR;
				break;
			default:
			case INTERNAL_YCBCR_FORMAT:
				layer_info.fb.mode = DISP_MOD_MB_UV_COMBINED;
				break;
			}

			layer_info.fb.br_swap = 0;
			if (os->vs->height < 720)
				layer_info.fb.cs_mode = DISP_BT601;
			else
				layer_info.fb.cs_mode = DISP_BT709;
			layer_info.fb.size.width = os->vs->width;
			layer_info.fb.size.height = os->vs->height;
			layer_info.src_win.x = os->video_src_rect.x0;
			layer_info.src_win.y = os->video_src_rect.y0;
			layer_info.src_win.width = os->video_src_rect.x1 - os->video_src_rect.x0;
			layer_info.src_win.height = os->video_src_rect.y1 - os->video_src_rect.y0;
			layer_info.scn_win.x = q->target->x + os->video_dst_rect.x0;
			layer_info.scn_win.y = q->target->y + os->video_dst_rect.y0;
			layer_info.scn_win.width = os->video_dst_rect.x1 - os->video_dst_rect.x0;
			layer_info.scn_win.height = os->video_dst_rect.y1 - os->video_dst_rect.y0;
			layer_info.ck_enable = q->device->osd_enabled ? 0 : 1;

			if (layer_info.scn_win.y < 0)
			{
				int cutoff = -(layer_info.scn_win.y);
				layer_info.src_win.y += cutoff;
				layer_info.src_win.height -= cutoff;
				layer_info.scn_win.y = 0;
				layer_info.scn_win.height -= cutoff;
			}

			layer_info.fb.addr[0] = ve_virt2phys(os->yuv->data) + 0x40000000;
			layer_info.fb.addr[1] = ve_virt2phys(os->yuv->data + os->vs->luma_size) + 0x40000000;
			layer_info.fb.addr[2] = ve_virt2phys(os->yuv->data + os->vs->luma_size + os->vs->luma_size / 4) + 0x40000000;

			args[2] = (unsigned long)(&layer_info);
			ioctl(q->target->fd, DISP_CMD_LAYER_SET_PARA, args);

			args[2] = 0;
			ioctl(q->target->fd, DISP_CMD_LAYER_OPEN, args);
			ioctl(q->target->fd, DISP_CMD_VIDEO_START, args);

			os->vs->start_flag = 0;		/* Initial run is done, only set video.addr[] in the next runs */
			q->target->start_flag = 0;
		}
		else
		{
			__disp_video_fb_t video;
			memset(&video, 0, sizeof(__disp_video_fb_t));
			video.id = last_id + 1;
			video.addr[0] = ve_virt2phys(os->yuv->data) + 0x40000000;
			video.addr[1] = ve_virt2phys(os->yuv->data + os->vs->luma_size) + 0x40000000;
			video.addr[2] = ve_virt2phys(os->yuv->data + os->vs->luma_size + os->vs->luma_size / 4) + 0x40000000;

			if (q->device->deint_enabled)
			{
				video.interlace = os->video_deinterlace;
				video.top_field_first = os->video_field ? 0 : 1;
			}

			args[2] = (unsigned long)(&video);
			int tmp, i = 0;
			while ((tmp = ioctl(q->target->fd, DISP_CMD_VIDEO_GET_FRAME_ID, args)) != last_id)
			{
				if (tmp == -1)
					break;
				VDPAU_LOG(LINFO, "Waiting for frame id ... tmp=%d, last_id=%d", tmp, last_id);

				usleep(1000);
				if (i++ > 10)
				{
					VDPAU_LOG(LWARN, "Waiting for frame id failed");
					break;
				}
			}

			ioctl(q->target->fd, DISP_CMD_VIDEO_SET_FB, args);
			last_id++;
		}

		/*
		 * Note: might be more reliable (but slower and problematic when there
		 * are driver issues and the GET functions return wrong values) to query the
		 * old values instead of relying on our internal csc_change.
		 * Since the driver calculates a matrix out of these values after each
		 * set doing this unconditionally is costly.
		 */
		if (os->csc_change)
		{
			ioctl(q->target->fd, DISP_CMD_LAYER_ENHANCE_OFF, args);

			/* scale VDPAU: -1.0 ~ 1.0 to SUNXI: 0 ~ 100 */
			args[2] = ((os->brightness + 1.0) * 50.0) + 0.5;
			ioctl(q->target->fd, DISP_CMD_LAYER_SET_BRIGHT, args);

			/* scale VDPAU: 0.0 ~ 10.0 to SUNXI: 0 ~ 100 */
			if (os->contrast <= 1.0)
				args[2] = (os->contrast * 50.0) + 0.5;
			else
				args[2] = (50.0 + (os->contrast - 1.0) * 50.0 / 9.0) + 0.5;
			ioctl(q->target->fd, DISP_CMD_LAYER_SET_CONTRAST, args);

			/* scale VDPAU: 0.0 ~ 10.0 to SUNXI: 0 ~ 100 */
			if (os->saturation <= 1.0)
				args[2] = (os->saturation * 50.0) + 0.5;
			else
				args[2] = (50.0 + (os->saturation - 1.0) * 50.0 / 9.0) + 0.5;
			ioctl(q->target->fd, DISP_CMD_LAYER_SET_SATURATION, args);

			/* scale VDPAU: -PI ~ PI   to SUNXI: 0 ~ 100 */
			args[2] = (((os->hue / M_PI) + 1.0) * 50.0) + 0.5;
			ioctl(q->target->fd, DISP_CMD_LAYER_SET_HUE, args);

			ioctl(q->target->fd, DISP_CMD_LAYER_ENHANCE_ON, args);

			VDPAU_LOG(LDBG, ">bright: %g, contrast: %g, saturation: %g, hue: %g",
			          (double)os->brightness, (double)os->contrast,
			          (double)os->saturation, (double)os->hue);

			os->csc_change = 0;
		}
	}
	else	/* No video surface present. Close the layer. */
	{
		uint32_t args[4] = { 0, q->target->layer, 0, 0 };
		ioctl(q->target->fd, DISP_CMD_LAYER_CLOSE, args);
	}

	/* OSD is disabled, so skip OSD displaying. */
	if (!q->device->osd_enabled)
		return VDP_STATUS_OK;

	/*
	 * Display the OSD layer
	 */
	if (os->rgba.flags & RGBA_FLAG_NEEDS_CLEAR)
		rgba_clear(&os->rgba);

	if (os->rgba.flags & RGBA_FLAG_DIRTY)
	{
		rgba_flush(&os->rgba);

		__disp_layer_info_t layer_info;
		memset(&layer_info, 0, sizeof(layer_info));

		uint32_t args[4] = { 0, q->target->layer_top, (unsigned long)(&layer_info), 0 };
		ioctl(q->target->fd, DISP_CMD_LAYER_GET_PARA, args);

		layer_info.pipe = 1;
		layer_info.mode = DISP_LAYER_WORK_MODE_NORMAL;
		layer_info.fb.mode = DISP_MOD_INTERLEAVED;
		layer_info.fb.format = DISP_FORMAT_ARGB8888;
		layer_info.fb.seq = DISP_SEQ_ARGB;
		switch (os->rgba.format)
		{
		case VDP_RGBA_FORMAT_R8G8B8A8:
			layer_info.fb.br_swap = 1;
			break;
		case VDP_RGBA_FORMAT_B8G8R8A8:
		default:
			layer_info.fb.br_swap = 0;
			break;
		}
		layer_info.fb.addr[0] = ve_virt2phys(os->rgba.data) + 0x40000000;
		layer_info.fb.cs_mode = DISP_BT601;
		layer_info.fb.size.width = os->rgba.width;
		layer_info.fb.size.height = os->rgba.height;
		layer_info.src_win.x = os->rgba.dirty.x0;
		layer_info.src_win.y = os->rgba.dirty.y0;
		layer_info.src_win.width = os->rgba.dirty.x1 - os->rgba.dirty.x0;
		layer_info.src_win.height = os->rgba.dirty.y1 - os->rgba.dirty.y0;
		layer_info.scn_win.x = q->target->x + os->rgba.dirty.x0;
		layer_info.scn_win.y = q->target->y + os->rgba.dirty.y0;
		layer_info.scn_win.width = min_nz(clip_width, os->rgba.dirty.x1) - os->rgba.dirty.x0;
		layer_info.scn_win.height = min_nz(clip_height, os->rgba.dirty.y1) - os->rgba.dirty.y0;

		args[2] = (unsigned long)(&layer_info);
		ioctl(q->target->fd, DISP_CMD_LAYER_SET_PARA, args);

		ioctl(q->target->fd, DISP_CMD_LAYER_OPEN, args);
	}
	else
	{
		uint32_t args[4] = { 0, q->target->layer_top, 0, 0 };
		ioctl(q->target->fd, DISP_CMD_LAYER_CLOSE, args);
	}

	return VDP_STATUS_OK;
}

static void *presentation_thread(void *param)
{
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	queue_ctx_t *q = (queue_ctx_t *)param;

	output_surface_ctx_t *os_prev = NULL;
	output_surface_ctx_t *os_pprev = NULL;

	while (!end_presentation) {
		if(!q_isEmpty(queue)) /* We have a task in the queue to display */
		{
			task_t *task;
			if (!q_pop_head(queue, (void *)&task)) /* remove it from queue */
			{
				/* do the VSync, if enabled */
				if (wait_for_vsync(q->device))
					VDPAU_LOG(LWARN, "VSync failed");
				frame_time = get_time();

				/* display the task */
				do_presentation_queue_display(task);

				/* Rotate the surfaces and set the status flags */
				if (os_prev) /* This is the actually displayed surface */
				{
					os_prev->first_presentation_time = frame_time;
					os_prev->status = VDP_PRESENTATION_QUEUE_STATUS_VISIBLE;
				}
				if (os_pprev) /* This is the previously displayed surface */
					os_pprev->status = VDP_PRESENTATION_QUEUE_STATUS_IDLE;

				output_surface_ctx_t *os_cur = handle_get(task->surface);
				os_pprev = os_prev;
				os_prev = os_cur;

				free(task);
			}
			else /* This should never happen! */
				VDPAU_LOG(LERR, "Error getting task");
		}
		/* We have no surface in the queue, so simply wait some period of time (find a suitable value!)
		 * Otherwise, while is doing a race, that it can't win.
		 */
		else
			usleep(1000);
	}
	return NULL;
}

VdpStatus vdp_presentation_queue_target_create_x11(VdpDevice device,
                                                   Drawable drawable,
                                                   VdpPresentationQueueTarget *target)
{
	if (!target || !drawable)
		return VDP_STATUS_INVALID_POINTER;

	device_ctx_t *dev = handle_get(device);
	if (!dev)
		return VDP_STATUS_INVALID_HANDLE;

	queue_target_ctx_t *qt = handle_create(sizeof(*qt), target);
	if (!qt)
		return VDP_STATUS_RESOURCES;

	qt->fd = open("/dev/disp", O_RDWR);
	if (qt->fd == -1)
	{
		handle_destroy(*target);
		return VDP_STATUS_ERROR;
	}

	int tmp = SUNXI_DISP_VERSION;
	if (ioctl(qt->fd, DISP_CMD_VERSION, &tmp) < 0)
	{
		close(qt->fd);
		handle_destroy(*target);
		return VDP_STATUS_ERROR;
	}

	uint32_t args[4] = { 0, DISP_LAYER_WORK_MODE_SCALER, 0, 0 };
	qt->layer = ioctl(qt->fd, DISP_CMD_LAYER_REQUEST, args);
	if (qt->layer == 0)
		goto out_layer;

	args[1] = qt->layer;
	ioctl(qt->fd, dev->osd_enabled ? DISP_CMD_LAYER_TOP : DISP_CMD_LAYER_BOTTOM, args);

	if (dev->osd_enabled)
	{
		args[1] = DISP_LAYER_WORK_MODE_NORMAL;
		qt->layer_top = ioctl(qt->fd, DISP_CMD_LAYER_REQUEST, args);
		if (qt->layer_top == 0)
			goto out_layer_top;

		args[1] = qt->layer_top;
		ioctl(qt->fd, DISP_CMD_LAYER_TOP, args);
	}
	else
	{
		__disp_colorkey_t ck;
		ck.ck_max.red = ck.ck_min.red = 0;
		ck.ck_max.green = ck.ck_min.green = 1;
		ck.ck_max.blue = ck.ck_min.blue = 2;
		ck.red_match_rule = 2;
		ck.green_match_rule = 2;
		ck.blue_match_rule = 2;

		args[1] = (unsigned long)(&ck);
		ioctl(qt->fd, DISP_CMD_SET_COLORKEY, args);
	}

	qt->start_flag = 1;
	qt->drawable_change = 0;
	qt->drawable_unmap = 0;
	qt->drawable_x = 0;
	qt->drawable_y = 0;
	qt->drawable_width = 0;
	qt->drawable_height = 0;

	qt->drawable = drawable;

	/* Register drawable and parent window for events */
	Window root, parent, *children;
	uint32_t nchildren;
	XQueryTree(dev->display, drawable, &root, &parent, &children, &nchildren);
	XSelectInput(dev->display, drawable, StructureNotifyMask);
	XSelectInput(dev->display, parent, StructureNotifyMask);

	/* get current window position */
	Window dummy;
	XTranslateCoordinates(dev->display, qt->drawable, RootWindow(dev->display, dev->screen), 0, 0, &qt->x, &qt->y, &dummy);
	XSetWindowBackground(dev->display, drawable, 0x000102);
	XClearWindow(dev->display, drawable);

	return VDP_STATUS_OK;

out_layer_top:
	args[1] = qt->layer;
	ioctl(qt->fd, DISP_CMD_LAYER_RELEASE, args);
out_layer:
	close(qt->fd);
	handle_destroy(*target);
	return VDP_STATUS_RESOURCES;
}

VdpStatus vdp_presentation_queue_target_destroy(VdpPresentationQueueTarget presentation_queue_target)
{
	queue_target_ctx_t *qt = handle_get(presentation_queue_target);
	if (!qt)
		return VDP_STATUS_INVALID_HANDLE;

	uint32_t args[4] = { 0, qt->layer, 0, 0 };

	ioctl(qt->fd, DISP_CMD_VIDEO_STOP, args);
	ioctl(qt->fd, DISP_CMD_LAYER_CLOSE, args);
	ioctl(qt->fd, DISP_CMD_LAYER_RELEASE, args);

	if (qt->layer_top)
	{
		args[1] = qt->layer_top;
		ioctl(qt->fd, DISP_CMD_LAYER_CLOSE, args);
		ioctl(qt->fd, DISP_CMD_LAYER_RELEASE, args);
	}

	close(qt->fd);

	handle_destroy(presentation_queue_target);

	return VDP_STATUS_OK;
}

VdpStatus vdp_presentation_queue_create(VdpDevice device,
                                        VdpPresentationQueueTarget presentation_queue_target,
                                        VdpPresentationQueue *presentation_queue)
{
	if (!presentation_queue)
		return VDP_STATUS_INVALID_POINTER;

	device_ctx_t *dev = handle_get(device);
	if (!dev)
		return VDP_STATUS_INVALID_HANDLE;

	queue_target_ctx_t *qt = handle_get(presentation_queue_target);
	if (!qt)
		return VDP_STATUS_INVALID_HANDLE;

	queue_ctx_t *q = handle_create(sizeof(*q), presentation_queue);
	if (!q)
		return VDP_STATUS_RESOURCES;

	q->target = qt;
	q->device = dev;

	/* initialize queue and launch worker thread */
	if (!queue) {
		end_presentation = 0;
		queue = q_queue_init();
		pthread_create(&presentation_thread_id, NULL, presentation_thread, q);
	}

	return VDP_STATUS_OK;
}

VdpStatus vdp_presentation_queue_destroy(VdpPresentationQueue presentation_queue)
{
	queue_ctx_t *q = handle_get(presentation_queue);
	if (!q)
		return VDP_STATUS_INVALID_HANDLE;

	end_presentation = 1;
	pthread_join(presentation_thread_id, NULL);

	q_queue_free(queue);
	queue = NULL;

	handle_destroy(presentation_queue);

	return VDP_STATUS_OK;
}

VdpStatus vdp_presentation_queue_set_background_color(VdpPresentationQueue presentation_queue,
                                                      VdpColor *const background_color)
{
	if (!background_color)
		return VDP_STATUS_INVALID_POINTER;

	queue_ctx_t *q = handle_get(presentation_queue);
	if (!q)
		return VDP_STATUS_INVALID_HANDLE;

	q->background.red = background_color->red;
	q->background.green = background_color->green;
	q->background.blue = background_color->blue;
	q->background.alpha = background_color->alpha;

	return VDP_STATUS_OK;
}

VdpStatus vdp_presentation_queue_get_background_color(VdpPresentationQueue presentation_queue,
                                                      VdpColor *const background_color)
{
	if (!background_color)
		return VDP_STATUS_INVALID_POINTER;

	queue_ctx_t *q = handle_get(presentation_queue);
	if (!q)
		return VDP_STATUS_INVALID_HANDLE;

	background_color->red = q->background.red;
	background_color->green = q->background.green;
	background_color->blue = q->background.blue;
	background_color->alpha = q->background.alpha;

	return VDP_STATUS_OK;
}

VdpStatus vdp_presentation_queue_get_time(VdpPresentationQueue presentation_queue,
                                          VdpTime *current_time)
{
	queue_ctx_t *q = handle_get(presentation_queue);
	if (!q)
		return VDP_STATUS_INVALID_HANDLE;

	*current_time = get_time();
	return VDP_STATUS_OK;
}

VdpStatus vdp_presentation_queue_block_until_surface_idle(VdpPresentationQueue presentation_queue,
                                                          VdpOutputSurface surface,
                                                          VdpTime *first_presentation_time)
{
	queue_ctx_t *q = handle_get(presentation_queue);
	if (!q)
		return VDP_STATUS_INVALID_HANDLE;

	output_surface_ctx_t *os = handle_get(surface);
	if (!os)
		return VDP_STATUS_INVALID_HANDLE;

	while (os->status != VDP_PRESENTATION_QUEUE_STATUS_IDLE) {
		usleep(1000);
		output_surface_ctx_t *os = handle_get(surface);
		if (!os)
			return VDP_STATUS_INVALID_HANDLE;
	}

	*first_presentation_time = os->first_presentation_time;

	return VDP_STATUS_OK;
}

VdpStatus vdp_presentation_queue_query_surface_status(VdpPresentationQueue presentation_queue,
                                                      VdpOutputSurface surface,
                                                      VdpPresentationQueueStatus *status,
                                                      VdpTime *first_presentation_time)
{
	if (!status || !first_presentation_time)
		return VDP_STATUS_INVALID_POINTER;

	queue_ctx_t *q = handle_get(presentation_queue);
	if (!q)
		return VDP_STATUS_INVALID_HANDLE;

	output_surface_ctx_t *os = handle_get(surface);
	if (!os)
		return VDP_STATUS_INVALID_HANDLE;

	*status = os->status;
	*first_presentation_time = os->first_presentation_time;

	return VDP_STATUS_OK;
}

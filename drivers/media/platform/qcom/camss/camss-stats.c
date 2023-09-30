// SPDX-License-Identifier: GPL-2.0
/*
 * camss-stats.c
 *
 * Qualcomm MSM Camera Subsystem - V4L2 stats device node
 *
 * Copyright (c) 2013-2015, The Linux Foundation. All rights reserved.
 * Copyright (C) 2015-2018 Linaro Ltd.
 */
#include <linux/slab.h>
#include <media/media-entity.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mc.h>
#include <media/videobuf2-dma-sg.h>
#include <uapi/linux/qcom-camss.h>

#include "camss-stats.h"
#include "camss.h"

/* -----------------------------------------------------------------------------
 * Video queue operations
 */

static int stats_queue_setup(struct vb2_queue *q,
	unsigned int *num_buffers, unsigned int *num_planes,
	unsigned int sizes[], struct device *alloc_devs[])
{
	*num_planes = 1;

	sizes[0] = sizeof(struct camss_stats_payload);

	return 0;
}

static int stats_buf_init(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct camss_stats_buffer *buffer = container_of(vbuf,
							 struct camss_stats_buffer,
							 vb);
	struct sg_table *sgt;

	sgt = vb2_dma_sg_plane_desc(vb, 0);
	if (!sgt)
		return -EFAULT;

	buffer->addr = sg_dma_address(sgt->sgl) + 16;

	return 0;
}

static int stats_buf_prepare(struct vb2_buffer *vb)
{
	if (vb2_plane_size(vb, 0) < sizeof(struct camss_stats_payload))
		return -EINVAL;

	vb2_set_plane_payload(vb, 0, sizeof(struct camss_stats_payload));

	return 0;
}

static void stats_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct camss_stats *stats = vb2_get_drv_priv(vb->vb2_queue);
	struct camss_stats_buffer *buffer =
		container_of(vbuf, struct camss_stats_buffer, vb);

	stats->ops->queue_buffer(stats, buffer);
}

static void stats_stop_streaming(struct vb2_queue *q)
{
	struct camss_stats *stats = vb2_get_drv_priv(q);

	stats->ops->flush_buffers(stats, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops msm_stats_vb2_q_ops = {
	.queue_setup     = stats_queue_setup,
	.wait_prepare    = vb2_ops_wait_prepare,
	.wait_finish     = vb2_ops_wait_finish,
	.buf_init        = stats_buf_init,
	.buf_prepare     = stats_buf_prepare,
	.buf_queue       = stats_buf_queue,
	.stop_streaming  = stats_stop_streaming,
};

/* -----------------------------------------------------------------------------
 * V4L2 ioctls
 */

static int stats_querycap(struct file *file, void *fh,
			  struct v4l2_capability *cap)
{
	strscpy(cap->driver, "qcom-camss", sizeof(cap->driver));
	strscpy(cap->card, "Qualcomm Camera Subsystem", sizeof(cap->card));

	return 0;
}

static int stats_enum_fmt(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	struct camss_stats *stats = video_drvdata(file);

	if (f->type != stats->type || f->index > 0)
		return -EINVAL;

	f->pixelformat = V4L2_META_FMT_QCOM_CAMSS_STATS;

	return 0;
}

static int stats_g_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct camss_stats *stats = video_drvdata(file);

	*f = stats->active_fmt;

	return 0;
}

static const struct v4l2_ioctl_ops msm_stats_ioctl_ops = {
	.vidioc_querycap		= stats_querycap,
	.vidioc_enum_fmt_meta_cap	= stats_enum_fmt,
	.vidioc_g_fmt_meta_cap		= stats_g_fmt,
	.vidioc_s_fmt_meta_cap		= stats_g_fmt,
	.vidioc_try_fmt_meta_cap	= stats_g_fmt,
	.vidioc_reqbufs			= vb2_ioctl_reqbufs,
	.vidioc_querybuf		= vb2_ioctl_querybuf,
	.vidioc_qbuf			= vb2_ioctl_qbuf,
	.vidioc_expbuf			= vb2_ioctl_expbuf,
	.vidioc_dqbuf			= vb2_ioctl_dqbuf,
	.vidioc_create_bufs		= vb2_ioctl_create_bufs,
	.vidioc_prepare_buf		= vb2_ioctl_prepare_buf,
	.vidioc_streamon		= vb2_ioctl_streamon,
	.vidioc_streamoff		= vb2_ioctl_streamoff,
};

/* -----------------------------------------------------------------------------
 * V4L2 file operations
 */

static int stats_open(struct file *file)
{
	struct video_device *vdev = video_devdata(file);
	struct camss_stats *stats = video_drvdata(file);
	struct v4l2_fh *vfh;
	int ret;

	mutex_lock(&stats->lock);

	vfh = kzalloc(sizeof(*vfh), GFP_KERNEL);
	if (vfh == NULL) {
		ret = -ENOMEM;
		goto error_alloc;
	}

	v4l2_fh_init(vfh, vdev);
	v4l2_fh_add(vfh);

	file->private_data = vfh;

	ret = v4l2_pipeline_pm_get(&vdev->entity);
	if (ret < 0) {
		dev_err(stats->camss->dev, "Failed to power up pipeline: %d\n",
			ret);
		goto error_pm_use;
	}

	mutex_unlock(&stats->lock);

	return 0;

error_pm_use:
	v4l2_fh_release(file);

error_alloc:
	mutex_unlock(&stats->lock);

	return ret;
}

static int stats_release(struct file *file)
{
	struct video_device *vdev = video_devdata(file);

	vb2_fop_release(file);

	v4l2_pipeline_pm_put(&vdev->entity);

	file->private_data = NULL;

	return 0;
}

static const struct v4l2_file_operations msm_vid_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = video_ioctl2,
	.open           = stats_open,
	.release        = stats_release,
	.poll           = vb2_fop_poll,
	.mmap		= vb2_fop_mmap,
	.read		= vb2_fop_read,
};

/* -----------------------------------------------------------------------------
 * CAMSS stats core
 */

static void msm_stats_release(struct video_device *vdev)
{
	struct camss_stats *stats = video_get_drvdata(vdev);

	media_entity_cleanup(&vdev->entity);

	mutex_destroy(&stats->q_lock);
	mutex_destroy(&stats->lock);

	if (atomic_dec_and_test(&stats->camss->ref_count))
		camss_delete(stats->camss);
}

/*
 * msm_stats_init_format - Helper function to initialize format
 * @stats: struct camss_stats
 *
 * Initialize pad format with default value.
 *
 * Return 0 on success or a negative error code otherwise
 */
static int msm_stats_init_format(struct camss_stats *stats)
{
	struct v4l2_format format = {
		.type = V4L2_BUF_TYPE_META_CAPTURE,
		.fmt.meta = {
			.dataformat = V4L2_META_FMT_QCOM_CAMSS_STATS,
			.buffersize = sizeof(struct camss_stats_payload),
		},
	};

	stats->active_fmt = format;

	return 0;
}

/*
 * msm_stats_assign_buf_type - Set the type of a stats buffer
 * @buf: buffer
 * @type: the stats type this buffer will be used for
 *
 * Modify the buffer contents to indicate what kind of stats it contains
 */
void msm_stats_assign_buf_type(struct camss_stats_buffer *buf,
			       enum camss_stats_type type)
{
	struct camss_stats_payload *payload;

	payload = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);
	payload->hdr.buf_type = type;
}

/*
 * msm_stats_register - Register a stats device node
 * @stats: struct camss_stats
 * @v4l2_dev: V4L2 device
 * @name: name to be used for the stats device node
 *
 * Initialize and register a stats device node to a V4L2 device. Also
 * initialize the vb2 queue.
 *
 * Return 0 on success or a negative error code otherwise
 */

int msm_stats_register(struct camss_stats *stats, struct v4l2_device *v4l2_dev,
		       const char *name)
{
	struct media_pad *pad = &stats->pad;
	struct video_device *vdev;
	struct vb2_queue *q;
	int ret;

	vdev = &stats->vdev;

	mutex_init(&stats->q_lock);

	q = &stats->vb2_q;
	q->drv_priv = stats;
	q->mem_ops = &vb2_dma_sg_memops;
	q->ops = &msm_stats_vb2_q_ops;
	q->type = V4L2_BUF_TYPE_META_CAPTURE;
	q->io_modes = VB2_DMABUF | VB2_MMAP | VB2_READ;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->buf_struct_size = sizeof(struct camss_stats_buffer);
	q->dev = stats->camss->dev;
	q->lock = &stats->q_lock;
	ret = vb2_queue_init(q);
	if (ret < 0) {
		dev_err(v4l2_dev->dev, "Failed to init vb2 queue: %d\n", ret);
		goto error_vb2_init;
	}

	pad->flags = MEDIA_PAD_FL_SINK;
	ret = media_entity_pads_init(&vdev->entity, 1, pad);
	if (ret < 0) {
		dev_err(v4l2_dev->dev, "Failed to init stats entity: %d\n",
			ret);
		goto error_vb2_init;
	}

	mutex_init(&stats->lock);

	ret = msm_stats_init_format(stats);
	if (ret < 0) {
		dev_err(v4l2_dev->dev, "Failed to init format: %d\n", ret);
		goto error_video_register;
	}

	vdev->fops = &msm_vid_fops;
	vdev->device_caps = V4L2_CAP_META_CAPTURE | V4L2_CAP_STREAMING;
	vdev->ioctl_ops = &msm_stats_ioctl_ops;
	vdev->release = msm_stats_release;
	vdev->v4l2_dev = v4l2_dev;
	vdev->vfl_dir = VFL_DIR_RX;
	vdev->queue = &stats->vb2_q;
	vdev->lock = &stats->lock;
	strscpy(vdev->name, name, sizeof(vdev->name));

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret < 0) {
		dev_err(v4l2_dev->dev, "Failed to register video device: %d\n",
			ret);
		goto error_video_register;
	}

	video_set_drvdata(vdev, stats);
	atomic_inc(&stats->camss->ref_count);

	return 0;

error_video_register:
	media_entity_cleanup(&vdev->entity);
	mutex_destroy(&stats->lock);
error_vb2_init:
	mutex_destroy(&stats->q_lock);

	return ret;
}

void msm_stats_unregister(struct camss_stats *stats)
{
	atomic_inc(&stats->camss->ref_count);
	vb2_video_unregister_device(&stats->vdev);
	atomic_dec(&stats->camss->ref_count);
}

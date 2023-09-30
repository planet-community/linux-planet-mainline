/* SPDX-License-Identifier: GPL-2.0 */
/*
 * camss-stats.h
 *
 * Qualcomm MSM Camera Subsystem - V4L2 stats device node
 *
 * Copyright (c) 2013-2015, The Linux Foundation. All rights reserved.
 * Copyright (C) 2015-2018 Linaro Ltd.
 */
#ifndef QC_MSM_CAMSS_STATS_H
#define QC_MSM_CAMSS_STATS_H

#include <linux/mutex.h>
#include <linux/videodev2.h>
#include <media/media-entity.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-mediabus.h>
#include <media/videobuf2-v4l2.h>

struct camss_stats_buffer {
	struct vb2_v4l2_buffer vb;
	dma_addr_t addr;
	struct list_head queue;
};

struct camss_stats;

struct camss_stats_ops {
	int (*queue_buffer)(struct camss_stats *stats,
			    struct camss_stats_buffer *buf);
	int (*flush_buffers)(struct camss_stats *stats,
			     enum vb2_buffer_state state);
};

struct camss_format_info;

struct camss_stats {
	struct camss *camss;
	struct vb2_queue vb2_q;
	struct video_device vdev;
	struct media_pad pad;
	struct v4l2_format active_fmt;
	enum v4l2_buf_type type;
	struct media_pipeline pipe;
	const struct camss_stats_ops *ops;
	struct mutex lock;
	struct mutex q_lock;
};

void msm_stats_assign_buf_type(struct camss_stats_buffer *buf,
			       enum camss_stats_type type);

int msm_stats_register(struct camss_stats *stats, struct v4l2_device *v4l2_dev,
		       const char *name);

void msm_stats_unregister(struct camss_stats *stats);

#endif /* QC_MSM_CAMSS_STATS_H */

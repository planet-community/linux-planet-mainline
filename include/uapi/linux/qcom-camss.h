/* SPDX-License-Identifier: ((GPL-2.0+ WITH Linux-syscall-note) OR MIT) */
#ifndef _UAPI_QCOM_CAMSS_H
#define _UAPI_QCOM_CAMSS_H

#include <linux/types.h>

enum camss_stats_type {
	CAMSS_STATS_TYPE_BAYER_EXPOSURE,
	CAMSS_STATS_TYPE_MAX,
};

struct camss_stats_be_data {
	__u32 sum_r;
	__u32 sum_b;
	__u32 sum_gr;
	__u32 sum_gb;
	__u16 count_r;
	__u16 count_b;
	__u16 count_gr;
	__u16 count_gb;
} __attribute__((packed));

struct camss_stats_payload {
	union {
		enum camss_stats_type buf_type;
		__u8 data[16];
	} hdr;
	union {
		struct camss_stats_be_data be[32 * 24];
	} data;
} __attribute__((packed));

#endif /* _UAPI_QCOM_CAMSS_H */

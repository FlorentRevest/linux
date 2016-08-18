/*
 * Sunxi Cedrus codec driver
 *
 * Copyright (C) 2016 Florent Revest
 * Florent Revest <florent.revest@free-electrons.com>
 *
 * Based on reverse engineering efforts of the 'Cedrus' project
 * Copyright (c) 2013-2014 Jens Kuske <jenskuske@gmail.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "sunxi_cedrus_common.h"

#include <media/videobuf2-dma-contig.h>

void process_h264(struct sunxi_cedrus_ctx *ctx,
			  struct vb2_v4l2_buffer *in_vb,
			  struct vb2_v4l2_buffer *out_vb)
{
	struct sunxi_cedrus_dev *dev = ctx->dev;
	dma_addr_t input_buffer, output_luma, output_chroma;
	const struct v4l2_ctrl_h264_frame_hdr *frame_hdr = ctx->h264_frame_hdr_ctrl->p_new.p;
	uint16_t width = (frame_hdr->width + 15) / 16;
	uint16_t height = (frame_hdr->height + 15) / 16;
	dma_addr_t forward_luma, forward_chroma, backward_luma, backward_chroma;

	input_buffer = vb2_dma_contig_plane_dma_addr(&in_vb->vb2_buf, 0);
	output_luma = vb2_dma_contig_plane_dma_addr(&out_vb->vb2_buf, 0);
	output_chroma = vb2_dma_contig_plane_dma_addr(&out_vb->vb2_buf, 1);
	if (!input_buffer || !output_luma || !output_chroma) {
		v4l2_err(&dev->v4l2_dev,
			 "Acquiring kernel pointers to buffers failed\n");
		return;
	}

	forward_luma = vb2_dma_contig_plane_dma_addr(ctx->dst_bufs[frame_hdr->forward_index], 0);
	forward_chroma = vb2_dma_contig_plane_dma_addr(ctx->dst_bufs[frame_hdr->forward_index], 1);

	backward_luma = vb2_dma_contig_plane_dma_addr(ctx->dst_bufs[frame_hdr->backward_index], 0);
	backward_chroma = vb2_dma_contig_plane_dma_addr(ctx->dst_bufs[frame_hdr->backward_index], 1);

	out_vb->vb2_buf.timestamp = in_vb->vb2_buf.timestamp;

	if (in_vb->flags & V4L2_BUF_FLAG_TIMECODE)
		out_vb->timecode = in_vb->timecode;
	out_vb->field = in_vb->field;
	out_vb->flags = in_vb->flags &
		(V4L2_BUF_FLAG_TIMECODE |
		 V4L2_BUF_FLAG_KEYFRAME |
		 V4L2_BUF_FLAG_PFRAME |
		 V4L2_BUF_FLAG_BFRAME |
		 V4L2_BUF_FLAG_TSTAMP_SRC_MASK);
}

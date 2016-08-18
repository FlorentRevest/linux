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

#define VOP_I	0
#define VOP_P	1
#define VOP_B	2
#define VOP_S	3

void process_mpeg4(struct sunxi_cedrus_ctx *ctx,
			  struct vb2_v4l2_buffer *in_vb,
			  struct vb2_v4l2_buffer *out_vb)
{
	struct sunxi_cedrus_dev *dev = ctx->dev;
	dma_addr_t input_buffer, output_luma, output_chroma;
	const struct v4l2_ctrl_mpeg4_frame_hdr *frame_hdr = ctx->mpeg4_frame_hdr_ctrl->p_new.p;
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

	if (!frame_hdr->vol_fields.resync_marker_disable)
	{
		v4l2_err(&dev->v4l2_dev, "Can not decode VOPs with resync markers");
		return;
	}

	/* Activates MPEG engine */
	sunxi_cedrus_write(dev, 0x00130000 | (VE_ENGINE_MPEG & 0xf), VE_CTRL);

	sunxi_cedrus_write(dev, dev->mbh_buffer  - PHYS_OFFSET, VE_MPEG_MBH_ADDR);
	sunxi_cedrus_write(dev, dev->dcac_buffer - PHYS_OFFSET, VE_MPEG_DCAC_ADDR);
	sunxi_cedrus_write(dev, dev->ncf_buffer  - PHYS_OFFSET, VE_MPEG_NCF_ADDR);

	// set output buffers
	sunxi_cedrus_write(dev, output_luma   - PHYS_OFFSET, VE_MPEG_REC_LUMA);
	sunxi_cedrus_write(dev, output_chroma - PHYS_OFFSET, VE_MPEG_REC_CHROMA);
	sunxi_cedrus_write(dev, output_luma   - PHYS_OFFSET, VE_MPEG_ROT_LUMA);
	sunxi_cedrus_write(dev, output_chroma - PHYS_OFFSET, VE_MPEG_ROT_CHROMA);

	// ??
	sunxi_cedrus_write(dev, 0x40620000, VE_MPEG_SDROT_CTRL);

	// set vop header
	sunxi_cedrus_write(dev, ((frame_hdr->vop_fields.vop_coding_type == VOP_B ? 0x1 : 0x0) << 28)
		| (frame_hdr->vol_fields.quant_type << 24)
		| (frame_hdr->vol_fields.quarter_sample << 23)
		| (frame_hdr->vol_fields.resync_marker_disable << 22)
		| (frame_hdr->vop_fields.vop_coding_type << 18)
		| (frame_hdr->vop_fields.vop_rounding_type << 17)
		| (frame_hdr->vop_fields.intra_dc_vlc_thr << 8)
		| (frame_hdr->vop_fields.top_field_first << 7)
		| (frame_hdr->vop_fields.alternate_vertical_scan_flag << 6)
		| ((frame_hdr->vop_fields.vop_coding_type != VOP_I ? frame_hdr->vop_fcode_forward : 0) << 3)
		| ((frame_hdr->vop_fields.vop_coding_type == VOP_B ? frame_hdr->vop_fcode_backward : 0) << 0)
		, VE_MPEG_VOP_HDR);

	// set size
	sunxi_cedrus_write(dev, (((width + 1) & ~0x1) << 16) | (width << 8) | height, VE_MPEG_SIZE);

	sunxi_cedrus_write(dev, ((width * 16) << 16) | (height * 16), VE_MPEG_FRAME_SIZE);

	sunxi_cedrus_write(dev, 0x0, VE_MPEG_MBA);

	// enable interrupt, unknown control flags
	sunxi_cedrus_write(dev, 0x80084118 | (0x1 << 7) | ((frame_hdr->vop_fields.vop_coding_type == VOP_P ? 0x1 : 0x0) << 12), VE_MPEG_CTRL);

	// set quantization parameter
	sunxi_cedrus_write(dev, frame_hdr->quant_precision, VE_MPEG_QP_INPUT);

	// set forward/backward predicion buffers
	sunxi_cedrus_write(dev, forward_luma    - PHYS_OFFSET, VE_MPEG_FWD_LUMA);
	sunxi_cedrus_write(dev, forward_chroma  - PHYS_OFFSET, VE_MPEG_FWD_CHROMA);
	sunxi_cedrus_write(dev, backward_luma   - PHYS_OFFSET, VE_MPEG_BACK_LUMA);
	sunxi_cedrus_write(dev, backward_chroma - PHYS_OFFSET, VE_MPEG_BACK_CHROMA);

	// set trb/trd
	if (frame_hdr->vop_fields.vop_coding_type == VOP_B)
	{
		sunxi_cedrus_write(dev, (frame_hdr->trb << 16) | (frame_hdr->trd << 0), VE_MPEG_TRBTRD_FRAME);
		// unverified:
		sunxi_cedrus_write(dev, 0x0, VE_MPEG_TRBTRD_FIELD); // TODO: is that right ?
	}

	sunxi_cedrus_write(dev, 0xffffffff, VE_MPEG_STATUS);

	// set input offset in bits
	sunxi_cedrus_write(dev, frame_hdr->slice_pos * 8, VE_MPEG_VLD_OFFSET);

	// set input length in bits
	sunxi_cedrus_write(dev, (frame_hdr->slice_len - frame_hdr->slice_pos) * 8, VE_MPEG_VLD_LEN);

	/* Input beginning and end */
	sunxi_cedrus_write(dev, ((input_buffer - PHYS_OFFSET) & 0x0ffffff0) | ((input_buffer - PHYS_OFFSET) >> 28) | (0x7 << 28), VE_MPEG_VLD_ADDR);
	sunxi_cedrus_write(dev, (input_buffer - PHYS_OFFSET) + 1024*1024 - 1, VE_MPEG_VLD_END);

	/* Starts the MPEG engine */
	sunxi_cedrus_write(dev, 0x8400000d | ((width * height) << 8), VE_MPEG_TRIGGER);
}


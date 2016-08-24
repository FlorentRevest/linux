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

void process_mpeg4(struct sunxi_cedrus_ctx *ctx, dma_addr_t in_buf,
		   dma_addr_t out_luma, dma_addr_t out_chroma,
		   struct v4l2_ctrl_mpeg4_frame_hdr *frame_hdr)
{
	struct sunxi_cedrus_dev *dev = ctx->dev;

	u16 width = DIV_ROUND_UP(frame_hdr->width, 16);
	u16 height = DIV_ROUND_UP(frame_hdr->height, 16);

	u32 vop_header = 0;
	u32 vld_len = frame_hdr->slice_len - frame_hdr->slice_pos;

	struct vb2_buffer *fwd_vb2_buf, *bwd_vb2_buf;
	dma_addr_t fwd_luma = 0, fwd_chroma = 0, bwd_luma = 0, bwd_chroma = 0;

	/*
	 * The VPU is only able to handle bus addresses so we have to subtract
	 * the RAM offset to the physcal addresses
	 */
	fwd_vb2_buf = ctx->dst_bufs[frame_hdr->forward_index];
	if (fwd_vb2_buf) {
		fwd_luma   = vb2_dma_contig_plane_dma_addr(fwd_vb2_buf, 0);
		fwd_chroma = vb2_dma_contig_plane_dma_addr(fwd_vb2_buf, 1);
		fwd_luma   -= PHYS_OFFSET;
		fwd_chroma -= PHYS_OFFSET;
	}

	bwd_vb2_buf = ctx->dst_bufs[frame_hdr->backward_index];
	if (bwd_vb2_buf) {
		bwd_luma   = vb2_dma_contig_plane_dma_addr(bwd_vb2_buf, 0);
		bwd_chroma = vb2_dma_contig_plane_dma_addr(bwd_vb2_buf, 1);
		bwd_chroma -= PHYS_OFFSET;
		bwd_luma   -= PHYS_OFFSET;
	}

	/* Activates MPEG engine */
	sunxi_cedrus_write(dev, VE_CTRL_MPEG, VE_CTRL);

	/* Quantization parameter */
	sunxi_cedrus_write(dev, frame_hdr->quant_scale, VE_MPEG_QP_INPUT);

	/* Intermediate buffers needed by the VPU */
	sunxi_cedrus_write(dev, dev->mbh_buf  - PHYS_OFFSET, VE_MPEG_MBH_ADDR);
	sunxi_cedrus_write(dev, dev->dcac_buf - PHYS_OFFSET, VE_MPEG_DCAC_ADDR);
	sunxi_cedrus_write(dev, dev->ncf_buf  - PHYS_OFFSET, VE_MPEG_NCF_ADDR);

	/* Image's dimensions */
	sunxi_cedrus_write(dev, width << 8  | height,      VE_MPEG_SIZE);
	sunxi_cedrus_write(dev, width << 20 | height << 4, VE_MPEG_FRAME_SIZE);

	/* MPEG VOP's header */
	vop_header |= (frame_hdr->vop_fields.vop_coding_type == VOP_B) << 28;
	vop_header |= frame_hdr->vol_fields.quant_type << 24;
	vop_header |= frame_hdr->vol_fields.quarter_sample << 23;
	vop_header |= frame_hdr->vol_fields.resync_marker_disable << 22;
	vop_header |= frame_hdr->vop_fields.vop_coding_type << 18;
	vop_header |= frame_hdr->vop_fields.vop_rounding_type << 17;
	vop_header |= frame_hdr->vop_fields.intra_dc_vlc_thr << 8;
	vop_header |= frame_hdr->vop_fields.top_field_first << 7;
	vop_header |= frame_hdr->vop_fields.alternate_vertical_scan_flag << 6;
	if (frame_hdr->vop_fields.vop_coding_type != VOP_I)
		vop_header |= frame_hdr->vop_fcode_forward << 3;
	if (frame_hdr->vop_fields.vop_coding_type == VOP_B)
		vop_header |= frame_hdr->vop_fcode_backward << 0;
	sunxi_cedrus_write(dev, vop_header, VE_MPEG_VOP_HDR);

	/* Enable interrupt and an unknown control flag */
	if (frame_hdr->vop_fields.vop_coding_type == VOP_P)
		sunxi_cedrus_write(dev, VE_MPEG_CTRL_MPEG4_P, VE_MPEG_CTRL);
	else
		sunxi_cedrus_write(dev, VE_MPEG_CTRL_MPEG4, VE_MPEG_CTRL);

	/* Temporal distances of B frames */
	if (frame_hdr->vop_fields.vop_coding_type == VOP_B) {
		u32 trbtrd = (frame_hdr->trb << 16) | frame_hdr->trd;

		sunxi_cedrus_write(dev, trbtrd, VE_MPEG_TRBTRD_FRAME);
		sunxi_cedrus_write(dev, 0, VE_MPEG_TRBTRD_FIELD);
	}

	/* Don't rotate or scale buffer */
	sunxi_cedrus_write(dev, VE_NO_SDROT_CTRL, VE_MPEG_SDROT_CTRL);

	/* Macroblock number */
	sunxi_cedrus_write(dev, 0, VE_MPEG_MBA);

	/* Clear previous status */
	sunxi_cedrus_write(dev, 0xffffffff, VE_MPEG_STATUS);

	/* Forward and backward prediction buffers (cached in dst_bufs) */
	sunxi_cedrus_write(dev, fwd_luma,   VE_MPEG_FWD_LUMA);
	sunxi_cedrus_write(dev, fwd_chroma, VE_MPEG_FWD_CHROMA);
	sunxi_cedrus_write(dev, bwd_luma,   VE_MPEG_BACK_LUMA);
	sunxi_cedrus_write(dev, bwd_chroma, VE_MPEG_BACK_CHROMA);

	/* Output luma and chroma buffers */
	sunxi_cedrus_write(dev, out_luma,   VE_MPEG_REC_LUMA);
	sunxi_cedrus_write(dev, out_chroma, VE_MPEG_REC_CHROMA);
	sunxi_cedrus_write(dev, out_luma,   VE_MPEG_ROT_LUMA);
	sunxi_cedrus_write(dev, out_chroma, VE_MPEG_ROT_CHROMA);

	/* Input offset and length in bits */
	sunxi_cedrus_write(dev, frame_hdr->slice_pos, VE_MPEG_VLD_OFFSET);
	sunxi_cedrus_write(dev, vld_len, VE_MPEG_VLD_LEN);

	/* Input beginning and end addresses */
	sunxi_cedrus_write(dev, VE_MPEG_VLD_ADDR_VAL(in_buf), VE_MPEG_VLD_ADDR);
	sunxi_cedrus_write(dev, in_buf + VBV_SIZE - 1, VE_MPEG_VLD_END);

	/* Starts the MPEG engine */
	sunxi_cedrus_write(dev, VE_TRIG_MPEG4(width, height), VE_MPEG_TRIGGER);
}

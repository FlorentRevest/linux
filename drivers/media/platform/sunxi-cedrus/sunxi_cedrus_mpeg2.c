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

static const uint8_t mpeg_default_intra_quant[64] =
{
	 8, 16, 16, 19, 16, 19, 22, 22,
	22, 22, 22, 22, 26, 24, 26, 27,
	27, 27, 26, 26, 26, 26, 27, 27,
	27, 29, 29, 29, 34, 34, 34, 29,
	29, 29, 27, 27, 29, 29, 32, 32,
	34, 34, 37, 38, 37, 35, 35, 34,
	35, 38, 38, 40, 40, 40, 48, 48,
	46, 46, 56, 56, 58, 69, 69, 83
};

static const uint8_t mpeg_default_non_intra_quant[64] =
{
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16
};

void process_mpeg2(struct sunxi_cedrus_ctx *ctx,
			  struct vb2_v4l2_buffer *in_vb,
			  struct vb2_v4l2_buffer *out_vb)
{
	struct sunxi_cedrus_dev *dev = ctx->dev;
	dma_addr_t input_buffer, output_luma, output_chroma;
	int i;
	const struct v4l2_ctrl_mpeg2_frame_hdr *frame_hdr = ctx->mpeg2_frame_hdr_ctrl->p_new.p;
	uint16_t width = (frame_hdr->width + 15) / 16;
	uint16_t height = (frame_hdr->height + 15) / 16;
	uint32_t pic_header = 0x00000000;
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

	/* Activates MPEG engine */
	sunxi_cedrus_write(dev, 0x00130000 | (VE_ENGINE_MPEG & 0xf), VE_CTRL);

	for (i = 0; i < 64; i++)
		sunxi_cedrus_write(dev, (uint32_t)(64 + i) << 8 | mpeg_default_intra_quant[i], VE_MPEG_IQ_MIN_INPUT);
	for (i = 0; i < 64; i++)
		sunxi_cedrus_write(dev, (uint32_t)(i) << 8 | mpeg_default_non_intra_quant[i], VE_MPEG_IQ_MIN_INPUT);

	/* Set size */
	sunxi_cedrus_write(dev, (width << 8) | height, VE_MPEG_SIZE);
	sunxi_cedrus_write(dev, ((width * 16) << 16) | (height * 16), VE_MPEG_FRAME_SIZE);

	pic_header |= ((frame_hdr->picture_coding_type & 0xf) << 28);
	pic_header |= ((frame_hdr->f_code[0][0] & 0xf) << 24);
	pic_header |= ((frame_hdr->f_code[0][1] & 0xf) << 20);
	pic_header |= ((frame_hdr->f_code[1][0] & 0xf) << 16);
	pic_header |= ((frame_hdr->f_code[1][1] & 0xf) << 12);
	pic_header |= ((frame_hdr->intra_dc_precision & 0x3) << 10);
	pic_header |= ((frame_hdr->picture_structure & 0x3) << 8);
	pic_header |= ((frame_hdr->top_field_first & 0x1) << 7);
	pic_header |= ((frame_hdr->frame_pred_frame_dct & 0x1) << 6);
	pic_header |= ((frame_hdr->concealment_motion_vectors & 0x1) << 5);
	pic_header |= ((frame_hdr->q_scale_type & 0x1) << 4);
	pic_header |= ((frame_hdr->intra_vlc_format & 0x1) << 3);
	pic_header |= ((frame_hdr->alternate_scan & 0x1) << 2);
	pic_header |= ((0 & 0x3) << 0);
	sunxi_cedrus_write(dev, pic_header, VE_MPEG_PIC_HDR);

	sunxi_cedrus_write(dev, 0x00000000, VE_MPEG_MBA);
	sunxi_cedrus_write(dev, 0x800001b8, VE_MPEG_CTRL);
	sunxi_cedrus_write(dev, 0x00000000, 0x100 + 0xc4);
	sunxi_cedrus_write(dev, 0x00000000, 0x100 + 0xc8);

	sunxi_cedrus_write(dev, forward_luma    - PHYS_OFFSET, VE_MPEG_FWD_LUMA);
	sunxi_cedrus_write(dev, forward_chroma  - PHYS_OFFSET, VE_MPEG_FWD_CHROMA);
	sunxi_cedrus_write(dev, backward_luma   - PHYS_OFFSET, VE_MPEG_BACK_LUMA);
	sunxi_cedrus_write(dev, backward_chroma - PHYS_OFFSET, VE_MPEG_BACK_CHROMA);

	/* Output luma and chroma buffers */
	sunxi_cedrus_write(dev, output_luma   - PHYS_OFFSET, VE_MPEG_REC_LUMA);
	sunxi_cedrus_write(dev, output_chroma - PHYS_OFFSET, VE_MPEG_REC_CHROMA);
	sunxi_cedrus_write(dev, output_luma   - PHYS_OFFSET, VE_MPEG_ROT_LUMA);
	sunxi_cedrus_write(dev, output_chroma - PHYS_OFFSET, VE_MPEG_ROT_CHROMA);

	/* set input offset in bits */
	sunxi_cedrus_write(dev, frame_hdr->slice_pos * 8, VE_MPEG_VLD_OFFSET);

	/* set input length in bits (+ little bit more, else it fails sometimes ??) */
	sunxi_cedrus_write(dev, (frame_hdr->slice_len - frame_hdr->slice_pos) * 8, VE_MPEG_VLD_LEN);

	/* Input beginning and end */
	sunxi_cedrus_write(dev, ((input_buffer - PHYS_OFFSET) & 0x0ffffff0) | ((input_buffer - PHYS_OFFSET) >> 28) | (0x7 << 28), VE_MPEG_VLD_ADDR);
	sunxi_cedrus_write(dev, (input_buffer - PHYS_OFFSET) + 1024*1024 -1, VE_MPEG_VLD_END);

	/* Starts the MPEG engine */
	sunxi_cedrus_write(dev, (frame_hdr->type ? 0x02000000 : 0x01000000) | 0x8000000f, VE_MPEG_TRIGGER);
}


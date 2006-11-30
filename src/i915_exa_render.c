/*
 * Copyright © 2006 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Wang Zhenyu <zhenyu.z.wang@intel.com>
 *    Eric Anholt <eric@anholt.net>
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "xf86.h"
#include "i830.h"
#include "i915_reg.h"
#include "i915_3d.h"

#ifdef I830DEBUG
#define DEBUG_I830FALLBACK 1
#endif

#ifdef DEBUG_I830FALLBACK
#define I830FALLBACK(s, arg...)				\
do {							\
	DPRINTF(PFX, "EXA fallback: " s "\n", ##arg); 	\
	return FALSE;					\
} while(0)
#else
#define I830FALLBACK(s, arg...) 			\
do { 							\
	return FALSE;					\
} while(0)
#endif

extern float scale_units[2][2];
extern Bool is_transform[2];
extern PictTransform *transform[2];
static CARD32 mapstate[6];
static CARD32 samplerstate[6];

struct formatinfo {
    int fmt;
    CARD32 card_fmt;
};

struct blendinfo {
    Bool dst_alpha;
    Bool src_alpha;
    CARD32 src_blend;
    CARD32 dst_blend;
};

extern Bool
I915EXACheckComposite(int op, PicturePtr pSrcPicture, PicturePtr pMaskPicture,
		      PicturePtr pDstPicture);

extern Bool
I915EXAPrepareComposite(int op, PicturePtr pSrcPicture,
			PicturePtr pMaskPicture, PicturePtr pDstPicture,
			PixmapPtr pSrc, PixmapPtr pMask, PixmapPtr pDst);

static struct blendinfo I915BlendOp[] = {
    /* Clear */
    {0, 0, BLENDFACT_ZERO,          BLENDFACT_ZERO},
    /* Src */
    {0, 0, BLENDFACT_ONE,           BLENDFACT_ZERO},
    /* Dst */
    {0, 0, BLENDFACT_ZERO,          BLENDFACT_ONE},
    /* Over */
    {0, 1, BLENDFACT_ONE,           BLENDFACT_INV_SRC_ALPHA},
    /* OverReverse */
    {1, 0, BLENDFACT_INV_DST_ALPHA, BLENDFACT_ONE},
    /* In */
    {1, 0, BLENDFACT_DST_ALPHA,     BLENDFACT_ZERO},
    /* InReverse */
    {0, 1, BLENDFACT_ZERO,          BLENDFACT_SRC_ALPHA},
    /* Out */
    {1, 0, BLENDFACT_INV_DST_ALPHA, BLENDFACT_ZERO},
    /* OutReverse */
    {0, 1, BLENDFACT_ZERO,          BLENDFACT_INV_SRC_ALPHA},
    /* Atop */
    {1, 1, BLENDFACT_DST_ALPHA,     BLENDFACT_INV_SRC_ALPHA},
    /* AtopReverse */
    {1, 1, BLENDFACT_INV_DST_ALPHA, BLENDFACT_SRC_ALPHA},
    /* Xor */
    {1, 1, BLENDFACT_INV_DST_ALPHA, BLENDFACT_INV_SRC_ALPHA},
    /* Add */
    {0, 0, BLENDFACT_ONE,           BLENDFACT_ONE},
};

static struct formatinfo I915TexFormats[] = {
        {PICT_a8r8g8b8, MAPSURF_32BIT | MT_32BIT_ARGB8888 },
        {PICT_x8r8g8b8, MAPSURF_32BIT | MT_32BIT_XRGB8888 },
        {PICT_a8b8g8r8, MAPSURF_32BIT | MT_32BIT_ABGR8888 },
        {PICT_x8b8g8r8, MAPSURF_32BIT | MT_32BIT_XBGR8888 },
        {PICT_r5g6b5,   MAPSURF_16BIT | MT_16BIT_RGB565   },
        {PICT_a1r5g5b5, MAPSURF_16BIT | MT_16BIT_ARGB1555 },
        {PICT_x1r5g5b5, MAPSURF_16BIT | MT_16BIT_ARGB1555 },
        {PICT_a8,       MAPSURF_8BIT | MT_8BIT_A8 	  },
};

static CARD32 I915GetBlendCntl(int op, PicturePtr pMask, CARD32 dst_format)
{
    CARD32 sblend, dblend;

    sblend = I915BlendOp[op].src_blend;
    dblend = I915BlendOp[op].dst_blend;

    /* If there's no dst alpha channel, adjust the blend op so that we'll treat
     * it as always 1.
     */
    if (PICT_FORMAT_A(dst_format) == 0 && I915BlendOp[op].dst_alpha) {
        if (sblend == BLENDFACT_DST_ALPHA)
            sblend = BLENDFACT_ONE;
        else if (sblend == BLENDFACT_INV_DST_ALPHA)
            sblend = BLENDFACT_ZERO;
    }

    /* If the source alpha is being used, then we should only be in a case where
     * the source blend factor is 0, and the source blend value is the mask
     * channels multiplied by the source picture's alpha.
     */
    if (pMask && pMask->componentAlpha && I915BlendOp[op].src_alpha) {
        if (dblend == BLENDFACT_SRC_ALPHA) {
	    dblend = BLENDFACT_SRC_COLR;
        } else if (dblend == BLENDFACT_INV_SRC_ALPHA) {
	    dblend = BLENDFACT_INV_SRC_COLR;
        }
    }

    return (sblend << S6_CBUF_SRC_BLEND_FACT_SHIFT) |
	(dblend << S6_CBUF_DST_BLEND_FACT_SHIFT);
}

static Bool I915GetDestFormat(PicturePtr pDstPicture, CARD32 *dst_format)
{
    switch (pDstPicture->format) {
    case PICT_a8r8g8b8:
    case PICT_x8r8g8b8:
        *dst_format = COLR_BUF_ARGB8888;
        break;
    case PICT_r5g6b5:
        *dst_format = COLR_BUF_RGB565;
        break;
    case PICT_a1r5g5b5:
    case PICT_x1r5g5b5:
        *dst_format = COLR_BUF_ARGB1555;
        break;
    /* COLR_BUF_8BIT is special for YUV surfaces.  While we may end up being
     * able to use it depending on how the hardware implements it, disable it
     * for now while we don't know what exactly it does (what channel does it
     * read from?
     */
    /*
    case PICT_a8:
        *dst_format = COLR_BUF_8BIT;
        break;
    */
    case PICT_a4r4g4b4:
    case PICT_x4r4g4b4:
	*dst_format = COLR_BUF_ARGB4444;
	break;
    default:
        I830FALLBACK("Unsupported dest format 0x%x\n",
                        (int)pDstPicture->format);
    }

    return TRUE;
}

static Bool I915CheckCompositeTexture(PicturePtr pPict, int unit)
{
    int w = pPict->pDrawable->width;
    int h = pPict->pDrawable->height;
    int i;

    if ((w > 0x7ff) || (h > 0x7ff))
        I830FALLBACK("Picture w/h too large (%dx%d)\n", w, h);

    for (i = 0; i < sizeof(I915TexFormats) / sizeof(I915TexFormats[0]); i++)
    {
        if (I915TexFormats[i].fmt == pPict->format)
            break;
    }
    if (i == sizeof(I915TexFormats) / sizeof(I915TexFormats[0]))
        I830FALLBACK("Unsupported picture format 0x%x\n",
                         (int)pPict->format);

    if (pPict->repeat && pPict->repeatType != RepeatNormal)
	I830FALLBACK("extended repeat (%d) not supported\n",
		     pPict->repeatType);

    if (pPict->filter != PictFilterNearest &&
        pPict->filter != PictFilterBilinear)
        I830FALLBACK("Unsupported filter 0x%x\n", pPict->filter);

    return TRUE;
}

Bool
I915EXACheckComposite(int op, PicturePtr pSrcPicture, PicturePtr pMaskPicture,
		      PicturePtr pDstPicture)
{
    CARD32 tmp1;

    /* Check for unsupported compositing operations. */
    if (op >= sizeof(I915BlendOp) / sizeof(I915BlendOp[0]))
        I830FALLBACK("Unsupported Composite op 0x%x\n", op);
    if (pMaskPicture != NULL && pMaskPicture->componentAlpha) {
        /* Check if it's component alpha that relies on a source alpha and on
         * the source value.  We can only get one of those into the single
         * source value that we get to blend with.
         */
        if (I915BlendOp[op].src_alpha &&
            (I915BlendOp[op].src_blend != BLENDFACT_ZERO))
            	I830FALLBACK("Component alpha not supported with source "
                            "alpha and source value blending.\n");
    }

    if (!I915CheckCompositeTexture(pSrcPicture, 0))
        I830FALLBACK("Check Src picture texture\n");
    if (pMaskPicture != NULL && !I915CheckCompositeTexture(pMaskPicture, 1))
        I830FALLBACK("Check Mask picture texture\n");

    if (!I915GetDestFormat(pDstPicture, &tmp1))
	I830FALLBACK("Get Color buffer format\n");

    return TRUE;
}

static Bool
I915TextureSetup(PicturePtr pPict, PixmapPtr pPix, int unit)
{
    ScrnInfoPtr pScrn = xf86Screens[pPict->pDrawable->pScreen->myNum];
    I830Ptr pI830 = I830PTR(pScrn);
    CARD32 format, offset, pitch, filter;
    int w, h, i;
    CARD32 wrap_mode = TEXCOORDMODE_CLAMP_BORDER;

    offset = exaGetPixmapOffset(pPix);
    pitch = exaGetPixmapPitch(pPix);
    w = pPict->pDrawable->width;
    h = pPict->pDrawable->height;
    scale_units[unit][0] = pPix->drawable.width;
    scale_units[unit][1] = pPix->drawable.height;

    for (i = 0; i < sizeof(I915TexFormats) / sizeof(I915TexFormats[0]); i++) {
        if (I915TexFormats[i].fmt == pPict->format)
	    break;
    }
    if (i == sizeof(I915TexFormats)/ sizeof(I915TexFormats[0]))
	I830FALLBACK("unknown texture format\n");
    format = I915TexFormats[i].card_fmt;

    if (pPict->repeat)
	wrap_mode = TEXCOORDMODE_WRAP;

    switch (pPict->filter) {
    case PictFilterNearest:
        filter = (FILTER_NEAREST << SS2_MAG_FILTER_SHIFT) |
			(FILTER_NEAREST << SS2_MIN_FILTER_SHIFT);
        break;
    case PictFilterBilinear:
        filter = (FILTER_LINEAR << SS2_MAG_FILTER_SHIFT) |
			(FILTER_LINEAR << SS2_MIN_FILTER_SHIFT);
        break;
    default:
	filter = 0;
        I830FALLBACK("Bad filter 0x%x\n", pPict->filter);
    }

    mapstate[unit * 3 + 0] = offset;
    mapstate[unit * 3 + 1] = format |
	((pPix->drawable.height - 1) << MS3_HEIGHT_SHIFT) |
	((pPix->drawable.width - 1) << MS3_WIDTH_SHIFT);
    if (!pI830->disableTiling)
	samplerstate[unit * 3 + 1] |= MS3_USE_FENCE_REGS;
    mapstate[unit * 3 + 2] = ((pitch / 4) - 1) << MS4_PITCH_SHIFT;

    samplerstate[unit * 3 + 0] = (MIPFILTER_NONE << SS2_MIP_FILTER_SHIFT);
    samplerstate[unit * 3 + 0] |= filter;
    samplerstate[unit * 3 + 1] = SS3_NORMALIZED_COORDS;
    samplerstate[unit * 3 + 1] |= wrap_mode << SS3_TCX_ADDR_MODE_SHIFT;
    samplerstate[unit * 3 + 1] |= wrap_mode << SS3_TCY_ADDR_MODE_SHIFT;
    samplerstate[unit * 3 + 1] |= unit << SS3_TEXTUREMAP_INDEX_SHIFT;
    samplerstate[unit * 3 + 2] = 0x00000000; /* border color */

    if (pPict->transform != 0) {
        is_transform[unit] = TRUE;
        transform[unit] = pPict->transform;
    } else {
        is_transform[unit] = FALSE;
    }

    return TRUE;
}

Bool
I915EXAPrepareComposite(int op, PicturePtr pSrcPicture,
			PicturePtr pMaskPicture, PicturePtr pDstPicture,
			PixmapPtr pSrc, PixmapPtr pMask, PixmapPtr pDst)
{
    ScrnInfoPtr pScrn = xf86Screens[pSrcPicture->pDrawable->pScreen->myNum];
    I830Ptr pI830 = I830PTR(pScrn);
    CARD32 dst_format, dst_offset, dst_pitch;
    CARD32 blendctl;

#ifdef I830DEBUG
    ErrorF("Enter i915 prepareComposite\n");
#endif

    pI830->last_3d = LAST_3D_RENDER;

    I915GetDestFormat(pDstPicture, &dst_format);
    dst_offset = exaGetPixmapOffset(pDst);
    dst_pitch = exaGetPixmapPitch(pDst);
    FS_LOCALS(20);

    if (!I915TextureSetup(pSrcPicture, pSrc, 0))
	I830FALLBACK("fail to setup src texture\n");
    if (pMask != NULL) {
	if (!I915TextureSetup(pMaskPicture, pMask, 1))
		I830FALLBACK("fail to setup mask texture\n");
    } else {
	is_transform[1] = FALSE;
	scale_units[1][0] = -1;
	scale_units[1][1] = -1;
    }

    if (pMask == NULL) {
	BEGIN_LP_RING(10);
	OUT_RING(_3DSTATE_MAP_STATE | 3);
	OUT_RING(0x00000001); /* map 0 */
	OUT_RING(mapstate[0]);
	OUT_RING(mapstate[1]);
	OUT_RING(mapstate[2]);

	OUT_RING(_3DSTATE_SAMPLER_STATE | 3);
	OUT_RING(0x00000001); /* sampler 0 */
	OUT_RING(samplerstate[0]);
	OUT_RING(samplerstate[1]);
	OUT_RING(samplerstate[2]);
	ADVANCE_LP_RING();
    } else {
	BEGIN_LP_RING(16);
	OUT_RING(_3DSTATE_MAP_STATE | 6);
	OUT_RING(0x00000003); /* map 0,1 */
	OUT_RING(mapstate[0]);
	OUT_RING(mapstate[1]);
	OUT_RING(mapstate[2]);
	OUT_RING(mapstate[3]);
	OUT_RING(mapstate[4]);
	OUT_RING(mapstate[5]);

	OUT_RING(_3DSTATE_SAMPLER_STATE | 6);
	OUT_RING(0x00000003); /* sampler 0,1 */
	OUT_RING(samplerstate[0]);
	OUT_RING(samplerstate[1]);
	OUT_RING(samplerstate[2]);
	OUT_RING(samplerstate[3]);
	OUT_RING(samplerstate[4]);
	OUT_RING(samplerstate[5]);
	ADVANCE_LP_RING();
    }
    {
	CARD32 ss2;

	BEGIN_LP_RING(18);
	/* color buffer
	 * XXX: Need to add USE_FENCE if we ever tile the X Server's pixmaps or
	 * visible screen.
	 */
	OUT_RING(_3DSTATE_BUF_INFO_CMD);
	OUT_RING(BUF_3D_ID_COLOR_BACK| BUF_3D_PITCH(dst_pitch));
	OUT_RING(BUF_3D_ADDR(dst_offset));

	OUT_RING(_3DSTATE_DST_BUF_VARS_CMD);
	OUT_RING(dst_format);

	OUT_RING(_3DSTATE_LOAD_STATE_IMMEDIATE_1 | I1_LOAD_S(2) |
		 I1_LOAD_S(4) | I1_LOAD_S(5) | I1_LOAD_S(6) | 3);
	ss2 = S2_TEXCOORD_FMT(0, TEXCOORDFMT_2D);
	if (pMask)
		ss2 |= S2_TEXCOORD_FMT(1, TEXCOORDFMT_2D);
	else
		ss2 |= S2_TEXCOORD_FMT(1, TEXCOORDFMT_NOT_PRESENT);
	ss2 |= S2_TEXCOORD_FMT(2, TEXCOORDFMT_NOT_PRESENT);
	ss2 |= S2_TEXCOORD_FMT(3, TEXCOORDFMT_NOT_PRESENT);
	ss2 |= S2_TEXCOORD_FMT(4, TEXCOORDFMT_NOT_PRESENT);
	ss2 |= S2_TEXCOORD_FMT(5, TEXCOORDFMT_NOT_PRESENT);
	ss2 |= S2_TEXCOORD_FMT(6, TEXCOORDFMT_NOT_PRESENT);
	ss2 |= S2_TEXCOORD_FMT(7, TEXCOORDFMT_NOT_PRESENT);
	OUT_RING(ss2);
	OUT_RING((1 << S4_POINT_WIDTH_SHIFT) | S4_LINE_WIDTH_ONE |
		 S4_CULLMODE_NONE| S4_VFMT_XY);
	blendctl = I915GetBlendCntl(op, pMaskPicture, pDstPicture->format);
	OUT_RING(0x00000000); /* Disable stencil buffer */
	OUT_RING(S6_CBUF_BLEND_ENABLE | S6_COLOR_WRITE_ENABLE |
		 (BLENDFUNC_ADD << S6_CBUF_BLEND_FUNC_SHIFT) | blendctl);

	/* issue a flush */
	OUT_RING(MI_FLUSH | MI_WRITE_DIRTY_STATE | MI_INVALIDATE_MAP_CACHE);
	OUT_RING(MI_NOOP);

	/* draw rect is unconditional */
	OUT_RING(_3DSTATE_DRAW_RECT_CMD);
	OUT_RING(0x00000000);
	OUT_RING(0x00000000);  /* ymin, xmin*/
	OUT_RING(DRAW_YMAX(pDst->drawable.height - 1) |
		 DRAW_XMAX(pDst->drawable.width - 1));
	OUT_RING(0x00000000);  /* yorig, xorig (relate to color buffer?)*/
	OUT_RING(MI_NOOP);
	ADVANCE_LP_RING();
    }

    FS_BEGIN();

    /* Declare the registers necessary for our program.  I don't think the
     * S then T ordering is necessary.
     */
    i915_fs_dcl(FS_S0);
    if (pMask)
	i915_fs_dcl(FS_S1);
    i915_fs_dcl(FS_T0);
    if (pMask)
	i915_fs_dcl(FS_T1);

    /* Load the pSrcPicture texel */
    i915_fs_texld(FS_R0, FS_S0, FS_T0);
    /* If the texture lacks an alpha channel, force the alpha to 1. */
    if (PICT_FORMAT_A(pSrcPicture->format) == 0)
	i915_fs_mov_masked(FS_R0, MASK_W, i915_fs_operand_one());

    if (!pMask) {
	/* No mask, so move to output color */
	i915_fs_mov(FS_OC, i915_fs_operand_reg(FS_R0));
    } else {
	/* Load the pMaskPicture texel */
	i915_fs_texld(FS_R1, FS_S1, FS_T1);
	/* If the texture lacks an alpha channel, force the alpha to 1. */
	if (PICT_FORMAT_A(pMaskPicture->format) == 0)
	    i915_fs_mov_masked(FS_R1, MASK_W, i915_fs_operand_one());

	/* If component alpha is set in the mask and the blend operation
	 * uses the source alpha, then we know we don't need the source
	 * value (otherwise we would have hit a fallback earlier), so we
	 * provide the source alpha (src.A * mask.X) as output color.
	 * Conversely, if CA is set and we don't need the source alpha, then
	 * we produce the source value (src.X * mask.X) and the source alpha
	 * is unused..  Otherwise, we provide the non-CA source value
	 * (src.X * mask.A).
	 */
	if (pMaskPicture->componentAlpha) {
	    if (I915BlendOp[op].src_alpha) {
		i915_fs_mul(FS_OC, i915_fs_operand(FS_R0, W, W, W, W),
			    i915_fs_operand_reg(FS_R1));
	    } else {
		i915_fs_mul(FS_OC, i915_fs_operand_reg(FS_R0),
			    i915_fs_operand_reg(FS_R1));
	    }
	} else {
	    i915_fs_mul(FS_OC, i915_fs_operand_reg(FS_R0),
			i915_fs_operand(FS_R1, W, W, W, W));
	}
    }
    FS_END();

    return TRUE;
}
/***************************************************************************

 Copyright 2000 Intel Corporation.  All Rights Reserved. 

 Permission is hereby granted, free of charge, to any person obtaining a 
 copy of this software and associated documentation files (the 
 "Software"), to deal in the Software without restriction, including 
 without limitation the rights to use, copy, modify, merge, publish, 
 distribute, sub license, and/or sell copies of the Software, and to 
 permit persons to whom the Software is furnished to do so, subject to 
 the following conditions: 

 The above copyright notice and this permission notice (including the 
 next paragraph) shall be included in all copies or substantial portions 
 of the Software. 

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS 
 OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF 
 MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. 
 IN NO EVENT SHALL INTEL, AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, 
 DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR 
 OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR 
 THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 **************************************************************************/
/* $XFree86: xc/programs/Xserver/hw/xfree86/drivers/i810/i830_video.c,v 1.11tsi Exp $ */

/*
 * i830_video.c: i830/i845 Xv driver. 
 *
 * Copyright © 2002 by Alan Hourihane and David Dawes
 *
 * Authors: 
 *	Alan Hourihane <alanh@tungstengraphics.com>
 *	David Dawes <dawes@xfree86.org>
 *
 * Derived from i810 Xv driver:
 *
 * Authors of i810 code:
 * 	Jonathan Bian <jonathan.bian@intel.com>
 *      Offscreen Images:
 *        Matt Sottek <matthew.j.sottek@intel.com>
 */

/*
 * XXX Could support more formats.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <inttypes.h>
#include <math.h>
#include <string.h>
#include <assert.h>

#include "xf86.h"
#include "xf86_OSproc.h"
#include "xf86Resources.h"
#include "compiler.h"
#include "xf86PciInfo.h"
#include "xf86Pci.h"
#include "xf86fbman.h"
#include "regionstr.h"
#include "randrstr.h"
#include "windowstr.h"
#include "damage.h"
#include "i830.h"
#include "i830_video.h"
#include "xf86xv.h"
#include <X11/extensions/Xv.h>
#include "xaa.h"
#include "xaalocal.h"
#include "dixstruct.h"
#include "fourcc.h"

#ifdef INTEL_XVMC
#define _INTEL_XVMC_SERVER_
#include "i830_hwmc.h"
#include "i915_hwmc.h"
#endif

#ifndef USE_USLEEP_FOR_VIDEO
#define USE_USLEEP_FOR_VIDEO 0
#endif

#define OFF_DELAY 	250		/* milliseconds */
#define FREE_DELAY 	15000

#define OFF_TIMER 	0x01
#define FREE_TIMER	0x02
#define CLIENT_VIDEO_ON	0x04

#define TIMER_MASK      (OFF_TIMER | FREE_TIMER)

static void vga_sync_fields(I830Ptr);
static void I830InitOffscreenImages(ScreenPtr);

static XF86VideoAdaptorPtr I830SetupImageVideoOverlay(ScreenPtr);
static XF86VideoAdaptorPtr I830SetupImageVideoTextured(ScreenPtr);
static void I830StopVideo(ScrnInfoPtr, pointer, Bool);
static int I830SetPortAttribute(ScrnInfoPtr, Atom, INT32, pointer);
static int I830SetPortAttributeTextured(ScrnInfoPtr, Atom, INT32, pointer);
static int I830GetPortAttribute(ScrnInfoPtr, Atom, INT32 *, pointer);
static void I830QueryBestSize(ScrnInfoPtr, Bool,
			      short, short, short, short, unsigned int *,
			      unsigned int *, pointer);
static int I830PutImage(ScrnInfoPtr, short, short, short, short, short, short,
			short, short, int, unsigned char *, short, short,
			Bool, RegionPtr, pointer, DrawablePtr);
static int I830QueryImageAttributesOverlay(ScrnInfoPtr, int, unsigned short *,
					   unsigned short *, int *, int *);
static int I830QueryImageAttributesTextured(ScrnInfoPtr, int, unsigned short *,
					    unsigned short *, int *, int *);

#define MAKE_ATOM(a) MakeAtom(a, sizeof(a) - 1, TRUE)

static Atom xvBrightness, xvContrast, xvSaturation, xvColorKey, xvPipe, xvDoubleBuffer;
static Atom xvGamma0, xvGamma1, xvGamma2, xvGamma3, xvGamma4, xvGamma5;

/* Limits for the overlay/textured video source sizes.  The documented hardware
 * limits are 2048x2048 or better for overlay and both of our textured video
 * implementations.  However, we run into the bigrequests limit of (currently)
 * 4MB, which even the planar format's 2048*2048*1.5 bytes is larger than.
 * Conveniently, the HD resolution, even in packed format, takes
 * (1920*1088*2) bytes, which is just shy of 4MB.  Additionally, on the 830
 * and 845, larger sizes resulted in the card hanging, so we keep the limits
 * lower there.
 *
 * While the HD resolution is actually 1920x1080, we increase our advertised
 * size to 1088 because some software wants to send an image aligned to
 * 16-pixel boundaries.
 */
#define IMAGE_MAX_WIDTH		1920
#define IMAGE_MAX_HEIGHT	1088
#define IMAGE_MAX_WIDTH_LEGACY	1024
#define IMAGE_MAX_HEIGHT_LEGACY	1088

/* overlay debugging printf function */
#if 0
#define OVERLAY_DEBUG ErrorF
#else
#define OVERLAY_DEBUG if (0) ErrorF
#endif

/* Oops, I never exported this function in EXA.  I meant to. */
#ifndef exaMoveInPixmap
void exaMoveInPixmap (PixmapPtr pPixmap);
#endif

/*
 * OCMD - Overlay Command Register
 */
#define OCMD_REGISTER		0x30168
#define MIRROR_MODE		(0x3<<17)
#define MIRROR_HORIZONTAL	(0x1<<17)
#define MIRROR_VERTICAL		(0x2<<17)
#define MIRROR_BOTH		(0x3<<17)
#define OV_BYTE_ORDER		(0x3<<14)
#define UV_SWAP			(0x1<<14)
#define Y_SWAP			(0x2<<14)
#define Y_AND_UV_SWAP		(0x3<<14)
#define SOURCE_FORMAT		(0xf<<10)
#define RGB_888			(0x1<<10)
#define	RGB_555			(0x2<<10)
#define	RGB_565			(0x3<<10)
#define	YUV_422			(0x8<<10)
#define	YUV_411			(0x9<<10)
#define	YUV_420			(0xc<<10)
#define	YUV_422_PLANAR		(0xd<<10)
#define	YUV_410			(0xe<<10)
#define TVSYNC_FLIP_PARITY	(0x1<<9)
#define TVSYNC_FLIP_ENABLE	(0x1<<7)
#define BUF_TYPE		(0x1<<5)
#define BUF_TYPE_FRAME		(0x0<<5)
#define BUF_TYPE_FIELD		(0x1<<5)
#define TEST_MODE		(0x1<<4)
#define BUFFER_SELECT		(0x3<<2)
#define BUFFER0			(0x0<<2)
#define BUFFER1			(0x1<<2)
#define FIELD_SELECT		(0x1<<1)
#define FIELD0			(0x0<<1)
#define FIELD1			(0x1<<1)
#define OVERLAY_ENABLE		0x1

#define OFC_UPDATE		0x1

/* OCONFIG register */
#define CC_OUT_8BIT		(0x1<<3)
#define OVERLAY_PIPE_MASK	(0x1<<18)		
#define OVERLAY_PIPE_A		(0x0<<18)		
#define OVERLAY_PIPE_B		(0x1<<18)		
#define GAMMA2_ENBL		(0x1<<16)
#define CSC_MODE_BT709		(0x1<<5)
#define CSC_MODE_BT601		(0x0<<5)
#define THREE_LINE_BUFFERS	(0x1<<0)
#define TWO_LINE_BUFFERS	(0x0<<0)

/* DCLRKM register */
#define DEST_KEY_ENABLE		(0x1<<31)

/* Polyphase filter coefficients */
#define N_HORIZ_Y_TAPS		5
#define N_VERT_Y_TAPS		3
#define N_HORIZ_UV_TAPS		3
#define N_VERT_UV_TAPS		3
#define N_PHASES		17
#define MAX_TAPS		5

/* Filter cutoff frequency limits. */
#define MIN_CUTOFF_FREQ		1.0
#define MAX_CUTOFF_FREQ		3.0

#define RGB16ToColorKey(c) \
(((c & 0xF800) << 8) | ((c & 0x07E0) << 5) | ((c & 0x001F) << 3))

#define RGB15ToColorKey(c) \
(((c & 0x7c00) << 9) | ((c & 0x03E0) << 6) | ((c & 0x001F) << 3))

/* client libraries expect an encoding */
static XF86VideoEncodingRec DummyEncoding[1] = {
    {
	0,
	"XV_IMAGE",
	IMAGE_MAX_WIDTH, IMAGE_MAX_HEIGHT,
	{1, 1}
    }
};

#define NUM_FORMATS 3

static XF86VideoFormatRec Formats[NUM_FORMATS] = {
    {15, TrueColor}, {16, TrueColor}, {24, TrueColor}
};

#define CLONE_ATTRIBUTES 1
static XF86AttributeRec CloneAttributes[CLONE_ATTRIBUTES] = {
    {XvSettable | XvGettable, -1, 1, "XV_PIPE"}
};

#define NUM_ATTRIBUTES 5
static XF86AttributeRec Attributes[NUM_ATTRIBUTES] = {
    {XvSettable | XvGettable, 0, (1 << 24) - 1, "XV_COLORKEY"},
    {XvSettable | XvGettable, -128, 127, "XV_BRIGHTNESS"},
    {XvSettable | XvGettable, 0, 255, "XV_CONTRAST"},
    {XvSettable | XvGettable, 0, 1023, "XV_SATURATION"},
    {XvSettable | XvGettable, 0, 1, "XV_DOUBLE_BUFFER"}
};

#define NUM_TEXTURED_ATTRIBUTES 2
static XF86AttributeRec TexturedAttributes[NUM_ATTRIBUTES] = {
    {XvSettable | XvGettable, -128, 127, "XV_BRIGHTNESS"},
    {XvSettable | XvGettable, 0, 255, "XV_CONTRAST"},
};

#define GAMMA_ATTRIBUTES 6
static XF86AttributeRec GammaAttributes[GAMMA_ATTRIBUTES] = {
    {XvSettable | XvGettable, 0, 0xffffff, "XV_GAMMA0"},
    {XvSettable | XvGettable, 0, 0xffffff, "XV_GAMMA1"},
    {XvSettable | XvGettable, 0, 0xffffff, "XV_GAMMA2"},
    {XvSettable | XvGettable, 0, 0xffffff, "XV_GAMMA3"},
    {XvSettable | XvGettable, 0, 0xffffff, "XV_GAMMA4"},
    {XvSettable | XvGettable, 0, 0xffffff, "XV_GAMMA5"}
};

#define NUM_IMAGES 5

static XF86ImageRec Images[NUM_IMAGES] = {
    XVIMAGE_YUY2,
    XVIMAGE_YV12,
    XVIMAGE_I420,
    XVIMAGE_UYVY,
#ifdef INTEL_XVMC
    {
        /*
         * Below, a dummy picture type that is used in XvPutImage only to do
         * an overlay update. Introduced for the XvMC client lib.
         * Defined to have a zero data size.
         */
        FOURCC_XVMC,
        XvYUV,
        LSBFirst,
        {'X', 'V', 'M', 'C',
         0x00, 0x00, 0x00, 0x10, 0x80, 0x00, 0x00, 0xAA, 0x00,
         0x38, 0x9B, 0x71},
        12,
        XvPlanar,
        3,
        0, 0, 0, 0,
        8, 8, 8,
        1, 2, 2,
        1, 2, 2,
        {'Y', 'V', 'U',
         0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
         0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        XvTopToBottom
    },
#endif
};

typedef struct {
    uint32_t OBUF_0Y;
    uint32_t OBUF_1Y;
    uint32_t OBUF_0U;
    uint32_t OBUF_0V;
    uint32_t OBUF_1U;
    uint32_t OBUF_1V;
    uint32_t OSTRIDE;
    uint32_t YRGB_VPH;
    uint32_t UV_VPH;
    uint32_t HORZ_PH;
    uint32_t INIT_PHS;
    uint32_t DWINPOS;
    uint32_t DWINSZ;
    uint32_t SWIDTH;
    uint32_t SWIDTHSW;
    uint32_t SHEIGHT;
    uint32_t YRGBSCALE;
    uint32_t UVSCALE;
    uint32_t OCLRC0;
    uint32_t OCLRC1;
    uint32_t DCLRKV;
    uint32_t DCLRKM;
    uint32_t SCLRKVH;
    uint32_t SCLRKVL;
    uint32_t SCLRKEN;
    uint32_t OCONFIG;
    uint32_t OCMD;
    uint32_t RESERVED1;			/* 0x6C */
    uint32_t OSTART_0Y; 		/* for i965 */
    uint32_t OSTART_1Y;		/* for i965 */
    uint32_t OSTART_0U;
    uint32_t OSTART_0V;
    uint32_t OSTART_1U;
    uint32_t OSTART_1V;
    uint32_t OTILEOFF_0Y;
    uint32_t OTILEOFF_1Y;
    uint32_t OTILEOFF_0U;
    uint32_t OTILEOFF_0V;
    uint32_t OTILEOFF_1U;
    uint32_t OTILEOFF_1V;
    uint32_t FASTHSCALE;			/* 0xA0 */
    uint32_t UVSCALEV;			/* 0xA4 */

    uint32_t RESERVEDC[(0x200 - 0xA8) / 4];		   /* 0xA8 - 0x1FC */
    uint16_t Y_VCOEFS[N_VERT_Y_TAPS * N_PHASES];		   /* 0x200 */
    uint16_t RESERVEDD[0x100 / 2 - N_VERT_Y_TAPS * N_PHASES];
    uint16_t Y_HCOEFS[N_HORIZ_Y_TAPS * N_PHASES];		   /* 0x300 */
    uint16_t RESERVEDE[0x200 / 2 - N_HORIZ_Y_TAPS * N_PHASES];
    uint16_t UV_VCOEFS[N_VERT_UV_TAPS * N_PHASES];		   /* 0x500 */
    uint16_t RESERVEDF[0x100 / 2 - N_VERT_UV_TAPS * N_PHASES];
    uint16_t UV_HCOEFS[N_HORIZ_UV_TAPS * N_PHASES];	   /* 0x600 */
    uint16_t RESERVEDG[0x100 / 2 - N_HORIZ_UV_TAPS * N_PHASES];
} I830OverlayRegRec, *I830OverlayRegPtr;

#define I830OVERLAYREG(pI830) ((I830OverlayRegPtr)\
			       ((pI830)->FbBase + \
				(pI830)->overlay_regs->offset))
#if VIDEO_DEBUG
static void
CompareOverlay(I830Ptr pI830, uint32_t * overlay, int size)
{
    int i;
    uint32_t val;
    int bad = 0;

    for (i = 0; i < size; i += 4) {
	val = INREG(0x30100 + i);
	if (val != overlay[i / 4]) {
	    OVERLAY_DEBUG("0x%05x value doesn't match (0x%lx != 0x%lx)\n",
			  0x30100 + i, val, overlay[i / 4]);
	    bad++;
	}
    }
    if (!bad)
	OVERLAY_DEBUG("CompareOverlay: no differences\n");
}
#endif

static void
I830SetOneLineModeRatio(ScrnInfoPtr pScrn);

static void
i830_overlay_switch_to_crtc (ScrnInfoPtr pScrn, xf86CrtcPtr crtc)
{
    I830Ptr		pI830 = I830PTR(pScrn);
    I830PortPrivPtr	pPriv = GET_PORT_PRIVATE(pScrn);
    I830CrtcPrivatePtr  intel_crtc = crtc->driver_private;
    int			pipeconf_reg = intel_crtc->pipe == 0 ? PIPEACONF : PIPEBCONF;

    /* overlay can't be used on pipe with double wide, and pipe must be enabled. */
    if ((!IS_I965G(pI830) && (INREG(pipeconf_reg) & PIPEACONF_DOUBLE_WIDE))
	    || (intel_crtc->dpms_mode == DPMSModeOff))
	pPriv->overlayOK = FALSE;
    else
	pPriv->overlayOK = TRUE;

    if (!pPriv->overlayOK)
	return;

    /* Check we have an LFP connected */
    if (i830PipeHasType(crtc, I830_OUTPUT_LVDS)) 
    {

	int	vtotal_reg = intel_crtc->pipe == 0 ? VTOTAL_A : VTOTAL_B;
	uint32_t size = intel_crtc->pipe ? INREG(PIPEBSRC) : INREG(PIPEASRC);
	uint32_t active;
	uint32_t hsize, vsize;

	hsize = (size >> 16) & 0x7FF;
	vsize = size & 0x7FF;
	active = INREG(vtotal_reg) & 0x7FF;

	if (vsize < active && hsize > 1024)
	    I830SetOneLineModeRatio(pScrn);

	if (pPriv->scaleRatio & 0xFFFE0000) 
	{
	    /* Possible bogus ratio, using in-accurate fallback */
	    xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		       "Bogus panel fit register, Xvideo positioning may not "
		       "be accurate.\n");
	    xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		       "Using fallback ratio - was 0x%x, now 0x%x\n",
		       pPriv->scaleRatio,
		       (int)(((float)active * 65536)/(float)vsize));

	    pPriv->scaleRatio = (int)(((float)active * 65536) / (float)vsize);
	}
    }
}

/*
 * This is more or less the correct way to initalise, update, and shut down
 * the overlay.
 *
 * XXX Need to make sure that the overlay engine is cleanly shutdown in
 * all modes of server exit.
 */

static void
i830_overlay_on(ScrnInfoPtr pScrn)
{
    I830Ptr		pI830 = I830PTR(pScrn);
    I830OverlayRegPtr	overlay = I830OVERLAYREG(pI830);
    I830PortPrivPtr	pPriv = pI830->adaptor->pPortPrivates[0].ptr;
    Bool		deactivate = FALSE;
    
    if (*pI830->overlayOn)
	return;

    /*
     * On I830, if pipe A is off when the overlayis enabled, it will fail to
     * turn on and blank the entire screen or lock up the ring. Light up pipe
     * A in this case to provide a clock for the overlay hardware
     */
    if (pPriv->current_crtc && i830_crtc_pipe (pPriv->current_crtc) != 0)
	deactivate = i830_pipe_a_require_activate (pScrn);

    overlay->OCMD &= ~OVERLAY_ENABLE;
    BEGIN_BATCH(6);
    OUT_BATCH(MI_FLUSH | MI_WRITE_DIRTY_STATE);
    OUT_BATCH(MI_NOOP);
    OUT_BATCH(MI_OVERLAY_FLIP | MI_OVERLAY_FLIP_ON);
    if (OVERLAY_NOPHYSICAL(pI830))
	OUT_BATCH(pI830->overlay_regs->offset | OFC_UPDATE);
    else
	OUT_BATCH(pI830->overlay_regs->bus_addr | OFC_UPDATE);
    /* Wait for the overlay to light up before attempting to use it */
    OUT_BATCH(MI_WAIT_FOR_EVENT | MI_WAIT_FOR_OVERLAY_FLIP);
    OUT_BATCH(MI_NOOP);
    ADVANCE_BATCH();
    i830WaitSync(pScrn);
    
    /*
     * If we turned pipe A on up above, turn it
     * back off
     */
    if (deactivate)
	i830_pipe_a_require_deactivate (pScrn);

    OVERLAY_DEBUG("overlay_on\n");
    *pI830->overlayOn = TRUE;

    overlay->OCMD |= OVERLAY_ENABLE;
}

static void
i830_overlay_continue(ScrnInfoPtr pScrn, Bool update_filter)
{
    I830Ptr		pI830 = I830PTR(pScrn);
    uint32_t		flip_addr;
    I830OverlayRegPtr	overlay = I830OVERLAYREG(pI830);

    if (!*pI830->overlayOn)
	return;

    if (OVERLAY_NOPHYSICAL(pI830))
	flip_addr = pI830->overlay_regs->offset;
    else
	flip_addr = pI830->overlay_regs->bus_addr;
    if (update_filter)
	flip_addr |= OFC_UPDATE;
    OVERLAY_DEBUG ("overlay_continue cmd 0x%08x  -> 0x%08x sta 0x%08x\n",
		   overlay->OCMD, INREG(OCMD_REGISTER), INREG(DOVSTA));
    BEGIN_BATCH(4);
    OUT_BATCH(MI_FLUSH | MI_WRITE_DIRTY_STATE);
    OUT_BATCH(MI_NOOP);
    OUT_BATCH(MI_OVERLAY_FLIP | MI_OVERLAY_FLIP_CONTINUE);
    OUT_BATCH(flip_addr);
    ADVANCE_BATCH();
    OVERLAY_DEBUG("overlay_continue\n");
}

static void
i830_overlay_off(ScrnInfoPtr pScrn)
{
    I830Ptr pI830 = I830PTR(pScrn);
    I830OverlayRegPtr	overlay = I830OVERLAYREG(pI830);

    if (!*pI830->overlayOn)
	return;

    /*
     * Wait for overlay to go idle. This has to be
     * separated from the turning off state by a WaitSync
     * to ensure the overlay will not read OCMD early and
     * disable the overlay before the commands here are
     * executed
     */
    {
	BEGIN_BATCH(2);
	OUT_BATCH(MI_WAIT_FOR_EVENT | MI_WAIT_FOR_OVERLAY_FLIP);
	OUT_BATCH(MI_NOOP);
	ADVANCE_BATCH();
	i830WaitSync(pScrn);
    }
    
    /*
     * Turn overlay off
     */
    {
	overlay->OCMD &= ~OVERLAY_ENABLE;
	OVERLAY_DEBUG ("overlay_off cmd 0x%08x -> 0x%08x sta 0x%08x\n",
		       overlay->OCMD, INREG(OCMD_REGISTER), INREG(DOVSTA));
	BEGIN_BATCH(6);
	OUT_BATCH(MI_FLUSH | MI_WRITE_DIRTY_STATE);
	OUT_BATCH(MI_NOOP);
	OUT_BATCH(MI_OVERLAY_FLIP | MI_OVERLAY_FLIP_CONTINUE);
	if (OVERLAY_NOPHYSICAL(pI830))
	    OUT_BATCH(pI830->overlay_regs->offset);
	else
	    OUT_BATCH(pI830->overlay_regs->bus_addr);
	OUT_BATCH(MI_WAIT_FOR_EVENT | MI_WAIT_FOR_OVERLAY_FLIP);
	OUT_BATCH(MI_NOOP);
	ADVANCE_BATCH();
	i830WaitSync(pScrn);
    }
    *pI830->overlayOn = FALSE;
    OVERLAY_DEBUG("overlay_off\n");
}

void
I830InitVideo(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    I830Ptr pI830 = I830PTR(pScrn);
    XF86VideoAdaptorPtr *adaptors, *newAdaptors = NULL;
    XF86VideoAdaptorPtr overlayAdaptor = NULL, texturedAdaptor = NULL;
    int num_adaptors;
#ifdef INTEL_XVMC
    Bool xvmc_status = FALSE;
#endif

#if 0
    {
	I830OverlayRegRec tmp;

	ErrorF("sizeof I830OverlayRegRec is 0x%x\n", sizeof(I830OverlayRegRec));
	ErrorF("Reserved C, D, E, F, G are %x, %x, %x, %x, %x\n",
	       (unsigned long)&(tmp.RESERVEDC[0]) - (unsigned long)&tmp,
	       (unsigned long)&(tmp.RESERVEDD[0]) - (unsigned long)&tmp,
	       (unsigned long)&(tmp.RESERVEDE[0]) - (unsigned long)&tmp,
	       (unsigned long)&(tmp.RESERVEDF[0]) - (unsigned long)&tmp,
	       (unsigned long)&(tmp.RESERVEDG[0]) - (unsigned long)&tmp);
    }
#endif

    num_adaptors = xf86XVListGenericAdaptors(pScrn, &adaptors);
    /* Give our adaptor list enough space for the overlay and/or texture video
     * adaptors.
     */
    newAdaptors = xalloc((num_adaptors + 2) * sizeof(XF86VideoAdaptorPtr *));
    if (newAdaptors == NULL)
	return;

    memcpy(newAdaptors, adaptors, num_adaptors * sizeof(XF86VideoAdaptorPtr));
    adaptors = newAdaptors;

    /* Add the adaptors supported by our hardware.  First, set up the atoms
     * that will be used by both output adaptors.
     */
    xvBrightness = MAKE_ATOM("XV_BRIGHTNESS");
    xvContrast = MAKE_ATOM("XV_CONTRAST");

#if 0 /* always use overlay */
    if (pI830->sync_fields) {
#else
    if (1) {
#endif
	/*
	 * we deactivate textured XV method for compatibility with older xine-lib
	 * versions not providing a configuration parameter for this.
	 * for further information see:
	 * 'video.device.xv_preferred_method' in file 'config_xineliboutput'
	 */
	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "VGA_SYNC_FIELDS disabled textured video mode\n");
    } else {

	/*
	 * Set up textured video if we can do it at this depth and we are on
	 * supported hardware.
	 */
	if (pScrn->bitsPerPixel >= 16 && (IS_I9XX(pI830) || IS_I965G(pI830)) &&
	    !(!IS_I965G(pI830) && pScrn->displayWidth > 2048))
	{
	    texturedAdaptor = I830SetupImageVideoTextured(pScreen);
	    if (texturedAdaptor != NULL) {
		adaptors[num_adaptors++] = texturedAdaptor;
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Set up textured video\n");
	    } else {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Failed to set up textured video\n");
	    }
	}
    }

    /* Set up overlay video if we can do it at this depth. */
    if (!OVERLAY_NOEXIST(pI830) && pScrn->bitsPerPixel != 8 &&
	    pI830->overlay_regs != NULL)
    {
	overlayAdaptor = I830SetupImageVideoOverlay(pScreen);
	if (overlayAdaptor != NULL) {
	    adaptors[num_adaptors++] = overlayAdaptor;
	    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Set up overlay video\n");
	} else {
	    xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		       "Failed to set up overlay video\n");
	}
	I830InitOffscreenImages(pScreen);
    }
#ifdef INTEL_XVMC
    if (intel_xvmc_probe(pScrn)) {
	if (texturedAdaptor)
	    xvmc_status = intel_xvmc_driver_init(pScreen, texturedAdaptor);
    }
#endif

    if (num_adaptors)
	xf86XVScreenInit(pScreen, adaptors, num_adaptors);

#ifdef INTEL_XVMC
    if (xvmc_status)
	intel_xvmc_screen_init(pScreen);
#endif
    xfree(adaptors);
}

static void
I830ResetVideo(ScrnInfoPtr pScrn)
{
    I830Ptr pI830 = I830PTR(pScrn);
    I830PortPrivPtr pPriv = pI830->adaptor->pPortPrivates[0].ptr;
    I830OverlayRegPtr	overlay = I830OVERLAYREG(pI830);

    OVERLAY_DEBUG("I830ResetVideo: base: %p, offset: 0x%lx, obase: %p\n",
		  pI830->FbBase, pI830->overlay_regs->offset, overlay);
    /*
     * Default to maximum image size in YV12
     */

    memset(overlay, 0, sizeof(*overlay));
    overlay->YRGB_VPH = 0;
    overlay->UV_VPH = 0;
    overlay->HORZ_PH = 0;
    overlay->INIT_PHS = 0;
    overlay->DWINPOS = 0;
    overlay->DWINSZ = 0;
    overlay->SWIDTH = 0;
    overlay->SWIDTHSW = 0;
    overlay->SHEIGHT = 0;
    overlay->OCLRC0 = (pPriv->contrast << 18) | (pPriv->brightness & 0xff);
    overlay->OCLRC1 = pPriv->saturation;
#if 0
    overlay->AWINPOS = 0;
    overlay->AWINSZ = 0;
#endif
    overlay->FASTHSCALE = 0;

    /*
     * Enable destination color keying
     */
    switch (pScrn->depth) {
    case 8:
	overlay->DCLRKV = 0;
	overlay->DCLRKM = 0xffffff | DEST_KEY_ENABLE;
	break;
    case 15:
	overlay->DCLRKV = RGB15ToColorKey(pPriv->colorKey);
	overlay->DCLRKM = 0x070707 | DEST_KEY_ENABLE;
	break;
    case 16:
	overlay->DCLRKV = RGB16ToColorKey(pPriv->colorKey);
	overlay->DCLRKM = 0x070307 | DEST_KEY_ENABLE;
	break;
    default:
	overlay->DCLRKV = pPriv->colorKey;
	overlay->DCLRKM = DEST_KEY_ENABLE;
	break;
    }

    overlay->SCLRKVH = 0;
    overlay->SCLRKVL = 0;
    overlay->SCLRKEN = 0;		/* source color key disable */
    overlay->OCONFIG = CC_OUT_8BIT;
    if (IS_I965GM(pI830))
	overlay->OCONFIG |= CSC_MODE_BT709;

    /*
     * Select which pipe the overlay is enabled on.
     */
    overlay->OCONFIG &= ~OVERLAY_PIPE_MASK;
    if (i830_crtc_pipe (pPriv->current_crtc) == 0)
	overlay->OCONFIG |= OVERLAY_PIPE_A;
    else 
	overlay->OCONFIG |= OVERLAY_PIPE_B;

#if 0
    /* 
     * XXX DUMP REGISTER CODE !!!
     * This allows us to dump the complete i845 registers and compare
     * with warm boot situations before we upload our first copy.
     */
    {
	int i;
	for (i = 0x30000; i < 0x31000; i += 4)
	    ErrorF("0x%x 0x%" PRIx32 "\n", i, INREG(i));
    }
#endif
}

#define PFIT_CONTROLS 0x61230
#define PFIT_AUTOVSCALE_MASK 0x200
#define PFIT_ON_MASK 0x80000000
#define PFIT_AUTOSCALE_RATIO 0x61238
#define PFIT_PROGRAMMED_SCALE_RATIO 0x61234

static void
I830SetOneLineModeRatio(ScrnInfoPtr pScrn)
{
    I830Ptr pI830 = I830PTR(pScrn);
    I830PortPrivPtr pPriv = pI830->adaptor->pPortPrivates[0].ptr;
    uint32_t panelFitControl = INREG(PFIT_CONTROLS);
    int vertScale;

    pPriv->scaleRatio = 0x10000;

    if (panelFitControl & PFIT_ON_MASK) {
	if (panelFitControl & PFIT_AUTOVSCALE_MASK) {
	    vertScale = INREG(PFIT_AUTOSCALE_RATIO) >> 16;
	} else {
	    vertScale = INREG(PFIT_PROGRAMMED_SCALE_RATIO) >> 16;
	}

	if (vertScale != 0)
	    pPriv->scaleRatio = ((double) 0x10000 / (double)vertScale) * 0x10000;

	pPriv->oneLineMode = TRUE;

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Enabling Xvideo one-line mode\n");
    }

    if (pPriv->scaleRatio == 0x10000)
	pPriv->oneLineMode = FALSE;
}

static uint32_t I830BoundGammaElt (uint32_t elt, uint32_t eltPrev)
{
    elt &= 0xff;
    eltPrev &= 0xff;
    if (elt < eltPrev)
	elt = eltPrev;
    else if ((elt - eltPrev) > 0x7e)
	elt = eltPrev + 0x7e;
    return elt;
}

static uint32_t I830BoundGamma (uint32_t gamma, uint32_t gammaPrev)
{
    return (I830BoundGammaElt (gamma >> 24, gammaPrev >> 24) << 24 |
	    I830BoundGammaElt (gamma >> 16, gammaPrev >> 16) << 16 |
	    I830BoundGammaElt (gamma >>  8, gammaPrev >>  8) <<  8 |
	    I830BoundGammaElt (gamma      , gammaPrev      ));
}

static void
I830UpdateGamma(ScrnInfoPtr pScrn)
{
    I830Ptr pI830 = I830PTR(pScrn);
    I830PortPrivPtr pPriv = pI830->adaptor->pPortPrivates[0].ptr;
    uint32_t gamma0 = pPriv->gamma0;
    uint32_t gamma1 = pPriv->gamma1;
    uint32_t gamma2 = pPriv->gamma2;
    uint32_t gamma3 = pPriv->gamma3;
    uint32_t gamma4 = pPriv->gamma4;
    uint32_t gamma5 = pPriv->gamma5;

#if 0
    ErrorF ("Original gamma: 0x%lx 0x%lx 0x%lx 0x%lx 0x%lx 0x%lx\n",
	    gamma0, gamma1, gamma2, gamma3, gamma4, gamma5);
#endif
    gamma1 = I830BoundGamma (gamma1, gamma0);
    gamma2 = I830BoundGamma (gamma2, gamma1);
    gamma3 = I830BoundGamma (gamma3, gamma2);
    gamma4 = I830BoundGamma (gamma4, gamma3);
    gamma5 = I830BoundGamma (gamma5, gamma4);
#if 0
    ErrorF ("Bounded  gamma: 0x%lx 0x%lx 0x%lx 0x%lx 0x%lx 0x%lx\n",
	    gamma0, gamma1, gamma2, gamma3, gamma4, gamma5);
#endif

    OUTREG(OGAMC5, gamma5);
    OUTREG(OGAMC4, gamma4);
    OUTREG(OGAMC3, gamma3);
    OUTREG(OGAMC2, gamma2);
    OUTREG(OGAMC1, gamma1);
    OUTREG(OGAMC0, gamma0);
}

static XF86VideoAdaptorPtr
I830SetupImageVideoOverlay(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    I830Ptr pI830 = I830PTR(pScrn);
    XF86VideoAdaptorPtr adapt;
    I830PortPrivPtr pPriv;
    XF86AttributePtr att;

    OVERLAY_DEBUG("I830SetupImageVideoOverlay\n");

    if (!(adapt = xcalloc(1, sizeof(XF86VideoAdaptorRec) +
			  sizeof(I830PortPrivRec) + sizeof(DevUnion))))
	return NULL;

    adapt->type = XvWindowMask | XvInputMask | XvImageMask;
    adapt->flags = VIDEO_OVERLAID_IMAGES /*| VIDEO_CLIP_TO_VIEWPORT*/;
    adapt->name = "Intel(R) Video Overlay";
    adapt->nEncodings = 1;
    adapt->pEncodings = DummyEncoding;
    /* update the DummyEncoding for these two chipsets */
    if (IS_845G(pI830) || IS_I830(pI830)) {
	adapt->pEncodings->width = IMAGE_MAX_WIDTH_LEGACY;
	adapt->pEncodings->height = IMAGE_MAX_HEIGHT_LEGACY;
    }
    adapt->nFormats = NUM_FORMATS;
    adapt->pFormats = Formats;
    adapt->nPorts = 1;
    adapt->pPortPrivates = (DevUnion *) (&adapt[1]);

    pPriv = (I830PortPrivPtr) (&adapt->pPortPrivates[1]);

    adapt->pPortPrivates[0].ptr = (pointer) (pPriv);
    adapt->nAttributes = NUM_ATTRIBUTES;
    adapt->nAttributes += CLONE_ATTRIBUTES;
    if (IS_I9XX(pI830))
	adapt->nAttributes += GAMMA_ATTRIBUTES; /* has gamma */
    adapt->pAttributes = xnfalloc(sizeof(XF86AttributeRec) * adapt->nAttributes);
    /* Now copy the attributes */
    att = adapt->pAttributes;
    memcpy((char *)att, (char*)Attributes, sizeof(XF86AttributeRec)* NUM_ATTRIBUTES);
    att+=NUM_ATTRIBUTES;
    memcpy((char*)att, (char*)CloneAttributes, sizeof(XF86AttributeRec) * CLONE_ATTRIBUTES);
    att+=CLONE_ATTRIBUTES;
    if (IS_I9XX(pI830)) {
	memcpy((char*)att, (char*)GammaAttributes, sizeof(XF86AttributeRec) * GAMMA_ATTRIBUTES);
	att+=GAMMA_ATTRIBUTES;
    }
    adapt->nImages = NUM_IMAGES;
    adapt->pImages = Images;
    adapt->PutVideo = NULL;
    adapt->PutStill = NULL;
    adapt->GetVideo = NULL;
    adapt->GetStill = NULL;
    adapt->StopVideo = I830StopVideo;
    adapt->SetPortAttribute = I830SetPortAttribute;
    adapt->GetPortAttribute = I830GetPortAttribute;
    adapt->QueryBestSize = I830QueryBestSize;
    adapt->PutImage = I830PutImage;
    adapt->QueryImageAttributes = I830QueryImageAttributesOverlay;

    pPriv->textured = FALSE;
    pPriv->colorKey = pI830->colorKey & ((1 << pScrn->depth) - 1);
    pPriv->videoStatus = 0;
    pPriv->brightness = -19; /* (255/219) * -16 */
    pPriv->contrast = 75;  /* 255/219 * 64 */
    pPriv->saturation = 146; /* 128/112 * 128 */
    pPriv->current_crtc = NULL;
    pPriv->desired_crtc = NULL;
    pPriv->buf = NULL;
    pPriv->currentBuf = 0;
    pPriv->gamma5 = 0xc0c0c0;
    pPriv->gamma4 = 0x808080;
    pPriv->gamma3 = 0x404040;
    pPriv->gamma2 = 0x202020;
    pPriv->gamma1 = 0x101010;
    pPriv->gamma0 = 0x080808;
    pPriv->doubleBuffer = 1;

    pPriv->rotation = RR_Rotate_0;

    /* gotta uninit this someplace */
    REGION_NULL(pScreen, &pPriv->clip);

    pI830->adaptor = adapt;

    /* With LFP's we need to detect whether we're in One Line Mode, which
     * essentially means a resolution greater than 1024x768, and fix up
     * the scaler accordingly. */
    pPriv->scaleRatio = 0x10000;
    pPriv->oneLineMode = FALSE;

    /*
     * Initialise pPriv->overlayOK.  Set it to TRUE here so that a warning will
     * be generated if i830_crtc_dpms_video() sets it to FALSE during mode
     * setup.
     */
    pPriv->overlayOK = TRUE;

    xvColorKey = MAKE_ATOM("XV_COLORKEY");
    xvBrightness = MAKE_ATOM("XV_BRIGHTNESS");
    xvContrast = MAKE_ATOM("XV_CONTRAST");
    xvSaturation = MAKE_ATOM("XV_SATURATION");
    xvDoubleBuffer = MAKE_ATOM("XV_DOUBLE_BUFFER");

    /* Allow the pipe to be switched from pipe A to B when in clone mode */
    xvPipe = MAKE_ATOM("XV_PIPE");

    if (IS_I9XX(pI830)) {
	xvGamma0 = MAKE_ATOM("XV_GAMMA0");
	xvGamma1 = MAKE_ATOM("XV_GAMMA1");
	xvGamma2 = MAKE_ATOM("XV_GAMMA2");
	xvGamma3 = MAKE_ATOM("XV_GAMMA3");
	xvGamma4 = MAKE_ATOM("XV_GAMMA4");
	xvGamma5 = MAKE_ATOM("XV_GAMMA5");
    }

    I830ResetVideo(pScrn);

    I830UpdateGamma(pScrn);

    return adapt;
}

static XF86VideoAdaptorPtr
I830SetupImageVideoTextured(ScreenPtr pScreen)
{
    XF86VideoAdaptorPtr adapt;
    XF86AttributePtr attrs;
    I830PortPrivPtr portPrivs;
    DevUnion *devUnions;
    int nports = 16, i;
    int nAttributes;

    OVERLAY_DEBUG("I830SetupImageVideoOverlay\n");

    nAttributes = NUM_TEXTURED_ATTRIBUTES;

    adapt = xcalloc(1, sizeof(XF86VideoAdaptorRec));
    portPrivs = xcalloc(nports, sizeof(I830PortPrivRec));
    devUnions = xcalloc(nports, sizeof(DevUnion));
    attrs = xcalloc(nAttributes, sizeof(XF86AttributeRec));
    if (adapt == NULL || portPrivs == NULL || devUnions == NULL ||
	attrs == NULL)
    {
	xfree(adapt);
	xfree(portPrivs);
	xfree(devUnions);
	xfree(attrs);
	return NULL;
    }

    adapt->type = XvWindowMask | XvInputMask | XvImageMask;
    adapt->flags = 0;
    adapt->name = "Intel(R) Textured Video";
    adapt->nEncodings = 1;
    adapt->pEncodings = DummyEncoding;
    adapt->nFormats = NUM_FORMATS;
    adapt->pFormats = Formats;
    adapt->nPorts = nports;
    adapt->pPortPrivates = devUnions;
    adapt->nAttributes = nAttributes;
    adapt->pAttributes = attrs;
    memcpy(attrs, TexturedAttributes, nAttributes * sizeof(XF86AttributeRec));
    adapt->nImages = NUM_IMAGES;
    adapt->pImages = Images;
    adapt->PutVideo = NULL;
    adapt->PutStill = NULL;
    adapt->GetVideo = NULL;
    adapt->GetStill = NULL;
    adapt->StopVideo = I830StopVideo;
    adapt->SetPortAttribute = I830SetPortAttributeTextured;
    adapt->GetPortAttribute = I830GetPortAttribute;
    adapt->QueryBestSize = I830QueryBestSize;
    adapt->PutImage = I830PutImage;
    adapt->QueryImageAttributes = I830QueryImageAttributesTextured;

    for (i = 0; i < nports; i++) {
	I830PortPrivPtr pPriv = &portPrivs[i];

	pPriv->textured = TRUE;
	pPriv->videoStatus = 0;
	pPriv->buf = NULL;
	pPriv->currentBuf = 0;
	pPriv->doubleBuffer = 0;

	pPriv->rotation = RR_Rotate_0;

	/* gotta uninit this someplace, XXX: shouldn't be necessary for textured */
	REGION_NULL(pScreen, &pPriv->clip);

	adapt->pPortPrivates[i].ptr = (pointer) (pPriv);
    }

    return adapt;
}

static Bool
RegionsEqual(RegionPtr A, RegionPtr B)
{
    int *dataA, *dataB;
    int num;

    num = REGION_NUM_RECTS(A);
    if (num != REGION_NUM_RECTS(B))
	return FALSE;

    if ((A->extents.x1 != B->extents.x1) ||
	(A->extents.x2 != B->extents.x2) ||
	(A->extents.y1 != B->extents.y1) || (A->extents.y2 != B->extents.y2))
	return FALSE;

    dataA = (int *)REGION_RECTS(A);
    dataB = (int *)REGION_RECTS(B);

    while (num--) {
	if ((dataA[0] != dataB[0]) || (dataA[1] != dataB[1]))
	    return FALSE;
	dataA += 2;
	dataB += 2;
    }

    return TRUE;
}

static void
I830StopVideo(ScrnInfoPtr pScrn, pointer data, Bool shutdown)
{
    I830PortPrivPtr pPriv = (I830PortPrivPtr) data;
    I830Ptr pI830 = I830PTR(pScrn);

    if (pPriv->textured)
	return;

    OVERLAY_DEBUG("I830StopVideo\n");

    REGION_EMPTY(pScrn->pScreen, &pPriv->clip);

    if (shutdown) {
	if (pPriv->videoStatus & CLIENT_VIDEO_ON) {
	    i830_overlay_off(pScrn);
	    if (pI830->entityPrivate)
		pI830->entityPrivate->XvInUse = -1;
	}
	/* Sync before freeing the buffer, because the pages will be unbound.
	 */
	I830Sync(pScrn);
	i830_free_memory(pScrn, pPriv->buf);
	pPriv->buf = NULL;
	pPriv->videoStatus = 0;
    } else {
	if (pPriv->videoStatus & CLIENT_VIDEO_ON) {
	    pPriv->videoStatus |= OFF_TIMER;
	    pPriv->offTime = currentTime.milliseconds + OFF_DELAY;
	}
    }

}

static int
I830SetPortAttributeTextured(ScrnInfoPtr pScrn,
			     Atom attribute, INT32 value, pointer data)
{
    I830PortPrivPtr pPriv = (I830PortPrivPtr) data;

    if (attribute == xvBrightness) {
	if ((value < -128) || (value > 127))
	    return BadValue;
	pPriv->brightness = value;
	return Success;
    } else if (attribute == xvContrast) {
	if ((value < 0) || (value > 255))
	    return BadValue;
	pPriv->contrast = value;
	return Success;
    } else {
	return BadMatch;
    }
}

static int
I830SetPortAttribute(ScrnInfoPtr pScrn,
		     Atom attribute, INT32 value, pointer data)
{
    I830PortPrivPtr pPriv = (I830PortPrivPtr) data;
    I830Ptr pI830 = I830PTR(pScrn);
    I830OverlayRegPtr overlay;

    overlay = I830OVERLAYREG(pI830);

    if (attribute == xvBrightness) {
	if ((value < -128) || (value > 127))
	    return BadValue;
	pPriv->brightness = value;
	overlay->OCLRC0 = (pPriv->contrast << 18) | (pPriv->brightness & 0xff);
	OVERLAY_DEBUG("BRIGHTNESS\n");
	i830_overlay_continue (pScrn, FALSE);
    } else if (attribute == xvContrast) {
	if ((value < 0) || (value > 255))
	    return BadValue;
	pPriv->contrast = value;
	overlay->OCLRC0 = (pPriv->contrast << 18) | (pPriv->brightness & 0xff);
	OVERLAY_DEBUG("CONTRAST\n");
	i830_overlay_continue (pScrn, FALSE);
    } else if (attribute == xvSaturation) {
	if ((value < 0) || (value > 1023))
	    return BadValue;
	pPriv->saturation = value;
	overlay->OCLRC1 = pPriv->saturation;
	i830_overlay_continue (pScrn, FALSE);
    } else if (attribute == xvPipe) {
	xf86CrtcConfigPtr   xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	if ((value < -1) || (value > xf86_config->num_crtc))
	    return BadValue;
	if (value < 0)
	    pPriv->desired_crtc = NULL;
	else
	    pPriv->desired_crtc = xf86_config->crtc[value];
	/*
	 * Leave this to be updated at the next frame
	 */
    } else if (attribute == xvGamma0 && (IS_I9XX(pI830))) {
	pPriv->gamma0 = value; 
    } else if (attribute == xvGamma1 && (IS_I9XX(pI830))) {
	pPriv->gamma1 = value;
    } else if (attribute == xvGamma2 && (IS_I9XX(pI830))) {
	pPriv->gamma2 = value;
    } else if (attribute == xvGamma3 && (IS_I9XX(pI830))) {
	pPriv->gamma3 = value;
    } else if (attribute == xvGamma4 && (IS_I9XX(pI830))) {
	pPriv->gamma4 = value;
    } else if (attribute == xvGamma5 && (IS_I9XX(pI830))) {
	pPriv->gamma5 = value;
    } else if (attribute == xvColorKey) {
	pPriv->colorKey = value;
	switch (pScrn->depth) {
	case 16:
	    overlay->DCLRKV = RGB16ToColorKey(pPriv->colorKey);
	    break;
	case 15:
	    overlay->DCLRKV = RGB15ToColorKey(pPriv->colorKey);
	    break;
	default:
	    overlay->DCLRKV = pPriv->colorKey;
	    break;
	}
	OVERLAY_DEBUG("COLORKEY\n");
	i830_overlay_continue (pScrn, FALSE);
	REGION_EMPTY(pScrn->pScreen, &pPriv->clip);
    } else if(attribute == xvDoubleBuffer) {
	if ((value < 0) || (value > 1))
	    return BadValue;
	/* Do not allow buffer change while playing video */
	if(!*pI830->overlayOn)
	    pPriv->doubleBuffer = value;
    } else
	return BadMatch;

    /* Ensure that the overlay is off, ready for updating */
    if ((attribute == xvGamma0 ||
	 attribute == xvGamma1 ||
	 attribute == xvGamma2 ||
	 attribute == xvGamma3 ||
	 attribute == xvGamma4 ||
	 attribute == xvGamma5) && (IS_I9XX(pI830))) {
	OVERLAY_DEBUG("GAMMA\n");
	I830UpdateGamma(pScrn);
    }

    return Success;
}

static int
I830GetPortAttribute(ScrnInfoPtr pScrn,
		     Atom attribute, INT32 * value, pointer data)
{
    I830Ptr pI830 = I830PTR(pScrn);
    I830PortPrivPtr pPriv = (I830PortPrivPtr) data;

    if (attribute == xvBrightness) {
	*value = pPriv->brightness;
    } else if (attribute == xvContrast) {
	*value = pPriv->contrast;
    } else if (attribute == xvSaturation) {
	*value = pPriv->saturation;
    } else if (attribute == xvPipe) {
	int		c;
	xf86CrtcConfigPtr	xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	for (c = 0; c < xf86_config->num_crtc; c++)
	    if (xf86_config->crtc[c] == pPriv->desired_crtc)
		break;
	if (c == xf86_config->num_crtc)
	    c = -1;
	*value = c;
    } else if (attribute == xvGamma0 && (IS_I9XX(pI830))) {
	*value = pPriv->gamma0;
    } else if (attribute == xvGamma1 && (IS_I9XX(pI830))) {
	*value = pPriv->gamma1;
    } else if (attribute == xvGamma2 && (IS_I9XX(pI830))) {
	*value = pPriv->gamma2;
    } else if (attribute == xvGamma3 && (IS_I9XX(pI830))) {
	*value = pPriv->gamma3;
    } else if (attribute == xvGamma4 && (IS_I9XX(pI830))) {
	*value = pPriv->gamma4;
    } else if (attribute == xvGamma5 && (IS_I9XX(pI830))) {
	*value = pPriv->gamma5;
    } else if (attribute == xvColorKey) {
	*value = pPriv->colorKey;
    } else if (attribute == xvDoubleBuffer) {
	*value = pPriv->doubleBuffer;
    } else 
	return BadMatch;

    return Success;
}

static void
I830QueryBestSize(ScrnInfoPtr pScrn,
		  Bool motion,
		  short vid_w, short vid_h,
		  short drw_w, short drw_h,
		  unsigned int *p_w, unsigned int *p_h, pointer data)
{
    if (vid_w > (drw_w << 1))
	drw_w = vid_w >> 1;
    if (vid_h > (drw_h << 1))
	drw_h = vid_h >> 1;

    *p_w = drw_w;
    *p_h = drw_h;
}

static void
I830CopyPackedData(ScrnInfoPtr pScrn, I830PortPrivPtr pPriv,
		   unsigned char *buf,
		   int srcPitch,
		   int dstPitch, int top, int left, int h, int w)
{
    I830Ptr pI830 = I830PTR(pScrn);
    unsigned char *src, *dst;
    int i,j;
    unsigned char *s;

#if 0
    ErrorF("I830CopyPackedData: (%d,%d) (%d,%d)\n"
	   "srcPitch: %d, dstPitch: %d\n", top, left, h, w,
	   srcPitch, dstPitch);
#endif

    src = buf + (top * srcPitch) + (left << 1);

    if (pPriv->currentBuf == 0)
	dst = pI830->FbBase + pPriv->YBuf0offset;
    else
	dst = pI830->FbBase + pPriv->YBuf1offset;

    switch (pPriv->rotation) {
    case RR_Rotate_0:
	w <<= 1;
	for (i = 0; i < h; i++) {
	    memcpy(dst, src, w);
	    src += srcPitch;
	    dst += dstPitch;
	}
	break;
    case RR_Rotate_90:
	h <<= 1;
	for (i = 0; i < h; i+=2) {
	    s = src;
	    for (j = 0; j < w; j++) {
		/* Copy Y */
		dst[(i + 0) + ((w - j - 1) * dstPitch)] = *s++; 
		(void)*s++;
	    }
	    src += srcPitch;
	}
	h >>= 1;
	src = buf + (top * srcPitch) + (left << 1);
	for (i = 0; i < h; i+=2) {
	    for (j = 0; j < w; j+=2) {
		/* Copy U */
		dst[((i*2) + 1) + ((w - j - 1) * dstPitch)] = src[(j*2) + 1 + (i * srcPitch)];
		dst[((i*2) + 1) + ((w - j - 2) * dstPitch)] = src[(j*2) + 1 + ((i+1) * srcPitch)];
		/* Copy V */
		dst[((i*2) + 3) + ((w - j - 1) * dstPitch)] = src[(j*2) + 3 + (i * srcPitch)];
		dst[((i*2) + 3) + ((w - j - 2) * dstPitch)] = src[(j*2) + 3 + ((i+1) * srcPitch)];
	    }
	}
	break;
    case RR_Rotate_180:
	w <<= 1;
	for (i = 0; i < h; i++) {
	    s = src;
	    for (j = 0; j < w; j+=4) {
		dst[(w - j - 4) + ((h - i - 1) * dstPitch)] = *s++;
		dst[(w - j - 3) + ((h - i - 1) * dstPitch)] = *s++;
		dst[(w - j - 2) + ((h - i - 1) * dstPitch)] = *s++;
		dst[(w - j - 1) + ((h - i - 1) * dstPitch)] = *s++;
	    }
	    src += srcPitch;
	}
	break;
    case RR_Rotate_270:
	h <<= 1;
	for (i = 0; i < h; i+=2) {
	    s = src;
	    for (j = 0; j < w; j++) {
		/* Copy Y */
		dst[(h - i - 2) + (j * dstPitch)] = *s++;
		(void)*s++;
	    }
	    src += srcPitch;
	}
	h >>= 1;
	src = buf + (top * srcPitch) + (left << 1);
	for (i = 0; i < h; i+=2) {
	    for (j = 0; j < w; j+=2) {
		/* Copy U */
		dst[(((h - i)*2) - 3) + (j * dstPitch)] = src[(j*2) + 1 + (i * srcPitch)];
		dst[(((h - i)*2) - 3) + ((j - 1) * dstPitch)] = src[(j*2) + 1 + ((i+1) * srcPitch)];
		/* Copy V */
		dst[(((h - i)*2) - 1) + (j * dstPitch)] = src[(j*2) + 3 + (i * srcPitch)];
		dst[(((h - i)*2) - 1) + ((j - 1) * dstPitch)] = src[(j*2) + 3 + ((i+1) * srcPitch)];
	    }
	}
	break;
    }
}

/* Copies planar data in *buf to UYVY-packed data in the screen atYBufXOffset.
 */
static void
I830CopyPlanarToPackedData(ScrnInfoPtr pScrn, I830PortPrivPtr pPriv,
			   unsigned char *buf, int srcPitch,
			   int srcPitch2, int dstPitch, int srcH,
			   int top, int left, int h, int w, int id)
{
    I830Ptr pI830 = I830PTR(pScrn);
    uint8_t *dst1, *srcy, *srcu, *srcv;
    int y;

    if (pPriv->currentBuf == 0)
	dst1 = pI830->FbBase + pPriv->YBuf0offset;
    else
	dst1 = pI830->FbBase + pPriv->YBuf1offset;

    srcy = buf + (top * srcPitch) + left;
    if (id == FOURCC_YV12) {
	srcu = buf + (srcH * srcPitch) + ((top / 2) * srcPitch2) + (left / 2);
	srcv = buf + (srcH * srcPitch) + ((srcH / 2) * srcPitch2) +
	((top / 2) * srcPitch2) + (left / 2);
    } else {
	srcv = buf + (srcH * srcPitch) + ((top / 2) * srcPitch2) + (left / 2);
	srcu = buf + (srcH * srcPitch) + ((srcH / 2) * srcPitch2) +
	((top / 2) * srcPitch2) + (left / 2);
    }

    for (y = 0; y < h; y++) {
	uint32_t *dst = (uint32_t *)dst1;
	uint8_t *sy = srcy;
	uint8_t *su = srcu;
	uint8_t *sv = srcv;
	int i;

	i = w / 2;
	while(i > 4) {
	    dst[0] = sy[0] | (sy[1] << 16) | (sv[0] << 8) | (su[0] << 24);
	    dst[1] = sy[2] | (sy[3] << 16) | (sv[1] << 8) | (su[1] << 24);
	    dst[2] = sy[4] | (sy[5] << 16) | (sv[2] << 8) | (su[2] << 24);
	    dst[3] = sy[6] | (sy[7] << 16) | (sv[3] << 8) | (su[3] << 24);
	    dst += 4; su += 4; sv += 4; sy += 8;
	    i -= 4;
	}
	while(i--) {
	    dst[0] = sy[0] | (sy[1] << 16) | (sv[0] << 8) | (su[0] << 24);
	    dst++; su++; sv++;
	    sy += 2;
	}

	dst1 += dstPitch;
	srcy += srcPitch;
	if (y & 1) {
	    srcu += srcPitch2;
	    srcv += srcPitch2;
	}	
    }
}

static void
I830CopyPlanarData(ScrnInfoPtr pScrn, I830PortPrivPtr pPriv,
		   unsigned char *buf, int srcPitch,
		   int srcPitch2, int dstPitch, int srcH, int top, int left,
		   int h, int w, int id)
{
    I830Ptr pI830 = I830PTR(pScrn);
    int i, j = 0;
    unsigned char *src1, *src2, *src3, *dst1, *dst2, *dst3;
    unsigned char *s;
    int dstPitch2 = dstPitch << 1;

#if 0
    ErrorF("I830CopyPlanarData: srcPitch %d, srcPitch %d, dstPitch %d\n"
	   "nlines %d, npixels %d, top %d, left %d\n",
	   srcPitch, srcPitch2, dstPitch,
	   h, w, top, left);
#endif

    /* Copy Y data */
    src1 = buf + (top * srcPitch) + left;
#if 0
    ErrorF("src1 is %p, offset is %ld\n", src1,
	   (unsigned long)src1 - (unsigned long)buf);
#endif
    if (pPriv->currentBuf == 0)
	dst1 = pI830->FbBase + pPriv->YBuf0offset;
    else
	dst1 = pI830->FbBase + pPriv->YBuf1offset;

    switch (pPriv->rotation) {
    case RR_Rotate_0:
	if (srcPitch == dstPitch2)
	    memcpy (dst1, src1, srcPitch * h);
	else
	    for (i = 0; i < h; i++) {
		memcpy(dst1, src1, w);
		src1 += srcPitch;
		dst1 += dstPitch2;
	    }
	break;
    case RR_Rotate_90:
	for (i = 0; i < h; i++) {
	    s = src1;
	    for (j = 0; j < w; j++) {
		dst1[(i) + ((w - j - 1) * dstPitch2)] = *s++;
	    }
	    src1 += srcPitch;
	}
	break;
    case RR_Rotate_180:
	for (i = 0; i < h; i++) {
	    s = src1;
	    for (j = 0; j < w; j++) {
		dst1[(w - j - 1) + ((h - i - 1) * dstPitch2)] = *s++;
	    }
	    src1 += srcPitch;
	}
	break;
    case RR_Rotate_270:
	for (i = 0; i < h; i++) {
	    s = src1;
	    for (j = 0; j < w; j++) {
		dst1[(h - i - 1) + (j * dstPitch2)] = *s++;
	    }
	    src1 += srcPitch;
	}
	break;
    }

    /* Copy V data for YV12, or U data for I420 */
    src2 = buf + (srcH * srcPitch) + ((top * srcPitch) >> 2) + (left >> 1);
#if 0
    ErrorF("src2 is %p, offset is %ld\n", src2,
	   (unsigned long)src2 - (unsigned long)buf);
#endif
    if (pPriv->currentBuf == 0) {
	if (id == FOURCC_I420)
	    dst2 = pI830->FbBase + pPriv->UBuf0offset;
	else
	    dst2 = pI830->FbBase + pPriv->VBuf0offset;
    } else {
	if (id == FOURCC_I420)
	    dst2 = pI830->FbBase + pPriv->UBuf1offset;
	else
	    dst2 = pI830->FbBase + pPriv->VBuf1offset;
    }

    switch (pPriv->rotation) {
    case RR_Rotate_0:
	if (srcPitch2 == dstPitch)
	    memcpy (dst2, src2, h/2 * srcPitch2);
	else
	    for (i = 0; i < h / 2; i++) {
		memcpy(dst2, src2, w / 2);
		src2 += srcPitch2;
		dst2 += dstPitch;
	    }
	break;
    case RR_Rotate_90:
	for (i = 0; i < (h/2); i++) {
	    s = src2;
	    for (j = 0; j < (w/2); j++) {
		dst2[(i) + (((w/2) - j - 1) * (dstPitch))] = *s++;
	    }
	    src2 += srcPitch2;
	}
	break;
    case RR_Rotate_180:
	for (i = 0; i < (h/2); i++) {
	    s = src2;
	    for (j = 0; j < (w/2); j++) {
		dst2[((w/2) - j - 1) + (((h/2) - i - 1) * dstPitch)] = *s++;
	    }
	    src2 += srcPitch2;
	}
	break;
    case RR_Rotate_270:
	for (i = 0; i < (h/2); i++) {
	    s = src2;
	    for (j = 0; j < (w/2); j++) {
		dst2[((h/2) - i - 1) + (j * dstPitch)] = *s++;
	    }
	    src2 += srcPitch2;
	}
	break;
    }

    /* Copy U data for YV12, or V data for I420 */
    src3 = buf + (srcH * srcPitch) + ((srcH >> 1) * srcPitch2) +
    ((top * srcPitch) >> 2) + (left >> 1);
#if 0
    ErrorF("src3 is %p, offset is %ld\n", src3,
	   (unsigned long)src3 - (unsigned long)buf);
#endif
    if (pPriv->currentBuf == 0) {
	if (id == FOURCC_I420)
	    dst3 = pI830->FbBase + pPriv->VBuf0offset;
	else
	    dst3 = pI830->FbBase + pPriv->UBuf0offset;
    } else {
	if (id == FOURCC_I420)
	    dst3 = pI830->FbBase + pPriv->VBuf1offset;
	else
	    dst3 = pI830->FbBase + pPriv->UBuf1offset;
    }

    switch (pPriv->rotation) {
    case RR_Rotate_0:
	if (srcPitch2 == dstPitch)
	    memcpy (dst3, src3, srcPitch2 * h/2);
	else
	    for (i = 0; i < h / 2; i++) {
		memcpy(dst3, src3, w / 2);
		src3 += srcPitch2;
		dst3 += dstPitch;
	    }
	break;
    case RR_Rotate_90:
	for (i = 0; i < (h/2); i++) {
	    s = src3;
	    for (j = 0; j < (w/2); j++) {
		dst3[(i) + (((w/2) - j - 1) * (dstPitch))] = *s++;
	    }
	    src3 += srcPitch2;
	}
	break;
    case RR_Rotate_180:
	for (i = 0; i < (h/2); i++) {
	    s = src3;
	    for (j = 0; j < (w/2); j++) {
		dst3[((w/2) - j - 1) + (((h/2) - i - 1) * dstPitch)] = *s++;
	    }
	    src3 += srcPitch2;
	}
	break;
    case RR_Rotate_270:
	for (i = 0; i < (h/2); i++) {
	    s = src3;
	    for (j = 0; j < (w/2); j++) {
		dst3[((h/2) - i - 1) + (j * dstPitch)] = *s++;
	    }
	    src3 += srcPitch2;
	}
	break;
    }
}

typedef struct {
    uint8_t sign;
    uint16_t mantissa;
    uint8_t exponent;
} coeffRec, *coeffPtr;

static Bool
SetCoeffRegs(double *coeff, int mantSize, coeffPtr pCoeff, int pos)
{
    int maxVal, icoeff, res;
    int sign;
    double c;

    sign = 0;
    maxVal = 1 << mantSize;
    c = *coeff;
    if (c < 0.0) {
	sign = 1;
	c = -c;
    }

    res = 12 - mantSize;
    if ((icoeff = (int)(c * 4 * maxVal + 0.5)) < maxVal) {
	pCoeff[pos].exponent = 3;
	pCoeff[pos].mantissa = icoeff << res;
	*coeff = (double)icoeff / (double)(4 * maxVal);
    } else if ((icoeff = (int)(c * 2 * maxVal + 0.5)) < maxVal) {
	pCoeff[pos].exponent = 2;
	pCoeff[pos].mantissa = icoeff << res;
	*coeff = (double)icoeff / (double)(2 * maxVal);
    } else if ((icoeff = (int)(c * maxVal + 0.5)) < maxVal) {
	pCoeff[pos].exponent = 1;
	pCoeff[pos].mantissa = icoeff << res;
	*coeff = (double)icoeff / (double)(maxVal);
    } else if ((icoeff = (int)(c * maxVal * 0.5 + 0.5)) < maxVal) {
	pCoeff[pos].exponent = 0;
	pCoeff[pos].mantissa = icoeff << res;
	*coeff = (double)icoeff / (double)(maxVal / 2);
    } else {
	/* Coeff out of range */
	return FALSE;
    }

    pCoeff[pos].sign = sign;
    if (sign)
	*coeff = -(*coeff);
    return TRUE;
}

static void
UpdateCoeff(int taps, double fCutoff, Bool isHoriz, Bool isY, coeffPtr pCoeff)
{
    int i, j, j1, num, pos, mantSize;
    double pi = 3.1415926535, val, sinc, window, sum;
    double rawCoeff[MAX_TAPS * 32], coeffs[N_PHASES][MAX_TAPS];
    double diff;
    int tapAdjust[MAX_TAPS], tap2Fix;
    Bool isVertAndUV;

    if (isHoriz)
	mantSize = 7;
    else
	mantSize = 6;

    isVertAndUV = !isHoriz && !isY;
    num = taps * 16;
    for (i = 0; i < num  * 2; i++) {
	val = (1.0 / fCutoff) * taps * pi * (i - num) / (2 * num);
	if (val == 0.0)
	    sinc = 1.0;
	else
	    sinc = sin(val) / val;

	/* Hamming window */
	window = (0.5 - 0.5 * cos(i * pi / num));
	rawCoeff[i] = sinc * window;
    }

    for (i = 0; i < N_PHASES; i++) {
	/* Normalise the coefficients. */
	sum = 0.0;
	for (j = 0; j < taps; j++) {
	    pos = i + j * 32;
	    sum += rawCoeff[pos];
	}
	for (j = 0; j < taps; j++) {
	    pos = i + j * 32;
	    coeffs[i][j] = rawCoeff[pos] / sum;
	}

	/* Set the register values. */
	for (j = 0; j < taps; j++) {
	    pos = j + i * taps;
	    if ((j == (taps - 1) / 2) && !isVertAndUV)
		SetCoeffRegs(&coeffs[i][j], mantSize + 2, pCoeff, pos);
	    else
		SetCoeffRegs(&coeffs[i][j], mantSize, pCoeff, pos);
	}

	tapAdjust[0] = (taps - 1) / 2;
	for (j = 1, j1 = 1; j <= tapAdjust[0]; j++, j1++) {
	    tapAdjust[j1] = tapAdjust[0] - j;
	    tapAdjust[++j1] = tapAdjust[0] + j;
	}

	/* Adjust the coefficients. */
	sum = 0.0;
	for (j = 0; j < taps; j++)
	    sum += coeffs[i][j];
	if (sum != 1.0) {
	    for (j1 = 0; j1 < taps; j1++) {
		tap2Fix = tapAdjust[j1];
		diff = 1.0 - sum;
		coeffs[i][tap2Fix] += diff;
		pos = tap2Fix + i * taps;
		if ((tap2Fix == (taps - 1) / 2) && !isVertAndUV)
		    SetCoeffRegs(&coeffs[i][tap2Fix], mantSize + 2, pCoeff, pos);
		else
		    SetCoeffRegs(&coeffs[i][tap2Fix], mantSize, pCoeff, pos);

		sum = 0.0;
		for (j = 0; j < taps; j++)
		    sum += coeffs[i][j];
		if (sum == 1.0)
		    break;
	    }
	}
    }
}

static void
i830_box_intersect (BoxPtr dest, BoxPtr a, BoxPtr b)
{
    dest->x1 = a->x1 > b->x1 ? a->x1 : b->x1;
    dest->x2 = a->x2 < b->x2 ? a->x2 : b->x2;
    dest->y1 = a->y1 > b->y1 ? a->y1 : b->y1;
    dest->y2 = a->y2 < b->y2 ? a->y2 : b->y2;
    if (dest->x1 >= dest->x2 || dest->y1 >= dest->y2)
	dest->x1 = dest->x2 = dest->y1 = dest->y2 = 0;
}

static void
i830_crtc_box (xf86CrtcPtr crtc, BoxPtr crtc_box)
{
    if (crtc->enabled)
    {
	crtc_box->x1 = crtc->x;
	crtc_box->x2 = crtc->x + xf86ModeWidth (&crtc->mode, crtc->rotation);
	crtc_box->y1 = crtc->y;
	crtc_box->y2 = crtc->y + xf86ModeHeight (&crtc->mode, crtc->rotation);
    }
    else
	crtc_box->x1 = crtc_box->x2 = crtc_box->y1 = crtc_box->y2 = 0;
}

static int
i830_box_area (BoxPtr box)
{
    return (int) (box->x2 - box->x1) * (int) (box->y2 - box->y1);
}

/*
 * Return the crtc covering 'box'. If two crtcs cover a portion of
 * 'box', then prefer 'desired'. If 'desired' is NULL, then prefer the crtc
 * with greater coverage
 */

static xf86CrtcPtr
i830_covering_crtc (ScrnInfoPtr pScrn,
		    BoxPtr	box,
		    xf86CrtcPtr desired,
		    BoxPtr	crtc_box_ret)
{
    xf86CrtcConfigPtr   xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
    xf86CrtcPtr		crtc, best_crtc;
    int			coverage, best_coverage;
    int			c;
    BoxRec		crtc_box, cover_box;

    best_crtc = NULL;
    best_coverage = 0;
    crtc_box_ret->x1 = 0;
    crtc_box_ret->x2 = 0;
    crtc_box_ret->y1 = 0;
    crtc_box_ret->y2 = 0;
    for (c = 0; c < xf86_config->num_crtc; c++)
    {
	crtc = xf86_config->crtc[c];
	i830_crtc_box (crtc, &crtc_box);
	i830_box_intersect (&cover_box, &crtc_box, box);
	coverage = i830_box_area (&cover_box);
	if (coverage && crtc == desired)
	{
	    *crtc_box_ret = crtc_box;
	    return crtc;
	}
	if (coverage > best_coverage)
	{
	    *crtc_box_ret = crtc_box;
	    best_crtc = crtc;
	    best_coverage = coverage;
	}
    }
    return best_crtc;
}

static int
i830_swidth (I830Ptr pI830, unsigned int offset,
	     unsigned int width, unsigned int mask, int shift)
{
    int	swidth = ((offset + width + mask) >> shift) - (offset >> shift);
    if (IS_I9XX(pI830))
	swidth <<= 1;
    swidth -= 1;
    return swidth << 2;
}

static void
i830_display_video(ScrnInfoPtr pScrn, xf86CrtcPtr crtc,
		   int id, short width, short height,
		   int dstPitch, int x1, int y1, int x2, int y2, BoxPtr dstBox,
		   short src_w, short src_h, short drw_w, short drw_h)
{
    I830Ptr		pI830 = I830PTR(pScrn);
    I830PortPrivPtr	pPriv = pI830->adaptor->pPortPrivates[0].ptr;
    I830OverlayRegPtr	overlay = I830OVERLAYREG(pI830);
    unsigned int	swidth, swidthy, swidthuv;
    unsigned int	mask, shift, offsety, offsetu;
    int			tmp;
    uint32_t		OCMD;
    Bool		scaleChanged = FALSE;

    OVERLAY_DEBUG("I830DisplayVideo: %dx%d (pitch %d)\n", width, height,
		  dstPitch);

#if VIDEO_DEBUG
    CompareOverlay(pI830, (uint32_t *) overlay, 0x100);
#endif
    
    /*
     * If the video isn't visible on any CRTC, turn it off
     */
    if (!crtc)
    {
	pPriv->current_crtc = NULL;
	i830_overlay_off (pScrn);
	return;
    }
    
    if (crtc != pPriv->current_crtc)
    {
	i830_overlay_switch_to_crtc (pScrn, crtc);
	if (pPriv->overlayOK) {
	    pPriv->current_crtc = crtc;
	    I830ResetVideo (pScrn);
	}
    }

    if (!pPriv->overlayOK)
	return;

    switch (crtc->rotation & 0xf) {
	/* for overlay, we should take it from crtc's screen
	 * coordinate to current crtc's display mode.
	 * yeah, a bit confusing.
	 */
    case RR_Rotate_0:
	dstBox->x1 -= crtc->x;
	dstBox->x2 -= crtc->x;
	dstBox->y1 -= crtc->y;
	dstBox->y2 -= crtc->y;
	break;
    case RR_Rotate_90:
	tmp = dstBox->x1;
	dstBox->x1 = dstBox->y1 - crtc->x;
	dstBox->y1 = pScrn->virtualX - tmp - crtc->y;
	tmp = dstBox->x2;
	dstBox->x2 = dstBox->y2 - crtc->x;
	dstBox->y2 = pScrn->virtualX - tmp - crtc->y;
	tmp = dstBox->y1;
	dstBox->y1 = dstBox->y2;
	dstBox->y2 = tmp;
	break;
    case RR_Rotate_180:
	tmp = dstBox->x1;
	dstBox->x1 = pScrn->virtualX - dstBox->x2 - crtc->x;
	dstBox->x2 = pScrn->virtualX - tmp - crtc->x;
	tmp = dstBox->y1;
	dstBox->y1 = pScrn->virtualY - dstBox->y2 - crtc->y;
	dstBox->y2 = pScrn->virtualY - tmp - crtc->y;
	break;
    case RR_Rotate_270:
	tmp = dstBox->x1;
	dstBox->x1 = pScrn->virtualY - dstBox->y1 - crtc->x;
	dstBox->y1 = tmp - crtc->y;
	tmp = dstBox->x2;
	dstBox->x2 = pScrn->virtualY - dstBox->y2 - crtc->x;
	dstBox->y2 = tmp - crtc->y;
	tmp = dstBox->x1;
	dstBox->x1 = dstBox->x2;
	dstBox->x2 = tmp;
	break;
    }

    if (crtc->rotation & (RR_Rotate_90 | RR_Rotate_270)) {
	tmp = width;
	width = height;
	height = tmp;
	tmp = drw_w;
	drw_w = drw_h;
	drw_h = tmp;
	tmp = src_w;
	src_w = src_h;
	src_h = tmp;
    }

    if (pPriv->oneLineMode) {
	/* change the coordinates with panel fitting active */
	dstBox->y1 = (((dstBox->y1 - 1) * pPriv->scaleRatio) >> 16) + 1;
	dstBox->y2 = ((dstBox->y2 * pPriv->scaleRatio) >> 16) + 1;

	/* Now, alter the height, so we scale to the correct size */
	drw_h = ((drw_h * pPriv->scaleRatio) >> 16) + 1;
    }

    if (IS_I9XX(pI830)) {
	shift = 6;
	mask = 0x3f;
    } else {
	shift = 5;
	mask = 0x1f;
    }

    if (pPriv->currentBuf == 0) {
	offsety = pPriv->YBuf0offset;
	offsetu = pPriv->UBuf0offset;
    } else {
	offsety = pPriv->YBuf1offset;
	offsetu = pPriv->UBuf1offset;
    }

    switch (id) {
    case FOURCC_YV12:
    case FOURCC_I420:
	overlay->SWIDTH = width | ((width/2 & 0x7ff) << 16);
	swidthy  = i830_swidth (pI830, offsety, width, mask, shift);
	swidthuv = i830_swidth (pI830, offsetu, width/2, mask, shift);
	overlay->SWIDTHSW = (swidthy) | (swidthuv << 16);
	overlay->SHEIGHT = height | ((height / 2) << 16);
	break;
    case FOURCC_UYVY:
    case FOURCC_YUY2:
    default:
	overlay->SWIDTH = width;
	swidth = ((offsety + (width << 1) + mask) >> shift) -
	(offsety >> shift);

	if (IS_I9XX(pI830))
	    swidth <<= 1;

	swidth -= 1;

	swidth <<= 2;
	
	OVERLAY_DEBUG("swidthsw is old %d new %d\n",
		      swidth,
		      i830_swidth (pI830, offsety, width << 1,
				   mask, shift));

	overlay->SWIDTHSW = swidth;
	overlay->SHEIGHT = height;
	break;
    }

    overlay->DWINPOS = (dstBox->y1 << 16) | dstBox->x1;

    overlay->DWINSZ = (((dstBox->y2 - dstBox->y1) << 16) |
		       (dstBox->x2 - dstBox->x1));

    OVERLAY_DEBUG("dstBox: x1: %d, y1: %d, x2: %d, y2: %d\n",
		  dstBox->x1, dstBox->y1, dstBox->x2, dstBox->y2);

    /* buffer locations */
    overlay->OBUF_0Y = pPriv->YBuf0offset;
    overlay->OBUF_0U = pPriv->UBuf0offset;
    overlay->OBUF_0V = pPriv->VBuf0offset;
    if(pPriv->doubleBuffer) {
	overlay->OBUF_1Y = pPriv->YBuf1offset;
	overlay->OBUF_1U = pPriv->UBuf1offset;
	overlay->OBUF_1V = pPriv->VBuf1offset;
    }

    OVERLAY_DEBUG("pos: 0x%x, size: 0x%x\n",
		  overlay->DWINPOS, overlay->DWINSZ);
    OVERLAY_DEBUG("dst: %d x %d, src: %d x %d\n", drw_w, drw_h, src_w, src_h);

    /* 
     * Calculate horizontal and vertical scaling factors and polyphase
     * coefficients.
     */

    {
	int xscaleInt, xscaleFract, yscaleInt, yscaleFract;
	int xscaleIntUV, xscaleFractUV;
	int yscaleIntUV, yscaleFractUV;
	/* UV is half the size of Y -- YUV420 */
	int uvratio = 2;
	uint32_t newval;
	coeffRec xcoeffY[N_HORIZ_Y_TAPS * N_PHASES];
	coeffRec xcoeffUV[N_HORIZ_UV_TAPS * N_PHASES];
	int i, j, pos;

	/*
	 * Y down-scale factor as a multiple of 4096.
	 */
	xscaleFract = ((src_w - 1) << 12) / drw_w;
	if (pI830->sync_fields) {
	    yscaleFract = (((src_h >> 1) - 1 + pI830->YScale_ftune) << 12) / drw_h;
	} else {
	    yscaleFract = ((src_h - 1) << 12) / drw_h;
	}

	/* Calculate the UV scaling factor. */
	xscaleFractUV = xscaleFract / uvratio;
	yscaleFractUV = yscaleFract / uvratio;

	/*
	 * To keep the relative Y and UV ratios exact, round the Y scales
	 * to a multiple of the Y/UV ratio.
	 */
	xscaleFract = xscaleFractUV * uvratio;
	yscaleFract = yscaleFractUV * uvratio;

	/* Integer (un-multiplied) values. */
	xscaleInt = xscaleFract >> 12;
	yscaleInt = yscaleFract >> 12;

	xscaleIntUV = xscaleFractUV >> 12;
	yscaleIntUV = yscaleFractUV >> 12;

	OVERLAY_DEBUG("xscale: %x.%03x, yscale: %x.%03x\n", xscaleInt,
		      xscaleFract & 0xFFF, yscaleInt, yscaleFract & 0xFFF);
	OVERLAY_DEBUG("UV xscale: %x.%03x, UV yscale: %x.%03x\n", xscaleIntUV,
		      xscaleFractUV & 0xFFF, yscaleIntUV, yscaleFractUV & 0xFFF);

	/* shouldn't get here */
	if (xscaleInt > 7) {
	    OVERLAY_DEBUG("xscale: bad scale\n");
	    return;
	}

	/* shouldn't get here */
	if (xscaleIntUV > 7) {
	    OVERLAY_DEBUG("xscaleUV: bad scale\n");
	    return;
	}

	newval = (xscaleInt << 16) |
	((xscaleFract & 0xFFF) << 3) | ((yscaleFract & 0xFFF) << 20);
	if (newval != overlay->YRGBSCALE) {
	    scaleChanged = TRUE;
	    overlay->YRGBSCALE = newval;
	}

	newval = (xscaleIntUV << 16) | ((xscaleFractUV & 0xFFF) << 3) |
	((yscaleFractUV & 0xFFF) << 20);
	if (newval != overlay->UVSCALE) {
	    scaleChanged = TRUE;
	    overlay->UVSCALE = newval;
	}

	newval = yscaleInt << 16 | yscaleIntUV;
	if (newval != overlay->UVSCALEV) {
	    scaleChanged = TRUE;
	    overlay->UVSCALEV = newval;
	}

	/* Recalculate coefficients if the scaling changed. */

	/*
	 * Only Horizontal coefficients so far.
	 */
	if (scaleChanged) {
	    double fCutoffY;
	    double fCutoffUV;

	    fCutoffY = xscaleFract / 4096.0;
	    fCutoffUV = xscaleFractUV / 4096.0;

	    /* Limit to between 1.0 and 3.0. */
	    if (fCutoffY < MIN_CUTOFF_FREQ)
		fCutoffY = MIN_CUTOFF_FREQ;
	    if (fCutoffY > MAX_CUTOFF_FREQ)
		fCutoffY = MAX_CUTOFF_FREQ;
	    if (fCutoffUV < MIN_CUTOFF_FREQ)
		fCutoffUV = MIN_CUTOFF_FREQ;
	    if (fCutoffUV > MAX_CUTOFF_FREQ)
		fCutoffUV = MAX_CUTOFF_FREQ;

	    UpdateCoeff(N_HORIZ_Y_TAPS, fCutoffY, TRUE, TRUE, xcoeffY);
	    UpdateCoeff(N_HORIZ_UV_TAPS, fCutoffUV, TRUE, FALSE, xcoeffUV);

	    for (i = 0; i < N_PHASES; i++) {
		for (j = 0; j < N_HORIZ_Y_TAPS; j++) {
		    pos = i * N_HORIZ_Y_TAPS + j;
		    overlay->Y_HCOEFS[pos] = (xcoeffY[pos].sign << 15 |
					      xcoeffY[pos].exponent << 12 |
					      xcoeffY[pos].mantissa);
		}
	    }
	    for (i = 0; i < N_PHASES; i++) {
		for (j = 0; j < N_HORIZ_UV_TAPS; j++) {
		    pos = i * N_HORIZ_UV_TAPS + j;
		    overlay->UV_HCOEFS[pos] = (xcoeffUV[pos].sign << 15 |
					       xcoeffUV[pos].exponent << 12 |
					       xcoeffUV[pos].mantissa);
		}
	    }
	}
    }
    if (pI830->sync_fields) {
	OCMD = OVERLAY_ENABLE | BUF_TYPE_FIELD | TVSYNC_FLIP_ENABLE;
	overlay->YRGB_VPH = pI830->YRGB_vphase;
	overlay->UV_VPH   = pI830->UV_vphase;
	overlay->HORZ_PH  = 0;
	overlay->INIT_PHS = 0;
    } else {
	OCMD = OVERLAY_ENABLE;
    }
    switch (id) {
    case FOURCC_YV12:
    case FOURCC_I420:
#ifdef INTEL_XVMC
    case FOURCC_XVMC:
#endif
	OVERLAY_DEBUG("YUV420\n");
#if 0
	/* set UV vertical phase to -0.25 */
	overlay->UV_VPH = 0x30003000;
#endif
	OVERLAY_DEBUG("UV stride is %d, Y stride is %d\n",
		      dstPitch, dstPitch * 2);
	overlay->OSTRIDE = (dstPitch * 2) | (dstPitch << 16);
	OCMD &= ~SOURCE_FORMAT;
	OCMD &= ~OV_BYTE_ORDER;
	OCMD |= YUV_420;
	break;
    case FOURCC_UYVY:
    case FOURCC_YUY2:
	OVERLAY_DEBUG("YUV422\n");
	overlay->OSTRIDE = dstPitch;
	OCMD &= ~SOURCE_FORMAT;
	OCMD |= YUV_422;
	OCMD &= ~OV_BYTE_ORDER;
	if (id == FOURCC_UYVY)
	    OCMD |= Y_SWAP;
	break;
    }

    OCMD &= ~(BUFFER_SELECT | FIELD_SELECT);
    if (pPriv->currentBuf == 0)
	OCMD |= BUFFER0;
    else
	OCMD |= BUFFER1;

    overlay->OCMD = OCMD;
    OVERLAY_DEBUG("OCMD is 0x%x\n", OCMD);

    /* make sure the overlay is on */
    i830_overlay_on (pScrn);
    /* and show this frame */
    i830_overlay_continue (pScrn, scaleChanged);
}

static Bool
i830_clip_video_helper (ScrnInfoPtr pScrn,
			xf86CrtcPtr *crtc_ret,
			BoxPtr	    dst,
			INT32	    *xa,
			INT32	    *xb,
			INT32	    *ya,
			INT32	    *yb,
			RegionPtr   reg,
			INT32	    width,
			INT32	    height)
{
    Bool	ret;
    RegionRec	crtc_region_local;
    RegionPtr	crtc_region = reg;
    
    /*
     * For overlay video, compute the relevant CRTC and
     * clip video to that
     */
    if (crtc_ret)
    {
	I830Ptr		pI830 = I830PTR(pScrn);
	I830PortPrivPtr pPriv = pI830->adaptor->pPortPrivates[0].ptr;
	BoxRec		crtc_box;
	xf86CrtcPtr	crtc = i830_covering_crtc (pScrn, dst,
						   pPriv->desired_crtc,
						   &crtc_box);
	
	if (crtc)
	{
	    REGION_INIT (pScreen, &crtc_region_local, &crtc_box, 1);
	    crtc_region = &crtc_region_local;
	    REGION_INTERSECT (pScreen, crtc_region, crtc_region, reg);
	}
	*crtc_ret = crtc;
    }
    ret = xf86XVClipVideoHelper (dst, xa, xb, ya, yb, 
				 crtc_region, width, height);
    if (crtc_region != reg)
	REGION_UNINIT (pScreen, &crtc_region_local);
    return ret;
}

static void
i830_fill_colorkey (ScreenPtr pScreen, uint32_t key, RegionPtr clipboxes)
{
   DrawablePtr root = &WindowTable[pScreen->myNum]->drawable;
   XID	       pval[2];
   BoxPtr      pbox = REGION_RECTS(clipboxes);
   int	       i, nbox = REGION_NUM_RECTS(clipboxes);
   xRectangle  *rects;
   GCPtr       gc;

   if(!xf86Screens[pScreen->myNum]->vtSema) return;

   gc = GetScratchGC(root->depth, pScreen);
   pval[0] = key;
   pval[1] = IncludeInferiors;
   (void) ChangeGC(gc, GCForeground|GCSubwindowMode, pval);
   ValidateGC(root, gc);

   rects = xalloc (nbox * sizeof(xRectangle));

   for(i = 0; i < nbox; i++, pbox++) 
   {
      rects[i].x = pbox->x1;
      rects[i].y = pbox->y1;
      rects[i].width = pbox->x2 - pbox->x1;
      rects[i].height = pbox->y2 - pbox->y1;
   }
   
   (*gc->ops->PolyFillRect)(root, gc, nbox, rects);
   
   xfree (rects);
   FreeScratchGC (gc);
}

/*
 * The source rectangle of the video is defined by (src_x, src_y, src_w, src_h).
 * The dest rectangle of the video is defined by (drw_x, drw_y, drw_w, drw_h).
 * id is a fourcc code for the format of the video.
 * buf is the pointer to the source data in system memory.
 * width and height are the w/h of the source data.
 * If "sync" is TRUE, then we must be finished with *buf at the point of return
 * (which we always are).
 * clipBoxes is the clipping region in screen space.
 * data is a pointer to our port private.
 * pDraw is a Drawable, which might not be the screen in the case of
 * compositing.  It's a new argument to the function in the 1.1 server.
 */
static int
I830PutImage(ScrnInfoPtr pScrn,
	     short src_x, short src_y,
	     short drw_x, short drw_y,
	     short src_w, short src_h,
	     short drw_w, short drw_h,
	     int id, unsigned char *buf,
	     short width, short height,
	     Bool sync, RegionPtr clipBoxes, pointer data,
	     DrawablePtr pDraw)
{
    I830Ptr pI830 = I830PTR(pScrn);
    I830PortPrivPtr pPriv = (I830PortPrivPtr) data;
    ScreenPtr pScreen = screenInfo.screens[pScrn->scrnIndex];
    I830OverlayRegPtr overlay;
    PixmapPtr pPixmap;
    INT32 x1, x2, y1, y2;
    int srcPitch, srcPitch2 = 0, dstPitch, destId;
    int top, left, npixels, nlines, size;
    BoxRec dstBox;
    int pitchAlignMask;
    int alloc_size, extraLinear;
    xf86CrtcPtr	crtc;

    if (pPriv->textured)
	overlay = NULL;
    else
	overlay = I830OVERLAYREG(pI830);

#if 0
    ErrorF("I830PutImage: src: (%d,%d)(%d,%d), dst: (%d,%d)(%d,%d)\n"
	   "width %d, height %d\n", src_x, src_y, src_w, src_h, drw_x, drw_y,
	   drw_w, drw_h, width, height);
#endif

    if (pI830->entityPrivate) {
	if (pI830->entityPrivate->XvInUse != -1 &&
	    pI830->entityPrivate->XvInUse != i830_crtc_pipe (pPriv->current_crtc)) {
#ifdef PANORAMIX
	    if (!noPanoramiXExtension) {
		return Success; /* faked for trying to share it */
	    } else
#endif
	    {
		return BadAlloc;
	    }
	}

	pI830->entityPrivate->XvInUse = i830_crtc_pipe (pPriv->current_crtc);;
    }

    /* Clamp dst width & height to 7x of src (overlay limit) */
    if(drw_w > (src_w * 7))
	drw_w = src_w * 7;

    if(drw_h > (src_h * 7))
	drw_h = src_h * 7;

    /* Clip */
    x1 = src_x;
    x2 = src_x + src_w;
    y1 = src_y;
    y2 = src_y + src_h;

    dstBox.x1 = drw_x;
    dstBox.x2 = drw_x + drw_w;
    dstBox.y1 = drw_y;
    dstBox.y2 = drw_y + drw_h;

    if (!i830_clip_video_helper(pScrn, 
				pPriv->textured ? NULL : &crtc,
				&dstBox, &x1, &x2, &y1, &y2, clipBoxes,
				width, height))
	return Success;

     if (!pPriv->textured) {
	 /* texture video handles rotation differently. */
	if (crtc)
	    pPriv->rotation = crtc->rotation;
	else {
	    xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		    "Fail to clip video to any crtc!\n");
	    return Success;
	}
     }

    destId = id;
    switch (id) {
    case FOURCC_YV12:
    case FOURCC_I420:
	srcPitch = (width + 0x3) & ~0x3;
	srcPitch2 = ((width >> 1) + 0x3) & ~0x3;
#ifdef INTEL_XVMC
        if (pI830->IsXvMCSurface) {
            srcPitch = (width + 0x3ff) & ~0x3ff;
            srcPitch2 = ((width >> 1) + 0x3ff) & ~0x3ff;
        }
#endif
	if (pPriv->textured && IS_I965G(pI830))
	    destId = FOURCC_YUY2;
	break;
    case FOURCC_UYVY:
    case FOURCC_YUY2:
    default:
	srcPitch = width << 1;
	break;
    }

    /* Only needs to be DWORD-aligned for textured on i915, but overlay has
     * stricter requirements.
     */
    if (pPriv->textured) {
	pitchAlignMask = 3;
    } else {
	if (IS_I965G(pI830))
	    pitchAlignMask = 255;
	else
	    pitchAlignMask = 63;
    }

    /* Determine the desired destination pitch (representing the chroma's pitch,
     * in the planar case.
     */
    switch (destId) {
    case FOURCC_YV12:
    case FOURCC_I420:
#ifdef INTEL_XVMC
    case FOURCC_XVMC:
#endif
	if (pPriv->rotation & (RR_Rotate_90 | RR_Rotate_270)) {
	    dstPitch = ((height / 2) + pitchAlignMask) & ~pitchAlignMask;
	    size = dstPitch * width * 3;
	} else {
	    dstPitch = ((width / 2) + pitchAlignMask) & ~pitchAlignMask;
	    size = dstPitch * height * 3;
	}
	break;
    case FOURCC_UYVY:
    case FOURCC_YUY2:

	if (pPriv->rotation & (RR_Rotate_90 | RR_Rotate_270)) {
	    dstPitch = ((height << 1) + pitchAlignMask) & ~pitchAlignMask;
	    size = dstPitch * width;
	} else {
	    dstPitch = ((width << 1) + pitchAlignMask) & ~pitchAlignMask;
	    size = dstPitch * height;
	}
	break;
    default:  
	dstPitch = 0;
	size = 0;
	break;
    }
#if 0
    ErrorF("srcPitch: %d, dstPitch: %d, size: %d\n", srcPitch, dstPitch, size);
#endif

    if (IS_I965G(pI830))
	extraLinear = BRW_LINEAR_EXTRA;
    else
	extraLinear = 0;

    alloc_size = size;
    if (pPriv->doubleBuffer)
	alloc_size *= 2;
    alloc_size += extraLinear;

    if (pPriv->buf) {
	/* Wait for any previous acceleration to the buffer to have completed.
	 * When we start using BOs for rendering, we won't have to worry
	 * because mapping or freeing will take care of it automatically.
	 */
	I830Sync(pScrn);
    }

    /* Free the current buffer if we're going to have to reallocate */
    if (pPriv->buf && pPriv->buf->size < alloc_size) {
	i830_free_memory(pScrn, pPriv->buf);
	pPriv->buf = NULL;
    }

    if (pPriv->buf == NULL) {
	pPriv->buf = i830_allocate_memory(pScrn, "xv buffer", alloc_size, 16,
					  0);
    }

    if (pPriv->buf == NULL)
	return BadAlloc;

    pPriv->extra_offset = pPriv->buf->offset +
    (pPriv->doubleBuffer ? size * 2 : size);

    /* fixup pointers */
    pPriv->YBuf0offset = pPriv->buf->offset;
    if (pPriv->rotation & (RR_Rotate_90 | RR_Rotate_270)) {
	pPriv->UBuf0offset = pPriv->YBuf0offset + (dstPitch * 2 * width);
	pPriv->VBuf0offset = pPriv->UBuf0offset + (dstPitch * width / 2);
	if(pPriv->doubleBuffer) {
	    pPriv->YBuf1offset = pPriv->YBuf0offset + size;
	    pPriv->UBuf1offset = pPriv->YBuf1offset + (dstPitch * 2 * width);
	    pPriv->VBuf1offset = pPriv->UBuf1offset + (dstPitch * width / 2);
	}
    } else {
	pPriv->UBuf0offset = pPriv->YBuf0offset + (dstPitch * 2 * height);
	pPriv->VBuf0offset = pPriv->UBuf0offset + (dstPitch * height / 2);
	if(pPriv->doubleBuffer) {
	    pPriv->YBuf1offset = pPriv->YBuf0offset + size;
	    pPriv->UBuf1offset = pPriv->YBuf1offset + (dstPitch * 2 * height);
	    pPriv->VBuf1offset = pPriv->UBuf1offset + (dstPitch * height / 2);
	}
    }

    /* Pick the idle buffer */
    if (!pPriv->textured && *pI830->overlayOn && pPriv->doubleBuffer)
	pPriv->currentBuf = !((INREG(DOVSTA) & OC_BUF) >> 20);

    /* copy data */
    top = y1 >> 16;
    left = (x1 >> 16) & ~1;
    npixels = ((((x2 + 0xffff) >> 16) + 1) & ~1) - left;

    switch (id) {
    case FOURCC_YV12:
    case FOURCC_I420:
	top &= ~1;
	nlines = ((((y2 + 0xffff) >> 16) + 1) & ~1) - top;
	if (pPriv->textured && IS_I965G(pI830)) {
	    I830CopyPlanarToPackedData(pScrn, pPriv, buf, srcPitch, srcPitch2,
				       dstPitch, height, top, left, nlines,
				       npixels, id);
	} else {
	    I830CopyPlanarData(pScrn, pPriv, buf, srcPitch, srcPitch2, dstPitch,
			       height, top, left, nlines, npixels, id);
	}
	break;
    case FOURCC_UYVY:
    case FOURCC_YUY2:
	nlines = ((y2 + 0xffff) >> 16) - top;
	I830CopyPackedData(pScrn, pPriv, buf, srcPitch, dstPitch, top, left,
			   nlines, npixels);
	break;
#ifdef INTEL_XVMC
    case FOURCC_XVMC:
	break;
#endif
    default:
	break;
    }

    if (pDraw->type == DRAWABLE_WINDOW) {
	pPixmap = (*pScreen->GetWindowPixmap)((WindowPtr)pDraw);
    } else {
	pPixmap = (PixmapPtr)pDraw;
    }

#ifdef I830_USE_EXA
    if (pPriv->textured && pI830->useEXA) {
	/* Force the pixmap into framebuffer so we can draw to it. */
	exaMoveInPixmap(pPixmap);
    }
#endif

    if (pPriv->textured && !pI830->useEXA &&
	    (((char *)pPixmap->devPrivate.ptr < (char *)pI830->FbBase) ||
	     ((char *)pPixmap->devPrivate.ptr >= (char *)pI830->FbBase +
	      pI830->FbMapSize))) {
	/* If the pixmap wasn't in framebuffer, then we have no way in XAA to
	 * force it there.  So, we simply refuse to draw and fail.
	 */
	return BadAlloc;
    }

    if (!pPriv->textured) {
	if (pI830->sync_fields) {
	    vga_sync_fields(pI830);
	}
	i830_display_video(pScrn, crtc, destId, width, height, dstPitch,
			   x1, y1, x2, y2, &dstBox, src_w, src_h,
			   drw_w, drw_h);
	
	/* update cliplist */
	if (!RegionsEqual(&pPriv->clip, clipBoxes)) {
	    REGION_COPY(pScrn->pScreen, &pPriv->clip, clipBoxes);
	    i830_fill_colorkey (pScreen, pPriv->colorKey, clipBoxes);
	}
    } else if (IS_I965G(pI830)) {
	I965DisplayVideoTextured(pScrn, pPriv, destId, clipBoxes, width, height,
				 dstPitch, x1, y1, x2, y2,
				 src_w, src_h, drw_w, drw_h, pPixmap);
    } else {
	I915DisplayVideoTextured(pScrn, pPriv, destId, clipBoxes, width, height,
				 dstPitch, x1, y1, x2, y2,
				 src_w, src_h, drw_w, drw_h, pPixmap);
    }
    if (pPriv->textured) {
	DamageDamageRegion(pDraw, clipBoxes);
    }

    pPriv->videoStatus = CLIENT_VIDEO_ON;

    return Success;
}

static int
I830QueryImageAttributes(ScrnInfoPtr pScrn,
			 int id,
			 unsigned short *w, unsigned short *h,
			 int *pitches, int *offsets, Bool textured)
{
    I830Ptr pI830 = I830PTR(pScrn);
    int size, tmp;

#if 0
    ErrorF("I830QueryImageAttributes: w is %d, h is %d\n", *w, *h);
#endif

    if (IS_845G(pI830) || IS_I830(pI830)) {
	if (*w > IMAGE_MAX_WIDTH_LEGACY)
	    *w = IMAGE_MAX_WIDTH_LEGACY;
	if (*h > IMAGE_MAX_HEIGHT_LEGACY)
	    *h = IMAGE_MAX_HEIGHT_LEGACY;
    } else {
	if (*w > IMAGE_MAX_WIDTH)
	    *w = IMAGE_MAX_WIDTH;
	if (*h > IMAGE_MAX_HEIGHT)
	    *h = IMAGE_MAX_HEIGHT;
    }

    *w = (*w + 1) & ~1;
    if (offsets)
	offsets[0] = 0;

    switch (id) {
	/* IA44 is for XvMC only */
    case FOURCC_IA44:
    case FOURCC_AI44:
	if (pitches)
	    pitches[0] = *w;
	size = *w * *h;
	break;
    case FOURCC_YV12:
    case FOURCC_I420:
	*h = (*h + 1) & ~1;
	size = (*w + 3) & ~3;
	if (pitches)
	    pitches[0] = size;
	size *= *h;
	if (offsets)
	    offsets[1] = size;
	tmp = ((*w >> 1) + 3) & ~3;
	if (pitches)
	    pitches[1] = pitches[2] = tmp;
	tmp *= (*h >> 1);
	size += tmp;
	if (offsets)
	    offsets[2] = size;
	size += tmp;
#if 0
	if (pitches)
	    ErrorF("pitch 0 is %d, pitch 1 is %d, pitch 2 is %d\n", pitches[0],
		   pitches[1], pitches[2]);
	if (offsets)
	    ErrorF("offset 1 is %d, offset 2 is %d\n", offsets[1], offsets[2]);
	if (offsets)
	    ErrorF("size is %d\n", size);
#endif
	break;
#ifdef INTEL_XVMC
    case FOURCC_XVMC:
        *h = (*h + 1) & ~1;
        size = sizeof(struct intel_xvmc_command);
        if (pitches)
            pitches[0] = size;
        break;
#endif
    case FOURCC_UYVY:
    case FOURCC_YUY2:
    default:
	size = *w << 1;
	if (pitches)
	    pitches[0] = size;
	size *= *h;
	break;
    }

    return size;
}

static int
I830QueryImageAttributesOverlay(ScrnInfoPtr pScrn,
				int id,
				unsigned short *w, unsigned short *h,
				int *pitches, int *offsets)
{
    return I830QueryImageAttributes(pScrn, id, w, h, pitches, offsets, FALSE);
}

static int
I830QueryImageAttributesTextured(ScrnInfoPtr pScrn,
				 int id,
				 unsigned short *w, unsigned short *h,
				 int *pitches, int *offsets)
{
    return I830QueryImageAttributes(pScrn, id, w, h, pitches, offsets, TRUE);
}

void
I830VideoBlockHandler(int i, pointer blockData, pointer pTimeout,
		      pointer pReadmask)
{
    ScrnInfoPtr pScrn = xf86Screens[i];
    I830Ptr pI830 = I830PTR(pScrn);
    I830PortPrivPtr pPriv;

    /* no overlay */
    if (pI830->adaptor == NULL)
        return;

    pPriv = GET_PORT_PRIVATE(pScrn);

    if (pPriv->videoStatus & TIMER_MASK) {
#if 1
	Time now = currentTime.milliseconds;
#else
	UpdateCurrentTime();
#endif
	if (pPriv->videoStatus & OFF_TIMER) {
	    if (pPriv->offTime < now) {
		/* Turn off the overlay */
		OVERLAY_DEBUG("BLOCKHANDLER\n");

		i830_overlay_off (pScrn);

		pPriv->videoStatus = FREE_TIMER;
		pPriv->freeTime = now + FREE_DELAY;

		if (pI830->entityPrivate)
		    pI830->entityPrivate->XvInUse = -1;
	    }
	} else {				/* FREE_TIMER */
	    if (pPriv->freeTime < now) {
		/* Sync before freeing the buffer, because the pages will be
		 * unbound.
		 */
		I830Sync(pScrn);
		i830_free_memory(pScrn, pPriv->buf);
		pPriv->buf = NULL;
		pPriv->videoStatus = 0;
	    }
	}
    }
}

/***************************************************************************
 * Offscreen Images
 ***************************************************************************/

typedef struct {
    i830_memory *buf;
    Bool isOn;
} OffscreenPrivRec, *OffscreenPrivPtr;

static int
I830AllocateSurface(ScrnInfoPtr pScrn,
		    int id,
		    unsigned short w,
		    unsigned short h, XF86SurfacePtr surface)
{
    int pitch, fbpitch, size;
    OffscreenPrivPtr pPriv;
    I830Ptr pI830 = I830PTR(pScrn);

    OVERLAY_DEBUG("I830AllocateSurface\n");

    if (IS_845G(pI830) || IS_I830(pI830)) {
	if ((w > IMAGE_MAX_WIDTH_LEGACY) || (h > IMAGE_MAX_HEIGHT_LEGACY))
	    return BadAlloc;
    } else {
	if ((w > IMAGE_MAX_WIDTH) || (h > IMAGE_MAX_HEIGHT))
	    return BadAlloc;
    }

    /* What to do when rotated ?? */
    if (pI830->rotation != RR_Rotate_0)
	return BadAlloc;

    if (!(surface->pitches = xalloc(sizeof(int))))
	return BadAlloc;
    if (!(surface->offsets = xalloc(sizeof(int)))) {
	xfree(surface->pitches);
	return BadAlloc;
    }
    if (!(pPriv = xalloc(sizeof(OffscreenPrivRec)))) {
	xfree(surface->pitches);
	xfree(surface->offsets);
	return BadAlloc;
    }

    w = (w + 1) & ~1;
    pitch = ((w << 1) + 15) & ~15;
    fbpitch = pI830->cpp * pScrn->displayWidth;
    size = pitch * h;

    pPriv->buf = i830_allocate_memory(pScrn, "xv surface buffer", size, 16, 0);
    if (pPriv->buf == NULL) {
	xfree(surface->pitches);
	xfree(surface->offsets);
	xfree(pPriv);
	return BadAlloc;
    }

    surface->width = w;
    surface->height = h;

    pPriv->isOn = FALSE;

    surface->pScrn = pScrn;
    surface->id = id;
    surface->pitches[0] = pitch;
    surface->offsets[0] = pPriv->buf->offset;
    surface->devPrivate.ptr = (pointer) pPriv;

    memset(pI830->FbBase + surface->offsets[0], 0, size);

    return Success;
}

static int
I830StopSurface(XF86SurfacePtr surface)
{
    OffscreenPrivPtr pPriv = (OffscreenPrivPtr) surface->devPrivate.ptr;
    ScrnInfoPtr pScrn = surface->pScrn;

    if (pPriv->isOn) {
	I830Ptr pI830 = I830PTR(pScrn);

	OVERLAY_DEBUG("StopSurface\n");

	i830_overlay_off (pScrn);

	if (pI830->entityPrivate)
	    pI830->entityPrivate->XvInUse = -1;

	pPriv->isOn = FALSE;
    }

    return Success;
}

static int
I830FreeSurface(XF86SurfacePtr surface)
{
    ScrnInfoPtr pScrn = surface->pScrn;
    OffscreenPrivPtr pPriv = (OffscreenPrivPtr) surface->devPrivate.ptr;

    I830StopSurface(surface);
    /* Sync before freeing the buffer, because the pages will be unbound. */
    I830Sync(pScrn);
    i830_free_memory(surface->pScrn, pPriv->buf);
    pPriv->buf = NULL;
    xfree(surface->pitches);
    xfree(surface->offsets);
    xfree(surface->devPrivate.ptr);

    return Success;
}

static int
I830GetSurfaceAttribute(ScrnInfoPtr pScrn, Atom attribute, INT32 * value)
{
    return I830GetPortAttribute(pScrn, attribute, value, NULL);
}

static int
I830SetSurfaceAttribute(ScrnInfoPtr pScrn, Atom attribute, INT32 value)
{
    return I830SetPortAttribute(pScrn, attribute, value, NULL);
}

static int
I830DisplaySurface(XF86SurfacePtr surface,
		   short src_x, short src_y,
		   short drw_x, short drw_y,
		   short src_w, short src_h,
		   short drw_w, short drw_h, RegionPtr clipBoxes)
{
    OffscreenPrivPtr pPriv = (OffscreenPrivPtr) surface->devPrivate.ptr;
    ScrnInfoPtr pScrn = surface->pScrn;
    ScreenPtr pScreen = screenInfo.screens[pScrn->scrnIndex];
    I830Ptr pI830 = I830PTR(pScrn);
    I830PortPrivPtr pI830Priv = GET_PORT_PRIVATE(pScrn);
    INT32 x1, y1, x2, y2;
    BoxRec dstBox;
    xf86CrtcPtr crtc;

    OVERLAY_DEBUG("I830DisplaySurface\n");

    if (pI830->entityPrivate) {
	if (pI830->entityPrivate->XvInUse != -1 &&
	    pI830->entityPrivate->XvInUse != i830_crtc_pipe (pI830Priv->current_crtc)) {
#ifdef PANORAMIX
	    if (!noPanoramiXExtension) {
		return Success; /* faked for trying to share it */
	    } else
#endif
	    {
		return BadAlloc;
	    }
	}

	pI830->entityPrivate->XvInUse = i830_crtc_pipe (pI830Priv->current_crtc);
    }

    x1 = src_x;
    x2 = src_x + src_w;
    y1 = src_y;
    y2 = src_y + src_h;

    dstBox.x1 = drw_x;
    dstBox.x2 = drw_x + drw_w;
    dstBox.y1 = drw_y;
    dstBox.y2 = drw_y + drw_h;

    if (!i830_clip_video_helper (pScrn, &crtc, &dstBox,
				 &x1, &x2, &y1, &y2, clipBoxes,
				 surface->width, surface->height))
	return Success;

    /* fixup pointers */
    pI830Priv->YBuf0offset = surface->offsets[0];
    pI830Priv->YBuf1offset = pI830Priv->YBuf0offset;

    /* Pick the idle buffer */
    if (!pI830Priv->textured && *pI830->overlayOn && pI830Priv->doubleBuffer) 
	pI830Priv->currentBuf = !((INREG(DOVSTA) & OC_BUF) >> 20);

    i830_display_video(pScrn, crtc, surface->id, surface->width, surface->height,
		     surface->pitches[0], x1, y1, x2, y2, &dstBox,
		     src_w, src_h, drw_w, drw_h);

    i830_fill_colorkey (pScreen, pI830Priv->colorKey, clipBoxes);

    pPriv->isOn = TRUE;
    /* we've prempted the XvImage stream so set its free timer */
    if (pI830Priv->videoStatus & CLIENT_VIDEO_ON) {
	REGION_EMPTY(pScrn->pScreen, &pI830Priv->clip);
	UpdateCurrentTime();
	pI830Priv->videoStatus = FREE_TIMER;
	pI830Priv->freeTime = currentTime.milliseconds + FREE_DELAY;
    }

    return Success;
}

static void
I830InitOffscreenImages(ScreenPtr pScreen)
{
    XF86OffscreenImagePtr offscreenImages;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    I830Ptr pI830 = I830PTR(pScrn);

    /* need to free this someplace */
    if (!(offscreenImages = xalloc(sizeof(XF86OffscreenImageRec)))) {
	return;
    }

    offscreenImages[0].image = &Images[0];
    offscreenImages[0].flags = VIDEO_OVERLAID_IMAGES /*| VIDEO_CLIP_TO_VIEWPORT*/;
    offscreenImages[0].alloc_surface = I830AllocateSurface;
    offscreenImages[0].free_surface = I830FreeSurface;
    offscreenImages[0].display = I830DisplaySurface;
    offscreenImages[0].stop = I830StopSurface;
    offscreenImages[0].setAttribute = I830SetSurfaceAttribute;
    offscreenImages[0].getAttribute = I830GetSurfaceAttribute;
    if (IS_845G(pI830) || IS_I830(pI830)) {
	offscreenImages[0].max_width = IMAGE_MAX_WIDTH_LEGACY;
	offscreenImages[0].max_height = IMAGE_MAX_HEIGHT_LEGACY;
    } else {
	offscreenImages[0].max_width = IMAGE_MAX_WIDTH;
	offscreenImages[0].max_height = IMAGE_MAX_HEIGHT; 
    }
    offscreenImages[0].num_attributes = 1;
    offscreenImages[0].attributes = Attributes;

    xf86XVRegisterOffscreenImages(pScreen, offscreenImages, 1);
}

void
i830_crtc_dpms_video(xf86CrtcPtr crtc, Bool on)
{
    ScrnInfoPtr pScrn = crtc->scrn;
    I830Ptr pI830 = I830PTR(pScrn);
    I830PortPrivPtr pPriv;

    /* no overlay */
    if (pI830->adaptor == NULL)
	return;

    pPriv = GET_PORT_PRIVATE(pScrn);

    if (crtc != pPriv->current_crtc)
	return;

    /* Check if it's the crtc the overlay is off */
    if (!on) {
	/* We stop the video when mode switching, so we don't lock up
	 * the engine. The overlayOK will determine whether we can re-enable
	 * with the current video on completion of the mode switch.
	 */
	I830StopVideo(pScrn, pPriv, TRUE);
	pPriv->current_crtc = NULL;
	pPriv->overlayOK = FALSE;
	pPriv->oneLineMode = FALSE;
    }
}

/* --- 8< --- */
/*
 * field cycle duration in usecs for PAL
 */
#define SYF_PAL_FIELD_CYCLE 20000

/*
 * frame cycle duration in usecs for PAL
 */
#define SYF_PAL_FRAME_CYCLE (SYF_PAL_FIELD_CYCLE << 1)

/*
 * dependent on interlaced (progressive) input frame rate 25 (50) fps
 * we set this to 0 (1). other params should adjust accordingly.
 */
#define SYF_INPUT_DOUBLE_RATE 0

/* NOT CURRENTLY USED
 * prefilter to prevent stray updates.
 * our software PLL will not try to lock for these.
 */
#define SYF_CATCH_RANGE (SYF_PAL_FRAME_CYCLE - 500 >> SYF_INPUT_DOUBLE_RATE)

/*
 * we average 25 (50) frames to yield a cycle time of
 * about one second for analysis of frame rate data.
 * this serves as frequency divider for our software PLL.
 */
#define SYF_PLL_DIVIDER (25 << SYF_INPUT_DOUBLE_RATE)

/*
 * input frame cycle duration in usecs for 25 (50) fps
 */
#define SYF_FRAME_CYCLE (SYF_PAL_FRAME_CYCLE >> SYF_INPUT_DOUBLE_RATE)

/* NOT CURRENTLY USED
 * we slightly displace sync point from center position. this makes the
 * system less susceptible to temporary field delivery irregularities.
 */
#define SYF_SYNC_POINT_OFFSET 0

/*
 * offset in usecs from double buffer switch where we preferredly try to place double
 * buffer updates. 
 * this minimizes sensivity to jitter of our software PLL phase comparator.
 */
#define SYF_SYNC_POINT (SYF_PAL_FIELD_CYCLE - SYF_SYNC_POINT_OFFSET)

/*
 * one trim increment compensates drift speed for about 29usec/sec.
 * this represents resolution of our VCO input. 
 */
#define SYF_MIN_STEP_USEC 700

/* NOT CURRENTLY USED
 * factor weighting drift against sync point displacement
 * when calculating overall compensation 
 */
#define SYF_DISP_DRIFT_FACTOR 1

/*
 * to allow for smooth adaption also on slower devices we delimit maximum
 * trim change per step size.
 * this implements some kind of low pass filter for our VCO input.
 */
#define SYF_MAX_TRIM_REL 1

/*
 * maximum absolute trim values allowed due to current
 * hardware/driver contraints.
 * this delimits 'maximum voltage' controlling our VCO.
 * currently allowed range is defined symmetrically around the center voltage.
 */
#define SYF_MAX_TRIM_ABS 2

#ifdef STANDALONE

#define DOVSTA          0x30008
#define HTOTAL_A        0x60000
#define HBLANK_A        0x60004
#define HSYNC_A         0x60008
#define PIPEA_DSL       0x70000
#define PIPEACONF       0x70008

#define ErrorF printf
#define FatalError ErrorF
#define B(a) (*(argv + (a)) ? *(argv + (a)) : 0)
#define INREG(reg) *(volatile unsigned *)(vptr + (reg))
#define OUTREG(reg, val) INREG(reg) = (val)
#define min(a, b) ((a) <= (b) ? (a) : (b))
#define max(a, b) ((a) >= (b) ? (a) : (b))
#define abs(a) ((a) >= 0 ? (a) : -(a))

#define VSF_SUB(a, b, c) \
    if ((a).tv_usec < (b).tv_usec) { \
        (c).tv_sec = (a).tv_sec - (b).tv_sec - 1; \
        (c).tv_usec = (a).tv_usec - (b).tv_usec + 1000000; \
    } else { \
        (c).tv_sec = (a).tv_sec - (b).tv_sec; \
        (c).tv_usec = (a).tv_usec - (b).tv_usec; \
    }

#define VSF_TV2USEC(a, b) \
    (b) = (a).tv_sec * 1000000 + (a).tv_usec;

struct _info_s {
    int SYF_debug;
} info = {
    801,
}, *pinfo = &info;

#else

#define B(a) (a)
#define pI830 pinfo

#endif

#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/shm.h>

#define SHMKEY 0x0815
#define OC_FIELD (1 << 19)
#define LFRAME (syf.lfield << 1)
#define SHIFTV (syf.lfield - syf.deadln)

typedef struct _syf_t {
    int log;
    int cnt; 
    int trim; 
    int drift; 
    int spoint;
    int *shm;

    /* HW specific */
    int triml;
    int lfield;
    int deadln;
    int dyzone;
    int tshift;
    int hsync_a;
    int htotal_a;
} syf_t;

extern void log_graph(int, int);

void
log_graph(val, symb)
{
    static char headr[] = "|<- -20ms                               0                              +20ms ->|";
    static char meter[sizeof(headr)];

    if (!symb || symb == 1) {
        time_t t; struct tm *tm;

        time(&t); tm = localtime(&t); 
        ErrorF("%02d:%02d:%02d %s", tm->tm_hour, tm->tm_min, tm->tm_sec, symb ? meter : headr);
        memset(meter, '-', sizeof(headr) - 1);
        return;
    }
    val = (SYF_SYNC_POINT + val) / 500;
    val = min(val, (int)sizeof(headr) - 2);
    val = max(val, 0);
    if (symb == ':') {
        meter[val] = meter[val] == '%' ? '#' : symb;
    } else if (symb == '*') {
        meter[val] = meter[val] == '%' ? '1' :
                     meter[val] == ':' ? '2' :
                     meter[val] == '#' ? '3' : symb;
    } else {
        meter[val] = symb;
    }
}
/* --- 8< --- */

void
vga_sync_fields(pinfo)
    I830Ptr pinfo;
{
    static syf_t syf;
    static int vbl_usec_prev = ~0;
    int dovsta, dovsta_1, pipea_dsl, pipea_dsl_1, vbl_usec;

/* --- 8< --- */
    if (vbl_usec_prev == ~0) {
        int shmid;

        syf.htotal_a = INREG(HTOTAL_A);
        syf.hsync_a = INREG(HSYNC_A);
        switch (syf.htotal_a & 0x0000ffff) {
          case 1439:
            syf.lfield = 313;
            syf.deadln = 287;
            syf.dyzone = 32;
            syf.tshift = 6;
            break;
          case 1599:
            syf.lfield = 619;
            syf.deadln = 599;
            syf.dyzone = 0;
            syf.tshift = 5;
            break;
          default:
            FatalError("vga_sync_fields: can't handle this timing\n");
        }
        if ((shmid = shmget(SHMKEY, sizeof(*syf.shm), IPC_CREAT | 0666)) < 0) {
            FatalError("shmget: %s\n", strerror(errno));
        }
        if ((int)(syf.shm = shmat(shmid, 0, 0)) == -1) {
            FatalError("shmat: %s\n", strerror(errno));
        }
    }

    /*
     * the next few lines implement a simple glitch detection/correction
     */
    dovsta = INREG(DOVSTA);
    pipea_dsl = INREG(PIPEA_DSL);
    dovsta_1 = INREG(DOVSTA);
    pipea_dsl_1 = INREG(PIPEA_DSL);
    if ((dovsta ^ dovsta_1) & OC_FIELD && pipea_dsl == pipea_dsl_1) {
        dovsta ^= ~0;
    }

    /*
     *  the chip does not provide current field status directly.
     *  but we can derive that from xor'ed dovsta and pipea_dsl contents.
     *      _______________                 ________
     * ____|               |_______________|         !(dovsta & OC_FIELD)
     * ____     ___________     ___________     ____
     *     |___|           |___|           |___|     pipea_dsl < DEADLN
     * ________           287   _______________
     *         |_______________|               |____ field status
     *         0              313
     *
     *  "1440x576_50i"     27.75   1440 1488 1609 1769   576  580  585  625  -hsync -vsync interlace
     *
     *  DOVSTA 0x80104000 PIPEA_DSL 0x00000000   0   0     0
     *  [...]
     *  DOVSTA 0x00104000 PIPEA_DSL 0x0000011e 286 286 18304
     *  DOVSTA 0x80085000 PIPEA_DSL 0x0000011f 287 287 18368
     *  [...]
     *  DOVSTA 0x80184000 PIPEA_DSL 0x00000138 312 312 19968
     *  DOVSTA 0x80184000 PIPEA_DSL 0x00000000   0 313 20032
     *  [...]
     *  DOVSTA 0x80184000 PIPEA_DSL 0x0000011e 286 599 38336
     *  DOVSTA 0x80105000 PIPEA_DSL 0x0000011f 287 600 38400
     *  [...]
     *  DOVSTA 0x80104000 PIPEA_DSL 0x00000138 312 625 40000
     *  DOVSTA 0x80104000 PIPEA_DSL 0x00000000   0   0     0
     *
     *  "1600x1200_50i"    65.92   1600 1696 1864 2131  1200 1203 1207 1238  -hsync +vsync interlace
     *
     *  DOVSTA 0x80104000 PIPEA_DSL 0x00000000   0    0     0
     *  [...]
     *  DOVSTA 0x80100000 PIPEA_DSL 0x00000256 598  598 19136
     *  DOVSTA 0x80185000 PIPEA_DSL 0x00000257 599  599 19168
     *  [...]
     *  DOVSTA 0x80184000 PIPEA_DSL 0x0000026a 618  618 19776
     *  DOVSTA 0x80180000 PIPEA_DSL 0x00000000   0  619 19808
     *  [...]
     *  DOVSTA 0x80184000 PIPEA_DSL 0x00000256 598 1217 38944
     *  DOVSTA 0x80105000 PIPEA_DSL 0x00000257 599 1218 38976
     *  [...]
     *  DOVSTA 0x80100000 PIPEA_DSL 0x0000026a 618 1237 39584
     *  DOVSTA 0x80104000 PIPEA_DSL 0x00000000   0    0     0
     */
    vbl_usec = ((pipea_dsl < syf.deadln ^ !(dovsta & OC_FIELD)) * syf.lfield + pipea_dsl + SHIFTV) % LFRAME << syf.tshift;

    if (vbl_usec_prev == ~0) {
        vbl_usec_prev = vbl_usec;
        log_graph(0, 0);
        ErrorF("      R\n");
#ifdef STANDALONE
        goto main_loop_end;
#else
        return;
#endif
    }
    if (pinfo->SYF_debug) {
        if (abs(vbl_usec - vbl_usec_prev) > pinfo->SYF_debug) {
            log_graph(vbl_usec - vbl_usec_prev, '%'); ++syf.log;
        }
        if (abs(vbl_usec - SYF_SYNC_POINT) > pinfo->SYF_debug) {
            log_graph(vbl_usec - SYF_SYNC_POINT, ':'); ++syf.log;
        }
    }

#ifndef STANDALONE

    /*
     * we must delay the next double buffer update
     * until even field has been processed. this occurs
     * after scan line 286 has been passed.
     * on a sufficiently synchronized system running at a
     * sync point of about 20000 usecs this imposes
     * no additional sleep time.
     *
     *  0      even       20000      odd      40000
     *  |-------------------|-------------------|
     *    ...---sleep--->|
     *                 18368
     */
    if (SYF_PAL_FIELD_CYCLE - vbl_usec > 0) {
        usleep(SYF_PAL_FIELD_CYCLE - vbl_usec + syf.dyzone);
    }
#endif
    syf.spoint += vbl_usec - SYF_SYNC_POINT;
    syf.drift += vbl_usec - vbl_usec_prev;
    vbl_usec_prev = vbl_usec;
    ++syf.cnt;
    if (!(syf.cnt % SYF_PLL_DIVIDER)) {
        syf.spoint /= SYF_PLL_DIVIDER;
        syf.trim = (syf.drift + syf.spoint / SYF_DISP_DRIFT_FACTOR) / SYF_MIN_STEP_USEC;
        syf.trim = max(syf.trim, -SYF_MAX_TRIM_REL);
        syf.trim = min(syf.trim,  SYF_MAX_TRIM_REL);

        if (pinfo->SYF_debug & 1 || syf.log) {
            log_graph(0, '+');
            log_graph(syf.spoint, '|');
            log_graph(syf.drift, '*');
            log_graph(0, 1);
            ErrorF("%7d %7d [%3d%+4d]\n",
                syf.drift, syf.spoint, syf.triml, syf.trim);
        }
        *syf.shm = syf.spoint;
        syf.spoint = 0;
        syf.drift = 0;
        syf.log = 0;
    }
    if (B(1) && syf.trim) {
        int t = syf.triml;

        if (syf.trim > 0) {
            t = min(t + 1,  SYF_MAX_TRIM_ABS);
            --syf.trim;
        } else {
            t = max(t - 1, -SYF_MAX_TRIM_ABS);
            ++syf.trim;
        }
        if (syf.triml != t) {
            syf.triml = t;
            OUTREG(HTOTAL_A,  syf.htotal_a + (t << 16 + 0));
            OUTREG(HBLANK_A,  syf.htotal_a + (t << 16 + 0));
            OUTREG(HSYNC_A,   syf.hsync_a  + (t << 16 + 1));
            OUTREG(PIPEACONF, INREG(PIPEACONF));
        }
    }
/* --- 8< --- */

}


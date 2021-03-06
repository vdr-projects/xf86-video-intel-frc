/*
 * Copyright © 2007 Intel Corporation
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
 *    Zhenyu Wang <zhenyu.z.wang@intel.com>
 *
 */
#ifndef INTEL_XVMC_H
#define INTEL_XVMC_H

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <assert.h>
#include <signal.h>

#include <xf86drm.h>
#include "i830_common.h"
#include "i830_hwmc.h"
#include <X11/X.h>
#include <X11/Xlibint.h>
#include <X11/Xutil.h>
#include <fourcc.h>
#include <X11/extensions/Xv.h>
#include <X11/extensions/Xvlib.h>
#include <X11/extensions/XvMC.h>
#include <X11/extensions/XvMClib.h>
#include <drm_sarea.h>

#include "xf86dri.h"

#include "intel_batchbuffer.h"

#define DEBUG 0

#define XVMC_ERR(s, arg...)					\
    do {							\
	fprintf(stderr, "[intel_xvmc] err: " s "\n", ##arg);	\
    } while (0)

#define XVMC_INFO(s, arg...)					\
    do {							\
	fprintf(stderr, "[intel_xvmc] info: " s "\n", ##arg);	\
    } while (0)

#define XVMC_DBG(s, arg...)						\
    do {								\
	if (DEBUG)							\
	    fprintf(stderr, "[intel_xvmc] debug: " s "\n", ##arg);	\
    } while (0)

/* Subpicture fourcc */
#define FOURCC_IA44 0x34344149

/*
  Definitions for temporary wire protocol hooks to be replaced
  when a HW independent libXvMC is created.
*/
extern Status _xvmc_create_context(Display *dpy, XvMCContext *context,
				   int *priv_count, CARD32 **priv_data);

extern Status _xvmc_destroy_context(Display *dpy, XvMCContext *context);

extern Status _xvmc_create_surface(Display *dpy, XvMCContext *context,
				   XvMCSurface *surface, int *priv_count,
				   CARD32 **priv_data);

extern Status _xvmc_destroy_surface(Display *dpy, XvMCSurface *surface);

extern Status  _xvmc_create_subpicture(Display *dpy, XvMCContext *context,
				       XvMCSubpicture *subpicture,
				       int *priv_count, uint **priv_data);

extern Status   _xvmc_destroy_subpicture(Display *dpy,
					 XvMCSubpicture *subpicture);

typedef struct _intel_xvmc_context {
    XvMCContext *context;
    drm_context_t hw_context;	/* context id to kernel drm */
    struct _intel_xvmc_context *next;
} intel_xvmc_context_t, *intel_xvmc_context_ptr;

typedef struct _intel_xvmc_surface {
    XvMCSurface *surface;
    XvImage *image;
    GC gc;
    Bool gc_init;
    Drawable last_draw;
    struct intel_xvmc_command data;
    struct _intel_xvmc_surface *next;
} intel_xvmc_surface_t, *intel_xvmc_surface_ptr;

typedef struct _intel_xvmc_drm_map {
    drm_handle_t handle;
    unsigned long offset;
    unsigned long size;
    unsigned long bus_addr;
    drmAddress map;
} intel_xvmc_drm_map_t, *intel_xvmc_drm_map_ptr;

typedef struct _intel_xvmc_driver {
    int type;			/* hw xvmc type - i830_hwmc.h */
    int screen;			/* current screen num*/

    int fd;			/* drm file handler */
    drm_handle_t hsarea;	/* DRI open connect */
    char busID[32];

    unsigned int sarea_size;
    drmAddress sarea_address;

    struct {
	unsigned int start_offset;
	unsigned int size;
	unsigned int space;
	unsigned char *ptr;
    } batch;

    struct
    {
        void *ptr;
        unsigned int size;
        unsigned int offset;
        unsigned int active_buf;
        unsigned int irq_emitted;
    } alloc;
    intel_xvmc_drm_map_t batchbuffer;
    unsigned int last_render;

    sigset_t sa_mask;
    pthread_mutex_t ctxmutex;
    int lock;   /* Lightweight lock to avoid locking twice */
    int locked;
    drmLock *driHwLock;

    int num_ctx;
    intel_xvmc_context_ptr ctx_list;
    int num_surf;
    intel_xvmc_surface_ptr surf_list;

    void *private;

    /* driver specific xvmc callbacks */
    Status (*create_context)(Display* display, XvMCContext *context,
	    int priv_count, CARD32 *priv_data);

    Status (*destroy_context)(Display* display, XvMCContext *context);

    Status (*create_surface)(Display* display, XvMCContext *context,
	    XvMCSurface *surface, int priv_count, CARD32 *priv_data);

    Status (*destroy_surface)(Display* display, XvMCSurface *surface);

    Status (*render_surface)(Display *display, XvMCContext *context,
	    unsigned int picture_structure,
	    XvMCSurface *target_surface,
	    XvMCSurface *past_surface,
	    XvMCSurface *future_surface,
	    unsigned int flags,
	    unsigned int num_macroblocks,
	    unsigned int first_macroblock,
	    XvMCMacroBlockArray *macroblock_array,
	    XvMCBlockArray *blocks);

    Status (*put_surface)(Display *display, XvMCSurface *surface,
	    Drawable draw, short srcx, short srcy,
	    unsigned short srcw, unsigned short srch,
	    short destx, short desty,
	    unsigned short destw, unsigned short desth,
	    int flags, struct intel_xvmc_command *data);

    Status (*get_surface_status)(Display *display, XvMCSurface *surface, int *stat);

    /* XXX more for vld */
} intel_xvmc_driver_t, *intel_xvmc_driver_ptr;

extern struct _intel_xvmc_driver i915_xvmc_mc_driver;
extern struct _intel_xvmc_driver *xvmc_driver;

#define SET_BLOCKED_SIGSET()   do {    \
        sigset_t bl_mask;                       \
        sigfillset(&bl_mask);           \
        sigdelset(&bl_mask, SIGFPE);    \
        sigdelset(&bl_mask, SIGILL);    \
        sigdelset(&bl_mask, SIGSEGV);   \
        sigdelset(&bl_mask, SIGBUS);    \
        sigdelset(&bl_mask, SIGKILL);   \
        pthread_sigmask(SIG_SETMASK, &bl_mask, &xvmc_driver->sa_mask); \
    } while (0)

#define RESTORE_BLOCKED_SIGSET() do {    \
        pthread_sigmask(SIG_SETMASK, &xvmc_driver->sa_mask, NULL); \
    } while (0)

#define PPTHREAD_MUTEX_LOCK() do {             \
        SET_BLOCKED_SIGSET();                  \
        pthread_mutex_lock(&xvmc_driver->ctxmutex);       \
    } while (0)

#define PPTHREAD_MUTEX_UNLOCK() do {           \
        pthread_mutex_unlock(&xvmc_driver->ctxmutex);     \
        RESTORE_BLOCKED_SIGSET();              \
    } while (0)

extern void LOCK_HARDWARE(drm_context_t);
extern void UNLOCK_HARDWARE(drm_context_t);

static inline const char* intel_xvmc_decoder_string(int flag)
{
    switch (flag) {
	case XVMC_I915_MPEG2_MC:
	    return "i915/945 MPEG2 MC decoder";
	case XVMC_I965_MPEG2_MC:
	    return "i965 MPEG2 MC decoder";
	case XVMC_I945_MPEG2_VLD:
	    return "i945 MPEG2 VLD decoder";
	case XVMC_I965_MPEG2_VLD:
	    return "i965 MPEG2 VLD decoder";
	default:
	    return "Unknown decoder";
    }
}

extern intel_xvmc_context_ptr intel_xvmc_find_context(XID id);
extern intel_xvmc_surface_ptr intel_xvmc_find_surface(XID id);

#endif

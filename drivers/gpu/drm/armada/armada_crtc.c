/*
 * Copyright (C) 2012 Russell King
 *  Rewritten from the dovefb driver, and Armada510 manuals.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/clk.h>
#include <drm/drmP.h>
#include <drm/drm_crtc_helper.h>
#include "armada_crtc.h"
#include "armada_drm.h"
#include "armada_fb.h"
#include "armada_gem.h"
#include "armada_hw.h"

struct armada_frame_work {
	struct drm_pending_vblank_event *event;
	struct armada_regs regs[4];
	struct drm_framebuffer *old_fb;
};

enum csc_mode {
	CSC_AUTO = 0,
	CSC_YUV_CCIR601 = 1,
	CSC_YUV_CCIR709 = 2,
	CSC_RGB_COMPUTER = 1,
	CSC_RGB_STUDIO = 2,
};

/*
 * A note about interlacing.  Let's consider HDMI 1920x1080i.
 * The timing parameters we have from X are:
 *  Hact HsyA HsyI Htot  Vact VsyA VsyI Vtot
 *  1920 2448 2492 2640  1080 1084 1094 1125
 * Which get translated to:
 *  Hact HsyA HsyI Htot  Vact VsyA VsyI Vtot
 *  1920 2448 2492 2640   540  542  547  562
 *
 * This is how it is defined by CEA-861-D - line and pixel numbers are
 * referenced to the rising edge of VSYNC and HSYNC.  Total clocks per
 * line: 2640.  The odd frame, the first active line is at line 21, and
 * the even frame, the first active line is 584.
 *
 * LN:    560     561     562     563             567     568    569
 * DE:    ~~~|____________________________//__________________________
 * HSYNC: ____|~|_____|~|_____|~|_____|~|_//__|~|_____|~|_____|~|_____
 * VSYNC: _________________________|~~~~~~//~~~~~~~~~~~~~~~|__________
 *  22 blanking lines.  VSYNC at 1320 (referenced to the HSYNC rising edge).
 *
 * LN:    1123   1124    1125      1               5       6      7
 * DE:    ~~~|____________________________//__________________________
 * HSYNC: ____|~|_____|~|_____|~|_____|~|_//__|~|_____|~|_____|~|_____
 * VSYNC: ____________________|~~~~~~~~~~~//~~~~~~~~~~|_______________
 *  23 blanking lines
 *
 * The Armada LCD Controller line and pixel numbers are, like X timings,
 * referenced to the top left of the active frame.
 *
 * So, translating these to our LCD controller:
 *  Odd frame, 563 total lines, VSYNC at line 543-548, pixel 1128.
 *  Even frame, 562 total lines, VSYNC at line 542-547, pixel 2448.
 * Note: Vsync front porch remains constant!
 *
 * if (odd_frame) {
 *   vtotal = mode->crtc_vtotal + 1;
 *   vbackporch = mode->crtc_vsync_start - mode->crtc_vdisplay + 1;
 *   vhorizpos = mode->crtc_hsync_start - mode->crtc_htotal / 2
 * } else {
 *   vtotal = mode->crtc_vtotal;
 *   vbackporch = mode->crtc_vsync_start - mode->crtc_vdisplay;
 *   vhorizpos = mode->crtc_hsync_start;
 * }
 * vfrontporch = mode->crtc_vtotal - mode->crtc_vsync_end;
 *
 * So, we need to reprogram these registers on each vsync event:
 *  LCD_SPU_V_PORCH, LCD_SPU_ADV_REG, LCD_SPUT_V_H_TOTAL
 *
 * Note: we do not use the frame done interrupts because these appear
 * to happen too early, and lead to jitter on the display (presumably
 * they occur at the end of the last active line, before the vsync back
 * porch, which we're reprogramming.)
 */

void
armada_drm_crtc_update_regs(struct armada_crtc *dcrtc, struct armada_regs *regs)
{
	while (regs->offset != ~0) {
		void __iomem *reg = dcrtc->base + regs->offset;
		uint32_t val;

		val = regs->mask;
		if (val != 0)
			val &= readl_relaxed(reg);
		writel_relaxed(val | regs->val, reg);
		++regs;
	}
}

#define dpms_blanked(dpms)	((dpms) != DRM_MODE_DPMS_ON)

static void armada_drm_crtc_update(struct armada_crtc *dcrtc)
{
	uint32_t dumb_ctrl;

	dumb_ctrl = dcrtc->cfg_dumb_ctrl;

	if (!dpms_blanked(dcrtc->dpms))
		dumb_ctrl |= CFG_DUMB_ENA;

	/*
	 * When the dumb interface isn't in DUMB24_RGB888_0 mode, it might
	 * be using SPI or GPIO.  If we set this to DUMB_BLANK, we will
	 * force LCD_D[23:0] to output blank color, overriding the GPIO or
	 * SPI usage.  So leave it as-is unless in DUMB24_RGB888_0 mode.
	 */
	if (dpms_blanked(dcrtc->dpms) &&
	    (dumb_ctrl & DUMB_MASK) == DUMB24_RGB888_0) {
		dumb_ctrl &= ~DUMB_MASK;
		dumb_ctrl |= DUMB_BLANK;
	}

	/*
	 * The documentation doesn't indicate what the normal state of
	 * the sync signals are.  Sebastian Hesselbart kindly probed
	 * these signals on his board to determine their state.
	 *
	 * The non-inverted state of the sync signals is active high.
	 * Setting these bits makes the appropriate signal active low.
	 */
	if (dcrtc->crtc.mode.flags & DRM_MODE_FLAG_NCSYNC)
		dumb_ctrl |= CFG_INV_CSYNC;
	if (dcrtc->crtc.mode.flags & DRM_MODE_FLAG_NHSYNC)
		dumb_ctrl |= CFG_INV_HSYNC;
	if (dcrtc->crtc.mode.flags & DRM_MODE_FLAG_NVSYNC)
		dumb_ctrl |= CFG_INV_VSYNC;

	if (dcrtc->dumb_ctrl != dumb_ctrl) {
		dcrtc->dumb_ctrl = dumb_ctrl;
		writel_relaxed(dumb_ctrl, dcrtc->base + LCD_SPU_DUMB_CTRL);
	}
}

static unsigned armada_drm_crtc_calc_fb(struct drm_framebuffer *fb,
	int x, int y, struct armada_regs *regs, bool interlaced)
{
	struct armada_gem_object *obj = drm_fb_obj(fb);
	unsigned pitch = fb->pitches[0];
	unsigned offset = y * pitch + x * fb->bits_per_pixel / 8;
	uint32_t addr_odd, addr_even;
	unsigned i = 0;

	DRM_DEBUG_DRIVER("pitch %u x %d y %d bpp %d\n",
		pitch, x, y, fb->bits_per_pixel);

	addr_odd = addr_even = obj->dev_addr + offset;

	if (interlaced) {
		addr_even += pitch;
		pitch *= 2;
	}

	/* write offset, base, and pitch */
	armada_reg_queue_set(regs, i, addr_odd, LCD_CFG_GRA_START_ADDR0);
	armada_reg_queue_set(regs, i, addr_even, LCD_CFG_GRA_START_ADDR1);
	armada_reg_queue_mod(regs, i, pitch, 0xffff, LCD_CFG_GRA_PITCH);

	return i;
}

static int armada_drm_crtc_queue_frame_work(struct armada_crtc *dcrtc,
	struct armada_frame_work *work)
{
	struct drm_device *dev = dcrtc->crtc.dev;
	unsigned long flags;
	int ret;

	ret = drm_vblank_get(dev, dcrtc->num);
	if (ret) {
		DRM_ERROR("failed to acquire vblank counter\n");
		return ret;
	}

	spin_lock_irqsave(&dev->event_lock, flags);
	if (!dcrtc->frame_work)
		dcrtc->frame_work = work;
	else
		ret = -EBUSY;
	spin_unlock_irqrestore(&dev->event_lock, flags);

	if (ret)
		drm_vblank_put(dev, dcrtc->num);

	return ret;
}

static void armada_drm_crtc_complete_frame_work(struct armada_crtc *dcrtc)
{
	struct drm_device *dev = dcrtc->crtc.dev;
	struct armada_frame_work *work = dcrtc->frame_work;

	dcrtc->frame_work = NULL;

	armada_drm_crtc_update_regs(dcrtc, work->regs);

	if (work->event)
		drm_send_vblank_event(dev, dcrtc->num, work->event);

	drm_vblank_put(dev, dcrtc->num);

	/* Finally, queue the process-half of the cleanup. */
	__armada_drm_queue_unref_work(dcrtc->crtc.dev, work->old_fb);
	kfree(work);
}

static void armada_drm_crtc_finish_fb(struct armada_crtc *dcrtc,
	struct drm_framebuffer *fb, bool force)
{
	struct armada_frame_work *work;

	if (!fb)
		return;

	if (force) {
		/* Display is disabled, so just drop the old fb */
		drm_framebuffer_unreference(fb);
		return;
	}

	work = kmalloc(sizeof(*work), GFP_KERNEL);
	if (work) {
		int i = 0;
		work->event = NULL;
		work->old_fb = fb;
		armada_reg_queue_end(work->regs, i);

		if (armada_drm_crtc_queue_frame_work(dcrtc, work) == 0)
			return;

		kfree(work);
	}

	/*
	 * Oops - just drop the reference immediately and hope for
	 * the best.  The worst that will happen is the buffer gets
	 * reused before it has finished being displayed.
	 */
	drm_framebuffer_unreference(fb);
}

static void armada_drm_vblank_off(struct armada_crtc *dcrtc)
{
	struct drm_device *dev = dcrtc->crtc.dev;

	/*
	 * Tell the DRM core that vblank IRQs aren't going to happen for
	 * a while.  This cleans up any pending vblank events for us.
	 */
	drm_vblank_off(dev, dcrtc->num);

	/* Handle any pending flip event. */
	spin_lock_irq(&dev->event_lock);
	if (dcrtc->frame_work)
		armada_drm_crtc_complete_frame_work(dcrtc);
	spin_unlock_irq(&dev->event_lock);
}

void armada_drm_crtc_gamma_set(struct drm_crtc *crtc, u16 r, u16 g, u16 b,
	int idx)
{
}

void armada_drm_crtc_gamma_get(struct drm_crtc *crtc, u16 *r, u16 *g, u16 *b,
	int idx)
{
}

/* The mode_config.mutex will be held for this call */
static void armada_drm_crtc_dpms(struct drm_crtc *crtc, int dpms)
{
	struct armada_crtc *dcrtc = drm_to_armada_crtc(crtc);

	if (dcrtc->dpms != dpms) {
		dcrtc->dpms = dpms;
		armada_drm_crtc_update(dcrtc);
		if (dpms_blanked(dpms))
			armada_drm_vblank_off(dcrtc);
	}
}

/*
 * Prepare for a mode set.  Turn off overlay to ensure that we don't end
 * up with the overlay size being bigger than the active screen size.
 * We rely upon X refreshing this state after the mode set has completed.
 *
 * The mode_config.mutex will be held for this call
 */
static void armada_drm_crtc_prepare(struct drm_crtc *crtc)
{
	struct armada_crtc *dcrtc = drm_to_armada_crtc(crtc);
	struct drm_plane *plane;

	/*
	 * If we have an overlay plane associated with this CRTC, disable
	 * it before the modeset to avoid its coordinates being outside
	 * the new mode parameters.  DRM doesn't provide help with this.
	 */
	plane = dcrtc->plane;
	if (plane) {
		struct drm_framebuffer *fb = plane->fb;

		plane->funcs->disable_plane(plane);
		plane->fb = NULL;
		plane->crtc = NULL;
		drm_framebuffer_unreference(fb);
	}
}

/* The mode_config.mutex will be held for this call */
static void armada_drm_crtc_commit(struct drm_crtc *crtc)
{
	struct armada_crtc *dcrtc = drm_to_armada_crtc(crtc);

	if (dcrtc->dpms != DRM_MODE_DPMS_ON) {
		dcrtc->dpms = DRM_MODE_DPMS_ON;
		armada_drm_crtc_update(dcrtc);
	}
}

/* The mode_config.mutex will be held for this call */
static bool armada_drm_crtc_mode_fixup(struct drm_crtc *crtc,
	const struct drm_display_mode *mode, struct drm_display_mode *adj)
{
	struct armada_private *priv = crtc->dev->dev_private;
	struct armada_crtc *dcrtc = drm_to_armada_crtc(crtc);
	int ret;

	/* We can't do interlaced modes if we don't have the SPU_ADV_REG */
	if (!priv->variant->has_spu_adv_reg &&
	    adj->flags & DRM_MODE_FLAG_INTERLACE)
		return false;

	/* Check whether the display mode is possible */
	ret = priv->variant->crtc_compute_clock(dcrtc, adj, NULL);
	if (ret)
		return false;

	return true;
}

void armada_drm_crtc_irq(struct armada_crtc *dcrtc, u32 stat)
{
	struct armada_vbl_event *e, *n;
	void __iomem *base = dcrtc->base;

	if (stat & DMA_FF_UNDERFLOW)
		DRM_ERROR("video underflow on crtc %u\n", dcrtc->num);
	if (stat & GRA_FF_UNDERFLOW)
		DRM_ERROR("graphics underflow on crtc %u\n", dcrtc->num);

	if (stat & VSYNC_IRQ)
		drm_handle_vblank(dcrtc->crtc.dev, dcrtc->num);

	spin_lock(&dcrtc->irq_lock);

	list_for_each_entry_safe(e, n, &dcrtc->vbl_list, node) {
		list_del_init(&e->node);
		drm_vblank_put(dcrtc->crtc.dev, dcrtc->num);
		e->fn(dcrtc, e->data);
	}

	if (stat & GRA_FRAME_IRQ && dcrtc->interlaced) {
		int i = stat & GRA_FRAME_IRQ0 ? 0 : 1;
		uint32_t val;

		writel_relaxed(dcrtc->v[i].spu_v_porch, base + LCD_SPU_V_PORCH);
		writel_relaxed(dcrtc->v[i].spu_v_h_total,
			       base + LCD_SPUT_V_H_TOTAL);

		val = readl_relaxed(base + LCD_SPU_ADV_REG);
		val &= ~(ADV_VSYNC_L_OFF | ADV_VSYNC_H_OFF | ADV_VSYNCOFFEN);
		val |= dcrtc->v[i].spu_adv_reg;
		writel_relaxed(val, dcrtc->base + LCD_SPU_ADV_REG);
	}
	spin_unlock(&dcrtc->irq_lock);

	if (stat & GRA_FRAME_IRQ) {
		struct drm_device *dev = dcrtc->crtc.dev;

		spin_lock(&dev->event_lock);
		if (dcrtc->frame_work)
			armada_drm_crtc_complete_frame_work(dcrtc);
		spin_unlock(&dev->event_lock);

		wake_up(&dcrtc->frame_wait);
	}
}

/* These are locked by dev->vbl_lock */
void armada_drm_crtc_disable_irq(struct armada_crtc *dcrtc, u32 mask)
{
	if (dcrtc->irq_ena & mask) {
		dcrtc->irq_ena &= ~mask;
		writel(dcrtc->irq_ena, dcrtc->base + LCD_SPU_IRQ_ENA);
	}
}

void armada_drm_crtc_enable_irq(struct armada_crtc *dcrtc, u32 mask)
{
	if ((dcrtc->irq_ena & mask) != mask) {
		dcrtc->irq_ena |= mask;
		writel(dcrtc->irq_ena, dcrtc->base + LCD_SPU_IRQ_ENA);
		if (readl_relaxed(dcrtc->base + LCD_SPU_IRQ_ISR) & mask)
			writel(0, dcrtc->base + LCD_SPU_IRQ_ISR);
	}
}

static uint32_t armada_drm_crtc_calculate_csc(struct armada_crtc *dcrtc)
{
	struct drm_display_mode *adj = &dcrtc->crtc.mode;
	uint32_t val = 0;

	if (dcrtc->csc_yuv_mode == CSC_YUV_CCIR709)
		val |= CFG_CSC_YUV_CCIR709;
	if (dcrtc->csc_rgb_mode == CSC_RGB_STUDIO)
		val |= CFG_CSC_RGB_STUDIO;

	/*
	 * In auto mode, set the colorimetry, based upon the HDMI spec.
	 * 1280x720p, 1920x1080p and 1920x1080i use ITU709, others use
	 * ITU601.  It may be more appropriate to set this depending on
	 * the source - but what if the graphic frame is YUV and the
	 * video frame is RGB?
	 */
	if ((adj->hdisplay == 1280 && adj->vdisplay == 720 &&
	     !(adj->flags & DRM_MODE_FLAG_INTERLACE)) ||
	    (adj->hdisplay == 1920 && adj->vdisplay == 1080)) {
		if (dcrtc->csc_yuv_mode == CSC_AUTO)
			val |= CFG_CSC_YUV_CCIR709;
	}

	/*
	 * We assume we're connected to a TV-like device, so the YUV->RGB
	 * conversion should produce a limited range.  We should set this
	 * depending on the connectors attached to this CRTC, and what
	 * kind of device they report being connected.
	 */
	if (dcrtc->csc_rgb_mode == CSC_AUTO)
		val |= CFG_CSC_RGB_STUDIO;

	return val;
}

/* The mode_config.mutex will be held for this call */
static int armada_drm_crtc_mode_set(struct drm_crtc *crtc,
	struct drm_display_mode *mode, struct drm_display_mode *adj,
	int x, int y, struct drm_framebuffer *old_fb)
{
	struct armada_private *priv = crtc->dev->dev_private;
	struct armada_crtc *dcrtc = drm_to_armada_crtc(crtc);
	struct armada_regs regs[17];
	uint32_t lm, rm, tm, bm, val, sclk;
	unsigned long flags;
	unsigned i;
	bool interlaced;

	drm_framebuffer_reference(crtc->fb);

	interlaced = !!(adj->flags & DRM_MODE_FLAG_INTERLACE);

	i = armada_drm_crtc_calc_fb(dcrtc->crtc.fb, x, y, regs, interlaced);

	rm = adj->crtc_hsync_start - adj->crtc_hdisplay;
	lm = adj->crtc_htotal - adj->crtc_hsync_end;
	bm = adj->crtc_vsync_start - adj->crtc_vdisplay;
	tm = adj->crtc_vtotal - adj->crtc_vsync_end;

	DRM_DEBUG_DRIVER("H: %d %d %d %d lm %d rm %d\n",
		adj->crtc_hdisplay,
		adj->crtc_hsync_start,
		adj->crtc_hsync_end,
		adj->crtc_htotal, lm, rm);
	DRM_DEBUG_DRIVER("V: %d %d %d %d tm %d bm %d\n",
		adj->crtc_vdisplay,
		adj->crtc_vsync_start,
		adj->crtc_vsync_end,
		adj->crtc_vtotal, tm, bm);

	/* Wait for pending flips to complete */
	wait_event(dcrtc->frame_wait, !dcrtc->frame_work);

	drm_vblank_pre_modeset(crtc->dev, dcrtc->num);

	crtc->mode = *adj;

	val = dcrtc->dumb_ctrl & ~CFG_DUMB_ENA;
	if (val != dcrtc->dumb_ctrl) {
		dcrtc->dumb_ctrl = val;
		writel_relaxed(val, dcrtc->base + LCD_SPU_DUMB_CTRL);
	}

	/* Now compute the divider for real */
	priv->variant->crtc_compute_clock(dcrtc, adj, &sclk);

	/* Ensure graphic fifo is enabled */
	armada_reg_queue_mod(regs, i, 0, CFG_PDWN64x66, LCD_SPU_SRAM_PARA1);
	armada_reg_queue_set(regs, i, sclk, LCD_CFG_SCLK_DIV);

	if (interlaced ^ dcrtc->interlaced) {
		if (adj->flags & DRM_MODE_FLAG_INTERLACE)
			drm_vblank_get(dcrtc->crtc.dev, dcrtc->num);
		else
			drm_vblank_put(dcrtc->crtc.dev, dcrtc->num);
		dcrtc->interlaced = interlaced;
	}

	spin_lock_irqsave(&dcrtc->irq_lock, flags);

	/* Even interlaced/progressive frame */
	dcrtc->v[1].spu_v_h_total = adj->crtc_vtotal << 16 |
				    adj->crtc_htotal;
	dcrtc->v[1].spu_v_porch = tm << 16 | bm;
	val = adj->crtc_hsync_start;
	dcrtc->v[1].spu_adv_reg = val << 20 | val | ADV_VSYNCOFFEN;

	if (interlaced) {
		/* Odd interlaced frame */
		dcrtc->v[0].spu_v_h_total = dcrtc->v[1].spu_v_h_total +
						(1 << 16);
		dcrtc->v[0].spu_v_porch = dcrtc->v[1].spu_v_porch + 1;
		val = adj->crtc_hsync_start - adj->crtc_htotal / 2;
		dcrtc->v[0].spu_adv_reg = val << 20 | val | ADV_VSYNCOFFEN;
	} else {
		dcrtc->v[0] = dcrtc->v[1];
	}

	val = adj->crtc_vdisplay << 16 | adj->crtc_hdisplay;

	armada_reg_queue_set(regs, i, val, LCD_SPU_V_H_ACTIVE);
	armada_reg_queue_set(regs, i, val, LCD_SPU_GRA_HPXL_VLN);
	armada_reg_queue_set(regs, i, val, LCD_SPU_GZM_HPXL_VLN);
	armada_reg_queue_set(regs, i, (lm << 16) | rm, LCD_SPU_H_PORCH);
	armada_reg_queue_set(regs, i, dcrtc->v[0].spu_v_porch, LCD_SPU_V_PORCH);
	armada_reg_queue_set(regs, i, dcrtc->v[0].spu_v_h_total,
			   LCD_SPUT_V_H_TOTAL);

	if (priv->variant->has_spu_adv_reg)
		armada_reg_queue_mod(regs, i, dcrtc->v[0].spu_adv_reg,
				     ADV_VSYNC_L_OFF | ADV_VSYNC_H_OFF |
				     ADV_VSYNCOFFEN, LCD_SPU_ADV_REG);

	val = CFG_GRA_ENA | CFG_GRA_HSMOOTH;
	val |= CFG_GRA_FMT(drm_fb_to_armada_fb(dcrtc->crtc.fb)->fmt);
	val |= CFG_GRA_MOD(drm_fb_to_armada_fb(dcrtc->crtc.fb)->mod);

	if (drm_fb_to_armada_fb(dcrtc->crtc.fb)->fmt > CFG_420)
		val |= CFG_PALETTE_ENA;

	if (interlaced)
		val |= CFG_GRA_FTOGGLE;

	armada_reg_queue_mod(regs, i, val, CFG_GRAFORMAT |
			     CFG_GRA_MOD(CFG_SWAPRB | CFG_SWAPUV |
					 CFG_SWAPYU | CFG_YUV2RGB) |
			     CFG_PALETTE_ENA | CFG_GRA_FTOGGLE,
			     LCD_SPU_DMA_CTRL0);

	val = adj->flags & DRM_MODE_FLAG_NVSYNC ? CFG_VSYNC_INV : 0;
	armada_reg_queue_mod(regs, i, val, CFG_VSYNC_INV, LCD_SPU_DMA_CTRL1);

	val = dcrtc->spu_iopad_ctrl | armada_drm_crtc_calculate_csc(dcrtc);
	armada_reg_queue_set(regs, i, val, LCD_SPU_IOPAD_CONTROL);
	armada_reg_queue_end(regs, i);

	armada_drm_crtc_update_regs(dcrtc, regs);
	spin_unlock_irqrestore(&dcrtc->irq_lock, flags);

	armada_drm_crtc_update(dcrtc);

	drm_vblank_post_modeset(crtc->dev, dcrtc->num);
	armada_drm_crtc_finish_fb(dcrtc, old_fb, dpms_blanked(dcrtc->dpms));

	return 0;
}

/* The mode_config.mutex will be held for this call */
static int armada_drm_crtc_mode_set_base(struct drm_crtc *crtc, int x, int y,
	struct drm_framebuffer *old_fb)
{
	struct armada_crtc *dcrtc = drm_to_armada_crtc(crtc);
	struct armada_regs regs[4];
	unsigned i;

	i = armada_drm_crtc_calc_fb(crtc->fb, crtc->x, crtc->y, regs,
				    dcrtc->interlaced);
	armada_reg_queue_end(regs, i);

	/* Wait for pending flips to complete */
	wait_event(dcrtc->frame_wait, !dcrtc->frame_work);

	/* Take a reference to the new fb as we're using it */
	drm_framebuffer_reference(crtc->fb);

	/* Update the base in the CRTC */
	armada_drm_crtc_update_regs(dcrtc, regs);

	/* Drop our previously held reference */
	armada_drm_crtc_finish_fb(dcrtc, old_fb, dpms_blanked(dcrtc->dpms));

	return 0;
}

static void armada_drm_crtc_load_lut(struct drm_crtc *crtc)
{
}

/* The mode_config.mutex will be held for this call */
static void armada_drm_crtc_disable(struct drm_crtc *crtc)
{
	struct armada_crtc *dcrtc = drm_to_armada_crtc(crtc);

	armada_drm_crtc_dpms(crtc, DRM_MODE_DPMS_OFF);
	armada_drm_crtc_finish_fb(dcrtc, crtc->fb, true);

	/* Power down most RAMs and FIFOs */
	writel_relaxed(CFG_PDWN256x32 | CFG_PDWN256x24 | CFG_PDWN256x8 |
		       CFG_PDWN32x32 | CFG_PDWN16x66 | CFG_PDWN32x66 |
		       CFG_PDWN64x66, dcrtc->base + LCD_SPU_SRAM_PARA1);
}

static const struct drm_crtc_helper_funcs armada_crtc_helper_funcs = {
	.dpms		= armada_drm_crtc_dpms,
	.prepare	= armada_drm_crtc_prepare,
	.commit		= armada_drm_crtc_commit,
	.mode_fixup	= armada_drm_crtc_mode_fixup,
	.mode_set	= armada_drm_crtc_mode_set,
	.mode_set_base	= armada_drm_crtc_mode_set_base,
	.load_lut	= armada_drm_crtc_load_lut,
	.disable	= armada_drm_crtc_disable,
};

static void armada_drm_crtc_destroy(struct drm_crtc *crtc)
{
	struct armada_crtc *dcrtc = drm_to_armada_crtc(crtc);
	struct armada_private *priv = crtc->dev->dev_private;

	priv->dcrtc[dcrtc->num] = NULL;
	drm_crtc_cleanup(&dcrtc->crtc);

	if (!IS_ERR(dcrtc->clk))
		clk_disable_unprepare(dcrtc->clk);

	kfree(dcrtc);
}

/*
 * The mode_config lock is held here, to prevent races between this
 * and a mode_set.
 */
static int armada_drm_crtc_page_flip(struct drm_crtc *crtc,
	struct drm_framebuffer *fb, struct drm_pending_vblank_event *event)
{
	struct armada_crtc *dcrtc = drm_to_armada_crtc(crtc);
	struct armada_frame_work *work;
	struct drm_device *dev = crtc->dev;
	unsigned long flags;
	unsigned i;
	int ret;

	/* We don't support changing the pixel format */
	if (fb->pixel_format != crtc->fb->pixel_format)
		return -EINVAL;

	work = kmalloc(sizeof(*work), GFP_KERNEL);
	if (!work)
		return -ENOMEM;

	work->event = event;
	work->old_fb = dcrtc->crtc.fb;

	i = armada_drm_crtc_calc_fb(fb, crtc->x, crtc->y, work->regs,
				    dcrtc->interlaced);
	armada_reg_queue_end(work->regs, i);

	/*
	 * Hold the old framebuffer for the work - DRM appears to drop our
	 * reference to the old framebuffer in drm_mode_page_flip_ioctl().
	 */
	drm_framebuffer_reference(work->old_fb);

	ret = armada_drm_crtc_queue_frame_work(dcrtc, work);
	if (ret) {
		/*
		 * Undo our reference above; DRM does not drop the reference
		 * to this object on error, so that's okay.
		 */
		drm_framebuffer_unreference(work->old_fb);
		kfree(work);
		return ret;
	}

	/*
	 * Don't take a reference on the new framebuffer;
	 * drm_mode_page_flip_ioctl() has already grabbed a reference and
	 * will _not_ drop that reference on successful return from this
	 * function.  Simply mark this new framebuffer as the current one.
	 */
	dcrtc->crtc.fb = fb;

	/*
	 * Finally, if the display is blanked, we won't receive an
	 * interrupt, so complete it now.
	 */
	if (dpms_blanked(dcrtc->dpms)) {
		spin_lock_irqsave(&dev->event_lock, flags);
		if (dcrtc->frame_work)
			armada_drm_crtc_complete_frame_work(dcrtc);
		spin_unlock_irqrestore(&dev->event_lock, flags);
	}

	return 0;
}

static int
armada_drm_crtc_set_property(struct drm_crtc *crtc,
	struct drm_property *property, uint64_t val)
{
	struct armada_private *priv = crtc->dev->dev_private;
	struct armada_crtc *dcrtc = drm_to_armada_crtc(crtc);
	bool update_csc = false;

	if (property == priv->csc_yuv_prop) {
		dcrtc->csc_yuv_mode = val;
		update_csc = true;
	} else if (property == priv->csc_rgb_prop) {
		dcrtc->csc_rgb_mode = val;
		update_csc = true;
	}

	if (update_csc) {
		uint32_t val;

		val = dcrtc->spu_iopad_ctrl |
		      armada_drm_crtc_calculate_csc(dcrtc);
		writel_relaxed(val, dcrtc->base + LCD_SPU_IOPAD_CONTROL);
	}

	return 0;
}

static struct drm_crtc_funcs armada_crtc_funcs = {
	.destroy	= armada_drm_crtc_destroy,
	.set_config	= drm_crtc_helper_set_config,
	.page_flip	= armada_drm_crtc_page_flip,
	.set_property	= armada_drm_crtc_set_property,
};

static struct drm_prop_enum_list armada_drm_csc_yuv_enum_list[] = {
	{ CSC_AUTO,        "Auto" },
	{ CSC_YUV_CCIR601, "CCIR601" },
	{ CSC_YUV_CCIR709, "CCIR709" },
};

static struct drm_prop_enum_list armada_drm_csc_rgb_enum_list[] = {
	{ CSC_AUTO,         "Auto" },
	{ CSC_RGB_COMPUTER, "Computer system" },
	{ CSC_RGB_STUDIO,   "Studio" },
};

static int armada_drm_crtc_create_properties(struct drm_device *dev)
{
	struct armada_private *priv = dev->dev_private;

	if (priv->csc_yuv_prop)
		return 0;

	priv->csc_yuv_prop = drm_property_create_enum(dev, 0,
				"CSC_YUV", armada_drm_csc_yuv_enum_list,
				ARRAY_SIZE(armada_drm_csc_yuv_enum_list));
	priv->csc_rgb_prop = drm_property_create_enum(dev, 0,
				"CSC_RGB", armada_drm_csc_rgb_enum_list,
				ARRAY_SIZE(armada_drm_csc_rgb_enum_list));

	if (!priv->csc_yuv_prop || !priv->csc_rgb_prop)
		return -ENOMEM;

	return 0;
}

int armada_drm_crtc_create(struct drm_device *dev, unsigned num,
	struct resource *res)
{
	struct armada_private *priv = dev->dev_private;
	struct armada_crtc *dcrtc;
	void __iomem *base;
	int ret;

	ret = armada_drm_crtc_create_properties(dev);
	if (ret)
		return ret;

	base = devm_request_and_ioremap(dev->dev, res);
	if (!base) {
		DRM_ERROR("failed to ioremap register\n");
		return -ENOMEM;
	}

	dcrtc = kzalloc(sizeof(*dcrtc), GFP_KERNEL);
	if (!dcrtc) {
		DRM_ERROR("failed to allocate Armada crtc\n");
		return -ENOMEM;
	}

	dcrtc->base = base;
	dcrtc->num = num;
	dcrtc->clk = ERR_PTR(-EINVAL);
	dcrtc->csc_yuv_mode = CSC_AUTO;
	dcrtc->csc_rgb_mode = CSC_AUTO;
	dcrtc->cfg_dumb_ctrl = DUMB24_RGB888_0;
	dcrtc->spu_iopad_ctrl = CFG_VSCALE_LN_EN | CFG_IOPAD_DUMB24;
	spin_lock_init(&dcrtc->irq_lock);
	dcrtc->irq_ena = CLEAN_SPU_IRQ_ISR;
	INIT_LIST_HEAD(&dcrtc->vbl_list);
	init_waitqueue_head(&dcrtc->frame_wait);

	/* Initialize some registers which we don't otherwise set */
	writel_relaxed(0x00000001, dcrtc->base + LCD_CFG_SCLK_DIV);
	writel_relaxed(0x00000000, dcrtc->base + LCD_SPU_BLANKCOLOR);
	writel_relaxed(dcrtc->spu_iopad_ctrl,
		       dcrtc->base + LCD_SPU_IOPAD_CONTROL);
	writel_relaxed(0x00000000, dcrtc->base + LCD_SPU_SRAM_PARA0);
	writel_relaxed(CFG_PDWN256x32 | CFG_PDWN256x24 | CFG_PDWN256x8 |
		       CFG_PDWN32x32 | CFG_PDWN16x66 | CFG_PDWN32x66 |
		       CFG_PDWN64x66, dcrtc->base + LCD_SPU_SRAM_PARA1);
	writel_relaxed(0x2032ff81, dcrtc->base + LCD_SPU_DMA_CTRL1);
	writel_relaxed(0x00000000, dcrtc->base + LCD_SPU_GRA_OVSA_HPXL_VLN);

	if (priv->variant->crtc_init) {
		ret = priv->variant->crtc_init(dcrtc);
		if (ret) {
			kfree(dcrtc);
			return ret;
		}
	}

	/* Ensure AXI pipeline is enabled */
	armada_updatel(CFG_ARBFAST_ENA, 0, dcrtc->base + LCD_SPU_DMA_CTRL0);

	priv->dcrtc[dcrtc->num] = dcrtc;

	drm_crtc_init(dev, &dcrtc->crtc, &armada_crtc_funcs);
	drm_crtc_helper_add(&dcrtc->crtc, &armada_crtc_helper_funcs);

	drm_object_attach_property(&dcrtc->crtc.base, priv->csc_yuv_prop,
				   dcrtc->csc_yuv_mode);
	drm_object_attach_property(&dcrtc->crtc.base, priv->csc_rgb_prop,
				   dcrtc->csc_rgb_mode);

	return armada_overlay_plane_create(dev, 1 << dcrtc->num);
}

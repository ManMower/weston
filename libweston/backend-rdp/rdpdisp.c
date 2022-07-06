/*
 * Copyright © 2020 Microsoft
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <linux/input.h>
#include <pthread.h>

#include <stdio.h>
#include <wchar.h>
#include <strings.h>

#include "rdp.h"

#include "libweston-internal.h"
#include "shared/xalloc.h"

#define rdp_disp_debug(b, ...) \
	weston_log_scope_printf(b->debug, __VA_ARGS__)

struct rdp_monitor_mode {
	rdpMonitor monitorDef; // in client coordinate.
	int scale; // per monitor DPI scaling.
	float clientScale;
	pixman_rectangle32_t rectWeston; // in weston coordinate.
};

struct monitor_private {
	struct weston_compositor *compositor;
	struct weston_log_scope *debug;
	pixman_region32_t regionClientHeads;
	pixman_region32_t regionWestonHeads;
	bool enable_hi_dpi_support;
	int debug_desktop_scaling_factor;
	bool enable_fractional_hi_dpi_support;
	bool enable_fractional_hi_dpi_roundup;
	struct weston_binding *debug_binding_M;
	struct weston_binding *debug_binding_W;
	struct wl_list head_list;
	uint32_t head_index;
	struct wl_list head_pending_list; // used during monitor layout change.
	struct wl_list head_move_pending_list; // used during monitor layout change.
};

struct rdp_head {
	struct weston_head base;

	uint32_t index;
	struct rdp_monitor_mode monitorMode;
	/*TODO: these region/rectangles can be moved to rdp_output */
	pixman_region32_t regionClient; // in client coordnate.
	pixman_region32_t regionWeston; // in weston coordnate.

	struct wl_list link; // rdp_backend::head_list
};

static struct rdp_head *
to_rdp_head(struct weston_head *base)
{
        return container_of(base, struct rdp_head, base);
}

static struct rdp_head *
rdp_head_create(struct monitor_private *mp, struct rdp_monitor_mode *monitorMode)
{
	struct rdp_head *head;
	char name[13] = {}; // 'rdp-' + 8 chars for hex uint32_t + NULL.

	assert(monitorMode);

	head = xzalloc(sizeof *head);
	if (!head)
		return NULL;
	head->index = mp->head_index++;
	head->monitorMode = *monitorMode;
	pixman_region32_init_rect(&head->regionClient,
				  monitorMode->monitorDef.x,
				  monitorMode->monitorDef.y,
				  monitorMode->monitorDef.width,
				  monitorMode->monitorDef.height);
	pixman_region32_init_rect(&head->regionWeston,
				  monitorMode->rectWeston.x,
				  monitorMode->rectWeston.y,
				  monitorMode->rectWeston.width,
				  monitorMode->rectWeston.height);
	if (monitorMode->monitorDef.is_primary) {
		rdp_disp_debug(mp, "Default head is being added\n");
	}
	wl_list_insert(&mp->head_list, &head->link);
	sprintf(name, "rdp-%x", head->index);

	weston_head_init(&head->base, name);
	weston_head_set_connection_status(&head->base, true);
	weston_compositor_add_head(mp->compositor, &head->base);

	return head;
}

static void
rdp_head_destroy(struct rdp_head *head)
{
	wl_list_remove(&head->link);
	pixman_region32_fini(&head->regionWeston);
	pixman_region32_fini(&head->regionClient);
	weston_head_release(&head->base);
	free(head);
}

static BOOL
is_line_intersected(int l1, int l2, int r1, int r2)
{
	int l = l1 > r1 ? l1 : r1;
	int r = l2 < r2 ? l2 : r2;
	return (l < r);
}

static int
compare_monitors_x(const void *p1, const void *p2)
{
	const struct rdp_monitor_mode *l = p1;
	const struct rdp_monitor_mode *r = p2;
	return l->monitorDef.x > r->monitorDef.x;
}

static int
compare_monitors_y(const void *p1, const void *p2)
{
	const struct rdp_monitor_mode *l = p1;
	const struct rdp_monitor_mode *r = p2;
	return l->monitorDef.y > r->monitorDef.y;
}

static float
disp_get_client_scale_from_monitor(struct monitor_private *mp, struct rdp_monitor_mode *monitorMode)
{
	if (mp->enable_hi_dpi_support) {
		if (mp->debug_desktop_scaling_factor)
			return (float)mp->debug_desktop_scaling_factor / 100.f;
		else if (mp->enable_fractional_hi_dpi_support)
			return (float)monitorMode->monitorDef.attributes.desktopScaleFactor / 100.0f;
		else if (mp->enable_fractional_hi_dpi_roundup)
			return (float)(int)((monitorMode->monitorDef.attributes.desktopScaleFactor + 50) / 100);
		else
			return (float)(int)(monitorMode->monitorDef.attributes.desktopScaleFactor / 100);
	} else {
		return 1.0f;
	}
}

static int
disp_get_output_scale_from_monitor(struct monitor_private *mp, struct rdp_monitor_mode *monitorMode)
{
	return (int)disp_get_client_scale_from_monitor(mp, monitorMode);
}

static void
disp_start_monitor_layout_change(struct monitor_private *mp, struct rdp_monitor_mode *monitorMode, UINT32 monitorCount, int *doneIndex)
{
	pixman_region32_clear(&mp->regionClientHeads);
	pixman_region32_clear(&mp->regionWestonHeads);
	/* move all heads to pending list */
	wl_list_init(&mp->head_pending_list);
	wl_list_insert_list(&mp->head_pending_list, &mp->head_list);
	wl_list_init(&mp->head_list);

	/* init move pending list */
	wl_list_init(&mp->head_move_pending_list);
	for (UINT32 i = 0; i < monitorCount; i++, monitorMode++) {
		struct rdp_head *current, *tmp;

		wl_list_for_each_safe(current, tmp, &mp->head_pending_list, link) {
			if (memcmp(&current->monitorMode, monitorMode, sizeof(*monitorMode)) == 0) {
				rdp_disp_debug(mp, "Head mode exact match:%s, x:%d, y:%d, width:%d, height:%d, is_primary: %d\n",
					       current->base.name,
					       current->monitorMode.monitorDef.x, current->monitorMode.monitorDef.y,
					       current->monitorMode.monitorDef.width, current->monitorMode.monitorDef.height,
					       current->monitorMode.monitorDef.is_primary);
				/* move from pending list to move pending list */
				wl_list_remove(&current->link);
				wl_list_insert(&mp->head_move_pending_list, &current->link);
				/* accumulate monitor layout */
				pixman_region32_union_rect(&mp->regionClientHeads, &mp->regionClientHeads,
					current->monitorMode.monitorDef.x, current->monitorMode.monitorDef.y,
					current->monitorMode.monitorDef.width, current->monitorMode.monitorDef.height);
				pixman_region32_union_rect(&mp->regionWestonHeads, &mp->regionWestonHeads,
					current->monitorMode.rectWeston.x, current->monitorMode.rectWeston.y,
					current->monitorMode.rectWeston.width, current->monitorMode.rectWeston.height);
				*doneIndex |= (1 << i);
				break;
			}
		}
	}
}

static void
disp_end_monitor_layout_change(struct monitor_private *mp)
{
	struct rdp_head *current, *next;

	/* move output to final location */
	wl_list_for_each_safe(current, next, &mp->head_move_pending_list, link) {
		/* move from move pending list to current list */
		wl_list_remove(&current->link);
		wl_list_insert(&mp->head_list, &current->link);
		if (current->base.output) {
			rdp_disp_debug(mp, "move head/output %s (%d,%d) -> (%d,%d)\n",
				       current->base.name,
				       current->base.output->x,
				       current->base.output->y,
				       current->monitorMode.rectWeston.x,
				       current->monitorMode.rectWeston.y);
			/* Notify clients for updated output position. */
			weston_output_move(current->base.output,
				current->monitorMode.rectWeston.x,
				current->monitorMode.rectWeston.y);
		} else {
			/* newly created head doesn't have output yet */
			/* position will be set at rdp_output_enable */
		}
	}
	assert(wl_list_empty(&mp->head_move_pending_list));
	wl_list_init(&mp->head_move_pending_list);
	/* remove all unsed head from pending list */
	if (!wl_list_empty(&mp->head_pending_list)) {
		wl_list_for_each_safe(current, next, &mp->head_pending_list, link)
			rdp_head_destroy(current);
		/* make sure nothing left in pending list */
		assert(wl_list_empty(&mp->head_pending_list));
		wl_list_init(&mp->head_pending_list);
	}
	/* make sure head list is not empty */
	assert(!wl_list_empty(&mp->head_list));

	BOOL is_primary_found = FALSE;
	wl_list_for_each(current, &mp->head_list, link) {
		if (current->monitorMode.monitorDef.is_primary) {
			rdp_disp_debug(mp, "client origin (0,0) is (%d,%d) in Weston space\n", 
				       current->monitorMode.rectWeston.x,
				       current->monitorMode.rectWeston.y);
			/* primary must be at (0,0) in client space */
			assert(current->monitorMode.monitorDef.x == 0);
			assert(current->monitorMode.monitorDef.y == 0);
			/* there must be only one primary */
			assert(is_primary_found == FALSE);
			is_primary_found = TRUE;
		}
	}
	rdp_disp_debug(mp, "client virtual desktop is (%d,%d) - (%d,%d)\n", 
		mp->regionClientHeads.extents.x1, mp->regionClientHeads.extents.y1,
		mp->regionClientHeads.extents.x2, mp->regionClientHeads.extents.y2);
	rdp_disp_debug(mp, "weston virtual desktop is (%d,%d) - (%d,%d)\n", 
		mp->regionWestonHeads.extents.x1, mp->regionWestonHeads.extents.y1,
		mp->regionWestonHeads.extents.x2, mp->regionWestonHeads.extents.y2);
}

static UINT
disp_set_monitor_layout_change(struct monitor_private *mp, struct rdp_monitor_mode *monitorMode)
{
	struct weston_output *output = NULL;
	struct weston_head *head = NULL;
	struct rdp_head *current;
	BOOL updateMode = FALSE;

	/* search for head matching configuration from pending list */
	wl_list_for_each(current, &mp->head_pending_list, link) {
		if (current->monitorMode.monitorDef.is_primary != monitorMode->monitorDef.is_primary)
			continue;

		if (current->monitorMode.monitorDef.width == monitorMode->monitorDef.width &&
			current->monitorMode.monitorDef.height == monitorMode->monitorDef.height &&
			current->monitorMode.scale == monitorMode->scale) {
			/* size mode (width/height/scale) */
			head = &current->base;
			output = head->output;
			break;
		} else if (current->monitorMode.monitorDef.x == monitorMode->monitorDef.x &&
			   current->monitorMode.monitorDef.y == monitorMode->monitorDef.y) {
			/* position match in client space */
			head = &current->base;
			output = head->output;
			updateMode = TRUE;
			break;
		}
	}
	if (!head) {
		/* just pick first one to change mode */
		wl_list_for_each(current, &mp->head_pending_list, link) {
			head = &current->base;
			output = head->output;
			updateMode = TRUE;
			break;
		}
	}

	if (head) {
		assert(output);
		assert(to_rdp_head(head) == current);
		rdp_disp_debug(mp, "Head mode change:%s OLD width:%d, height:%d, scale:%d, clientScale:%f\n",
			output->name, current->monitorMode.monitorDef.width,
				      current->monitorMode.monitorDef.height,
				      current->monitorMode.scale,
				      current->monitorMode.clientScale);
		/* reusing exising head */
		current->monitorMode = *monitorMode;
		/* update monitor region in client */
		pixman_region32_clear(&current->regionClient);
		pixman_region32_init_rect(&current->regionClient,
			monitorMode->monitorDef.x, monitorMode->monitorDef.y,
			monitorMode->monitorDef.width, monitorMode->monitorDef.height);
		pixman_region32_clear(&current->regionWeston);
		pixman_region32_init_rect(&current->regionWeston,
			monitorMode->rectWeston.x, monitorMode->rectWeston.y,
			monitorMode->rectWeston.width, monitorMode->rectWeston.height);
		/* move from pending list to move pending list */
		wl_list_remove(&current->link);
		wl_list_insert(&mp->head_move_pending_list, &to_rdp_head(head)->link);
	} else {
		/* no head found, create one */
		if (rdp_head_create(mp, monitorMode) == NULL)
			return ERROR_INTERNAL_ERROR;
	}

	if (updateMode) {
		if (output) {
			assert(head);
			/* ask weston to adjust size */
			struct weston_mode new_mode = {};
			new_mode.width = monitorMode->monitorDef.width;
			new_mode.height = monitorMode->monitorDef.height;
			rdp_disp_debug(mp, "Head mode change:%s NEW width:%d, height:%d, scale:%d, clientScale:%f\n",
				output->name, monitorMode->monitorDef.width,
					      monitorMode->monitorDef.height,
					      monitorMode->scale,
					      monitorMode->clientScale);
			if (output->scale != monitorMode->scale) {
				weston_output_disable(output);
				output->scale = 0; /* reset scale first, otherwise assert */
				weston_output_set_scale(output, monitorMode->scale);
				weston_output_enable(output);
			}
			weston_output_mode_set_native(output, &new_mode, monitorMode->scale);
			weston_head_set_physical_size(head,
				monitorMode->monitorDef.attributes.physicalWidth,
				monitorMode->monitorDef.attributes.physicalHeight);
			/* Notify clients for updated resolution/scale. */
			weston_output_set_transform(output, WL_OUTPUT_TRANSFORM_NORMAL);
			/* output size must match with monitor's rect in weston space */
			assert(output->width == (int32_t) monitorMode->rectWeston.width);
			assert(output->height == (int32_t) monitorMode->rectWeston.height);
		} else {
			/* if head doesn't have output yet, mode is set at rdp_output_set_size */
			rdp_disp_debug(mp, "output doesn't exist for head %s\n", head->name);
		}
	}

	/* accumulate monitor layout */
	pixman_region32_union_rect(&mp->regionClientHeads, &mp->regionClientHeads,
		monitorMode->monitorDef.x, monitorMode->monitorDef.y,
		monitorMode->monitorDef.width, monitorMode->monitorDef.height);
	pixman_region32_union_rect(&mp->regionWestonHeads, &mp->regionWestonHeads,
		monitorMode->rectWeston.x, monitorMode->rectWeston.y,
		monitorMode->rectWeston.width, monitorMode->rectWeston.height);

	return 0;
}

static BOOL
disp_monitor_validate_and_compute_layout(struct monitor_private *mp, struct rdp_monitor_mode *monitorMode, UINT32 monitorCount)
{
	bool isConnected_H = false;
	bool isConnected_V = false;
	bool isScalingUsed = false;
	bool isScalingSupported = true;
	uint32_t primaryCount = 0;
	int upperLeftX = 0;
	int upperLeftY = 0;
	uint32_t i;

	/* dump client monitor topology */
	rdp_disp_debug(mp, "%s:---INPUT---\n", __func__);
	for (i = 0; i < monitorCount; i++) {
		rdp_disp_debug(mp, "	rdpMonitor[%d]: x:%d, y:%d, width:%d, height:%d, is_primary:%d\n",
			i, monitorMode[i].monitorDef.x, monitorMode[i].monitorDef.y,
			   monitorMode[i].monitorDef.width, monitorMode[i].monitorDef.height,
			   monitorMode[i].monitorDef.is_primary);
		rdp_disp_debug(mp, "	rdpMonitor[%d]: physicalWidth:%d, physicalHeight:%d, orientation:%d\n",
			i, monitorMode[i].monitorDef.attributes.physicalWidth,
			   monitorMode[i].monitorDef.attributes.physicalHeight,
			   monitorMode[i].monitorDef.attributes.orientation);
		rdp_disp_debug(mp, "	rdpMonitor[%d]: desktopScaleFactor:%d, deviceScaleFactor:%d\n",
			i, monitorMode[i].monitorDef.attributes.desktopScaleFactor,
			   monitorMode[i].monitorDef.attributes.deviceScaleFactor);
		rdp_disp_debug(mp, "	rdpMonitor[%d]: scale:%d, client scale :%3.2f\n",
			i, monitorMode[i].scale, monitorMode[i].clientScale);
	}

	for (i = 0; i < monitorCount; i++) {
		/* make sure there is only one primary and its position at client */
		if (monitorMode[i].monitorDef.is_primary) {
			/* count number of primary */
			if (++primaryCount > 1) {
				weston_log("%s: RDP client reported unexpected primary count (%d)\n",__func__, primaryCount);
				return FALSE;
			}
			/* primary must be at (0,0) in client space */
			if (monitorMode[i].monitorDef.x != 0 || monitorMode[i].monitorDef.y != 0) {
				weston_log("%s: RDP client reported primary is not at (0,0) but (%d,%d).\n",
					__func__, monitorMode[i].monitorDef.x, monitorMode[i].monitorDef.y);
				return FALSE;
			}
		}

		/* check if any monitor has scaling enabled */
		if (monitorMode[i].clientScale != 1.0f)
			isScalingUsed = true;

		/* find upper-left corner of combined monitors in client space */
		if (upperLeftX > monitorMode[i].monitorDef.x)
			upperLeftX = monitorMode[i].monitorDef.x;
		if (upperLeftY > monitorMode[i].monitorDef.y)
			upperLeftY = monitorMode[i].monitorDef.y;
	}
	assert(upperLeftX <= 0);
	assert(upperLeftY <= 0);
	rdp_disp_debug(mp, "Client desktop upper left coordinate (%d,%d)\n", upperLeftX, upperLeftY);

	if (monitorCount > 1) {
		int32_t offsetFromOriginClient;

		/* first, sort monitors horizontally */
		qsort(monitorMode, monitorCount, sizeof(*monitorMode), compare_monitors_x);
		assert(upperLeftX == monitorMode[0].monitorDef.x);

		/* check if monitors are horizontally connected each other */
		offsetFromOriginClient = monitorMode[0].monitorDef.x + monitorMode[0].monitorDef.width;
		for (i = 1; i < monitorCount; i++) {
			if (offsetFromOriginClient != monitorMode[i].monitorDef.x) {
				rdp_disp_debug(mp, "\tRDP client reported monitors not horizontally connected each other at %d (x check)\n", i);
				break;
			}
			offsetFromOriginClient += monitorMode[i].monitorDef.width;

			if (!is_line_intersected(monitorMode[i-1].monitorDef.y,
						 monitorMode[i-1].monitorDef.y + monitorMode[i-1].monitorDef.height,
						 monitorMode[i].monitorDef.y,
						 monitorMode[i].monitorDef.y + monitorMode[i].monitorDef.height)) {
				rdp_disp_debug(mp, "\tRDP client reported monitors not horizontally connected each other at %d (y check)\n\n", i);
				break;
			}
		}
		if (i == monitorCount) {
			rdp_disp_debug(mp, "\tAll monitors are horizontally placed\n");
			isConnected_H = true;
		} else {
			/* next, trying sort monitors vertically */
			qsort(monitorMode, monitorCount, sizeof(*monitorMode), compare_monitors_y);
			assert(upperLeftY == monitorMode[0].monitorDef.y);

			/* make sure monitors are horizontally connected each other */
			offsetFromOriginClient = monitorMode[0].monitorDef.y + monitorMode[0].monitorDef.height;
			for (i = 1; i < monitorCount; i++) {
				if (offsetFromOriginClient != monitorMode[i].monitorDef.y) {
					rdp_disp_debug(mp, "\tRDP client reported monitors not vertically connected each other at %d (y check)\n", i);
					break;
				}
				offsetFromOriginClient += monitorMode[i].monitorDef.height;

				if (!is_line_intersected(monitorMode[i-1].monitorDef.x,
							 monitorMode[i-1].monitorDef.x + monitorMode[i-1].monitorDef.width,
							 monitorMode[i].monitorDef.x,
							 monitorMode[i].monitorDef.x + monitorMode[i].monitorDef.width)) {
					rdp_disp_debug(mp, "\tRDP client reported monitors not horizontally connected each other at %d (x check)\n\n", i);
					break;
				}
			}

			if (i == monitorCount) {
				rdp_disp_debug(mp, "\tAll monitors are vertically placed\n");
				isConnected_V = true;
			}
		}
	} else {
		isConnected_H = true;
	}

	if (isScalingUsed && (!isConnected_H && !isConnected_V)) {
		/* scaling can't be supported in complex monitor placement */
		weston_log("\nWARNING\nWARNING\nWARNING: Scaling is used, but can't be supported in complex monitor placement\nWARNING\nWARNING\n");
		isScalingSupported = false;
	}

	if (isScalingUsed && isScalingSupported) {
		uint32_t offsetFromOriginWeston = 0;
		for (i = 0; i < monitorCount; i++) {
			monitorMode[i].rectWeston.width = monitorMode[i].monitorDef.width / monitorMode[i].scale;
			monitorMode[i].rectWeston.height = monitorMode[i].monitorDef.height / monitorMode[i].scale;
			if (isConnected_H) {
				assert(isConnected_V == false);
				monitorMode[i].rectWeston.x = offsetFromOriginWeston;
				monitorMode[i].rectWeston.y = abs((upperLeftY - monitorMode[i].monitorDef.y) / monitorMode[i].scale);
				offsetFromOriginWeston += monitorMode[i].rectWeston.width;
			} else {
				assert(isConnected_V == true);
				monitorMode[i].rectWeston.x = abs((upperLeftX - monitorMode[i].monitorDef.x) / monitorMode[i].scale);
				monitorMode[i].rectWeston.y = offsetFromOriginWeston;
				offsetFromOriginWeston += monitorMode[i].rectWeston.height;
			}
			assert(monitorMode[i].rectWeston.x >= 0);
			assert(monitorMode[i].rectWeston.y >= 0);
		}
	} else {
		/* no scaling is used or monitor placement is too complex to scale in weston space, fallback to 1.0f */
		for (i = 0; i < monitorCount; i++) {
			monitorMode[i].rectWeston.width = monitorMode[i].monitorDef.width;
			monitorMode[i].rectWeston.height = monitorMode[i].monitorDef.height;
			monitorMode[i].rectWeston.x = monitorMode[i].monitorDef.x + abs(upperLeftX);
			monitorMode[i].rectWeston.y = monitorMode[i].monitorDef.y + abs(upperLeftY);
			assert(monitorMode[i].rectWeston.x >= 0);
			assert(monitorMode[i].rectWeston.y >= 0);
			monitorMode[i].scale = 1;
			monitorMode[i].clientScale = 1.0f;
		}
	}

	rdp_disp_debug(mp, "%s:---OUTPUT---\n", __func__);
	for (UINT32 i = 0; i < monitorCount; i++) {
		rdp_disp_debug(mp, "	rdpMonitor[%d]: x:%d, y:%d, width:%d, height:%d, is_primary:%d\n",
			i, monitorMode[i].monitorDef.x, monitorMode[i].monitorDef.y,
			   monitorMode[i].monitorDef.width, monitorMode[i].monitorDef.height,
			   monitorMode[i].monitorDef.is_primary);
		rdp_disp_debug(mp, "	rdpMonitor[%d]: weston x:%d, y:%d, width:%d, height:%d\n",
			i, monitorMode[i].rectWeston.x, monitorMode[i].rectWeston.y,
			   monitorMode[i].rectWeston.width, monitorMode[i].rectWeston.height);
		rdp_disp_debug(mp, "	rdpMonitor[%d]: physicalWidth:%d, physicalHeight:%d, orientation:%d\n",
			i, monitorMode[i].monitorDef.attributes.physicalWidth,
			   monitorMode[i].monitorDef.attributes.physicalHeight,
			   monitorMode[i].monitorDef.attributes.orientation);
		rdp_disp_debug(mp, "	rdpMonitor[%d]: desktopScaleFactor:%d, deviceScaleFactor:%d\n",
			i, monitorMode[i].monitorDef.attributes.desktopScaleFactor,
			   monitorMode[i].monitorDef.attributes.deviceScaleFactor);
		rdp_disp_debug(mp, "	rdpMonitor[%d]: scale:%d, clientScale:%3.2f\n",
			i, monitorMode[i].scale, monitorMode[i].clientScale);
	}

	return TRUE;
}

static void
print_matrix_type(FILE *fp, unsigned int type)
{
	fprintf(fp, "        matrix type: %x: ", type);
	if (type == 0) {
		fprintf(fp, "identify ");
	} else {
		if (type & WESTON_MATRIX_TRANSFORM_TRANSLATE)
			fprintf(fp, "translate ");
		if (type & WESTON_MATRIX_TRANSFORM_SCALE)
			fprintf(fp, "scale ");
		if (type & WESTON_MATRIX_TRANSFORM_ROTATE)
			fprintf(fp, "rotate ");
		if (type & WESTON_MATRIX_TRANSFORM_OTHER)
			fprintf(fp, "other ");
	}
	fprintf(fp, "\n");
}

static void
print_matrix(FILE *fp, const char *name, const struct weston_matrix *matrix)
{
	int i;
	if (name)
		fprintf(fp,"    %s\n", name);
	print_matrix_type(fp, matrix->type);
	for (i = 0; i < 4; i++)
		fprintf(fp,"        %8.2f, %8.2f, %8.2f, %8.2f\n",
			matrix->d[4*i+0], matrix->d[4*i+1], matrix->d[4*1+2], matrix->d[4*i+3]);
}

static void
print_rdp_head(FILE *fp, const struct rdp_head *current)
{
	fprintf(fp,"    rdp_head: %s: index:%d: is_primary:%d\n",
		current->base.name, current->index,
		current->monitorMode.monitorDef.is_primary);
	fprintf(fp,"    x:%d, y:%d, RDP client x:%d, y:%d\n",
		current->base.output->x, current->base.output->y,
		current->monitorMode.monitorDef.x, current->monitorMode.monitorDef.y);
	fprintf(fp,"    width:%d, height:%d, RDP client width:%d, height: %d\n",
		current->base.output->width, current->base.output->height,
		current->monitorMode.monitorDef.width, current->monitorMode.monitorDef.height);
	fprintf(fp,"    physicalWidth:%dmm, physicalHeight:%dmm, orientation:%d\n",
		current->monitorMode.monitorDef.attributes.physicalWidth,
		current->monitorMode.monitorDef.attributes.physicalHeight,
		current->monitorMode.monitorDef.attributes.orientation);
	fprintf(fp,"    desktopScaleFactor:%d, deviceScaleFactor:%d\n",
		current->monitorMode.monitorDef.attributes.desktopScaleFactor,
		current->monitorMode.monitorDef.attributes.deviceScaleFactor);
	fprintf(fp,"    scale:%d, client scale :%3.2f\n",
		current->monitorMode.scale, current->monitorMode.clientScale);
	fprintf(fp,"    regionClient: x1:%d, y1:%d, x2:%d, y2:%d\n",
		current->regionClient.extents.x1, current->regionClient.extents.y1,
		current->regionClient.extents.x2, current->regionClient.extents.y2);
	fprintf(fp,"    regionWeston: x1:%d, y1:%d, x2:%d, y2:%d\n",
		current->regionWeston.extents.x1, current->regionWeston.extents.y1,
		current->regionWeston.extents.x2, current->regionWeston.extents.y2);
	fprintf(fp,"    connected:%d, non_desktop:%d\n",
		current->base.connected, current->base.non_desktop);
	fprintf(fp,"    assigned output: %s\n",
		current->base.output ? current->base.output->name : "(no output)");
	if (current->base.output) {
		fprintf(fp,"    output extents box: x1:%d, y1:%d, x2:%d, y2:%d\n",
			current->base.output->region.extents.x1, current->base.output->region.extents.y1,
			current->base.output->region.extents.x2, current->base.output->region.extents.y2);
		fprintf(fp,"    output scale:%d, output native_scale:%d\n",
			current->base.output->scale, current->base.output->native_scale);
		print_matrix(fp, "global to output matrix:", &current->base.output->matrix);
		print_matrix(fp, "output to global matrix:", &current->base.output->inverse_matrix);
	}
}

static void
rdp_rail_dump_monitor_binding(struct weston_keyboard *keyboard,
			const struct timespec *time, uint32_t key, void *data)
{
	struct monitor_private *mp = data;

	if (mp) {
		struct rdp_head *current;
		int err;
		char *str;
		size_t len;
		FILE *fp = open_memstream(&str, &len);
		assert(fp);
		fprintf(fp,"\nrdp debug binding 'M' - dump all monitor.\n");
		wl_list_for_each(current, &mp->head_list, link) {
			print_rdp_head(fp, current);
			fprintf(fp,"\n");
		}
		err = fclose(fp);
		assert(err == 0);
		weston_log("%s", str);
		free(str);
	}
}

struct rdp_rail_dump_window_context {
	FILE *fp;
	RdpPeerContext *peerCtx;
};

static void
rdp_rail_dump_window_label(struct weston_surface *surface, char *label, uint32_t label_size)
{
	if (surface->get_label) {
		strcpy(label, "Label: "); // 7 chars
		surface->get_label(surface, label + 7, label_size - 7);
	} else if (surface->role_name) {
		snprintf(label, label_size, "RoleName: %s", surface->role_name);
	} else {
		strcpy(label, "(No Label, No Role name)");
	}
}

static void
rdp_rail_dump_window_iter(void *element, void *data)
{
	struct weston_surface *surface = (struct weston_surface *)element;
	struct weston_surface_rail_state *rail_state = (struct weston_surface_rail_state *)surface->backend_state;
	struct rdp_rail_dump_window_context *context = (struct rdp_rail_dump_window_context *)data;
	struct rdp_backend *b = context->peerCtx->rdpBackend;
	assert(rail_state); // this iter is looping from window hash table, thus it must have rail_state initialized.
	FILE *fp = context->fp;
	char label[256] = {};
	struct weston_geometry windowGeometry = {};
	struct weston_view *view;
	int contentBufferWidth, contentBufferHeight;
	weston_surface_get_content_size(surface, &contentBufferWidth, &contentBufferHeight);

	if (b->rdprail_shell_api &&
		b->rdprail_shell_api->get_window_geometry)
		b->rdprail_shell_api->get_window_geometry(surface, &windowGeometry);

	rdp_rail_dump_window_label(surface, label, sizeof(label));
	fprintf(fp,"    %s\n", label);
	fprintf(fp,"    WindowId:0x%x, SurfaceId:0x%x\n",
		rail_state->window_id, rail_state->surface_id);
	fprintf(fp,"    PoolId:0x%x, BufferId:0x%x\n",
		rail_state->pool_id, rail_state->buffer_id);
	fprintf(fp,"    Position x:%d, y:%d width:%d height:%d\n",
		rail_state->pos.x, rail_state->pos.y,
		rail_state->pos.width, rail_state->pos.height);
	fprintf(fp,"    RDP client position x:%d, y:%d width:%d height:%d\n",
		rail_state->clientPos.x, rail_state->clientPos.y,
		rail_state->clientPos.width, rail_state->clientPos.height);
	fprintf(fp,"    Window geometry x:%d, y:%d, width:%d height:%d\n",
		windowGeometry.x, windowGeometry.y,
		windowGeometry.width, windowGeometry.height);
	fprintf(fp,"    input extents: x1:%d, y1:%d, x2:%d, y2:%d\n",
		surface->input.extents.x1, surface->input.extents.y1,
		surface->input.extents.x2, surface->input.extents.y2);
	fprintf(fp,"    bufferWidth:%d, bufferHeight:%d\n",
		rail_state->bufferWidth, rail_state->bufferHeight);
	fprintf(fp,"    bufferScaleFactorWidth:%.2f, bufferScaleFactorHeight:%.2f\n",
		rail_state->bufferScaleFactorWidth, rail_state->bufferScaleFactorHeight);
	fprintf(fp,"    contentBufferWidth:%d, contentBufferHeight:%d\n",
		contentBufferWidth, contentBufferHeight);
	fprintf(fp,"    is_opaque:%d\n", surface->is_opaque);
	if (!surface->is_opaque && pixman_region32_not_empty(&surface->opaque)) {
		int numRects = 0;
		pixman_box32_t *rects = pixman_region32_rectangles(&surface->opaque, &numRects);
		fprintf(fp, "    opaque region: numRects:%d\n", numRects);
		for (int n = 0; n < numRects; n++)
			fprintf(fp, "        [%d]: (%d, %d) - (%d, %d)\n",
				n, rects[n].x1, rects[n].y1, rects[n].x2, rects[n].y2);
	}
	fprintf(fp,"    parent_surface:%p, isCursor:%d, isWindowCreated:%d\n",
		rail_state->parent_surface, rail_state->isCursor, rail_state->isWindowCreated);
	fprintf(fp,"    isWindowMinimized:%d, isWindowMinimizedRequested:%d\n",
		rail_state->is_minimized, rail_state->is_minimized_requested);
	fprintf(fp,"    isWindowMaximized:%d, isWindowMaximizedRequested:%d\n",
		rail_state->is_maximized, rail_state->is_maximized_requested);
	fprintf(fp,"    isWindowFullscreen:%d, isWindowFullscreenRequested:%d\n",
		rail_state->is_fullscreen, rail_state->is_fullscreen_requested);
	fprintf(fp,"    forceRecreateSurface:%d, error:%d\n",
		rail_state->forceRecreateSurface, rail_state->error);
	fprintf(fp,"    isUdatePending:%d, isFirstUpdateDone:%d\n",
		rail_state->isUpdatePending, rail_state->isFirstUpdateDone);
	fprintf(fp,"    surface:0x%p\n", surface);
	wl_list_for_each(view, &surface->views, surface_link) {
		fprintf(fp,"    view: %p\n", view);
		fprintf(fp,"    view's alpha: %3.2f\n", view->alpha);
		fprintf(fp,"    view's opaque region: x1:%d, y1:%d, x2:%d, y2:%d\n",
			view->transform.opaque.extents.x1,
			view->transform.opaque.extents.y1,
			view->transform.opaque.extents.x2,
			view->transform.opaque.extents.y2);
		if (pixman_region32_not_empty(&view->transform.opaque)) {
			int numRects = 0;
			pixman_box32_t *rects = pixman_region32_rectangles(&view->transform.opaque, &numRects);
			fprintf(fp,"    view's opaque region: numRects:%d\n", numRects);
			for (int n = 0; n < numRects; n++)
				fprintf(fp, "        [%d]: (%d, %d) - (%d, %d)\n",
					n, rects[n].x1, rects[n].y1, rects[n].x2, rects[n].y2);
		}
		fprintf(fp,"    view's boundingbox: x1:%d, y1:%d, x2:%d, y2:%d\n",
			view->transform.boundingbox.extents.x1,
			view->transform.boundingbox.extents.y1,
			view->transform.boundingbox.extents.x2,
			view->transform.boundingbox.extents.y2);
		fprintf(fp,"    view's scissor: x1:%d, y1:%d, x2:%d, y2:%d\n",
			view->geometry.scissor.extents.x1,
			view->geometry.scissor.extents.y1,
			view->geometry.scissor.extents.x2,
			view->geometry.scissor.extents.y2);
		fprintf(fp,"    view's transform: enabled:%d\n",
			view->transform.enabled);
		if (view->transform.enabled)
			print_matrix(fp, NULL, &view->transform.matrix);
	}
	print_matrix(fp, "buffer to surface matrix:", &surface->buffer_to_surface_matrix);
	print_matrix(fp, "surface to buffer matrix:", &surface->surface_to_buffer_matrix);
	fprintf(fp,"    output:0x%p (%s)\n", surface->output,
		surface->output ? surface->output->name : "(no output assigned)");
	if (surface->output) {
		struct weston_head *base_head;
		wl_list_for_each(base_head, &surface->output->head_list, output_link)
			print_rdp_head(fp, to_rdp_head(base_head));
	}
	fprintf(fp,"\n");
}

static void
rdp_rail_dump_window_binding(struct weston_keyboard *keyboard,
			const struct timespec *time, uint32_t key, void *data)
{
	struct rdp_backend *b = (struct rdp_backend *)data;
	RdpPeerContext *peerCtx;
	if (b && b->rdp_peer && b->rdp_peer->context) {
		/* print window from window hash table */
		struct rdp_rail_dump_window_context context;
		int err;
		char *str;
		size_t len;
		FILE *fp = open_memstream(&str, &len);
		assert(fp);
		fprintf(fp,"\nrdp debug binding 'W' - dump all window.\n");
		peerCtx = (RdpPeerContext *)b->rdp_peer->context;
		dump_id_manager_state(fp, &peerCtx->windowId, "windowId");
		dump_id_manager_state(fp, &peerCtx->surfaceId, "surfaceId");
#ifdef HAVE_FREERDP_GFXREDIR_H
		dump_id_manager_state(fp, &peerCtx->poolId, "poolId");
		dump_id_manager_state(fp, &peerCtx->bufferId, "bufferId");
#endif // HAVE_FREERDP_GFXREDIR_H
		context.peerCtx = peerCtx;
		context.fp = fp;
		rdp_id_manager_for_each(&peerCtx->windowId, rdp_rail_dump_window_iter, (void*)&context);
		err = fclose(fp);
		assert(err == 0);
		weston_log("%s", str);
		free(str);

		/* print out compositor's scene graph */
		str = weston_compositor_print_scene_graph(b->compositor);
		weston_log("%s", str);
		free(str);
	}
}

void *
init_multi_monitor(struct weston_compositor *comp, void *output_handler_config)
{
	struct rdp_output_handler_config *config = output_handler_config;
	struct monitor_private *mp;

	mp = xzalloc(sizeof *mp);
	mp->compositor = comp;
	pixman_region32_init(&mp->regionWestonHeads);
	pixman_region32_init(&mp->regionClientHeads);
	mp->debug = weston_log_ctx_add_log_scope(comp->weston_log_ctx,
						 "rdp-multihead",
						 "Debug messages from RDP multi-head\n",
						 NULL, NULL, NULL);

	rdp_disp_debug(mp, "RDP output handler: enable_hi_dpi_support = %d\n", config->enable_hi_dpi_support);
	mp->enable_hi_dpi_support = config->enable_hi_dpi_support;

	rdp_disp_debug(mp, "RDP output handler: debug_desktop_scaling_factor = %d\n", config->debug_desktop_scaling_factor);
	mp->debug_desktop_scaling_factor = config->debug_desktop_scaling_factor;

	rdp_disp_debug(mp, "RDP output handler: enable_fractional_hi_dpi_support = %d\n", config->enable_fractional_hi_dpi_support);
	mp->enable_fractional_hi_dpi_support = config->enable_fractional_hi_dpi_support;

	rdp_disp_debug(mp, "RDP output handler: enable_fractional_hi_dpi_roundup = %d\n", config->enable_fractional_hi_dpi_roundup);
	mp->enable_fractional_hi_dpi_roundup = config->enable_fractional_hi_dpi_roundup;

	/* M to dump all outstanding monitor info */
	mp->debug_binding_M = weston_compositor_add_debug_binding(mp->compositor, KEY_M,
								 rdp_rail_dump_monitor_binding, mp);
	/* W to dump all outstanding window info */
	mp->debug_binding_W = weston_compositor_add_debug_binding(mp->compositor, KEY_W,
								 rdp_rail_dump_window_binding, mp);
	/* Trigger to enter debug key : CTRL+SHIFT+SPACE */
        weston_install_debug_key_binding(mp->compositor, MODIFIER_CTRL);

	wl_list_init(&mp->head_list);

	return mp;
}

bool
rdp_disp_handle_adjust_monitor_layout(void *priv, int monitor_count, rdpMonitor *monitors)
{
	struct monitor_private *mp = priv;
	bool success = true;
	struct rdp_monitor_mode *monitorMode = NULL;
	int i;

	monitorMode = xmalloc(sizeof(struct rdp_monitor_mode) * monitor_count);

	for (i = 0; i < monitor_count; i++) {
		monitorMode[i].monitorDef = monitors[i];
		monitorMode[i].monitorDef.orig_screen = 0;
		monitorMode[i].scale = disp_get_output_scale_from_monitor(mp, &monitorMode[i]);
		monitorMode[i].clientScale = disp_get_client_scale_from_monitor(mp, &monitorMode[i]);
	}

	if (!disp_monitor_validate_and_compute_layout(mp, monitorMode, monitor_count)) {
		success = true;
		goto exit;
	}

	int doneIndex = 0;
	disp_start_monitor_layout_change(mp, monitorMode, monitor_count, &doneIndex);
	for (int i = 0; i < monitor_count; i++) {
		if ((doneIndex & (1 << i)) == 0)
			if (disp_set_monitor_layout_change(mp, &monitorMode[i]) != 0) {
				success = true;
				goto exit;
			}
	}
	disp_end_monitor_layout_change(mp);

exit:
	free(monitorMode);
	return success;
}

static inline void
to_weston_scale_only(struct weston_output *output, float scale, int *x, int *y)
{
	//rdp_matrix_transform_scale(&output->inverse_matrix, x, y);
	/* TODO: built-in to matrix */
	*x = (float)(*x) * scale;
	*y = (float)(*y) * scale;
}

/* Input x/y in client space, output x/y in weston space */
struct weston_output *
to_weston_coordinate(RdpPeerContext *peerContext, int32_t *x, int32_t *y, uint32_t *width, uint32_t *height)
{
	struct rdp_backend *b = peerContext->rdpBackend;
	struct monitor_private *mp = b->monitor_private;
	int sx = *x, sy = *y;
	struct rdp_head *head_iter;

	/* First, find which monitor contains this x/y. */
	wl_list_for_each(head_iter, &mp->head_list, link) {
		if (pixman_region32_contains_point(&head_iter->regionClient, sx, sy, NULL)) {
			struct weston_output *output = head_iter->base.output;
			float scale = 1.0f / head_iter->monitorMode.clientScale;

			/* translate x/y to offset from this output on client space. */
			sx -= head_iter->monitorMode.monitorDef.x;
			sy -= head_iter->monitorMode.monitorDef.y;
			/* scale x/y to client output space. */
			to_weston_scale_only(output, scale, &sx, &sy);
			if (width && height)
				to_weston_scale_only(output, scale, width, height);
			/* translate x/y to offset from this output on weston space. */
			sx += head_iter->monitorMode.rectWeston.x;
			sy += head_iter->monitorMode.rectWeston.y;
			rdp_disp_debug(mp, "%s: (x:%d, y:%d) -> (sx:%d, sy:%d) at head:%s\n",
				       __func__, *x, *y, sx, sy, head_iter->base.name);
			*x = sx;
			*y = sy;
			return output; // must be only 1 head per output.
		}
	}
	/* x/y is outside of any monitors. */
	return NULL;
}

static inline void
to_client_scale_only(struct weston_output *output, float scale, int *x, int *y)
{
	//rdp_matrix_transform_scale(&output->matrix, x, y);
	/* TODO: built-in to matrix */
	*x = (float)(*x) * scale;
	*y = (float)(*y) * scale;
}

/* Input x/y in weston space, output x/y in client space */
void
to_client_coordinate(RdpPeerContext *peerContext, struct weston_output *output, int32_t *x, int32_t *y, uint32_t *width, uint32_t *height)
{
	struct monitor_private *mp = peerContext->rdpBackend->monitor_private;
	int sx = *x, sy = *y;
	struct weston_head *head_iter;

	/* Pick first head from output. */
	wl_list_for_each(head_iter, &output->head_list, output_link) {
		struct rdp_head *head = to_rdp_head(head_iter);
		float scale = head->monitorMode.clientScale;

		/* translate x/y to offset from this output on weston space. */
		sx -= head->monitorMode.rectWeston.x;
		sy -= head->monitorMode.rectWeston.y;
		/* scale x/y to client output space. */
		to_client_scale_only(output, scale, &sx, &sy);
		if (width && height)
			to_client_scale_only(output, scale, width, height);
		/* translate x/y to offset from this output on client space. */
		sx += head->monitorMode.monitorDef.x;
		sy += head->monitorMode.monitorDef.y;
		rdp_disp_debug(mp, "%s: (x:%d, y:%d) -> (sx:%d, sy:%d) at head:%s\n",
			       __func__, *x, *y, sx, sy, head_iter->name);
		*x = sx;
		*y = sy;
		return; // must be only 1 head per output.
	}
}

void
get_client_extents(void *priv, int32_t *x1, int32_t *y1, int32_t *x2, int32_t *y2)
{
	struct monitor_private *mp = priv;

	if (x1)
		*x1 = mp->regionClientHeads.extents.x1;
	if (y1)
		*y1 = mp->regionClientHeads.extents.y1;
	if (x2)
		*x2 = mp->regionClientHeads.extents.x2;
	if (y2)
		*y2 = mp->regionClientHeads.extents.y2;
}

void
free_private(void **priv)
{
	struct weston_head *base, *next;
	struct monitor_private *mp = *priv;
	struct weston_compositor *ec = mp->compositor;

	wl_list_for_each_safe(base, next, &ec->head_list, compositor_link)
		rdp_head_destroy(to_rdp_head(base));

	assert(wl_list_empty(&mp->head_list));

	pixman_region32_fini(&mp->regionClientHeads);
	pixman_region32_fini(&mp->regionWestonHeads);
	weston_log_scope_destroy(mp->debug);
	free(mp);
	*priv = NULL;
}

int
rdp_output_get_config(struct weston_output *base,
		      int *width, int *height, int *scale)
{
	struct rdp_output *output = to_rdp_output(base);
	struct rdp_backend *rdpBackend = to_rdp_backend(base->compositor);
	struct monitor_private *mp = rdpBackend->monitor_private;
	freerdp_peer *client = rdpBackend->rdp_peer;
	struct weston_head *head;

	wl_list_for_each(head, &output->base.head_list, output_link) {
		struct rdp_head *h = to_rdp_head(head);

		rdp_disp_debug(mp, "get_config: attached head [%d]: make:%s, mode:%s, name:%s, (%p)\n",
			  h->index, head->make, head->model, head->name, head);
		rdp_disp_debug(mp, "get_config: attached head [%d]: x:%d, y:%d, width:%d, height:%d\n",
			  h->index, h->monitorMode.monitorDef.x, h->monitorMode.monitorDef.y,
			  h->monitorMode.monitorDef.width, h->monitorMode.monitorDef.height);

		/* In HiDef RAIL mode, get monitor resolution from RDP client if provided. */
		if (client && client->context->settings->HiDefRemoteApp) {
			if (h->monitorMode.monitorDef.width && h->monitorMode.monitorDef.height) {
				/* Return true client resolution (not adjusted by DPI) */
				*width = h->monitorMode.monitorDef.width;
				*height = h->monitorMode.monitorDef.height;
				*scale = h->monitorMode.scale;
			}
			break; // only one head per output in HiDef.
		}
	}
	return 0;
}

struct weston_output *
rdpdisp_get_primary_output(void *priv)
{
	struct monitor_private *mp = priv;
	struct rdp_head *current;

	wl_list_for_each(current, &mp->head_list, link) {
		if (current->monitorMode.monitorDef.is_primary)
			return current->base.output;
	}
	return NULL;
}

void
rdpdisp_get_primary_size(void *priv, int *width, int *height)
{
	struct monitor_private *mp = priv;
	struct rdp_head *current;

	wl_list_for_each(current, &mp->head_list, link) {
		if (current->monitorMode.monitorDef.is_primary) {
			*width = current->monitorMode.monitorDef.width;
			*height = current->monitorMode.monitorDef.height;
			break;
		}
	}
}

void rdpdisp_head_get_physical_size(struct weston_head *base, int *phys_width, int *phys_height)
{
	struct rdp_head *head = to_rdp_head(base);

	*phys_width = head->monitorMode.monitorDef.attributes.physicalWidth;
	*phys_height = head->monitorMode.monitorDef.attributes.physicalHeight;
}

void rdpdisp_output_enable(void *priv, struct weston_output *out)
{
	struct monitor_private *mp = priv;
	struct weston_head *eh;

	wl_list_for_each(eh, &out->head_list, output_link) {
		struct rdp_head *h = to_rdp_head(eh);

		rdp_disp_debug(mp, "move head/output %s (%d,%d) -> (%d,%d)\n",
			       out->name, out->x, out->y,
			       h->monitorMode.rectWeston.x,
			       h->monitorMode.rectWeston.y);
		weston_output_move(out,
				   h->monitorMode.rectWeston.x,
				   h->monitorMode.rectWeston.y);
		break; // must be only 1 head per output.
	}
}

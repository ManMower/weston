/*
 * Copyright © 2012 Scott Moreau
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include <assert.h>
#include <stdlib.h>
#include <stdbool.h>

#include "compositor.h"
#include "text-cursor-position-server-protocol.h"

static void
weston_zoom_frame_z(struct weston_animation *animation,
		struct weston_output *output, uint32_t msecs)
{
	if (animation->frame_counter <= 1)
		output->zoom.spring_z.timestamp = msecs;

	weston_spring_update(&output->zoom.spring_z, msecs);

	if (output->zoom.spring_z.current > output->zoom.max_level)
		output->zoom.spring_z.current = output->zoom.max_level;
	else if (output->zoom.spring_z.current < 0.0)
		output->zoom.spring_z.current = 0.0;

	if (weston_spring_done(&output->zoom.spring_z)) {
		if (output->zoom.active && output->zoom.level <= 0.0) {
			output->zoom.active = false;
			output->zoom.seat = NULL;
			output->disable_planes--;
			if (output->zoom.has_listener) {
				wl_list_remove(&output->zoom.motion_listener.link);
				output->zoom.has_listener = false;
			}
		}
		output->zoom.spring_z.current = output->zoom.level;
		wl_list_remove(&animation->link);
		wl_list_init(&animation->link);
	}

	output->dirty = 1;
	weston_output_damage(output);
}

static void
weston_zoom_frame_xy(struct weston_animation *animation,
		struct weston_output *output, uint32_t msecs)
{
	wl_fixed_t x, y;

	if (animation->frame_counter <= 1)
		output->zoom.spring_xy.timestamp = msecs;

	weston_spring_update(&output->zoom.spring_xy, msecs);

	x = output->zoom.from.x - ((output->zoom.from.x - output->zoom.to.x) *
						output->zoom.spring_xy.current);
	y = output->zoom.from.y - ((output->zoom.from.y - output->zoom.to.y) *
						output->zoom.spring_xy.current);

	output->zoom.current.x = x;
	output->zoom.current.y = y;

	if (weston_spring_done(&output->zoom.spring_xy)) {
		output->zoom.spring_xy.current = output->zoom.spring_xy.target;
		weston_get_zoom_target(output, NULL,
				       &output->zoom.current.x,
				       &output->zoom.current.y);
		wl_list_remove(&animation->link);
		wl_list_init(&animation->link);
	}

	output->dirty = 1;
	weston_output_damage(output);
}

static void
zoom_area_center_from_point(struct weston_output *output,
			    wl_fixed_t *x, wl_fixed_t *y)
{
	float level = output->zoom.spring_z.current;
	wl_fixed_t offset_x = wl_fixed_from_int(output->x);
	wl_fixed_t offset_y = wl_fixed_from_int(output->y);
	wl_fixed_t w = wl_fixed_from_int(output->width);
	wl_fixed_t h = wl_fixed_from_int(output->height);

	*x -= ((((*x - offset_x) / (float) w) - 0.5) * (w * (1.0 - level)));
	*y -= ((((*y - offset_y) / (float) h) - 0.5) * (h * (1.0 - level)));
}

static void
weston_output_update_zoom_transform(struct weston_output *output)
{
	float global_x, global_y;
	wl_fixed_t x = output->zoom.current.x;
	wl_fixed_t y = output->zoom.current.y;
	float trans_min, trans_max;
	float ratio, level;

	level = output->zoom.spring_z.current;
	ratio = 1 / level;

	if (!output->zoom.active || level > output->zoom.max_level ||
	    level == 0.0f)
		return;

	if (wl_list_empty(&output->zoom.animation_xy.link))
		zoom_area_center_from_point(output, &x, &y);

	global_x = wl_fixed_to_double(x);
	global_y = wl_fixed_to_double(y);

	output->zoom.trans_x =
		((((global_x - output->x) / output->width) *
		(level * 2)) - level) * ratio;
	output->zoom.trans_y =
		((((global_y - output->y) / output->height) *
		(level * 2)) - level) * ratio;

	trans_max = level * 2 - level;
	trans_min = -trans_max;

	/* Clip zoom area to output */
	if (output->zoom.trans_x > trans_max)
		output->zoom.trans_x = trans_max;
	else if (output->zoom.trans_x < trans_min)
		output->zoom.trans_x = trans_min;
	if (output->zoom.trans_y > trans_max)
		output->zoom.trans_y = trans_max;
	else if (output->zoom.trans_y < trans_min)
		output->zoom.trans_y = trans_min;
}

static void
weston_zoom_transition(struct weston_output *output, wl_fixed_t x, wl_fixed_t y)
{
	if (output->zoom.level != output->zoom.spring_z.current) {
		output->zoom.spring_z.target = output->zoom.level;
		if (wl_list_empty(&output->zoom.animation_z.link)) {
			output->zoom.animation_z.frame_counter = 0;
			wl_list_insert(output->animation_list.prev,
				&output->zoom.animation_z.link);
		}
	}

	output->dirty = 1;
	weston_output_damage(output);
}

WL_EXPORT void
weston_output_update_zoom(struct weston_output *output)
{
	wl_fixed_t target_x, target_y, x, y;

	assert(output->zoom.active);

	if (weston_get_zoom_target(output, NULL, &target_x, &target_y) < 0)
		return;

	x = target_x;
	y = target_y;
	zoom_area_center_from_point(output, &x, &y);

	if (wl_list_empty(&output->zoom.animation_xy.link)) {
		output->zoom.current.x = target_x;
		output->zoom.current.y = target_y;
	} else {
		output->zoom.to.x = x;
		output->zoom.to.y = y;
	}

	weston_zoom_transition(output, x, y);
	weston_output_update_zoom_transform(output);
}

static void
motion(struct wl_listener *listener, void *data)
{
	struct weston_output_zoom *zoom =
		container_of(listener, struct weston_output_zoom, motion_listener);
	struct weston_output *output =
		container_of(zoom, struct weston_output, zoom);

	weston_output_update_zoom(output);
}

/** Add a motion listener for a zoomed output
 *
 * This will be called at the start of a zoom or during hotplug
 * if there was no pointer when the zoom started.
 *
 * \param output Output to add listener to
 * \param seat Seat that controls the zoom location
 */
WL_EXPORT void
weston_output_zoom_add_motion_listener(struct weston_output *output,
				       struct weston_seat *seat)
{
	if (!output->zoom.active ||
	    output->zoom.seat != seat ||
	    !seat->pointer)
		return;

	wl_signal_add(&seat->pointer->motion_signal,
		      &output->zoom.motion_listener);
	output->zoom.has_listener = true;
}

WL_EXPORT void
weston_output_activate_zoom(struct weston_output *output,
			    struct weston_seat *seat)
{
	if (output->zoom.active)
		return;

	output->zoom.active = true;
	output->zoom.seat = seat;
	output->disable_planes++;

	weston_output_zoom_add_motion_listener(output, seat);
}

WL_EXPORT void
weston_output_init_zoom(struct weston_output *output)
{
	output->zoom.active = false;
	output->zoom.seat = NULL;
	output->zoom.increment = 0.07;
	output->zoom.max_level = 0.95;
	output->zoom.level = 0.0;
	output->zoom.trans_x = 0.0;
	output->zoom.trans_y = 0.0;
	weston_spring_init(&output->zoom.spring_z, 250.0, 0.0, 0.0);
	output->zoom.spring_z.friction = 1000;
	output->zoom.animation_z.frame = weston_zoom_frame_z;
	wl_list_init(&output->zoom.animation_z.link);
	weston_spring_init(&output->zoom.spring_xy, 250.0, 0.0, 0.0);
	output->zoom.spring_xy.friction = 1000;
	output->zoom.animation_xy.frame = weston_zoom_frame_xy;
	wl_list_init(&output->zoom.animation_xy.link);
	output->zoom.motion_listener.notify = motion;
}

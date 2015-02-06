/*
 * Copyright © 2010-2012 Intel Corporation
 * Copyright © 2011-2012 Collabora, Ltd.
 * Copyright © 2013 Raspberry Pi Foundation
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

#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "shell.h"
#include "desktop-shell-server-protocol.h"
#include "input-method-server-protocol.h"

struct input_panel_surface {
	struct wl_resource *resource;
	struct wl_signal destroy_signal;

	struct input_method *input_method;

	struct desktop_shell *shell;

	struct wl_list link;
	struct weston_surface *surface;
	struct weston_view *view;
	struct wl_listener surface_destroy_listener;

	struct wl_listener surface_show_listener;
	struct wl_listener surface_hide_listener;
	struct wl_listener update_input_panel_listener;
	struct wl_listener caps_changed_listener;

	pixman_box32_t cursor_rectangle;

	struct weston_view_animation *anim;

	struct weston_output *output;
	uint32_t panel;

	bool visible;
};

static void
input_panel_slide_done(struct weston_view_animation *animation, void *data)
{
	struct input_panel_surface *ipsurf = data;

	ipsurf->anim = NULL;
}

static void
size_input_panel_surface(struct input_panel_surface *ipsurf,
			 float *x,
			 float *y)
{
	if (ipsurf->panel) {
		*x = ipsurf->cursor_rectangle.x2;
		*y = ipsurf->cursor_rectangle.y2;
	} else {
		*x = ipsurf->output->x +
		     (ipsurf->output->width - ipsurf->surface->width) / 2;
		*y = ipsurf->output->y +
		     ipsurf->output->height - ipsurf->surface->height;
	}
}

static void
show_input_panel_surface(struct input_panel_surface *ipsurf)
{
	struct desktop_shell *shell = ipsurf->shell;
	float x, y;

	if (ipsurf->visible)
		return;

	ipsurf->visible = true;

	size_input_panel_surface(ipsurf, &x, &y);
	weston_view_set_position(ipsurf->view, x, y);

	weston_layer_entry_insert(&shell->input_panel_layer.view_list,
	                          &ipsurf->view->layer_link);
	if (ipsurf->panel) {
		struct weston_view *view = NULL;
		struct weston_surface *surface;
		surface = input_method_get_text_input_surface(ipsurf->input_method);
		if (surface)
			view = get_default_view(surface);
		if (view)
			weston_view_set_transform_parent(ipsurf->view, view);
	}
	weston_view_geometry_dirty(ipsurf->view);
	weston_view_update_transform(ipsurf->view);
	weston_surface_damage(ipsurf->surface);

	if (ipsurf->anim)
		weston_view_animation_destroy(ipsurf->anim);

	ipsurf->anim =
		weston_slide_run(ipsurf->view,
				 ipsurf->surface->height * 0.9, 0,
				 input_panel_slide_done, ipsurf);
}

static void
hide_input_panel_surface(struct input_panel_surface *ipsurf)
{
	if (!ipsurf->visible)
		return;

	ipsurf->visible = false;
	weston_view_unmap(ipsurf->view);
}

static void
update_input_panels(struct wl_listener *listener, void *data)
{
	struct input_panel_surface *ipsurf =
		container_of(listener, struct input_panel_surface,
			     update_input_panel_listener);

	memcpy(&ipsurf->cursor_rectangle, data, sizeof(pixman_box32_t));
}

static int
input_panel_get_label(struct weston_surface *surface, char *buf, size_t len)
{
	return snprintf(buf, len, "input panel");
}

static void
input_panel_configure(struct weston_surface *surface, int32_t sx, int32_t sy)
{
	struct input_panel_surface *ip_surface = surface->configure_private;
	float x, y;

	if (surface->width == 0)
		return;

	size_input_panel_surface(ip_surface, &x, &y);
	weston_view_set_position(ip_surface->view, x, y);
}

static void
destroy_input_panel_surface(struct input_panel_surface *input_panel_surface)
{
	wl_signal_emit(&input_panel_surface->destroy_signal, input_panel_surface);

	wl_list_remove(&input_panel_surface->surface_destroy_listener.link);
	wl_list_remove(&input_panel_surface->surface_show_listener.link);
	wl_list_remove(&input_panel_surface->surface_hide_listener.link);
	wl_list_remove(&input_panel_surface->update_input_panel_listener.link);
	wl_list_remove(&input_panel_surface->link);

	input_panel_surface->surface->configure = NULL;
	weston_surface_set_label_func(input_panel_surface->surface, NULL);
	weston_view_destroy(input_panel_surface->view);

	free(input_panel_surface);
}

static struct input_panel_surface *
get_input_panel_surface(struct weston_surface *surface)
{
	if (surface->configure == input_panel_configure) {
		return surface->configure_private;
	} else {
		return NULL;
	}
}

static void
input_panel_handle_surface_destroy(struct wl_listener *listener, void *data)
{
	struct input_panel_surface *ipsurface = container_of(listener,
							     struct input_panel_surface,
							     surface_destroy_listener);

	if (ipsurface->resource) {
		wl_resource_destroy(ipsurface->resource);
	} else {
		destroy_input_panel_surface(ipsurface);
	}
}

static void
handle_show_surface(struct wl_listener *listener, void *data)
{
	struct input_panel_surface *surface =
				    container_of(listener,
						 struct input_panel_surface,
						 surface_show_listener);

	show_input_panel_surface(surface);
}

static void
handle_hide_surface(struct wl_listener *listener, void *data)
{
	struct input_panel_surface *surface =
				    container_of(listener,
						 struct input_panel_surface,
						 surface_hide_listener);

	hide_input_panel_surface(surface);
}

static struct input_panel_surface *
create_input_panel_surface(struct desktop_shell *shell,
			   struct weston_surface *surface,
			   struct input_method *method)
{
	struct input_panel_surface *input_panel_surface;

	input_panel_surface = calloc(1, sizeof *input_panel_surface);
	if (!input_panel_surface)
		return NULL;

	surface->configure = input_panel_configure;
	surface->configure_private = input_panel_surface;
	weston_surface_set_label_func(surface, input_panel_get_label);

	input_panel_surface->input_method = method;
	input_panel_surface->shell = shell;

	input_panel_surface->surface = surface;
	input_panel_surface->view = weston_view_create(surface);

	wl_signal_init(&input_panel_surface->destroy_signal);
	input_panel_surface->surface_destroy_listener.notify = input_panel_handle_surface_destroy;
	wl_signal_add(&surface->destroy_signal,
		      &input_panel_surface->surface_destroy_listener);

	input_panel_surface->surface_show_listener.notify = handle_show_surface;
	input_panel_surface->surface_hide_listener.notify = handle_hide_surface;
	input_panel_surface->update_input_panel_listener.notify =
							update_input_panels;

	text_backend_setup_input_panel_signals(method,
		       &input_panel_surface->surface_show_listener,
		       &input_panel_surface->surface_hide_listener,
		       &input_panel_surface->update_input_panel_listener);

	wl_list_init(&input_panel_surface->link);

	return input_panel_surface;
}

static void
input_panel_surface_set_toplevel(struct wl_client *client,
				 struct wl_resource *resource,
				 struct wl_resource *output_resource,
				 uint32_t position)
{
	struct input_panel_surface *input_panel_surface =
		wl_resource_get_user_data(resource);
	struct desktop_shell *shell = input_panel_surface->shell;

	wl_list_insert(&shell->input_panel.surfaces,
		       &input_panel_surface->link);

	input_panel_surface->output = wl_resource_get_user_data(output_resource);
	input_panel_surface->panel = 0;
}

static void
input_panel_surface_set_overlay_panel(struct wl_client *client,
				      struct wl_resource *resource)
{
	struct input_panel_surface *input_panel_surface =
		wl_resource_get_user_data(resource);
	struct desktop_shell *shell = input_panel_surface->shell;

	wl_list_insert(&shell->input_panel.surfaces,
		       &input_panel_surface->link);

	input_panel_surface->panel = 1;
}

static const struct wl_input_panel_surface_interface input_panel_surface_implementation = {
	input_panel_surface_set_toplevel,
	input_panel_surface_set_overlay_panel
};

static void
destroy_input_panel_surface_resource(struct wl_resource *resource)
{
	struct input_panel_surface *ipsurf =
		wl_resource_get_user_data(resource);

	destroy_input_panel_surface(ipsurf);
}

static void
input_panel_get_input_panel_surface(struct wl_client *client,
				    struct wl_resource *resource,
				    uint32_t id,
				    struct wl_resource *method_resource,
				    struct wl_resource *surface_resource)
{
	struct weston_surface *surface =
		wl_resource_get_user_data(surface_resource);
	struct input_method *method =
		wl_resource_get_user_data(method_resource);
	struct desktop_shell *shell = wl_resource_get_user_data(resource);
	struct input_panel_surface *ipsurf;

	if (get_input_panel_surface(surface)) {
		wl_resource_post_error(surface_resource,
				       WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "wl_input_panel::get_input_panel_surface already requested");
		return;
	}

	ipsurf = create_input_panel_surface(shell, surface, method);
	if (!ipsurf) {
		wl_resource_post_error(surface_resource,
				       WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "surface->configure already set");
		return;
	}

	ipsurf->resource =
		wl_resource_create(client,
				   &wl_input_panel_surface_interface, 1, id);
	wl_resource_set_implementation(ipsurf->resource,
				       &input_panel_surface_implementation,
				       ipsurf,
				       destroy_input_panel_surface_resource);
}

static const struct wl_input_panel_interface input_panel_implementation = {
	input_panel_get_input_panel_surface
};

static void
unbind_input_panel(struct wl_resource *resource)
{
	struct desktop_shell *shell = wl_resource_get_user_data(resource);

	shell->input_panel.binding = NULL;
}

static void
bind_input_panel(struct wl_client *client,
	      void *data, uint32_t version, uint32_t id)
{
	struct desktop_shell *shell = data;
	struct wl_resource *resource;

	resource = wl_resource_create(client,
				      &wl_input_panel_interface, 1, id);

	if (shell->input_panel.binding == NULL) {
		wl_resource_set_implementation(resource,
					       &input_panel_implementation,
					       shell, unbind_input_panel);
		shell->input_panel.binding = resource;
		return;
	}

	wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
			       "interface object already bound");
}

int
input_panel_setup(struct desktop_shell *shell)
{
	wl_list_init(&shell->input_panel.surfaces);

	if (wl_global_create(shell->compositor->wl_display,
			     &wl_input_panel_interface, 2,
			     shell, bind_input_panel) == NULL)
		return -1;

	wl_list_insert(&shell->compositor->cursor_layer.link,
		       &shell->input_panel_layer.link);

	return 0;
}

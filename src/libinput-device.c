/*
 * Copyright © 2010 Intel Corporation
 * Copyright © 2013 Jonas Ådahl
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

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input.h>
#include <unistd.h>
#include <fcntl.h>
#include <mtdev.h>
#include <assert.h>
#include <libinput.h>

#include "compositor.h"
#include "libinput-device.h"

#define DEFAULT_AXIS_STEP_DISTANCE wl_fixed_from_int(10)

void
evdev_led_update(struct evdev_device *device, enum weston_led weston_leds)
{
	enum libinput_led leds = 0;

	if (weston_leds & LED_NUM_LOCK)
		leds |= LIBINPUT_LED_NUM_LOCK;
	if (weston_leds & LED_CAPS_LOCK)
		leds |= LIBINPUT_LED_CAPS_LOCK;
	if (weston_leds & LED_SCROLL_LOCK)
		leds |= LIBINPUT_LED_SCROLL_LOCK;

	libinput_device_led_update(device->device, leds);
}

static void
handle_keyboard_key(struct libinput_device *libinput_device,
		    struct libinput_event_keyboard *keyboard_event)
{
	struct evdev_device *device =
		libinput_device_get_user_data(libinput_device);
	int key_state =
		libinput_event_keyboard_get_key_state(keyboard_event);
	int seat_key_count =
		libinput_event_keyboard_get_seat_key_count(keyboard_event);

	/* Ignore key events that are not seat wide state changes. */
	if ((key_state == LIBINPUT_KEY_STATE_PRESSED &&
	     seat_key_count != 1) ||
	    (key_state == LIBINPUT_KEY_STATE_RELEASED &&
	     seat_key_count != 0))
		return;

	notify_key(device->seat,
		   libinput_event_keyboard_get_time(keyboard_event),
		   libinput_event_keyboard_get_key(keyboard_event),
		   libinput_event_keyboard_get_key_state(keyboard_event),
		   STATE_UPDATE_AUTOMATIC);
}

static void
handle_pointer_motion(struct libinput_device *libinput_device,
		      struct libinput_event_pointer *pointer_event)
{
	struct evdev_device *device =
		libinput_device_get_user_data(libinput_device);
	wl_fixed_t dx, dy;

	dx = wl_fixed_from_double(libinput_event_pointer_get_dx(pointer_event));
	dy = wl_fixed_from_double(libinput_event_pointer_get_dy(pointer_event));
	notify_motion(device->seat,
		      libinput_event_pointer_get_time(pointer_event),
		      dx,
		      dy);
}

static void
handle_pointer_motion_absolute(
	struct libinput_device *libinput_device,
	struct libinput_event_pointer *pointer_event)
{
	struct evdev_device *device =
		libinput_device_get_user_data(libinput_device);
	struct weston_output *output = device->output;
	uint32_t time;
	wl_fixed_t x, y;
	uint32_t width, height;

	if (!output)
		return;

	time = libinput_event_pointer_get_time(pointer_event);
	width = device->output->current_mode->width;
	height = device->output->current_mode->height;

	x = wl_fixed_from_double(
		libinput_event_pointer_get_absolute_x_transformed(pointer_event,
								  width));
	y = wl_fixed_from_double(
		libinput_event_pointer_get_absolute_y_transformed(pointer_event,
								  height));

	weston_output_transform_coordinate(device->output, x, y, &x, &y);
	notify_motion_absolute(device->seat, time, x, y);
}

static void
handle_pointer_button(struct libinput_device *libinput_device,
		      struct libinput_event_pointer *pointer_event)
{
	struct evdev_device *device =
		libinput_device_get_user_data(libinput_device);
	int button_state =
		libinput_event_pointer_get_button_state(pointer_event);
	int seat_button_count =
		libinput_event_pointer_get_seat_button_count(pointer_event);

	/* Ignore button events that are not seat wide state changes. */
	if ((button_state == LIBINPUT_BUTTON_STATE_PRESSED &&
	     seat_button_count != 1) ||
	    (button_state == LIBINPUT_BUTTON_STATE_RELEASED &&
	     seat_button_count != 0))
		return;

	notify_button(device->seat,
		      libinput_event_pointer_get_time(pointer_event),
		      libinput_event_pointer_get_button(pointer_event),
		      libinput_event_pointer_get_button_state(pointer_event));
}

static double
normalize_scroll(struct libinput_event_pointer *pointer_event,
		 enum libinput_pointer_axis axis)
{
	static int warned;
	enum libinput_pointer_axis_source source;
	double value;

	source = libinput_event_pointer_get_axis_source(pointer_event);
	/* libinput < 0.8 sent wheel click events with value 10. Since 0.8
	   the value is the angle of the click in degrees. To keep
	   backwards-compat with existing clients, we just send multiples of
	   the click count.
	 */
	switch (source) {
	case LIBINPUT_POINTER_AXIS_SOURCE_WHEEL:
		value = 10 * libinput_event_pointer_get_axis_value_discrete(
								   pointer_event,
								   axis);
		break;
	case LIBINPUT_POINTER_AXIS_SOURCE_FINGER:
	case LIBINPUT_POINTER_AXIS_SOURCE_CONTINUOUS:
		value = libinput_event_pointer_get_axis_value(pointer_event,
							      axis);
		break;
	default:
		value = 0;
		if (warned < 5) {
			weston_log("Unknown scroll source %d. Event discarded\n",
				   source);
			warned++;
		}
		break;
	}

	return value;
}

static void
handle_pointer_axis(struct libinput_device *libinput_device,
		    struct libinput_event_pointer *pointer_event)
{
	struct evdev_device *device =
		libinput_device_get_user_data(libinput_device);
	double value;
	enum libinput_pointer_axis axis;

	axis = LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL;
	if (libinput_event_pointer_has_axis(pointer_event, axis)) {
		value = normalize_scroll(pointer_event, axis);
		notify_axis(device->seat,
			    libinput_event_pointer_get_time(pointer_event),
			    WL_POINTER_AXIS_VERTICAL_SCROLL,
			    wl_fixed_from_double(value));
	}

	axis = LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL;
	if (libinput_event_pointer_has_axis(pointer_event, axis)) {
		value = normalize_scroll(pointer_event, axis);
		notify_axis(device->seat,
			    libinput_event_pointer_get_time(pointer_event),
			    WL_POINTER_AXIS_HORIZONTAL_SCROLL,
			    wl_fixed_from_double(value));
	}
}

static void
handle_touch_with_coords(struct libinput_device *libinput_device,
			 struct libinput_event_touch *touch_event,
			 int touch_type)
{
	struct evdev_device *device =
		libinput_device_get_user_data(libinput_device);
	wl_fixed_t x;
	wl_fixed_t y;
	uint32_t width, height;
	uint32_t time;
	int32_t slot;

	if (!device->output)
		return;

	time = libinput_event_touch_get_time(touch_event);
	slot = libinput_event_touch_get_seat_slot(touch_event);

	width = device->output->current_mode->width;
	height = device->output->current_mode->height;
	x = wl_fixed_from_double(
		libinput_event_touch_get_x_transformed(touch_event, width));
	y = wl_fixed_from_double(
		libinput_event_touch_get_y_transformed(touch_event, height));

	weston_output_transform_coordinate(device->output,
					   x, y, &x, &y);

	notify_touch(device->seat, time, slot, x, y, touch_type);
}

static void
handle_touch_down(struct libinput_device *device,
		  struct libinput_event_touch *touch_event)
{
	handle_touch_with_coords(device, touch_event, WL_TOUCH_DOWN);
}

static void
handle_touch_motion(struct libinput_device *device,
		    struct libinput_event_touch *touch_event)
{
	handle_touch_with_coords(device, touch_event, WL_TOUCH_MOTION);
}

static void
handle_touch_up(struct libinput_device *libinput_device,
		struct libinput_event_touch *touch_event)
{
	struct evdev_device *device =
		libinput_device_get_user_data(libinput_device);
	uint32_t time = libinput_event_touch_get_time(touch_event);
	int32_t slot = libinput_event_touch_get_seat_slot(touch_event);

	notify_touch(device->seat, time, slot, 0, 0, WL_TOUCH_UP);
}

static void
handle_touch_frame(struct libinput_device *libinput_device,
		   struct libinput_event_touch *touch_event)
{
	struct evdev_device *device =
		libinput_device_get_user_data(libinput_device);
	struct weston_seat *seat = device->seat;

	notify_touch_frame(seat);
}

int
evdev_device_process_event(struct libinput_event *event)
{
	struct libinput_device *libinput_device =
		libinput_event_get_device(event);
	int handled = 1;

	switch (libinput_event_get_type(event)) {
	case LIBINPUT_EVENT_KEYBOARD_KEY:
		handle_keyboard_key(libinput_device,
				    libinput_event_get_keyboard_event(event));
		break;
	case LIBINPUT_EVENT_POINTER_MOTION:
		handle_pointer_motion(libinput_device,
				      libinput_event_get_pointer_event(event));
		break;
	case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
		handle_pointer_motion_absolute(
			libinput_device,
			libinput_event_get_pointer_event(event));
		break;
	case LIBINPUT_EVENT_POINTER_BUTTON:
		handle_pointer_button(libinput_device,
				      libinput_event_get_pointer_event(event));
		break;
	case LIBINPUT_EVENT_POINTER_AXIS:
		handle_pointer_axis(libinput_device,
				    libinput_event_get_pointer_event(event));
		break;
	case LIBINPUT_EVENT_TOUCH_DOWN:
		handle_touch_down(libinput_device,
				  libinput_event_get_touch_event(event));
		break;
	case LIBINPUT_EVENT_TOUCH_MOTION:
		handle_touch_motion(libinput_device,
				    libinput_event_get_touch_event(event));
		break;
	case LIBINPUT_EVENT_TOUCH_UP:
		handle_touch_up(libinput_device,
				libinput_event_get_touch_event(event));
		break;
	case LIBINPUT_EVENT_TOUCH_FRAME:
		handle_touch_frame(libinput_device,
				   libinput_event_get_touch_event(event));
		break;
	default:
		handled = 0;
		weston_log("unknown libinput event %d\n",
			   libinput_event_get_type(event));
	}

	return handled;
}

static void
notify_output_destroy(struct wl_listener *listener, void *data)
{
	struct evdev_device *device =
		container_of(listener,
			     struct evdev_device, output_destroy_listener);
	struct weston_compositor *c = device->seat->compositor;
	struct weston_output *output;

	if (!device->output_name && !wl_list_empty(&c->output_list)) {
		output = container_of(c->output_list.next,
				      struct weston_output, link);
		evdev_device_set_output(device, output);
	} else {
		device->output = NULL;
	}
}

/**
 * The WL_CALIBRATION property requires a pixel-specific matrix to be
 * applied after scaling device coordinates to screen coordinates. libinput
 * can't do that, so we need to convert the calibration to the normalized
 * format libinput expects.
 */
static void
evdev_device_set_calibration(struct evdev_device *device)
{
	struct udev *udev;
	struct udev_device *udev_device = NULL;
	const char *sysname = libinput_device_get_sysname(device->device);
	const char *calibration_values;
	uint32_t width, height;
	float calibration[6];
	enum libinput_config_status status;

	if (!device->output)
		return;

	width = device->output->width;
	height = device->output->height;
	if (width == 0 || height == 0)
		return;

	/* If libinput has a pre-set calibration matrix, don't override it */
	if (!libinput_device_config_calibration_has_matrix(device->device) ||
	    libinput_device_config_calibration_get_default_matrix(
							  device->device,
							  calibration) != 0)
		return;

	udev = udev_new();
	if (!udev)
		return;

	udev_device = udev_device_new_from_subsystem_sysname(udev,
							     "input",
							     sysname);
	if (!udev_device)
		goto out;

	calibration_values =
		udev_device_get_property_value(udev_device,
					       "WL_CALIBRATION");

	if (!calibration_values || sscanf(calibration_values,
					  "%f %f %f %f %f %f",
					  &calibration[0],
					  &calibration[1],
					  &calibration[2],
					  &calibration[3],
					  &calibration[4],
					  &calibration[5]) != 6)
		goto out;

	weston_log("Applying calibration: %f %f %f %f %f %f "
		   "(normalized %f %f)\n",
		    calibration[0],
		    calibration[1],
		    calibration[2],
		    calibration[3],
		    calibration[4],
		    calibration[5],
		    calibration[2] / width,
		    calibration[5] / height);

	/* normalize to a format libinput can use. There is a chance of
	   this being wrong if the width/height don't match the device
	   width/height but I'm not sure how to fix that */
	calibration[2] /= width;
	calibration[5] /= height;

	status = libinput_device_config_calibration_set_matrix(device->device,
							       calibration);
	if (status != LIBINPUT_CONFIG_STATUS_SUCCESS)
		weston_log("Failed to apply calibration.\n");

out:
	if (udev_device)
		udev_device_unref(udev_device);
	udev_unref(udev);
}

void
evdev_device_set_output(struct evdev_device *device,
			struct weston_output *output)
{
	if (device->output_destroy_listener.notify) {
		wl_list_remove(&device->output_destroy_listener.link);
		device->output_destroy_listener.notify = NULL;
	}

	device->output = output;
	device->output_destroy_listener.notify = notify_output_destroy;
	wl_signal_add(&output->destroy_signal,
		      &device->output_destroy_listener);
	evdev_device_set_calibration(device);
}

static void
configure_device(struct evdev_device *device)
{
	struct weston_compositor *compositor = device->seat->compositor;
	struct weston_config_section *s;
	int enable_tap;
	int enable_tap_default;

	s = weston_config_get_section(compositor->config,
				      "libinput", NULL, NULL);

	if (libinput_device_config_tap_get_finger_count(device->device) > 0) {
		enable_tap_default =
			libinput_device_config_tap_get_default_enabled(
				device->device);
		weston_config_section_get_bool(s, "enable_tap",
					       &enable_tap,
					       enable_tap_default);
		libinput_device_config_tap_set_enabled(device->device,
						       enable_tap);
	}

	evdev_device_set_calibration(device);
}

struct evdev_device *
evdev_device_create(struct libinput_device *libinput_device,
		    struct weston_seat *seat)
{
	struct evdev_device *device;

	device = zalloc(sizeof *device);
	if (device == NULL)
		return NULL;

	device->seat = seat;
	wl_list_init(&device->link);
	device->device = libinput_device;

	if (libinput_device_has_capability(libinput_device,
					   LIBINPUT_DEVICE_CAP_KEYBOARD)) {
		weston_seat_init_keyboard(seat, NULL);
		device->seat_caps |= EVDEV_SEAT_KEYBOARD;
	}
	if (libinput_device_has_capability(libinput_device,
					   LIBINPUT_DEVICE_CAP_POINTER)) {
		weston_seat_init_pointer(seat);
		device->seat_caps |= EVDEV_SEAT_POINTER;
	}
	if (libinput_device_has_capability(libinput_device,
					   LIBINPUT_DEVICE_CAP_TOUCH)) {
		weston_seat_init_touch(seat);
		device->seat_caps |= EVDEV_SEAT_TOUCH;
	}

	libinput_device_set_user_data(libinput_device, device);
	libinput_device_ref(libinput_device);

	configure_device(device);

	return device;
}

void
evdev_device_destroy(struct evdev_device *device)
{
	if (device->seat_caps & EVDEV_SEAT_POINTER)
		weston_seat_release_pointer(device->seat);
	if (device->seat_caps & EVDEV_SEAT_KEYBOARD)
		weston_seat_release_keyboard(device->seat);
	if (device->seat_caps & EVDEV_SEAT_TOUCH)
		weston_seat_release_touch(device->seat);

	if (device->output)
		wl_list_remove(&device->output_destroy_listener.link);
	wl_list_remove(&device->link);
	libinput_device_unref(device->device);
	free(device->devnode);
	free(device->output_name);
	free(device);
}

void
evdev_notify_keyboard_focus(struct weston_seat *seat,
			    struct wl_list *evdev_devices)
{
	struct weston_keyboard *keyboard = weston_seat_get_keyboard(seat);
	struct wl_array keys;

	if (!keyboard)
		return;

	wl_array_init(&keys);
	notify_keyboard_focus_in(seat, &keys, STATE_UPDATE_AUTOMATIC);
	wl_array_release(&keys);
}

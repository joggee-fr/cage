/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2020 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#include "config.h"

#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_idle.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#if CAGE_HAS_XWAYLAND
#include <wlr/types/wlr_xcursor_manager.h>
#endif
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#if CAGE_HAS_XWAYLAND
#include <wlr/xwayland.h>
#endif

#include "idle_inhibit_v1.h"
#include "output.h"
#include "seat.h"
#include "server.h"
#include "xdg_shell.h"
#include "xwayland.h"

// TODO
static bool
server_xwayland_init(struct cg_server *server)
{
#if CAGE_HAS_XWAYLAND
	server->xwayland = wlr_xwayland_create(server->wl_display, server->compositor, true);
	if (!server->xwayland) {
		wlr_log(WLR_ERROR, "Cannot create XWayland server");
		return false;
	}
	server->new_xwayland_surface.notify = handle_xwayland_surface_new;
	wl_signal_add(&server->xwayland->events.new_surface, &server->new_xwayland_surface);

	server->xcursor_manager = wlr_xcursor_manager_create(DEFAULT_XCURSOR, XCURSOR_SIZE);
	if (!server->xcursor_manager) {
		wlr_log(WLR_ERROR, "Cannot create XWayland XCursor manager");
		return false;
	}

	if (!wlr_xcursor_manager_load(server->xcursor_manager, 1)) {
		wlr_log(WLR_ERROR, "Cannot load XWayland XCursor theme");
	}
	struct wlr_xcursor *xcursor = wlr_xcursor_manager_get_xcursor(server->xcursor_manager, DEFAULT_XCURSOR, 1);
	if (xcursor) {
		struct wlr_xcursor_image *image = xcursor->images[0];
		wlr_xwayland_set_cursor(server->xwayland, image->buffer, image->width * 4, image->width, image->height,
					image->hotspot_x, image->hotspot_y);
	}

	wlr_xwayland_set_seat(server->xwayland, server->seat->seat);
#endif

	return true;
}

bool
server_init(struct cg_server *server)
{
	server->wl_display = wl_display_create();
	if (!server->wl_display) {
		wlr_log(WLR_ERROR, "Cannot allocate a Wayland display");
		return false;
	}

	server->backend = wlr_backend_autocreate(server->wl_display);
	if (!server->backend) {
		wlr_log(WLR_ERROR, "Unable to create the wlroots backend");
		return false;
	}

	server->renderer = wlr_renderer_autocreate(server->backend);
	if (!server->renderer) {
		wlr_log(WLR_ERROR, "Unable to create the wlroots renderer");
		return false;
	}

	server->allocator = wlr_allocator_autocreate(server->backend, server->renderer);
	if (!server->allocator) {
		wlr_log(WLR_ERROR, "Unable to create the wlroots allocator");
		return false;
	}

	wlr_renderer_init_wl_display(server->renderer, server->wl_display);

	wl_list_init(&server->views);
	wl_list_init(&server->outputs);

	server->output_layout = wlr_output_layout_create();
	if (!server->output_layout) {
		wlr_log(WLR_ERROR, "Unable to create output layout");
		return false;
	}
	server->output_layout_change.notify = handle_output_layout_change;
	wl_signal_add(&server->output_layout->events.change, &server->output_layout_change);

	server->scene = wlr_scene_create();
	if (!server->scene) {
		wlr_log(WLR_ERROR, "Unable to create scene");
		return false;
	}
	wlr_scene_attach_output_layout(server->scene, server->output_layout);

	if (!wlr_compositor_create(server->wl_display, server->renderer)) {
		wlr_log(WLR_ERROR, "Unable to create the wlroots compositor");
		return false;
	}

	if (!wlr_subcompositor_create(server->wl_display)) {
		wlr_log(WLR_ERROR, "Unable to create the wlroots subcompositor");
		return false;
	}

	if (!wlr_data_device_manager_create(server->wl_display)) {
		wlr_log(WLR_ERROR, "Unable to create the data device manager");
		return false;
	}

	/* Configure a listener to be notified when new outputs are
	 * available on the backend. We use this only to detect the
	 * first output and ignore subsequent outputs. */
	server->new_output.notify = handle_new_output;
	wl_signal_add(&server->backend->events.new_output, &server->new_output);

	server->seat = seat_create(server, server->backend);
	if (!server->seat) {
		wlr_log(WLR_ERROR, "Unable to create the seat");
		return false;
	}

	server->idle = wlr_idle_create(server->wl_display);
	if (!server->idle) {
		wlr_log(WLR_ERROR, "Unable to create the idle tracker");
		return false;
	}

	server->idle_inhibit_v1 = wlr_idle_inhibit_v1_create(server->wl_display);
	if (!server->idle_inhibit_v1) {
		wlr_log(WLR_ERROR, "Cannot create the idle inhibitor");
		return false;
	}

	server->new_idle_inhibitor_v1.notify = handle_idle_inhibitor_v1_new;
	wl_signal_add(&server->idle_inhibit_v1->events.new_inhibitor, &server->new_idle_inhibitor_v1);
	wl_list_init(&server->inhibitors);

	struct wlr_xdg_shell *xdg_shell = wlr_xdg_shell_create(server->wl_display, 4);
	if (!xdg_shell) {
		wlr_log(WLR_ERROR, "Unable to create the XDG shell interface");
		return false;
	}
	server->new_xdg_shell_surface.notify = handle_xdg_shell_surface_new;
	wl_signal_add(&xdg_shell->events.new_surface, &server->new_xdg_shell_surface);

	struct wlr_xdg_decoration_manager_v1 *xdg_decoration_manager = wlr_xdg_decoration_manager_v1_create(server->wl_display);
	if (!xdg_decoration_manager) {
		wlr_log(WLR_ERROR, "Unable to create the XDG decoration manager");
		return false;
	}
	wl_signal_add(&xdg_decoration_manager->events.new_toplevel_decoration, &server->xdg_toplevel_decoration);
	server->xdg_toplevel_decoration.notify = handle_xdg_toplevel_decoration;

	struct wlr_server_decoration_manager *server_decoration_manager =
		wlr_server_decoration_manager_create(server->wl_display);
	if (!server_decoration_manager) {
		wlr_log(WLR_ERROR, "Unable to create the server decoration manager");
		return false;
	}
	wlr_server_decoration_manager_set_default_mode(
		server_decoration_manager, server->xdg_decoration ? WLR_SERVER_DECORATION_MANAGER_MODE_SERVER
								  : WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT);

	if (!wlr_viewporter_create(server->wl_display)) {
		wlr_log(WLR_ERROR, "Unable to create the viewporter interface");
		return false;
	}

	struct wlr_presentation *presentation = wlr_presentation_create(server->wl_display, server->backend);
	if (!presentation) {
		wlr_log(WLR_ERROR, "Unable to create the presentation interface");
		return false;
	}
	wlr_scene_set_presentation(server->scene, presentation);

	if (!wlr_export_dmabuf_manager_v1_create(server->wl_display)) {
		wlr_log(WLR_ERROR, "Unable to create the export DMABUF manager");
		return false;
	}

	if (!wlr_screencopy_manager_v1_create(server->wl_display)) {
		wlr_log(WLR_ERROR, "Unable to create the screencopy manager");
		return false;
	}

	if (!wlr_single_pixel_buffer_manager_v1_create(server->wl_display)) {
		wlr_log(WLR_ERROR, "Unable to create the single pixel buffer manager");
		return false;
	}

	if (!wlr_xdg_output_manager_v1_create(server->wl_display, server->output_layout)) {
		wlr_log(WLR_ERROR, "Unable to create the output manager");
		return false;
	}

	server->output_manager_v1 = wlr_output_manager_v1_create(server->wl_display);
	if (!server->output_manager_v1) {
		wlr_log(WLR_ERROR, "Unable to create the output manager");
		return false;
	}
	server->output_manager_apply.notify = handle_output_manager_apply;
	wl_signal_add(&server->output_manager_v1->events.apply, &server->output_manager_apply);
	server->output_manager_test.notify = handle_output_manager_test;
	wl_signal_add(&server->output_manager_v1->events.test, &server->output_manager_test);

	if (!wlr_gamma_control_manager_v1_create(server->wl_display)) {
		wlr_log(WLR_ERROR, "Unable to create the gamma control manager");
		return false;
	}

	struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard = wlr_virtual_keyboard_manager_v1_create(server->wl_display);
	if (!virtual_keyboard) {
		wlr_log(WLR_ERROR, "Unable to create the virtual keyboard manager");
		return false;
	}
	wl_signal_add(&virtual_keyboard->events.new_virtual_keyboard, &server->new_virtual_keyboard);

	struct wlr_virtual_pointer_manager_v1 *virtual_pointer = wlr_virtual_pointer_manager_v1_create(server->wl_display);
	if (!virtual_pointer) {
		wlr_log(WLR_ERROR, "Unable to create the virtual pointer manager");
		return false;
	}
	wl_signal_add(&virtual_pointer->events.new_virtual_pointer, &server->new_virtual_pointer);

	server->relative_pointer_manager = wlr_relative_pointer_manager_v1_create(server->wl_display);

	if (!server_xwayland_init(server)) {
		return false;
	}

	server->socket = wl_display_add_socket_auto(server->wl_display);
	if (!server->socket) {
		wlr_log_errno(WLR_ERROR, "Unable to open Wayland socket");
		return false;
	}

	// TODO: part of init?
	if (!wlr_backend_start(server->backend)) {
		wlr_log(WLR_ERROR, "Unable to start the wlroots backend");
		return false;
	}

	return true;
}

void
server_term(struct cg_server *server)
{
#if CAGE_HAS_XWAYLAND
	wlr_xwayland_destroy(server->xwayland);
	wlr_xcursor_manager_destroy(server->xcursor_manager);
#endif

	seat_destroy(server->seat);
	/* This function is not null-safe, but we only ever get here
	   with a proper wl_display. */
	wl_display_destroy(server->wl_display);
	wlr_output_layout_destroy(server->output_layout);
}
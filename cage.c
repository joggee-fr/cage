/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2020 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#define _POSIX_C_SOURCE 200112L

#include "config.h"

#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/util/log.h>
#if CAGE_HAS_XWAYLAND
#include <wlr/xwayland.h>
#endif

#include "seat.h"
#include "server.h"

static int
sigchld_handler(int fd, uint32_t mask, void *data)
{
	struct cg_server *server = data;

	/* Close Cage's read pipe. */
	close(fd);

	if (mask & WL_EVENT_HANGUP) {
		wlr_log(WLR_DEBUG, "Child process closed normally");
	} else if (mask & WL_EVENT_ERROR) {
		wlr_log(WLR_DEBUG, "Connection closed by server");
	}

	server->return_app_code = true;
	wl_display_terminate(server->wl_display);
	return 0;
}

static bool
set_cloexec(int fd)
{
	int flags = fcntl(fd, F_GETFD);

	if (flags == -1) {
		wlr_log(WLR_ERROR, "Unable to set the CLOEXEC flag: fnctl failed");
		return false;
	}

	flags = flags | FD_CLOEXEC;
	if (fcntl(fd, F_SETFD, flags) == -1) {
		wlr_log(WLR_ERROR, "Unable to set the CLOEXEC flag: fnctl failed");
		return false;
	}

	return true;
}

static bool
spawn_primary_client(struct cg_server *server, char *argv[], pid_t *pid_out, struct wl_event_source **sigchld_source)
{
	int fd[2];
	if (pipe(fd) != 0) {
		wlr_log(WLR_ERROR, "Unable to create pipe");
		return false;
	}

	pid_t pid = fork();
	if (pid == 0) {
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		/* Close read, we only need write in the primary client process. */
		close(fd[0]);
		execvp(argv[0], argv);
		/* execvp() returns only on failure */
		wlr_log_errno(WLR_ERROR, "Failed to spawn client");
		_exit(1);
	} else if (pid == -1) {
		wlr_log_errno(WLR_ERROR, "Unable to fork");
		return false;
	}

	/* Set this early so that if we fail, the client process will be cleaned up properly. */
	*pid_out = pid;

	if (!set_cloexec(fd[0]) || !set_cloexec(fd[1])) {
		return false;
	}

	/* Close write, we only need read in Cage. */
	close(fd[1]);

	struct wl_event_loop *event_loop = wl_display_get_event_loop(server->wl_display);
	uint32_t mask = WL_EVENT_HANGUP | WL_EVENT_ERROR;
	*sigchld_source = wl_event_loop_add_fd(event_loop, fd[0], mask, sigchld_handler, server);

	wlr_log(WLR_DEBUG, "Child process created with pid %d", pid);
	return true;
}

static int
cleanup_primary_client(pid_t pid)
{
	int status;

	waitpid(pid, &status, 0);

	if (WIFEXITED(status)) {
		wlr_log(WLR_DEBUG, "Child exited normally with exit status %d", WEXITSTATUS(status));
		return WEXITSTATUS(status);
	} else if (WIFSIGNALED(status)) {
		/* Mimic Bash and other shells for the exit status */
		wlr_log(WLR_DEBUG, "Child was terminated by a signal (%d)", WTERMSIG(status));
		return 128 + WTERMSIG(status);
	}

	return 0;
}

static bool
detect_suid(void)
{
	if (geteuid() != 0 && getegid() != 0) {
		return false;
	}

	if (getuid() == geteuid() && getgid() == getegid()) {
		return false;
	}

	wlr_log(WLR_ERROR, "SUID operation is no longer supported, refusing to start");
	return true;
}

static int
handle_signal(int signal, void *data)
{
	struct wl_display *display = data;

	switch (signal) {
	case SIGINT:
		/* Fallthrough */
	case SIGTERM:
		wl_display_terminate(display);
		return 0;
	default:
		return 0;
	}
}

static void
usage(FILE *file, const char *cage)
{
	fprintf(file,
		"Usage: %s [OPTIONS] [--] APPLICATION\n"
		"\n"
		" -d\t Don't draw client side decorations, when possible\n"
		" -h\t Display this help message\n"
		" -m extend Extend the display across all connected outputs (default)\n"
		" -m last Use only the last connected output\n"
		" -s\t Allow VT switching\n"
		" -v\t Show the version number and exit\n"
		"\n"
		" Use -- when you want to pass arguments to APPLICATION\n",
		cage);
}

static bool
parse_args(struct cg_server *server, int argc, char *argv[])
{
	int c;
	while ((c = getopt(argc, argv, "dhm:sv")) != -1) {
		switch (c) {
		case 'd':
			server->xdg_decoration = true;
			break;
		case 'h':
			usage(stdout, argv[0]);
			return false;
		case 'm':
			if (strcmp(optarg, "last") == 0) {
				server->output_mode = CAGE_MULTI_OUTPUT_MODE_LAST;
			} else if (strcmp(optarg, "extend") == 0) {
				server->output_mode = CAGE_MULTI_OUTPUT_MODE_EXTEND;
			}
			break;
		case 's':
			server->allow_vt_switch = true;
			break;
		case 'v':
			fprintf(stdout, "Cage version " CAGE_VERSION "\n");
			exit(0);
		default:
			usage(stderr, argv[0]);
			return false;
		}
	}

	if (optind >= argc) {
		usage(stderr, argv[0]);
		return false;
	}

	return true;
}

int
main(int argc, char *argv[])
{
	struct cg_server server = {0};
	struct wl_event_source *sigchld_source = NULL;
	pid_t pid = 0;
	int ret = 0, app_ret = 0;

	if (!parse_args(&server, argc, argv)) {
		return 1;
	}

#ifdef DEBUG
	wlr_log_init(WLR_DEBUG, NULL);
#else
	wlr_log_init(WLR_ERROR, NULL);
#endif

	/* SUID operation is deprecated, so block it for now. */
	if (detect_suid()) {
		return 1;
	}

	/* Wayland requires XDG_RUNTIME_DIR to be set. */
	if (!getenv("XDG_RUNTIME_DIR")) {
		wlr_log(WLR_ERROR, "XDG_RUNTIME_DIR is not set in the environment");
		return 1;
	}

	if (!server_init(&server)) {
		return 1;
	}

	struct wl_event_loop *event_loop = wl_display_get_event_loop(server.wl_display);
	struct wl_event_source *sigint_source =
		wl_event_loop_add_signal(event_loop, SIGINT, handle_signal, &server.wl_display);
	struct wl_event_source *sigterm_source =
		wl_event_loop_add_signal(event_loop, SIGTERM, handle_signal, &server.wl_display);


#if CAGE_HAS_XWAYLAND
	if (server->xwayland) {
		if (setenv("DISPLAY", xwayland->display_name, true) < 0) {
			wlr_log_errno(WLR_ERROR,
					"Unable to set DISPLAY for XWayland. Clients may not be able to connect");
		} else {
			wlr_log(WLR_DEBUG, "XWayland is running on display %s", xwayland->display_name);
		}

		wlr_xwayland_set_seat(xwayland, server.seat->seat);
	}
#endif

	if (setenv("WAYLAND_DISPLAY", server.socket, true) < 0) {
		wlr_log_errno(WLR_ERROR, "Unable to set WAYLAND_DISPLAY. Clients may not be able to connect");
	} else {
		wlr_log(WLR_DEBUG, "Cage " CAGE_VERSION " is running on Wayland display %s", server.socket);
	}

	if (!spawn_primary_client(&server, argv + optind, &pid, &sigchld_source)) {
		ret = 1;
		goto end;
	}

	seat_center_cursor(server.seat);
	wl_display_run(server.wl_display);

	app_ret = cleanup_primary_client(pid);
	if (!ret && server.return_app_code) {
		ret = app_ret;
	}

end:
	wl_event_source_remove(sigint_source);
	wl_event_source_remove(sigterm_source);
	if (sigchld_source) {
		wl_event_source_remove(sigchld_source);
	}

	server_term(&server);
	return ret;
}

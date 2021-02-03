/*
 * Copyright © 2019 Manuel Stoeckl
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

#include "main.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static inline uint32_t conntoken_header(bool reconnectable, bool update)
{
	return (WAYPIPE_PROTOCOL_VERSION << 16) |
	       (update ? CONN_UPDATE_BIT : 0) |
	       (reconnectable ? CONN_RECONNECTABLE_BIT : 0) | CONN_FIXED_BIT;
}

/** Fill the key for a token using random data with a very low accidental
 * collision probability. Whatever data was in the key before will be shuffled
 * in.*/
static void fill_random_key(struct connection_token *token)
{
	token->key[0] *= 13;
	token->key[1] *= 17;
	token->key[2] *= 29;

	struct timespec tp;
	clock_gettime(CLOCK_REALTIME, &tp);
	token->key[0] += (uint32_t)getpid();
	token->key[1] += 1 + (uint32_t)tp.tv_sec;
	token->key[2] += 1 + (uint32_t)tp.tv_nsec;

	int devrand = open("/dev/urandom", O_RDONLY);
	if (devrand != -1) {
		errno = 0;
		(void)read(devrand, token->key, sizeof(token->key));
		checked_close(devrand);
	}
}

static int read_sockaddr(int control_pipe, struct sockaddr_un *sockaddr)
{
	/* It is unlikely that a signal would interrupt a read of a ~100 byte
	 * sockaddr; and if used properly, the control pipe should never be
	 * sent much more data than that */
	char path[4096];
	ssize_t amt = read(control_pipe, path, sizeof(path) - 1);
	if (amt == -1) {
		wp_error("Failed to read from control pipe: %s",
				strerror(errno));
		return -1;
	}
	path[amt] = '\0';
	if (strlen(path) >= sizeof(sockaddr->sun_path)) {
		wp_error("Socket path read from control pipe is too long (%zu bytes, expected <= %zu): %s",
				strlen(path), sizeof(sockaddr->sun_path) - 1,
				path);
		return -1;
	}
	strcpy(sockaddr->sun_path, path);
	return 0;
}

static int run_single_server_reconnector(int control_pipe, int linkfd,
		const struct connection_token *flagged_token)
{
	int retcode = EXIT_SUCCESS;
	while (!shutdown_flag) {
		struct pollfd pf[2];
		pf[0].fd = control_pipe;
		pf[0].events = POLLIN;
		pf[0].revents = 0;
		pf[1].fd = linkfd;
		pf[1].events = 0;
		pf[1].revents = 0;

		int r = poll(pf, 2, -1);
		if (r == -1 && errno == EINTR) {
			continue;
		} else if (r == -1) {
			retcode = EXIT_FAILURE;
			break;
		} else if (r == 0) {
			// Nothing to read
			continue;
		}

		if (pf[1].revents & POLLHUP) {
			/* Hang up, main thread has closed its link */
			break;
		}
		if (pf[0].revents & POLLIN) {
			struct sockaddr_un new_sockaddr;
			if (read_sockaddr(control_pipe, &new_sockaddr) == -1) {
				retcode = EXIT_FAILURE;
				break;
			}

			int new_conn = connect_to_socket(&new_sockaddr);
			if (new_conn == -1) {
				wp_error("Socket path \"%s\" was invalid: %s",
						new_sockaddr.sun_path,
						strerror(errno));
				/* Socket path was invalid */
				continue;
			}

			if (write(new_conn, flagged_token,
					    sizeof(*flagged_token)) !=
					sizeof(*flagged_token)) {
				wp_error("Failed to write to new connection: %s",
						strerror(errno));
				checked_close(new_conn);
				continue;
			}

			if (send_one_fd(linkfd, new_conn) == -1) {
				wp_error("Failed to send new connection to subprocess: %s",
						strerror(errno));
			}
			checked_close(new_conn);
		}
	}
	checked_close(control_pipe);
	checked_close(linkfd);
	return retcode;
}

static int run_single_server(int control_pipe,
		const struct sockaddr_un *socket_addr, bool unlink_at_end,
		int server_link, const struct main_config *config)
{
	int chanfd = connect_to_socket(socket_addr);
	if (chanfd == -1) {
		goto fail_srv;
	}
	/* Only unlink the socket if it actually was a socket */
	if (unlink_at_end) {
		unlink(socket_addr->sun_path);
	}
	bool reconnectable = control_pipe != -1;

	struct connection_token token;
	memset(&token, 0, sizeof(token));
	fill_random_key(&token);
	token.header = conntoken_header(reconnectable, false);
	wp_debug("Connection token header: %08" PRIx32, token.header);
	if (write(chanfd, &token, sizeof(token)) != sizeof(token)) {
		wp_error("Failed to write connection token to socket");
		goto fail_cfd;
	}

	int linkfds[2] = {-1, -1};
	if (control_pipe != -1) {
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, linkfds) == -1) {
			wp_error("Failed to create socketpair: %s",
					strerror(errno));
			goto fail_cfd;
		}

		pid_t reco_pid = fork();
		if (reco_pid == -1) {
			wp_debug("Fork failure");
			checked_close(linkfds[0]);
			checked_close(linkfds[1]);
			goto fail_cfd;
		} else if (reco_pid == 0) {
			checked_close(chanfd);
			checked_close(linkfds[0]);
			checked_close(server_link);

			/* Further uses of the token will be to reconnect */
			token.header |= CONN_UPDATE_BIT;
			int rc = run_single_server_reconnector(
					control_pipe, linkfds[1], &token);
			exit(rc);
		}
		checked_close(control_pipe);
		checked_close(linkfds[1]);
	}

	int ret = main_interface_loop(
			chanfd, server_link, linkfds[0], config, false);
	return ret;

fail_cfd:
	checked_close(chanfd);
fail_srv:
	checked_close(server_link);
	return EXIT_FAILURE;
}

static int handle_new_server_connection(
		const struct sockaddr_un *current_sockaddr, int control_pipe,
		int wdisplay_socket, int appfd, struct conn_map *connmap,
		const struct main_config *config,
		const struct connection_token *new_token)
{
	bool reconnectable = control_pipe != -1;
	if (reconnectable && buf_ensure_size(connmap->count + 1,
					     sizeof(struct conn_addr),
					     &connmap->size,
					     (void **)&connmap->data) == -1) {
		wp_error("Failed to allocate memory to track new connection");
		goto fail_appfd;
	}

	int chanfd = connect_to_socket(current_sockaddr);
	if (chanfd == -1) {
		goto fail_appfd;
	}
	if (write(chanfd, new_token, sizeof(*new_token)) !=
			sizeof(*new_token)) {
		wp_error("Failed to write connection token: %s",
				strerror(errno));
		goto fail_chanfd;
	}

	int linksocks[2] = {-1, -1};
	if (reconnectable) {
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, linksocks) == -1) {
			wp_error("Socketpair for process link failed: %s",
					strerror(errno));
			goto fail_chanfd;
		}
	}

	pid_t npid = fork();
	if (npid == 0) {
		// Run forked process, with the only shared state being the
		// new channel socket
		checked_close(wdisplay_socket);
		if (reconnectable) {
			checked_close(control_pipe);
			checked_close(linksocks[0]);
		}
		for (int i = 0; i < connmap->count; i++) {
			if (connmap->data[i].linkfd != -1) {
				checked_close(connmap->data[i].linkfd);
			}
		}
		int rc = main_interface_loop(
				chanfd, appfd, linksocks[1], config, false);
		check_unclosed_fds();
		exit(rc);
	} else if (npid == -1) {
		wp_debug("Fork failure");
		if (reconnectable) {
			checked_close(linksocks[0]);
			checked_close(linksocks[1]);
		}
		goto fail_chanfd;
	}

	// This process no longer needs the application connection
	checked_close(chanfd);
	checked_close(appfd);
	if (reconnectable) {
		checked_close(linksocks[1]);

		connmap->data[connmap->count++] = (struct conn_addr){
				.token = *new_token,
				.pid = npid,
				.linkfd = linksocks[0],
		};
	}

	return 0;
fail_chanfd:
	checked_close(chanfd);
fail_appfd:
	checked_close(appfd);
	return -1;
}

static int update_connections(struct sockaddr_un *current_sockaddr,
		const struct sockaddr_un *new_sockaddr,
		struct conn_map *connmap, bool unlink_at_end)
{
	/* TODO: what happens if there's a partial failure? */
	for (int i = 0; i < connmap->count; i++) {
		int chanfd = connect_to_socket(new_sockaddr);
		if (chanfd == -1) {
			wp_error("Failed to connect to socket at \"%s\": %s",
					new_sockaddr->sun_path,
					strerror(errno));
			return -1;
		}
		struct connection_token flagged_token = connmap->data[i].token;
		flagged_token.header |= CONN_UPDATE_BIT;
		if (write(chanfd, &flagged_token, sizeof(flagged_token)) !=
				sizeof(flagged_token)) {
			wp_error("Failed to write token to replacement connection: %s",
					strerror(errno));
			checked_close(chanfd);
			return -1;
		}

		if (send_one_fd(connmap->data[i].linkfd, chanfd) == -1) {
			// TODO: what happens if data has changed?
			checked_close(chanfd);
			return -1;
		}
		checked_close(chanfd);
	}
	/* If switching connections succeeded, adopt the new socket */
	if (unlink_at_end && strcmp(current_sockaddr->sun_path,
					     new_sockaddr->sun_path)) {
		unlink(current_sockaddr->sun_path);
	}
	*current_sockaddr = *new_sockaddr;
	return 0;
}

static int run_multi_server(int control_pipe,
		const struct sockaddr_un *socket_addr, bool unlink_at_end,
		int wdisplay_socket, const struct main_config *config,
		pid_t *child_pid)
{
	struct conn_map connmap = {.data = NULL, .count = 0, .size = 0};
	struct sockaddr_un current_sockaddr = *socket_addr;

	struct pollfd pfs[2];
	pfs[0].fd = wdisplay_socket;
	pfs[0].events = POLLIN;
	pfs[0].revents = 0;
	pfs[1].fd = control_pipe;
	pfs[1].events = POLLIN;
	pfs[1].revents = 0;
	int retcode = EXIT_SUCCESS;
	struct connection_token token;
	memset(&token, 0, sizeof(token));
	token.header = conntoken_header(control_pipe != -1, false);
	wp_debug("Connection token header: %08" PRIx32, token.header);
	while (!shutdown_flag) {
		int status = -1;
		if (wait_for_pid_and_clean(
				    child_pid, &status, WNOHANG, &connmap)) {
			wp_debug("Child program has died, exiting");
			retcode = WEXITSTATUS(status);
			break;
		}

		int r = poll(pfs, 1 + (control_pipe != -1), -1);
		if (r == -1) {
			if (errno == EINTR) {
				// If SIGCHLD, we will check the child.
				// If SIGINT, the loop ends
				continue;
			}
			fprintf(stderr, "Poll failed: %s", strerror(errno));
			retcode = EXIT_FAILURE;
			break;
		} else if (r == 0) {
			continue;
		}
		if (pfs[1].revents & POLLIN) {
			struct sockaddr_un new_sockaddr;
			if (read_sockaddr(control_pipe, &new_sockaddr) == -1) {

			} else {
				update_connections(&current_sockaddr,
						&new_sockaddr, &connmap,
						unlink_at_end);
			}
		}

		if (pfs[0].revents & POLLIN) {
			int appfd = accept(wdisplay_socket, NULL, NULL);
			if (appfd == -1) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					// The wakeup may have been
					// spurious
					continue;
				}
				wp_error("Connection failure: %s",
						strerror(errno));
				retcode = EXIT_FAILURE;
				break;
			} else {
				fill_random_key(&token);
				if (handle_new_server_connection(
						    &current_sockaddr,
						    control_pipe,
						    wdisplay_socket, appfd,
						    &connmap, config,
						    &token) == -1) {
					retcode = EXIT_FAILURE;
					break;
				}
			}
		}
	}
	if (unlink_at_end) {
		unlink(current_sockaddr.sun_path);
	}
	checked_close(wdisplay_socket);
	if (control_pipe != -1) {
		checked_close(control_pipe);
	}

	for (int i = 0; i < connmap.count; i++) {
		checked_close(connmap.data[i].linkfd);
	}
	free(connmap.data);
	return retcode;
}

/* requires >=256 byte shell/shellname buffers */
static void setup_login_shell_command(char shell[static 256],
		char shellname[static 256], bool login_shell)
{
	strcpy(shellname, "-sh");
	strcpy(shell, "/bin/sh");

	// Select the preferred shell on the system
	char *shell_env = getenv("SHELL");
	if (!shell_env) {
		return;
	}
	int len = (int)strlen(shell_env);
	if (len >= 254) {
		wp_error("Environment variable $SHELL is too long at %d bytes, falling back to %s",
				len, shell);
		return;
	}
	strcpy(shell, shell_env);
	if (login_shell) {
		/* Create a login shell. The convention for this is to prefix
		 * the name of the shell with a single hyphen */
		int start = len;
		for (; start-- > 0;) {
			if (shell[start] == '/') {
				start++;
				break;
			}
		}
		shellname[0] = '-';
		strcpy(shellname + 1, shell + start);
	} else {
		strcpy(shellname, shell);
	}
}

int run_server(const struct sockaddr_un *socket_addr,
		const char *wayland_display, const char *control_path,
		const struct main_config *config, bool oneshot,
		bool unlink_at_end, char *const app_argv[],
		bool login_shell_if_backup)
{
	wp_debug("I'm a server on %s, running: %s", socket_addr->sun_path,
			app_argv[0]);

	struct sockaddr_un display_path;
	memset(&display_path, 0, sizeof(display_path));
	if (!oneshot) {
		if (wayland_display[0] == '/') {
			if (strlen(wayland_display) >=
					sizeof(display_path.sun_path)) {
				wp_error("Absolute path '%s' specified for WAYLAND_DISPLAY is too long (%zu bytes > %zu)",
						wayland_display,
						strlen(wayland_display),
						sizeof(display_path.sun_path) -
								1);
				return EXIT_FAILURE;
			}
			strcpy(display_path.sun_path, wayland_display);
		} else {
			const char *xdg_dir = getenv("XDG_RUNTIME_DIR");
			if (!xdg_dir) {
				wp_error("Env. var XDG_RUNTIME_DIR not available, cannot place display socket for WAYLAND_DISPLAY=\"%s\"",
						wayland_display);
				return EXIT_FAILURE;
			}
			if (strlen(xdg_dir) + 1 + strlen(wayland_display) >=
					sizeof(display_path.sun_path)) {
				wp_error("Path '%s/%s' specified for WAYLAND_DISPLAY is too long (%zu + 1 + %zu bytes > %zu)",
						xdg_dir, wayland_display,
						strlen(xdg_dir),
						strlen(wayland_display),
						sizeof(display_path.sun_path) -
								1);
				return EXIT_FAILURE;
			}
			multi_strcat(display_path.sun_path,
					sizeof(display_path.sun_path), xdg_dir,
					"/", wayland_display, NULL);
		}
	}

	// Setup connection to program
	int wayland_socket = -1, server_link = -1, wdisplay_socket = -1;
	if (oneshot) {
		int csockpair[2];
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, csockpair) == -1) {
			wp_error("Socketpair failed: %s", strerror(errno));
			return EXIT_FAILURE;
		}
		wayland_socket = csockpair[1];
		server_link = csockpair[0];
	} else {
		// Bind a socket for WAYLAND_DISPLAY, and listen
		int nmaxclients = 128;
		wdisplay_socket = setup_nb_socket(&display_path, nmaxclients);
		if (wdisplay_socket == -1) {
			// Error messages already made
			return EXIT_FAILURE;
		}
	}

	// Launch program
	pid_t pid = fork();
	if (pid == -1) {
		wp_error("Fork failed");
		if (!oneshot) {
			unlink(display_path.sun_path);
		}
		return EXIT_FAILURE;
	} else if (pid == 0) {
		if (oneshot) {
			char bufs2[16];
			sprintf(bufs2, "%d", wayland_socket);

			// Provide the other socket in the pair to child
			// application
			unsetenv("WAYLAND_DISPLAY");
			setenv("WAYLAND_SOCKET", bufs2, 1);
			checked_close(server_link);
		} else {
			// Since Wayland 1.15, absolute paths are supported in
			// WAYLAND_DISPLAY
			unsetenv("WAYLAND_SOCKET");
			setenv("WAYLAND_DISPLAY", wayland_display, 1);
			checked_close(wdisplay_socket);
		}

		const char *application = app_argv[0];
		char shell[256];
		char shellname[256];
		char *shellcmd[2] = {shellname, NULL};
		if (!application) {
			setup_login_shell_command(shell, shellname,
					login_shell_if_backup);
			application = shell;
			app_argv = shellcmd;
		}

		execvp(application, app_argv);
		wp_error("Failed to execvp \'%s\': %s", application,
				strerror(errno));
		return EXIT_FAILURE;
	}
	if (oneshot) {
		// We no longer need to see this side
		checked_close(wayland_socket);
	}

	int control_pipe = -1;
	if (control_path) {
		if (mkfifo(control_path, 0644) == -1) {
			wp_error("Failed to make a control FIFO at %s: %s",
					control_path, strerror(errno));
		} else {
			/* To prevent getting POLLHUP spam after the first user
			 * closes this pipe, open both read and write ends of
			 * the named pipe */
			control_pipe = open(control_path, O_RDWR | O_NONBLOCK);
			if (control_pipe == -1) {
				wp_error("Failed to open created FIFO for reading: %s",
						control_path, strerror(errno));
			}
		}
	}

	int retcode = EXIT_SUCCESS;
	/* These functions will close server_link, wdisplay_socket, and
	 * control_pipe */
	if (oneshot) {
		retcode = run_single_server(control_pipe, socket_addr,
				unlink_at_end, server_link, config);
	} else {
		retcode = run_multi_server(control_pipe, socket_addr,
				unlink_at_end, wdisplay_socket, config, &pid);
	}
	if (control_pipe != -1) {
		unlink(control_path);
	}
	if (!oneshot) {
		unlink(display_path.sun_path);
	}

	// Wait for child processes to exit
	wp_debug("Waiting for child handlers and program");

	int status = -1;
	if (wait_for_pid_and_clean(
			    &pid, &status, shutdown_flag ? WNOHANG : 0, NULL)) {
		wp_debug("Child program has died, exiting");
		retcode = WEXITSTATUS(status);
	}
	wp_debug("Program ended");
	return retcode;
}

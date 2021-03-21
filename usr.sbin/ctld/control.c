/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2021 Richard Kojedzinszky
 * All rights reserved.
 *
 * This software was developed by Edward Tomasz Napierala under sponsorship
 * from the FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/queue.h>
#include <sys/time.h>
#include <cam/scsi/scsi_all.h>
#include <cam/ctl/ctl.h>
#include <cam/ctl/ctl_io.h>
#include <cam/ctl/ctl_ioctl.h>

#include "ctld.h"
#include "control.h"

static int
fd_add(int fd, fd_set *fdset, int nfds);

struct client {
	TAILQ_ENTRY(client)	te;
	int fd;

	// read buffer
	char rbuf[16384];
};

struct arg {
	const char *name;
	char *value;

};

static
void client_new(int fd);

static
void client_do_read(struct client *cl, struct conf *config);

static
void client_do_write(struct client *cl, const char *fmt, ...);

static
void client_close(struct client *cl);

static
void client_auth_group_set(struct client *cl, struct conf *config, const char *id, const  struct arg *args);

static
void client_auth_group_del(struct client *cl, struct conf *config, const char *id, const struct arg *args);

static
void client_lun_set(struct client *cl, struct conf *config, const char *id, const  struct arg *args);

static
void client_lun_del(struct client *cl, struct conf *config, const char *id, const  struct arg *args);

static
void client_target_add(struct client *cl, struct conf *config, const char *id, const  struct arg *args);

static
void client_target_set_lun(struct client *cl, struct conf *config, const char *id, const  struct arg *args);

static
void client_target_del(struct client *cl, struct conf *config, const char *id, const struct arg *args);

static struct command {
	const char *cmd;
	void (*f)(struct client *cl, struct conf *config, const char *id, const struct arg *args);
} commands[] = {
	{.cmd = "auth-group-set", .f = client_auth_group_set},
	{.cmd = "auth-group-del", .f = client_auth_group_del},
	{.cmd = "lun-set", .f = client_lun_set},
	{.cmd = "lun-del", .f = client_lun_del},
	{.cmd = "target-add", .f = client_target_add},
	{.cmd = "target-set-lun", .f = client_target_set_lun},
	{.cmd = "target-del", .f = client_target_del},
	{.cmd = 0},
};

static
bool update_string(char **ptr, const char *newval);

static
bool update_int(int *ptr, const char *newval);

static
bool update_int64_t(int64_t *ptr, const char *newval);

static
char* unquote(const char *src);

static TAILQ_HEAD(, client) clients = TAILQ_HEAD_INITIALIZER(clients);

static int control_fd = -1;

static struct sockaddr_un addr = {.sun_family=AF_UNIX};

extern int kernel_get_ctl_fd(void);

int control_init(const char *sock)
{
	control_fd = socket(PF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0);
	if (control_fd == -1) {
		return -1;
	}

	strlcpy(addr.sun_path, sock, sizeof(addr.sun_path));

	if (bind(control_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
		close(control_fd);
		control_fd = -1;
		return -1;
	}

	if (listen(control_fd, 10) == -1) {
		close(control_fd);
		control_fd = -1;
		return -1;
	}

	return 0;
}

void control_shutdown(void)
{
	struct client *cl, *tcl;

	if (control_fd >= 0) {
		close(control_fd);
		control_fd = -1;
		unlink(addr.sun_path);
	}

	TAILQ_FOREACH_SAFE(cl, &clients, te, tcl) {
		client_close(cl);
	}
}

int control_add_fds(fd_set *fdset, int nfds)
{
	struct client *cl;

	if (control_fd >= 0) {
		nfds = fd_add(control_fd, fdset, nfds);
	}

	TAILQ_FOREACH(cl, &clients, te)
		nfds = fd_add(cl->fd, fdset, nfds);

	return nfds;
}

void control_handle_fds(struct conf *config, fd_set *fdset)
{
	struct client *cl, *tcl;

	if (control_fd == -1)
		return;

	if (FD_ISSET(control_fd, fdset)) {
		struct sockaddr_un cl_addr;
		socklen_t cl_addr_len = sizeof(cl_addr);
		int clfd;

		clfd = accept(control_fd, (struct sockaddr*)&cl_addr, &cl_addr_len);
		if (clfd >= 0) {
			client_new(clfd);
		}
	}

	TAILQ_FOREACH_SAFE(cl, &clients, te, tcl) {
		if (FD_ISSET(cl->fd, fdset)) {
			client_do_read(cl, config);
		}
	}
}

static
void client_new(int fd)
{
	struct client *cl;

	cl = calloc(1, sizeof(*cl));
	if (cl == NULL)
		log_err(1, "calloc");

	cl->fd = fd;
	TAILQ_INSERT_TAIL(&clients, cl, te);
}

void client_close(struct client *cl)
{
	TAILQ_REMOVE(&clients, cl, te);

	close(cl->fd);
	free(cl);
}

#define MAX_ARGS 64

void client_do_read(struct client *cl, struct conf *config)
{
	int len;
	char *p;
	const char *cmd;
	char *id;
	struct command *c;
	struct arg args[MAX_ARGS];
	int argc;

	len = read(cl->fd, cl->rbuf, sizeof(cl->rbuf) - 1);
	if (len <= 0) {
		client_close(cl);
		return;
	}

	// last valid character
	p = &cl->rbuf[len - 1];

	// strip trailing newline
	while (p >= cl->rbuf && (*p == '\n' || *p == '\r'))
		p--;

	// p points to last non-newline char, terminate it
	p[1] = 0;

	p = cl->rbuf;
	cmd = strsep(&p, " ");
	id = unquote(strsep(&p, " "));

	for (c = &commands[0]; c->cmd != NULL; c++) {
		if (strcmp(cmd, c->cmd) == 0) {
			break;
		}
	}

	if (c->cmd == NULL) {
		client_do_write(cl, "UNKNOWN COMMAND");

		free(id);

		return;
	}

	for (argc = 0; p != NULL && argc < MAX_ARGS - 1; argc++) {
		char *tmp = strsep(&p, " ");

		args[argc].name = strsep(&tmp, "=");
		args[argc].value = unquote(tmp);
	}

	args[argc].name = NULL;

	c->f(cl, config, id, args);

	for (int i = 0; i < argc; i++) {
		free(args[i].value);
	}

	free(id);
}

static
void client_do_write(struct client *cl, const char *fmt, ...)
{
	char buf[1024];
	int len;
	va_list ap;

	va_start(ap, fmt);
	len = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
	va_end(ap);

	strcat(buf+len, "\n");
	len += 1;

	if (send(cl->fd, buf, len, MSG_NOSIGNAL) != len) {
		client_close(cl);
	}
}

static int
fd_add(int fd, fd_set *fdset, int nfds)
{
	FD_SET(fd, fdset);
	if (fd > nfds)
		nfds = fd;
	return (nfds);
}

static
void client_auth_group_set(struct client *cl, struct conf *config, const char *id, const struct arg *args)
{
	static const char *help = "Usage: <id> [type[=none|deny|chap|chap-mutual]] [auth[=user:secret[:user2:secret2]]]...";

	struct auth_group *ag;
	struct auth *authp, *authn;
	const char *type;

	if (id == NULL) {
		client_do_write(cl, help);

		return;
	}

	if (strcmp(id, "default") == 0) {
		client_do_write(cl, "ERR: cannot update default");

		return;
	}

	ag = auth_group_find(config, id);
	if (ag == NULL) {
		ag = auth_group_new(config, id);
	}

	for (const struct arg *arg = args; arg->name != NULL; arg++) {
		if (strcmp(arg->name, "type") == 0) {
			ag->ag_type = AG_TYPE_UNKNOWN;
			TAILQ_FOREACH_SAFE(authp, &ag->ag_auths, a_next, authn)
				auth_delete(authp);

			if (arg->value != NULL) {
				if (auth_group_set_type(ag, arg->value) != 0) {
					client_do_write(cl, "Failed to set auth-type to %s", type);

					return;
				}
			}
		} else if (strcmp(arg->name, "auth") == 0) {
			if (arg->value == NULL) {
				TAILQ_FOREACH_SAFE(authp, &ag->ag_auths, a_next, authn)
					auth_delete(authp);
			} else {
				char *user, *secret, *user2, *secret2;
				char *value = arg->value;

				user = strsep(&value, ":");
				secret = strsep(&value, ":");
				user2 = strsep(&value, ":");
				secret2 = strsep(&value, ":");

				if (user == NULL || secret == NULL) {
					client_do_write(cl, "AG %s: invalid auth, incomplete", id);

					return;
				}

				user = unquote(user);
				secret = unquote(secret);
				user2 = unquote(user2);
				secret2 = unquote(secret2);

				if (user2 == NULL || secret2 == NULL) {
					auth_new_chap(ag, user, secret);
				} else {
					auth_new_chap_mutual(ag, user, secret, user2, secret2);
				}

				free(user);
				free(secret);
				free(user2);
				free(secret2);
			}
		} else {
			client_do_write(cl, "UNKNOWN_ARG");
		}
	}

	client_do_write(cl, "OK");
}

static
void client_auth_group_del(struct client *cl, struct conf *config, const char *id, const struct arg *args)
{
	static const char *help = "Usage: <id>";
	(void)args;
	struct auth_group *ag;
	struct target *tgt;

	if (id == NULL) {
		client_do_write(cl, help);

		return;
	}

	if (strcmp(id, "default") == 0) {
		client_do_write(cl, "ERR: cannot delete default");

		return;
	}

	ag = auth_group_find(config, id);
	if (ag == NULL) {
		client_do_write(cl, "OK - not found");

		return;
	}

	TAILQ_FOREACH(tgt, &config->conf_targets, t_next) {
		if (tgt->t_auth_group == ag) {
			tgt->t_auth_group = auth_group_find(config, "default");
		}
	}

	auth_group_delete(ag);

	client_do_write(cl, "OK");
}

static
void client_lun_set(struct client *cl, struct conf *config, const char *id, const struct arg *args)
{
	static const char *help = "Usage: <id> [backend[=block|ramdisk]] [blocksize[=size]] [ctl-lun=lun_id] [device-id[=string]] [device-type[=type]] [option=name[=val]] [path=path] [serial[=string]] [size[=size]]";
	struct lun *lun;
	struct lun old;
	bool need_remove = false;
	bool exists = false;
	char old_backend[32];

	if (id == NULL) {
		client_do_write(cl, help);

		return;
	}

	lun = lun_find(config, id);
	if (lun == NULL) {
		lun = lun_new(config, id);

		lun->l_backend = checked_strdup("block");

		need_remove = false;
	} else {
		strlcpy(old_backend, lun->l_backend, sizeof(old_backend));
		old.l_backend = old_backend;
		old.l_ctl_lun = lun->l_ctl_lun;

		exists = true;
	}

	// Process args
	for (const struct arg *arg = args; arg->name != NULL; arg++) {
		if (strcmp(arg->name, "backend") == 0) {
			const char *backend = arg->value ? arg->value : "block";

			need_remove |= update_string(&lun->l_backend, backend);

		} else if (strcmp(arg->name, "blocksize") == 0) {
			need_remove |= update_int(&lun->l_blocksize, arg->value);

		} else if (strcmp(arg->name, "ctl-lun") == 0) {
			if (lun->l_ctl_lun == -1) {
				update_int(&lun->l_ctl_lun, arg->value);
			}

		} else if (strcmp(arg->name, "device-id") == 0) {
			need_remove |= update_string(&lun->l_device_id, arg->value);

		} else if (strcmp(arg->name, "device-type") == 0) {
			uint64_t tmp;

			if (strcasecmp(arg->value, "disk") == 0 ||
				strcasecmp(arg->value, "direct") == 0)
				tmp = 0;
			else if (strcasecmp(arg->value, "processor") == 0)
				tmp = 3;
			else if (strcasecmp(arg->value, "cd") == 0 ||
				strcasecmp(arg->value, "cdrom") == 0 ||
				strcasecmp(arg->value, "dvd") == 0 ||
				strcasecmp(arg->value, "dvdrom") == 0)
				tmp = 5;
			else {
				char *endp;

				tmp = strtol(arg->value, &endp, 0);
				if (*arg->value == 0 || *endp != 0 || tmp > 15) {
					client_do_write(cl, "ERR: device-type: invalid value");

					return;
				}
			}

			if (lun->l_device_type != tmp) {
				lun->l_device_type = tmp;

				need_remove = true;
			}

		} else if (strcmp(arg->name, "option") == 0) {
			char *value = arg->value;
			const char *opname = strsep(&value, "=");
			struct option *op;

			if (opname == NULL) {
				client_do_write(cl, "ERR: must specify option name");

				return;
			}

			op = option_find(&lun->l_options, opname);

			value = unquote(value);

			if (value != NULL) {
				if (op == NULL) {
					option_new(&lun->l_options, opname, value);
					need_remove = true;
				} else {
					if (strcmp(op->o_value, value) != 0) {
						option_set(op, value);
						need_remove = true;
					}
				}
			} else {
				if (op != NULL) {
					option_delete(&lun->l_options, op);
					need_remove = true;
				}
			}

			free(value);

		} else if (strcmp(arg->name, "path") == 0) {
			need_remove |= update_string(&lun->l_path, arg->value);

		} else if (strcmp(arg->name, "serial") == 0) {
			need_remove |= update_string(&lun->l_serial, arg->value);

		} else if (strcmp(arg->name, "size") == 0) {
			update_int64_t(&lun->l_size, arg->value);

		} else {
			client_do_write(cl, "ERR: unknown arg: %s", arg->name);

			return;
		}
	}

	if (need_remove && exists) {
		kernel_lun_remove(&old);
	}

	if (exists && !need_remove) {
		kernel_lun_modify(lun);
	} else {
		kernel_lun_add(lun);
	}

	client_do_write(cl, "OK");
}

static
void client_lun_del(struct client *cl, struct conf *config, const char *id, const struct arg *args)
{
	static const char *help = "Usage: <id>";
	struct lun *lun;
	(void)args;

	if (id == NULL) {
		client_do_write(cl, help);

		return;
	}

	lun = lun_find(config, id);
	if (lun == NULL) {
		client_do_write(cl, "OK - not found");

		return;
	}

	kernel_lun_remove(lun);

	lun_delete(lun);

	client_do_write(cl, "OK");
}

static
void client_target_add(struct client *cl, struct conf *config, const char *id, const struct arg *args)
{
	static const char *help = "Usage: <id> [alias[=text]] [auth-group=name] [portal-group=pgname[:agname]]";
	struct target *tgt;

	if (id == NULL) {
		client_do_write(cl, help);

		return;
	}

	tgt = target_new(config, id);
	if (tgt == NULL) {
		client_do_write(cl, "ERR: error creating target: wrong name or target already exists");

		return;
	}

	for (const struct arg *arg = args; arg->name != NULL; arg++) {
		if (strcmp(arg->name, "alias") == 0) {
			update_string(&tgt->t_alias, arg->value);

		} else if (strcmp(arg->name, "auth-group") == 0) {
			struct auth_group *ag;
			char *agname;

			if (arg->value == NULL) {
				client_do_write(cl, "ERR: must specify auth-group");

				return;
			}

			agname = unquote(arg->value);
			ag = auth_group_find(config, agname);
			free(agname);

			if (ag != NULL) {
				tgt->t_auth_group = ag;
			} else {
				client_do_write(cl, "ERR: unknown auth-group");

				return;
			}
		} else if (strcmp(arg->name, "portal-group") == 0) {
			char *pgname, *agname;
			struct portal_group *pg;
			struct auth_group *ag;
			struct port *p;

			if (arg->value == NULL) {
				client_do_write(cl, "ERR: must specify portal-group");

				return;
			}

			agname = arg->value;
			pgname = strsep(&agname, ":");

			pgname = unquote(pgname);
			pg = portal_group_find(config, pgname);
			free(pgname);

			if (pg == NULL) {
				client_do_write(cl, "ERR: unknown portal-group");

				return;
			}

			p = port_new(config, tgt, pg);

			kernel_port_add(p);

			if (agname != NULL) {
				agname = unquote(agname);
				ag = auth_group_find(config, agname);
				free(agname);

				if (ag == NULL) {
					client_do_write(cl, "ERR: unknown auth-group");

					return;
				}

				p->p_auth_group = ag;
			}
		}
	}

	if (tgt->t_auth_group == NULL) {
		tgt->t_auth_group = auth_group_find(config, "default");
	}

	client_do_write(cl, "OK");
}

static
void client_target_set_lun(struct client *cl, struct conf *config, const char *id, const struct arg *args)
{
	static const char *help = "Usage: <id> [lunX=vol]...";
	struct target *tgt;

	if (id == NULL) {
		client_do_write(cl, help);

		return;
	}

	tgt = target_find(config, id);
	if (tgt == NULL) {
		client_do_write(cl, "ERR: target not found");

		return;
	}

	for (const struct arg *arg = args; arg->name != NULL; arg++) {
		if (strncmp(arg->name, "lun", 3) == 0) {
			struct lun *lun;
			struct port *p;

			int idx = strtol(arg->name + 3, NULL, 0);
			if (idx < 0 || idx > MAX_LUNS - 1) {
				client_do_write(cl, "ERR: invalid lun index");

				return;
			}

			if (arg->value != NULL) {
				char *lunname = unquote(arg->value);
				lun = lun_find(config, lunname);
				free(lunname);

				if (lun == NULL) {
					client_do_write(cl, "ERR: lun not found: %s", arg->value);

					return;
				}
			} else {
				lun = NULL;
			}

			tgt->t_luns[idx] = lun;

			// update kernel port
			TAILQ_FOREACH(p, &tgt->t_ports, p_ts) {
				struct ctl_lun_map lm;
				int error;

				lm.port = p->p_ctl_port;
				lm.plun = idx;

				if (lun == NULL) {
					lm.lun = UINT32_MAX;
				} else {
					lm.lun = lun->l_ctl_lun;
				}

				error = ioctl(kernel_get_ctl_fd(), CTL_LUN_MAP, &lm);
				if (error != 0)
					log_warn("CTL_LUN_MAP ioctl failed");
			}
		}
	}

	client_do_write(cl, "OK");
}

static
void client_target_del(struct client *cl, struct conf *config, const char *id, const struct arg *args)
{
	static const char *help = "Usage: <id>";
	struct target *tgt;
	struct port *p, *pp;
	(void)args;

	if (id == NULL) {
		client_do_write(cl, help);

		return;
	}

	tgt = target_find(config, id);
	if (tgt == NULL) {
		client_do_write(cl, "OK - not found");

		return;
	}

	TAILQ_FOREACH_SAFE(p, &tgt->t_ports, p_ts, pp) {
		kernel_port_remove(p);
		port_delete(p);
	}

	target_delete(tgt);

	client_do_write(cl, "OK");
}

static
bool update_string(char **ptr, const char *newval)
{
	bool updated = false;
	char *val = unquote(newval);

	if (val != NULL) {
		if (*ptr == NULL) {
			*ptr = checked_strdup(val);
			updated = true;
		} else {
			if (strcmp(*ptr, val) != 0) {
				free(*ptr);
				*ptr = checked_strdup(val);
				updated = true;
			}
		}
	} else {
		if (*ptr != NULL) {
			free(*ptr);
			*ptr = NULL;
			updated = true;
		}
	}

	free(val);

	return updated;
}

static
bool update_int(int *ptr, const char *newval)
{
	int new;

	if (newval != NULL) {
		new = strtol(newval, NULL, 0);
	} else {
		new = 0;
	}

	if (*ptr != new) {
		*ptr = new;

		return true;
	}

	return false;
}

static
bool update_int64_t(int64_t *ptr, const char *newval)
{
	int64_t new;

	if (newval != NULL) {
		new = strtoll(newval, NULL, 0);
	} else {
		new = 0;
	}

	if (*ptr != new) {
		*ptr = new;

		return true;
	}

	return false;
}

static
inline char nibble(char ch)
{
	switch (ch) {
		case '0' ... '9':
			return ch - '0';
		case 'a' ... 'f':
			return ch - 'a' + 10;
		case 'A' ... 'F':
			return ch - 'A' + 10;
	}

	return 0;
}

static
char* unquote(const char *src)
{
	int len;
	char *dst;
	const char *s;
	char *d;

	if (src == NULL)
		return NULL;

	len = strlen(src);
	dst = malloc(len+1);
	if (dst == NULL)
		log_err(2, "unquote: malloc failed");

	for (s = src, d = dst; *s != 0; s++, d++) {
		char ch = *s;

		if (ch == '%') {
			if (s[1] == 0 || s[2] == 0) {
				break;
			}

			ch = (nibble(s[1]) << 4) | nibble(s[2]);
			s += 2;
		}

		*d = ch;
	}

	*d = 0;

	return dst;
}

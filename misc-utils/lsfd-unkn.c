/*
 * lsfd-unkn.c - handle associations opening unknown objects
 *
 * Copyright (C) 2021 Red Hat, Inc. All rights reserved.
 * Written by Masatake YAMATO <yamato@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "xalloc.h"
#include "nls.h"
#include "libsmartcols.h"

#include "lsfd.h"

struct unkn {
	struct file file;
	const struct anon_ops *anon_ops;
	void *anon_data;
};

struct anon_ops {
	const char *class;
	bool (*probe)(const char *);
	char * (*get_name)(struct unkn *);
	/* Return true is handled the column. */
	bool (*fill_column)(struct proc *,
			    struct unkn *,
			    struct libscols_line *,
			    int,
			    size_t,
			    char **str);
	void (*init)(struct unkn *);
	void (*free)(struct unkn *);
	int (*handle_fdinfo)(struct unkn *, const char *, const char *);
	void (*attach_xinfo)(struct unkn *);
	const struct ipc_class *ipc_class;
};

static const struct anon_ops *anon_probe(const char *);

static char * anon_get_class(struct unkn *unkn)
{
	char *name;

	if (unkn->anon_ops->class)
		return xstrdup(unkn->anon_ops->class);

	/* See unkn_init_content() */
	name = ((struct file *)unkn)->name + 11;
	/* Does it have the form anon_inode:[class]? */
	if (*name == '[') {
		size_t len = strlen(name + 1);
		if (*(name + 1 + len - 1) == ']')
			return strndup(name + 1, len - 1);
	}

	return xstrdup(name);
}

static bool unkn_fill_column(struct proc *proc,
			     struct file *file,
			     struct libscols_line *ln,
			     int column_id,
			     size_t column_index)
{
	char *str = NULL;
	struct unkn *unkn = (struct unkn *)file;

	switch(column_id) {
	case COL_NAME:
		if (unkn->anon_ops && unkn->anon_ops->get_name) {
			str = unkn->anon_ops->get_name(unkn);
			if (str)
				break;
		}
		return false;
	case COL_TYPE:
		if (!unkn->anon_ops)
			return false;
		/* FALL THROUGH */
	case COL_AINODECLASS:
		if (unkn->anon_ops) {
			str = anon_get_class(unkn);
			break;
		}
		return false;
	case COL_SOURCE:
		if (unkn->anon_ops) {
			str = xstrdup("anon_inodefs");
			break;
		}
		return false;
	default:
		if (unkn->anon_ops && unkn->anon_ops->fill_column) {
			if (unkn->anon_ops->fill_column(proc, unkn, ln,
							column_id, column_index, &str))
				break;
		}
		return false;
	}

	if (!str)
		err(EXIT_FAILURE, _("failed to add output data"));
	if (scols_line_refer_data(ln, column_index, str))
		err(EXIT_FAILURE, _("failed to add output data"));
	return true;
}

static void unkn_attach_xinfo(struct file *file)
{
	struct unkn *unkn = (struct unkn *)file;
	if (unkn->anon_ops && unkn->anon_ops->attach_xinfo)
		unkn->anon_ops->attach_xinfo(unkn);
}

static const struct ipc_class *unkn_get_ipc_class(struct file *file)
{
	struct unkn *unkn = (struct unkn *)file;

	if (unkn->anon_ops && unkn->anon_ops->ipc_class)
		return unkn->anon_ops->ipc_class;
	return NULL;
}

static void unkn_init_content(struct file *file)
{
	struct unkn *unkn = (struct unkn *)file;

	assert(file);
	unkn->anon_ops = NULL;
	unkn->anon_data = NULL;

	if (major(file->stat.st_dev) == 0
	    && strncmp(file->name, "anon_inode:", 11) == 0) {
		const char *rest = file->name + 11;

		unkn->anon_ops = anon_probe(rest);

		if (unkn->anon_ops->init)
			unkn->anon_ops->init(unkn);
	}
}

static void unkn_content_free(struct file *file)
{
	struct unkn *unkn = (struct unkn *)file;

	assert(file);
	if (unkn->anon_ops && unkn->anon_ops->free)
		unkn->anon_ops->free((struct unkn *)file);
}

static int unkn_handle_fdinfo(struct file *file, const char *key, const char *value)
{
	struct unkn *unkn = (struct unkn *)file;

	assert(file);
	if (unkn->anon_ops && unkn->anon_ops->handle_fdinfo)
		return unkn->anon_ops->handle_fdinfo(unkn, key, value);
	return 0;		/* Should be handled in parents */
}

/*
 * pidfd
 */
struct anon_pidfd_data {
	pid_t pid;
	char *nspid;
};

static bool anon_pidfd_probe(const char *str)
{
	return strncmp(str, "[pidfd]", 7) == 0;
}

static char *anon_pidfd_get_name(struct unkn *unkn)
{
	char *str = NULL;
	struct anon_pidfd_data *data = (struct anon_pidfd_data *)unkn->anon_data;

	char *comm = NULL;
	struct proc *proc = get_proc(data->pid);
	if (proc)
		comm = proc->command;

	xasprintf(&str, "pid=%d comm=%s nspid=%s",
		  data->pid,
		  comm? comm: "",
		  data->nspid? data->nspid: "");
	return str;
}

static void anon_pidfd_init(struct unkn *unkn)
{
	unkn->anon_data = xcalloc(1, sizeof(struct anon_pidfd_data));
}

static void anon_pidfd_free(struct unkn *unkn)
{
	struct anon_pidfd_data *data = (struct anon_pidfd_data *)unkn->anon_data;

	if (data->nspid)
		free(data->nspid);
	free(data);
}

static int anon_pidfd_handle_fdinfo(struct unkn *unkn, const char *key, const char *value)
{
	if (strcmp(key, "Pid") == 0) {
		uint64_t pid;

		int rc = ul_strtou64(value, &pid, 10);
		if (rc < 0)
			return 0; /* ignore -- parse failed */
		((struct anon_pidfd_data *)unkn->anon_data)->pid = (pid_t)pid;
		return 1;
	}
	else if (strcmp(key, "NSpid") == 0) {
		((struct anon_pidfd_data *)unkn->anon_data)->nspid = xstrdup(value);
		return 1;

	}
	return 0;
}

static bool anon_pidfd_fill_column(struct proc *proc  __attribute__((__unused__)),
				   struct unkn *unkn,
				   struct libscols_line *ln __attribute__((__unused__)),
				   int column_id,
				   size_t column_index __attribute__((__unused__)),
				   char **str)
{
	struct anon_pidfd_data *data = (struct anon_pidfd_data *)unkn->anon_data;

	switch(column_id) {
	case COL_PIDFD_COMM: {
		struct proc *pidfd_proc = get_proc(data->pid);
		char *pidfd_comm = NULL;
		if (pidfd_proc)
			pidfd_comm = pidfd_proc->command;
		if (pidfd_comm) {
			*str = xstrdup(pidfd_comm);
			return true;
		}
		break;
	}
	case COL_PIDFD_NSPID:
		if (data->nspid) {
			*str = xstrdup(data->nspid);
			return true;
		}
		break;
	case COL_PIDFD_PID:
		xasprintf(str, "%d", (int)data->pid);
		return true;
	}

	return false;
}

static const struct anon_ops anon_pidfd_ops = {
	.class = "pidfd",
	.probe = anon_pidfd_probe,
	.get_name = anon_pidfd_get_name,
	.fill_column = anon_pidfd_fill_column,
	.init = anon_pidfd_init,
	.free = anon_pidfd_free,
	.handle_fdinfo = anon_pidfd_handle_fdinfo,
};

/*
 * eventfd
 */
struct anon_eventfd_data {
	int id;
	struct unkn *backptr;
	struct ipc_endpoint endpoint;
};

struct eventfd_ipc {
	struct ipc ipc;
	int id;
};

static unsigned int anon_eventfd_get_hash(struct file *file)
{
	struct unkn *unkn = (struct unkn *)file;
	struct anon_eventfd_data *data = (struct anon_eventfd_data *)unkn->anon_data;

	return (unsigned int)data->id;
}

static bool anon_eventfd_is_suitable_ipc(struct ipc *ipc, struct file *file)
{
	struct unkn *unkn = (struct unkn *)file;
	struct anon_eventfd_data *data = (struct anon_eventfd_data *)unkn->anon_data;

	return ((struct eventfd_ipc *)ipc)->id == data->id;
}

static const struct ipc_class anon_eventfd_ipc_class = {
	.size = sizeof(struct eventfd_ipc),
	.get_hash = anon_eventfd_get_hash,
	.is_suitable_ipc = anon_eventfd_is_suitable_ipc,
	.free = NULL,
};

static bool anon_eventfd_probe(const char *str)
{
	return (strncmp(str, "[eventfd]", 9) == 0);
}

static char *anon_eventfd_get_name(struct unkn *unkn)
{
	char *str = NULL;
	struct anon_eventfd_data *data = (struct anon_eventfd_data *)unkn->anon_data;

	xasprintf(&str, "id=%d", data->id);
	return str;
}

static void anon_eventfd_init(struct unkn *unkn)
{
	struct anon_eventfd_data *data = xcalloc(1, sizeof(struct anon_eventfd_data));
	init_endpoint(&data->endpoint);
	data->backptr = unkn;
	unkn->anon_data = data;
}

static void anon_eventfd_free(struct unkn *unkn)
{
	free(unkn->anon_data);
}

static void anon_eventfd_attach_xinfo(struct unkn *unkn)
{
	struct anon_eventfd_data *data = (struct anon_eventfd_data *)unkn->anon_data;
	unsigned int hash;
	struct ipc *ipc = get_ipc(&unkn->file);
	if (ipc)
		goto link;

	ipc = new_ipc(&anon_eventfd_ipc_class);
	((struct eventfd_ipc *)ipc)->id = data->id;

	hash = anon_eventfd_get_hash(&unkn->file);
	add_ipc(ipc, hash);

 link:
	add_endpoint(&data->endpoint, ipc);
}

static int anon_eventfd_handle_fdinfo(struct unkn *unkn, const char *key, const char *value)
{
	if (strcmp(key, "eventfd-id") == 0) {
		int64_t id;

		int rc = ul_strtos64(value, &id, 10);
		if (rc < 0)
			return 0;
		((struct anon_eventfd_data *)unkn->anon_data)->id = (int)id;
		return 1;
	}
	return 0;
}

static inline char *anon_eventfd_data_xstrendpoint(struct file *file)
{
	char *str = NULL;
	xasprintf(&str, "%d,%s,%d",
		  file->proc->pid, file->proc->command, file->association);
	return str;
}

static bool anon_eventfd_fill_column(struct proc *proc  __attribute__((__unused__)),
				     struct unkn *unkn,
				     struct libscols_line *ln __attribute__((__unused__)),
				     int column_id,
				     size_t column_index __attribute__((__unused__)),
				     char **str)
{
	struct anon_eventfd_data *data = (struct anon_eventfd_data *)unkn->anon_data;

	switch(column_id) {
	case COL_EVENTFD_ID:
		xasprintf(str, "%d", data->id);
		return true;
	case COL_ENDPOINTS: {
		struct list_head *e;
		char *estr;
		foreach_endpoint(e, data->endpoint) {
			struct anon_eventfd_data *other = list_entry(e,
								     struct anon_eventfd_data,
								     endpoint.endpoints);
			if (data == other)
				continue;
			if (*str)
				xstrputc(str, '\n');
			estr = anon_eventfd_data_xstrendpoint(&other->backptr->file);
			xstrappend(str, estr);
			free(estr);
		}
		if (!*str)
			return false;
		return true;
	}
	default:
		return false;
	}
}

static const struct anon_ops anon_eventfd_ops = {
	.class = "eventfd",
	.probe = anon_eventfd_probe,
	.get_name = anon_eventfd_get_name,
	.fill_column = anon_eventfd_fill_column,
	.init = anon_eventfd_init,
	.free = anon_eventfd_free,
	.handle_fdinfo = anon_eventfd_handle_fdinfo,
	.attach_xinfo = anon_eventfd_attach_xinfo,
	.ipc_class = &anon_eventfd_ipc_class,
};

/*
 * eventpoll
 */
struct anon_eventpoll_data {
	size_t count;
	int *tfds;
};


static bool anon_eventpoll_probe(const char *str)
{
	return strncmp(str, "[eventpoll]", 11) == 0;
}

static void anon_eventpoll_init(struct unkn *unkn)
{
	unkn->anon_data = xcalloc(1, sizeof(struct anon_eventpoll_data));
}

static void anon_eventpoll_free(struct unkn *unkn)
{
	struct anon_eventpoll_data *data = unkn->anon_data;
	free (data->tfds);
	free (data);
}

static int anon_eventpoll_handle_fdinfo(struct unkn *unkn, const char *key, const char *value)
{
	struct anon_eventpoll_data *data;
	if (strcmp(key, "tfd") == 0) {
		unsigned long tfd;
		char *end = NULL;

		errno = 0;
		tfd = strtoul(value, &end, 0);
		if (errno != 0)
			return 0; /* ignore -- parse failed */

		data = (struct anon_eventpoll_data *)unkn->anon_data;
		data->tfds = xreallocarray (data->tfds, ++data->count, sizeof(int));
		data->tfds[data->count - 1] = (int)tfd;
		return 1;
	}
	return 0;
}

static int intcmp (const void *a, const void *b)
{
	int ai = *(int *)a;
	int bi = *(int *)b;

	return ai - bi;
}

static void anon_eventpoll_attach_xinfo(struct unkn *unkn)
{
	struct anon_eventpoll_data *data = (struct anon_eventpoll_data *)unkn->anon_data;
	qsort (data->tfds, data->count, sizeof (data->tfds[0]),
	       intcmp);
}

static char *anon_eventpoll_make_tfds_string(struct anon_eventpoll_data *data,
					     const char *prefix)
{
	char *str = prefix? xstrdup(prefix): NULL;

	char buf[256];
	for (size_t i = 0; i < data->count; i++) {
		size_t offset = 0;

		if (i > 0) {
			buf[0] = ',';
			offset = 1;
		}
		snprintf(buf + offset, sizeof(buf) - offset, "%d", data->tfds[i]);
		xstrappend(&str, buf);
	}
	return str;
}

static char *anon_eventpoll_get_name(struct unkn *unkn)
{
	return anon_eventpoll_make_tfds_string ((struct anon_eventpoll_data *)unkn->anon_data,
						"tfds=");
}

static bool anon_eventpoll_fill_column(struct proc *proc  __attribute__((__unused__)),
				       struct unkn *unkn,
				       struct libscols_line *ln __attribute__((__unused__)),
				       int column_id,
				       size_t column_index __attribute__((__unused__)),
				       char **str)
{
	struct anon_eventpoll_data *data = (struct anon_eventpoll_data *)unkn->anon_data;

	switch(column_id) {
	case COL_EVENTPOLL_TFDS:
		*str =anon_eventpoll_make_tfds_string (data, NULL);
		if (*str)
			return true;
		break;
	}

	return false;
}

static const struct anon_ops anon_eventpoll_ops = {
	.class = "eventpoll",
	.probe = anon_eventpoll_probe,
	.get_name = anon_eventpoll_get_name,
	.fill_column = anon_eventpoll_fill_column,
	.init = anon_eventpoll_init,
	.free = anon_eventpoll_free,
	.handle_fdinfo = anon_eventpoll_handle_fdinfo,
	.attach_xinfo = anon_eventpoll_attach_xinfo,
};

/*
 * generic (fallback implementation)
 */
static const struct anon_ops anon_generic_ops = {
	.class = NULL,
	.get_name = NULL,
	.fill_column = NULL,
	.init = NULL,
	.free = NULL,
	.handle_fdinfo = NULL,
};

static const struct anon_ops *anon_ops[] = {
	&anon_pidfd_ops,
	&anon_eventfd_ops,
	&anon_eventpoll_ops,
};

static const struct anon_ops *anon_probe(const char *str)
{
	for (size_t i = 0; i < ARRAY_SIZE(anon_ops); i++)
		if (anon_ops[i]->probe(str))
			return anon_ops[i];
	return &anon_generic_ops;
}

const struct file_class unkn_class = {
	.super = &file_class,
	.size = sizeof(struct unkn),
	.fill_column = unkn_fill_column,
	.initialize_content = unkn_init_content,
	.free_content = unkn_content_free,
	.handle_fdinfo = unkn_handle_fdinfo,
	.attach_xinfo = unkn_attach_xinfo,
	.get_ipc_class = unkn_get_ipc_class,
};

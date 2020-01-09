/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * Process groups and sessions
 *
 * Copyright 2019 Phoenix Systems
 * Author: Jan Sikorski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include "proc.h"

#define DEBUG_LOG(fmt, ...) // lib_printf("%s:%d  %s(): " fmt "\n", __FILE__, __LINE__, __FUNCTION__, ##__VA_ARGS__)
#define process_pid(p) (p->id)

/* process groups and sessions */

static void ses_destroy(session_t *ses)
{
	DEBUG_LOG("%d", ses->id);
	vm_kfree(ses);
}


static int ses_leader(process_t *p)
{
	return process_pid(p) == p->group->session->id;
}


static void ses_add(session_t *ses, process_group_t *pg)
{
	pg->session = ses;
	LIST_ADD(&ses->members, pg);
}


static void ses_remove(process_group_t *pg)
{
	session_t *ses = pg->session;

	if (ses != NULL) {
		LIST_REMOVE(&ses->members, pg);

		if (ses->members == NULL)
			ses_destroy(ses);

		pg->session = NULL;
	}
}


static int ses_new(process_t *p)
{
	session_t *ses;

	if ((ses = vm_kmalloc(sizeof(*ses))) == NULL)
		return ENOMEM;

	ses->ctty = NULL;
	ses->members = NULL;
	ses->id = process_pid(p);

	DEBUG_LOG("%d", ses->id);

	ses_remove(p->group);
	ses_add(ses, p->group);

	return EOK;
}


static void pg_destroy(process_group_t *pg)
{
	DEBUG_LOG("%d", pg->id);
	vm_kfree(pg);
}


static int pg_leader(process_t *p)
{
	return process_pid(p) == p->group->id;
}


static void pg_add(process_group_t *pg, process_t *p)
{
	p->group = pg;
	LIST_ADD_EX(&pg->members, p, pg_next, pg_prev);
}


static void pg_remove(process_t *p)
{
	process_group_t *pg = p->group;

	if (pg != NULL) {
		LIST_REMOVE_EX(&pg->members, p, pg_next, pg_prev);

		if (pg->members == NULL) {
			ses_remove(pg);
			pg_destroy(pg);
		}

		p->group = NULL;
	}
}


static int pg_new(process_t *p)
{
	process_group_t *pg;

	if ((pg = vm_kmalloc(sizeof(*pg))) == NULL)
		return -ENOMEM;

	pg->members = NULL;
	pg->session = NULL;
	pg->id = process_pid(p);

	DEBUG_LOG("creating pg %d", pg->id);

	if (p->group != NULL)
		ses_add(p->group->session, pg);

	pg_remove(p);
	pg_add(pg, p);

	return EOK;
}


pid_t proc_setsid(process_t *p)
{
	DEBUG_LOG("enter");
	pid_t retval;

	process_lock(p);
	proctree_lock();

	if (pg_leader(p))
		retval = -EPERM;

	else if (pg_new(p) != EOK || ses_new(p) != EOK)
		retval = -ENOMEM;

	else
		retval = p->group->session->id;

	proctree_unlock();
	process_unlock(p);

	return retval;
}


int proc_setpgid(process_t *p, pid_t pid, pid_t pgid)
{
	process_t *s;
	process_group_t *pg;
	int err = EOK;

	DEBUG_LOG("enter %d, %d", pid, pgid);

	if (pgid < 0)
		return -EINVAL;

	process_lock(p);
	if (!pid) {
		pid = process_pid(p);
		s = p;
	}
	else if ((s = p->children) != NULL) {
		do {
			if (process_pid(s) == pid)
				break;
		} while ((s = s->next) != p->children);
	}

	if (!s || process_pid(s) != pid) {
		DEBUG_LOG("child not found");
		process_unlock(p);
		return -ESRCH;
	}

	/* TODO: return EACCESS if trying to setpgid a child that has already called exec() */
	proctree_lock();
	if (ses_leader(s) || s->group->session != p->group->session) {
		DEBUG_LOG("leader or different session");
		err = -EPERM;
	}
	else if (s->group->id == pgid) {
		err = EOK;
	}
	else if (pgid == 0 || pgid == pid) {
		pg_new(s);
	}
	else {
		pg = s->group;
		do {
			if (pg->id == pgid)
				break;
		} while ((pg = pg->next) != s->group);

		if (pg->id == pgid) {
			pg_remove(s);
			pg_add(pg, s);
		}
		else {
			DEBUG_LOG("pgid %d not found", pgid);
			err = -EPERM;
		}
	}
	proctree_unlock();
	process_unlock(p);

	return err;
}


pid_t proc_getpgid(process_t *p, pid_t pid)
{
	process_t *s;
	pid_t retval;
	DEBUG_LOG("enter");

	if (pid < 0)
		return -EINVAL;

	if (pid != 0)
		s = proc_find(pid);
	else
		s = p;

	if (s == NULL)
		return -ESRCH;

	proctree_lock();
#if 0
	if (s->group->session->id != p->group->session->id) {
		DEBUG_LOG("process %d in different session", pid);
		/* NOTE: disallowing this is optional */
		retval = -EPERM;
	}
	else
#endif
	{
		retval = s->group->id;
	}
	proctree_unlock();

	if (pid != 0)
		proc_put(s);

	return retval;
}


pid_t proc_getsid(process_t *p, pid_t pid)
{
	process_t *s;
	pid_t retval;

	DEBUG_LOG("enter");

	if (pid < 0)
		return -EINVAL;

	if (pid != 0)
		s = proc_find(pid);
	else
		s = p;

	if (s == NULL)
		return -ESRCH;

	proctree_lock();
#if 0
	if (s->group->session->id != p->group->session->id) {
		DEBUG_LOG("process %d in different session", pid);
		/* NOTE: disallowing this is optional */
		retval = -EPERM;
	}
	else
#endif
	{
		retval = s->group->session->id;
	}
	proctree_unlock();

	if (pid != 0)
		proc_put(s);

	return retval;
}


void proc_groupLeave(process_t *process)
{
	proctree_lock();
	pg_remove(process);
	proctree_unlock();
}


int proc_groupInit(process_t *process, process_t *parent)
{
	int err = EOK;

	proctree_lock();
	if (parent != NULL) {
		pg_add(parent->group, process);
	}
	else {
		if ((err = pg_new(process)) == EOK)
			err = ses_new(process);
	}
	proctree_unlock();

	return err;
}

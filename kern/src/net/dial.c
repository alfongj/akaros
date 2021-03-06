// INFERNO
#include <vfs.h>
#include <kfs.h>
#include <slab.h>
#include <kmalloc.h>
#include <kref.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <error.h>
#include <cpio.h>
#include <pmap.h>
#include <smp.h>
#include <ip.h>

typedef struct DS DS;

static int call(char *cp, char *cp1, DS * DS);
static int csdial(DS * DS);
static void _dial_string_parse(char *cp, DS * DS);
static int nettrans(char *cp, char *cp1, int na, char *cp2, int i);

enum {
	Maxstring = 128,
};

struct DS {
	char buf[Maxstring];		/* dist string */
	char *netdir;
	char *proto;
	char *rem;
	char *local;				/* other args */
	char *dir;
	int *cfdp;
};

/* only used here for now. */
static void kerrstr(void *err, int len)
{
	strncpy(err, current_errstr(), len);
}

/*
 *  the dialstring is of the form '[/net/]proto!dest'
 */
int kdial(char *dest, char *local, char *dir, int *cfdp)
{
	DS ds;
	int rv;
	char err[ERRMAX], alterr[ERRMAX];

	ds.local = local;
	ds.dir = dir;
	ds.cfdp = cfdp;

	_dial_string_parse(dest, &ds);
	if (ds.netdir)
		return csdial(&ds);

	ds.netdir = "/net";
	rv = csdial(&ds);
	if (rv >= 0)
		return rv;

	err[0] = 0;
	strncpy(err, current_errstr(), sizeof err);
	if (strstr(err, "refused") != 0) {
		return rv;
	}

	ds.netdir = "/net.alt";
	rv = csdial(&ds);
	if (rv >= 0)
		return rv;

	alterr[0] = 0;
	kerrstr(alterr, sizeof err);

	if (strstr(alterr, "translate") || strstr(alterr, "does not exist"))
		kerrstr(err, sizeof err);
	else
		kerrstr(alterr, sizeof alterr);
	return rv;
}

static int csdial(DS * ds)
{
	int n, fd, rv;
	char *p, buf[Maxstring], clone[Maxpath], err[ERRMAX], besterr[ERRMAX];

	/*
	 *  open connection server
	 */
	snprintf(buf, sizeof(buf), "%s/cs", ds->netdir);
	fd = sysopen(buf, ORDWR);
	if (fd < 0) {
		/* no connection server, don't translate */
		snprintf(clone, sizeof(clone), "%s/%s/clone", ds->netdir, ds->proto);
		return call(clone, ds->rem, ds);
	}

	/*
	 *  ask connection server to translate
	 */
	snprintf(buf, sizeof(buf), "%s!%s", ds->proto, ds->rem);
	if (syswrite(fd, buf, strlen(buf)) < 0) {
		kerrstr(err, sizeof err);
		sysclose(fd);
		set_errstr("%s (%s)", err, buf);
		return -1;
	}

	/*
	 *  loop through each address from the connection server till
	 *  we get one that works.
	 */
	*besterr = 0;
	strncpy(err, Egreg, sizeof(err));
	rv = -1;
	sysseek(fd, 0, 0);
	while ((n = sysread(fd, buf, sizeof(buf) - 1)) > 0) {
		buf[n] = 0;
		p = strchr(buf, ' ');
		if (p == 0)
			continue;
		*p++ = 0;
		rv = call(buf, p, ds);
		if (rv >= 0)
			break;
		err[0] = 0;
		kerrstr(err, sizeof err);
		if (strstr(err, "does not exist") == 0)
			memmove(besterr, err, sizeof besterr);
	}
	sysclose(fd);

	if (rv < 0 && *besterr)
		kerrstr(besterr, sizeof besterr);
	else
		kerrstr(err, sizeof err);
	return rv;
}

static int call(char *clone, char *dest, DS * ds)
{
	int fd, cfd, n;
	char name[Maxpath], data[Maxpath], err[ERRMAX], *p;

	cfd = sysopen(clone, ORDWR);
	if (cfd < 0) {
		kerrstr(err, sizeof err);
		set_errstr("%s (%s)", err, clone);
		return -1;
	}

	/* get directory name */
	n = sysread(cfd, name, sizeof(name) - 1);
	if (n < 0) {
		kerrstr(err, sizeof err);
		sysclose(cfd);
		set_errstr("read %s: %s", clone, err);
		return -1;
	}
	name[n] = 0;
	for (p = name; *p == ' '; p++) ;
	snprintf(name, sizeof(name), "%ld", strtoul(p, 0, 0));
	p = strrchr(clone, '/');
	*p = 0;
	if (ds->dir)
		snprintf(ds->dir, NETPATHLEN, "%s/%s", clone, name);
	snprintf(data, sizeof(data), "%s/%s/data", clone, name);

	/* connect */
	if (ds->local)
		snprintf(name, sizeof(name), "connect %s %s", dest, ds->local);
	else
		snprintf(name, sizeof(name), "connect %s", dest);
	if (syswrite(cfd, name, strlen(name)) < 0) {
		err[0] = 0;
		kerrstr(err, sizeof err);
		sysclose(cfd);
		set_errstr("%s (%s)", err, name);
		return -1;
	}

	/* open data connection */
	fd = sysopen(data, ORDWR);
	if (fd < 0) {
		err[0] = 0;
		kerrstr(err, sizeof err);
		set_errstr("%s (%s)", err, data);
		sysclose(cfd);
		return -1;
	}
	if (ds->cfdp)
		*ds->cfdp = cfd;
	else
		sysclose(cfd);

	return fd;
}

/*
 *  parse a dial string
 */
static void _dial_string_parse(char *str, DS * ds)
{
	char *p, *p2;

	strncpy(ds->buf, str, Maxstring);
	ds->buf[Maxstring - 1] = 0;

	p = strchr(ds->buf, '!');
	if (p == 0) {
		ds->netdir = 0;
		ds->proto = "net";
		ds->rem = ds->buf;
	} else {
		if (*ds->buf != '/' && *ds->buf != '#') {
			ds->netdir = 0;
			ds->proto = ds->buf;
		} else {
			for (p2 = p; *p2 != '/'; p2--) ;
			*p2++ = 0;
			ds->netdir = ds->buf;
			ds->proto = p2;
		}
		*p = 0;
		ds->rem = p + 1;
	}
}

/*
 *  announce a network service.
 */
int kannounce(char *addr, char *dir)
{
	int ctl, n, m;
	char buf[NETPATHLEN];
	char buf2[Maxpath];
	char netdir[NETPATHLEN];
	char naddr[Maxpath];
	char *cp;

	/*
	 *  translate the address
	 */
	if (nettrans(addr, naddr, sizeof(naddr), netdir, sizeof(netdir)) < 0)
		return -1;

	/*
	 * get a control channel
	 */
	ctl = sysopen(netdir, ORDWR);
	if (ctl < 0)
		return -1;
	cp = strrchr(netdir, '/');
	*cp = 0;

	/*
	 *  find out which line we have
	 */
	n = snprintf(buf, sizeof(buf), "%.*s/", sizeof buf, netdir);
	m = sysread(ctl, &buf[n], sizeof(buf) - n - 1);
	if (m <= 0) {
		sysclose(ctl);
		return -1;
	}
	buf[n + m] = 0;

	/*
	 *  make the call
	 */
	n = snprintf(buf2, sizeof buf2, "announce %s", naddr);
	if (syswrite(ctl, buf2, n) != n) {
		sysclose(ctl);
		return -1;
	}

	/*
	 *  return directory etc.
	 */
	if (dir)
		strncpy(dir, buf, sizeof(dir));
	return ctl;
}

/*
 *  listen for an incoming call
 */
int klisten(char *dir, char *newdir)
{
	int ctl, n, m;
	char buf[NETPATHLEN];
	char *cp;

	/*
	 *  open listen, wait for a call
	 */
	snprintf(buf, sizeof buf, "%s/listen", dir);
	ctl = sysopen(buf, ORDWR);
	if (ctl < 0)
		return -1;

	/*
	 *  find out which line we have
	 */
	strncpy(buf, dir, sizeof(buf));
	cp = strrchr(buf, '/');
	*++cp = 0;
	n = cp - buf;
	m = sysread(ctl, cp, sizeof(buf) - n - 1);
	if (m <= 0) {
		sysclose(ctl);
		return -1;
	}
	buf[n + m] = 0;

	/*
	 *  return directory etc.
	 */
	if (newdir)
		strncpy(newdir, buf, sizeof(newdir));
	return ctl;

}

/*
 *  perform the identity translation (in case we can't reach cs)
 */
static int
identtrans(char *netdir, char *addr, char *naddr, int na, char *file, int nf)
{
	char proto[Maxpath];
	char *p;

	/* parse the protocol */
	strncpy(proto, addr, sizeof(proto));
	proto[sizeof(proto) - 1] = 0;
	p = strchr(proto, '!');
	if (p)
		*p++ = 0;

	snprintf(file, nf, "%s/%s/clone", netdir, proto);
	strncpy(naddr, p, na);
	naddr[na - 1] = 0;

	return 1;
}

/*
 *  call up the connection server and get a translation
 */
static int nettrans(char *addr, char *naddr, int na, char *file, int nf)
{
	int i, fd;
	char buf[Maxpath];
	char netdir[NETPATHLEN];
	char *p, *p2;
	long n;

	/*
	 *  parse, get network directory
	 */
	p = strchr(addr, '!');
	if (p == 0) {
		set_errstr("bad dial string: %s", addr);
		return -1;
	}
	if (*addr != '/') {
		strncpy(netdir, "/net", sizeof(netdir));
	} else {
		for (p2 = p; *p2 != '/'; p2--) ;
		i = p2 - addr;
		if (i == 0 || i >= sizeof(netdir)) {
			set_errstr("bad dial string: %s", addr);
			return -1;
		}
		strncpy(netdir, addr, i);
		netdir[i] = 0;
		addr = p2 + 1;
	}

	/*
	 *  ask the connection server
	 */
	snprintf(buf, sizeof(buf), "%s/cs", netdir);
	fd = sysopen(buf, ORDWR);
	if (fd < 0)
		return identtrans(netdir, addr, naddr, na, file, nf);
	if (syswrite(fd, addr, strlen(addr)) < 0) {
		sysclose(fd);
		return -1;
	}
	sysseek(fd, 0, 0);
	n = sysread(fd, buf, sizeof(buf) - 1);
	sysclose(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;

	/*
	 *  parse the reply
	 */
	p = strchr(buf, ' ');
	if (p == 0)
		return -1;
	*p++ = 0;
	strncpy(naddr, p, na);
	naddr[na - 1] = 0;
	strncpy(file, buf, nf);
	file[nf - 1] = 0;
	return 0;
}

/* 
 * This file is part of the UCB release of Plan 9. It is subject to the license
 * terms in the LICENSE file found in the top-level directory of this
 * distribution and at http://akaros.cs.berkeley.edu/files/Plan9License. No
 * part of the UCB release of Plan 9, including this file, may be copied,
 * modified, propagated, or distributed except according to the terms contained
 * in the LICENSE file.
 */
#include <stdlib.h>

#include <stdio.h>
#include <fcntl.h>
#include <parlib.h>
#include <unistd.h>
#include <signal.h>
#include <iplib.h>

static int	nettrans(char*, char*, int na, char*, int);

enum
{
	Maxpath=	256,
};

/*
 *  announce a network service.
 */
int
announce(char *addr, char *dir)
{
	int ctl, n, m;
	char buf[Maxpath];
	char buf2[Maxpath];
	char netdir[Maxpath];
	char naddr[Maxpath];
	char *cp;

	/*
	 *  translate the address
	 */
	if(nettrans(addr, naddr, sizeof(naddr), netdir, sizeof(netdir)) < 0)
		return -1;

	/*
	 * get a control channel
	 */
	ctl = open(netdir, O_RDWR);
	if(ctl<0){
		fprintf(stderr,"announce opening %s: %r", netdir);
		return -1;
	}
	cp = strrchr(netdir, '/');
	if(cp == NULL){
		fprintf(stderr,"announce arg format %s", netdir);
		close(ctl);
		return -1;
	}
	*cp = 0;

	/*
	 *  find out which line we have
	 */
	n = snprintf(buf, sizeof(buf), "%s/", netdir);
	m = read(ctl, &buf[n], sizeof(buf)-n-1);
	if(m <= 0){
		fprintf(stderr,"announce reading %s: %r", netdir);
		close(ctl);
		return -1;
	}
	buf[n+m] = 0;

	/*
	 *  make the call
	 */
	n = snprintf(buf2, sizeof(buf2), "announce %s", naddr);
	if(write(ctl, buf2, n)!=n){
		fprintf(stderr,"announce writing %s: %r", netdir);
		close(ctl);
		return -1;
	}

	/*
	 *  return directory etc.
	 */
	if(dir){
		strncpy(dir, buf, NETPATHLEN);
		dir[NETPATHLEN-1] = 0;
	}
	return ctl;
}

/*
 *  listen for an incoming call
 */
int
listen(char *dir, char *newdir)
{
	int ctl, n, m;
	char buf[Maxpath];
	char *cp;

	/*
	 *  open listen, wait for a call
	 */
	snprintf(buf, sizeof(buf), "%s/listen", dir);
	ctl = open(buf, O_RDWR);
	if(ctl < 0){
		fprintf(stderr,"listen opening %s: %r", buf);
		return -1;
	}

	/*
	 *  find out which line we have
	 */
	strncpy(buf, dir, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = 0;
	cp = strrchr(buf, '/');
	if(cp == NULL){
		close(ctl);
		fprintf(stderr,"listen arg format %s", dir);
		return -1;
	}
	*++cp = 0;
	n = cp-buf;
	m = read(ctl, cp, sizeof(buf) - n - 1);
	if(m <= 0){
		close(ctl);
		fprintf(stderr,"listen reading %s/listen: %r", dir);
		return -1;
	}
	buf[n+m] = 0;

	/*
	 *  return directory etc.
	 */
	if(newdir){
		strncpy(newdir, buf, NETPATHLEN);
		newdir[NETPATHLEN-1] = 0;
	}
	return ctl;

}

/*
 *  accept a call, return an fd to the open data file
 */
int
accept(int ctl, char *dir)
{
	char buf[Maxpath];
	char *num;
	long n;

	num = strrchr(dir, '/');
	if(num == NULL)
		num = dir;
	else
		num++;

	n = snprintf(buf, sizeof(buf), "accept %s", num);
	write(ctl, buf, n); /* ignore return value, network might not need accepts */

	snprintf(buf, sizeof(buf), "%s/data", dir);
	return open(buf, O_RDWR);
}

/*
 *  reject a call, tell device the reason for the rejection
 */
int
reject(int ctl, char *dir, char *cause)
{
	char buf[Maxpath];
	char *num;
	long n;

	num = strrchr(dir, '/');
	if(num == 0)
		num = dir;
	else
		num++;
	snprintf(buf, sizeof(buf), "reject %s %s", num, cause);
	n = strlen(buf);
	if(write(ctl, buf, n) != n)
		return -1;
	return 0;
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
	proto[sizeof(proto)-1] = 0;
	p = strchr(proto, '!');
	if(p)
		*p++ = 0;

	snprintf(file, nf, "%s/%s/clone", netdir, proto);
	strncpy(naddr, p, na);
	naddr[na-1] = 0;

	return 1;
}

/*
 *  call up the connection server and get a translation
 */
static int
nettrans(char *addr, char *naddr, int na, char *file, int nf)
{
	int i, fd;
	char buf[Maxpath];
	char netdir[Maxpath];
	char *p, *p2;
	long n;

	/*
	 *  parse, get network directory
	 */
	p = strchr(addr, '!');
	if(p == 0){
		fprintf(stderr,"bad dial string: %s", addr);
		return -1;
	}
	if(*addr != '/'){
		strncpy(netdir, "/net", sizeof(netdir));
		netdir[sizeof(netdir) - 1] = 0;
	} else {
		for(p2 = p; *p2 != '/'; p2--)
			;
		i = p2 - addr;
		if(i == 0 || i >= sizeof(netdir)){
			fprintf(stderr,"bad dial string: %s", addr);
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
	fd = open(buf, O_RDWR);
	if(fd < 0)
		return identtrans(netdir, addr, naddr, na, file, nf);
	if(write(fd, addr, strlen(addr)) < 0){
		close(fd);
		return -1;
	}
	lseek(fd, 0, 0);
	n = read(fd, buf, sizeof(buf)-1);
	close(fd);
	if(n <= 0)
		return -1;
	buf[n] = 0;

	/*
	 *  parse the reply
	 */
	p = strchr(buf, ' ');
	if(p == 0)
		return -1;
	*p++ = 0;
	strncpy(naddr, p, na);
	naddr[na-1] = 0;

	if(buf[0] == '/'){
		p = strchr(buf+1, '/');
		if(p == NULL)
			p = buf;
		else 
			p++;
	}
	snprintf(file, nf, "%s/%s", netdir, p);
	return 0;
}

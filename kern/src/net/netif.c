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

static int netown(struct netfile *, char *unused_char_p_t, int);
static int openfile(struct netif *, int);
static char *matchtoken(char *unused_char_p_t, char *);
static char *netmulti(struct netif *, struct netfile *,
					  uint8_t * unused_uint8_p_t, int);
static int parseaddr(uint8_t * unused_uint8_p_t, char *unused_char_p_t, int);

/*
 *  set up a new network interface
 */
void netifinit(struct netif *nif, char *name, int nfile, uint32_t limit)
{
	qlock_init(&nif->qlock);
	strncpy(nif->name, name, KNAMELEN - 1);
	nif->name[KNAMELEN - 1] = 0;
	nif->nfile = nfile;
	nif->f = kzmalloc(nfile * sizeof(struct netfile *), 0);
	if (nif->f)
		memset(nif->f, 0, nfile * sizeof(struct netfile *));
	else
		nif->nfile = 0;
	nif->limit = limit;
}

/*
 *  generate a 3 level directory
 */
static int
netifgen(struct chan *c, char *unused_char_p_t, struct dirtab *vp,
		 int unused_int, int i, struct dir *dp)
{
	struct qid q;
	struct netif *nif = (struct netif *)vp;
	struct netfile *f;
	int perm;
	char *o;

	q.type = QTFILE;
	q.vers = 0;

	/* top level directory contains the name of the network */
	if (c->qid.path == 0) {
		switch (i) {
			case DEVDOTDOT:
				q.path = 0;
				q.type = QTDIR;
				devdir(c, q, ".", 0, eve, 0555, dp);
				break;
			case 0:
				q.path = N2ndqid;
				q.type = QTDIR;
				strncpy(get_cur_genbuf(), nif->name, GENBUF_SZ);
				devdir(c, q, get_cur_genbuf(), 0, eve, 0555, dp);
				break;
			default:
				return -1;
		}
		return 1;
	}

	/* second level contains clone plus all the conversations.
	 *
	 * This ancient comment is from plan9.  Inferno and nxm both had issues
	 * here.  You couldn't ls /net/ether0/ when it didn't have any convs.  There
	 * were also issues with nxm where you couldn't stat ether0/x/stats
	 * properly.
	 *
	 * The issue is that if we handle things like Nstatqid, then we will never
	 * pass it down to the third level. And since we just set the path ==
	 * Nstatqid, we won't have the NETID muxed in. If someone isn't trying to
	 * generate a chan, but instead is looking it up (devwalk generates, devstat
	 * already has the chan), then they are also looking for a devdir with path
	 * containing ID << 5. So if you stat ether0/1/ifstats, devstat is looking
	 * for path 41, but we return path 9 (41 = 32 + 9). (these numbers are
	 * before we tracked NETID + 1).
	 *
	 * We (akaros and plan9) had a big if here, that would catch things that do
	 * not exist in the subdirs of a netif. Things like clone make sense here.
	 * I guess addr too, though that seems to be added since the original
	 * comment. You can see what the 3rd level was expecting to parse by looking
	 * farther down in the code.
	 *
	 * The root of the problem was that the old code couldn't tell the
	 * difference between no netid and netid 0. Now, we determine if we're at
	 * the second level by the lack of a netid, instead of trying to enumerate
	 * the qid types that the second level could have. The latter approach
	 * allowed for something like ether0/1/stats, but we couldn't actually
	 * devstat ether0/stats directly. It's worth noting that there is no
	 * difference to the content of ether0/stats and ether0/x/stats (when you
	 * read), but they have different chan qids.
	 *
	 * Here's the old if block:
	 t = NETTYPE(c->qid.path);
	 if (t == N2ndqid || t == Ncloneqid || t == Naddrqid) {
	 */
	if (NETID(c->qid.path) == -1) {
		switch (i) {
			case DEVDOTDOT:
				q.type = QTDIR;
				q.path = 0;
				devdir(c, q, ".", 0, eve, DMDIR | 0555, dp);
				break;
			case 0:
				q.path = Ncloneqid;
				devdir(c, q, "clone", 0, eve, 0666, dp);
				break;
			case 1:
				q.path = Naddrqid;
				devdir(c, q, "addr", 0, eve, 0666, dp);
				break;
			case 2:
				q.path = Nstatqid;
				devdir(c, q, "stats", 0, eve, 0444, dp);
				break;
			case 3:
				q.path = Nifstatqid;
				devdir(c, q, "ifstats", 0, eve, 0444, dp);
				break;
			default:
				i -= 4;
				if (i >= nif->nfile)
					return -1;
				if (nif->f[i] == 0)
					return 0;
				q.type = QTDIR;
				q.path = NETQID(i, N3rdqid);
				snprintf(get_cur_genbuf(), GENBUF_SZ, "%d", i);
				devdir(c, q, get_cur_genbuf(), 0, eve, DMDIR | 0555, dp);
				break;
		}
		return 1;
	}

	/* third level */
	f = nif->f[NETID(c->qid.path)];
	if (f == 0)
		return 0;
	if (*f->owner) {
		o = f->owner;
		perm = f->mode;
	} else {
		o = eve;
		perm = 0666;
	}
	switch (i) {
		case DEVDOTDOT:
			q.type = QTDIR;
			q.path = N2ndqid;
			strncpy(get_cur_genbuf(), nif->name, GENBUF_SZ);
			devdir(c, q, get_cur_genbuf(), 0, eve, DMDIR | 0555, dp);
			break;
		case 0:
			q.path = NETQID(NETID(c->qid.path), Ndataqid);
			devdir(c, q, "data", 0, o, perm, dp);
			break;
		case 1:
			q.path = NETQID(NETID(c->qid.path), Nctlqid);
			devdir(c, q, "ctl", 0, o, perm, dp);
			break;
		case 2:
			q.path = NETQID(NETID(c->qid.path), Nstatqid);
			devdir(c, q, "stats", 0, eve, 0444, dp);
			break;
		case 3:
			q.path = NETQID(NETID(c->qid.path), Ntypeqid);
			devdir(c, q, "type", 0, eve, 0444, dp);
			break;
		case 4:
			q.path = NETQID(NETID(c->qid.path), Nifstatqid);
			devdir(c, q, "ifstats", 0, eve, 0444, dp);
			break;
		default:
			return -1;
	}
	return 1;
}

struct walkqid *netifwalk(struct netif *nif, struct chan *c, struct chan *nc,
						  char **name, int nname)
{
	return devwalk(c, nc, name, nname, (struct dirtab *)nif, 0, netifgen);
}

struct chan *netifopen(struct netif *nif, struct chan *c, int omode)
{
	int id;
	struct netfile *f;

	id = 0;
	if (c->qid.type & QTDIR) {
		if (!IS_RDONLY(omode))
			error(Eperm);
	} else {
		switch (NETTYPE(c->qid.path)) {
			case Ndataqid:
			case Nctlqid:
				id = NETID(c->qid.path);
				openfile(nif, id);
				break;
			case Ncloneqid:
				id = openfile(nif, -1);
				c->qid.path = NETQID(id, Nctlqid);
				break;
			default:
				if (!IS_RDONLY(omode))
					error(Ebadarg);
		}
		switch (NETTYPE(c->qid.path)) {
			case Ndataqid:
			case Nctlqid:
				f = nif->f[id];
				if (netown(f, current->user, omode & 7) < 0)
					error(Eperm);
				break;
		}
	}
	c->mode = openmode(omode);
	c->flag |= COPEN;
	c->offset = 0;
	c->iounit = qiomaxatomic;
	return c;
}

long
netifread(struct netif *nif, struct chan *c, void *a, long n, uint32_t offset)
{
	int i, j;
	struct netfile *f;
	char *p;

	if (c->qid.type & QTDIR)
		return devdirread(c, a, n, (struct dirtab *)nif, 0, netifgen);

	switch (NETTYPE(c->qid.path)) {
		case Ndataqid:
			f = nif->f[NETID(c->qid.path)];
			return qread(f->in, a, n);
		case Nctlqid:
			return readnum(offset, a, n, NETID(c->qid.path), NUMSIZE);
		case Nstatqid:
			p = kzmalloc(READSTR, 0);
			if (p == NULL)
				return 0;
			j = snprintf(p, READSTR, "in: %d\n", nif->inpackets);
			j += snprintf(p + j, READSTR - j, "link: %d\n", nif->link);
			j += snprintf(p + j, READSTR - j, "out: %d\n", nif->outpackets);
			j += snprintf(p + j, READSTR - j, "crc errs: %d\n", nif->crcs);
			j += snprintf(p + j, READSTR - j, "overflows: %d\n",
						  nif->overflows);
			j += snprintf(p + j, READSTR - j, "soft overflows: %d\n",
						  nif->soverflows);
			j += snprintf(p + j, READSTR - j, "framing errs: %d\n",
						  nif->frames);
			j += snprintf(p + j, READSTR - j, "buffer errs: %d\n", nif->buffs);
			j += snprintf(p + j, READSTR - j, "output errs: %d\n", nif->oerrs);
			j += snprintf(p + j, READSTR - j, "prom: %d\n", nif->prom);
			j += snprintf(p + j, READSTR - j, "mbps: %d\n", nif->mbps);
			j += snprintf(p + j, READSTR - j, "addr: ");
			for (i = 0; i < nif->alen; i++)
				j += snprintf(p + j, READSTR - j, "%02.2x", nif->addr[i]);
			snprintf(p + j, READSTR - j, "\n");
			n = readstr(offset, a, n, p);
			kfree(p);
			return n;
		case Naddrqid:
			p = kzmalloc(READSTR, 0);
			if (p == NULL)
				return 0;
			j = 0;
			for (i = 0; i < nif->alen; i++)
				j += snprintf(p + j, READSTR - j, "%02.2x", nif->addr[i]);
			n = readstr(offset, a, n, p);
			kfree(p);
			return n;
		case Ntypeqid:
			f = nif->f[NETID(c->qid.path)];
			return readnum(offset, a, n, f->type, NUMSIZE);
		case Nifstatqid:
			return 0;
	}
	error(Ebadarg);
	return -1;	/* not reached */
}

struct block *netifbread(struct netif *nif, struct chan *c, long n,
						 uint32_t offset)
{
	if ((c->qid.type & QTDIR) || NETTYPE(c->qid.path) != Ndataqid)
		return devbread(c, n, offset);

	return qbread(nif->f[NETID(c->qid.path)]->in, n);
}

/*
 *  make sure this type isn't already in use on this device
 */
static int typeinuse(struct netif *nif, int type)
{
	struct netfile *f, **fp, **efp;

	if (type <= 0)
		return 0;

	efp = &nif->f[nif->nfile];
	for (fp = nif->f; fp < efp; fp++) {
		f = *fp;
		if (f == 0)
			continue;
		if (f->type == type)
			return 1;
	}
	return 0;
}

/*
 *  the devxxx.c that calls us handles writing data, it knows best
 */
long netifwrite(struct netif *nif, struct chan *c, void *a, long n)
{
	ERRSTACK(1);
	struct netfile *f;
	int type;
	char *p, buf[64];
	uint8_t binaddr[Nmaxaddr];

	if (NETTYPE(c->qid.path) != Nctlqid)
		error(Eperm);

	if (n >= sizeof(buf))
		n = sizeof(buf) - 1;
	memmove(buf, a, n);
	buf[n] = 0;

	if (waserror()) {
		qunlock(&nif->qlock);
		nexterror();
	}

	qlock(&nif->qlock);
	f = nif->f[NETID(c->qid.path)];
	if ((p = matchtoken(buf, "connect")) != 0) {
		type = strtol(p, 0, 0);	/* allows any base, though usually hex */
		if (typeinuse(nif, type))
			error(Einuse);
		f->type = type;
		if (f->type < 0)
			nif->all++;
	} else if (matchtoken(buf, "promiscuous")) {
		if (f->prom == 0) {
			if (nif->prom == 0 && nif->promiscuous != NULL)
				nif->promiscuous(nif->arg, 1);
			f->prom = 1;
			nif->prom++;
		}
	} else if ((p = matchtoken(buf, "scanbs")) != 0) {
		/* scan for base stations */
		if (f->scan == 0) {
			type = strtol(p, 0, 0);	/* allows any base, though usually hex */
			if (type < 5)
				type = 5;
			if (nif->scanbs != NULL)
				nif->scanbs(nif->arg, type);
			f->scan = type;
			nif->scan++;
		}
	} else if (matchtoken(buf, "bridge")) {
		f->bridge = 1;
	} else if (matchtoken(buf, "headersonly")) {
		f->headersonly = 1;
	} else if ((p = matchtoken(buf, "addmulti")) != 0) {
		if (parseaddr(binaddr, p, nif->alen) < 0)
			error("bad address");
		p = netmulti(nif, f, binaddr, 1);
		if (p)
			error(p);
	} else if ((p = matchtoken(buf, "remmulti")) != 0) {
		if (parseaddr(binaddr, p, nif->alen) < 0)
			error("bad address");
		p = netmulti(nif, f, binaddr, 0);
		if (p)
			error(p);
	} else
		n = -1;
	qunlock(&nif->qlock);
	poperror();
	return n;
}

int netifwstat(struct netif *nif, struct chan *c, uint8_t * db, int n)
{
	struct dir *dir;
	struct netfile *f;
	int m;

	f = nif->f[NETID(c->qid.path)];
	if (f == 0) {
		set_errno(ENOENT);
		error(Enonexist);
	}

	if (netown(f, current->user, OWRITE) < 0)
		error(Eperm);

	dir = kzmalloc(sizeof(struct dir) + n, 0);
	m = convM2D(db, n, &dir[0], (char *)&dir[1]);
	if (m == 0) {
		kfree(dir);
		error(Eshortstat);
	}
	if (!emptystr(dir[0].uid))
		strncpy(f->owner, dir[0].uid, KNAMELEN);
	if (dir[0].mode != ~0UL)
		f->mode = dir[0].mode;
	kfree(dir);
	return m;
}

int netifstat(struct netif *nif, struct chan *c, uint8_t * db, int n)
{
	return devstat(c, db, n, (struct dirtab *)nif, 0, netifgen);
}

void netifclose(struct netif *nif, struct chan *c)
{
	struct netfile *f;
	int t;
	struct netaddr *ap;

	if ((c->flag & COPEN) == 0)
		return;

	t = NETTYPE(c->qid.path);
	if (t != Ndataqid && t != Nctlqid)
		return;

	f = nif->f[NETID(c->qid.path)];
	qlock(&f->qlock);
	if (--(f->inuse) == 0) {
		if (f->prom) {
			qlock(&nif->qlock);
			if (--(nif->prom) == 0 && nif->promiscuous != NULL)
				nif->promiscuous(nif->arg, 0);
			qunlock(&nif->qlock);
			f->prom = 0;
		}
		if (f->scan) {
			qlock(&nif->qlock);
			if (--(nif->scan) == 0 && nif->scanbs != NULL)
				nif->scanbs(nif->arg, 0);
			qunlock(&nif->qlock);
			f->prom = 0;
			f->scan = 0;
		}
		if (f->nmaddr) {
			qlock(&nif->qlock);
			t = 0;
			for (ap = nif->maddr; ap; ap = ap->next) {
				if (f->maddr[t / 8] & (1 << (t % 8)))
					netmulti(nif, f, ap->addr, 0);
			}
			qunlock(&nif->qlock);
			f->nmaddr = 0;
		}
		if (f->type < 0) {
			qlock(&nif->qlock);
			--(nif->all);
			qunlock(&nif->qlock);
		}
		f->owner[0] = 0;
		f->type = 0;
		f->bridge = 0;
		f->headersonly = 0;
		qclose(f->in);
	}
	qunlock(&f->qlock);
}

spinlock_t netlock = SPINLOCK_INITIALIZER;

static int netown(struct netfile *p, char *o, int omode)
{
	static int access[] = { 0400, 0200, 0600, 0100 };
	int mode;
	int t;

	spin_lock(&netlock);
	if (*p->owner) {
		if (strncmp(o, p->owner, KNAMELEN) == 0)	/* User */
			mode = p->mode;
		else if (strncmp(o, eve, KNAMELEN) == 0)	/* Bootes is group */
			mode = p->mode << 3;
		else
			mode = p->mode << 6;	/* Other */

		t = access[omode & 3];
		if ((t & mode) == t) {
			spin_unlock(&netlock);
			return 0;
		} else {
			spin_unlock(&netlock);
			return -1;
		}
	}
	strncpy(p->owner, o, KNAMELEN);
	p->mode = 0660;
	spin_unlock(&netlock);
	return 0;
}

/*
 *  Increment the reference count of a network device.
 *  If id < 0, return an unused ether device.
 */
static int openfile(struct netif *nif, int id)
{
	ERRSTACK(1);
	struct netfile *f, **fp, **efp;

	if (id >= 0) {
		f = nif->f[id];
		if (f == 0)
			error(Enodev);
		qlock(&f->qlock);
		qreopen(f->in);
		f->inuse++;
		qunlock(&f->qlock);
		return id;
	}

	qlock(&nif->qlock);
	if (waserror()) {
		qunlock(&nif->qlock);
		nexterror();
	}
	efp = &nif->f[nif->nfile];
	for (fp = nif->f; fp < efp; fp++) {
		f = *fp;
		if (f == 0) {
			f = kzmalloc(sizeof(struct netfile), 0);
			if (f == 0)
				exhausted("memory");
			/* since we lock before netifinit (if we ever call that...) */
			qlock_init(&f->qlock);
			f->in = qopen(nif->limit, Qmsg, 0, 0);
			if (f->in == NULL) {
				kfree(f);
				exhausted("memory");
			}
			*fp = f;
			qlock(&f->qlock);
		} else {
			qlock(&f->qlock);
			if (f->inuse) {
				qunlock(&f->qlock);
				continue;
			}
		}
		f->inuse = 1;
		qreopen(f->in);
		netown(f, current->user, 0);
		qunlock(&f->qlock);
		qunlock(&nif->qlock);
		poperror();
		return fp - nif->f;
	}
	error(Enodev);
	return -1;	/* not reached */
}

/*
 *  look for a token starting a string,
 *  return a pointer to first non-space char after it
 */
static char *matchtoken(char *p, char *token)
{
	int n;

	n = strlen(token);
	if (strncmp(p, token, n))
		return 0;
	p += n;
	if (*p == 0)
		return p;
	if (*p != ' ' && *p != '\t' && *p != '\n')
		return 0;
	while (*p == ' ' || *p == '\t' || *p == '\n')
		p++;
	return p;
}

static uint32_t hash(uint8_t * a, int len)
{
	uint32_t sum = 0;

	while (len-- > 0)
		sum = (sum << 1) + *a++;
	return sum % Nmhash;
}

int activemulti(struct netif *nif, uint8_t * addr, int alen)
{
	struct netaddr *hp;

	for (hp = nif->mhash[hash(addr, alen)]; hp; hp = hp->hnext)
		if (memcmp(addr, hp->addr, alen) == 0) {
			if (hp->ref)
				return 1;
			else
				break;
		}
	return 0;
}

static int parseaddr(uint8_t * to, char *from, int alen)
{
	char nip[4];
	char *p;
	int i;

	p = from;
	for (i = 0; i < alen; i++) {
		if (*p == 0)
			return -1;
		nip[0] = *p++;
		if (*p == 0)
			return -1;
		nip[1] = *p++;
		nip[2] = 0;
		to[i] = strtoul(nip, 0, 16);
		if (*p == ':')
			p++;
	}
	return 0;
}

/*
 *  keep track of multicast addresses
 */
static char *netmulti(struct netif *nif, struct netfile *f, uint8_t * addr,
					  int add)
{
	struct netaddr **l, *ap;
	int i;
	uint32_t h;

	if (nif->multicast == NULL)
		return "interface does not support multicast";

	l = &nif->maddr;
	i = 0;
	for (ap = *l; ap; ap = *l) {
		if (memcmp(addr, ap->addr, nif->alen) == 0)
			break;
		i++;
		l = &ap->next;
	}

	if (add) {
		if (ap == 0) {
			/* TODO: AFAIK, this never gets freed.  if we fix that, we can use a
			 * kref too (instead of int ap->ref). */
			*l = ap = kzmalloc(sizeof(*ap), 0);
			memmove(ap->addr, addr, nif->alen);
			ap->next = 0;
			ap->ref = 1;
			h = hash(addr, nif->alen);
			ap->hnext = nif->mhash[h];
			nif->mhash[h] = ap;
		} else {
			ap->ref++;
		}
		if (ap->ref == 1) {
			nif->nmaddr++;
			nif->multicast(nif->arg, addr, 1);
		}
		if (i < 8 * sizeof(f->maddr)) {
			if ((f->maddr[i / 8] & (1 << (i % 8))) == 0)
				f->nmaddr++;
			f->maddr[i / 8] |= 1 << (i % 8);
		}
	} else {
		if (ap == 0 || ap->ref == 0)
			return 0;
		ap->ref--;
		if (ap->ref == 0) {
			nif->nmaddr--;
			nif->multicast(nif->arg, addr, 0);
		}
		if (i < 8 * sizeof(f->maddr)) {
			if ((f->maddr[i / 8] & (1 << (i % 8))) != 0)
				f->nmaddr--;
			f->maddr[i / 8] &= ~(1 << (i % 8));
		}
	}
	return 0;
}

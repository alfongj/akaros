// INFERNO
#define DEBUG
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

#define DPRINT if(0)print

enum {
	UDP_UDPHDR_SZ = 8,

	UDP4_PHDR_OFF = 8,
	UDP4_PHDR_SZ = 12,
	UDP4_IPHDR_SZ = 20,
	UDP6_IPHDR_SZ = 40,
	UDP6_PHDR_SZ = 40,
	UDP6_PHDR_OFF = 0,

	IP_UDPPROTO = 17,
	UDP_USEAD7 = 52,
	UDP_USEAD6 = 36,

	Udprxms = 200,
	Udptickms = 100,
	Udpmaxxmit = 10,
};

typedef struct Udp4hdr Udp4hdr;
struct Udp4hdr {
	/* ip header */
	uint8_t vihl;				/* Version and header length */
	uint8_t tos;				/* Type of service */
	uint8_t length[2];			/* packet length */
	uint8_t id[2];				/* Identification */
	uint8_t frag[2];			/* Fragment information */
	uint8_t Unused;
	uint8_t udpproto;			/* Protocol */
	uint8_t udpplen[2];			/* Header plus data length */
	uint8_t udpsrc[IPv4addrlen];	/* Ip source */
	uint8_t udpdst[IPv4addrlen];	/* Ip destination */

	/* udp header */
	uint8_t udpsport[2];		/* Source port */
	uint8_t udpdport[2];		/* Destination port */
	uint8_t udplen[2];			/* data length */
	uint8_t udpcksum[2];		/* Checksum */
};

typedef struct Udp6hdr Udp6hdr;
struct Udp6hdr {
	uint8_t viclfl[4];
	uint8_t len[2];
	uint8_t nextheader;
	uint8_t hoplimit;
	uint8_t udpsrc[IPaddrlen];
	uint8_t udpdst[IPaddrlen];

	/* udp header */
	uint8_t udpsport[2];		/* Source port */
	uint8_t udpdport[2];		/* Destination port */
	uint8_t udplen[2];			/* data length */
	uint8_t udpcksum[2];		/* Checksum */
};

/* MIB II counters */
typedef struct Udpstats Udpstats;
struct Udpstats {
	uint32_t udpInDatagrams;
	uint32_t udpNoPorts;
	uint32_t udpInErrors;
	uint32_t udpOutDatagrams;
};

typedef struct Udppriv Udppriv;
struct Udppriv {
	struct Ipht ht;

	/* MIB counters */
	Udpstats ustats;

	/* non-MIB stats */
	uint32_t csumerr;			/* checksum errors */
	uint32_t lenerr;			/* short packet */
};

void (*etherprofiler) (char *name, int qlen);
void udpkick(void *x, struct block *bp);

/*
 *  protocol specific part of Conv
 */
typedef struct Udpcb Udpcb;
struct Udpcb {
	qlock_t qlock;
	uint8_t headers;
};

static char *udpconnect(struct conv *c, char **argv, int argc)
{
	char *e;
	Udppriv *upriv;

	upriv = c->p->priv;
	e = Fsstdconnect(c, argv, argc);
	Fsconnected(c, e);
	if (e != NULL)
		return e;

	iphtadd(&upriv->ht, c);
	return NULL;
}

static int udpstate(struct conv *c, char *state, int n)
{
	return snprintf(state, n, "%s qin %d qout %d",
					c->inuse ? "Open" : "Closed",
					c->rq ? qlen(c->rq) : 0, c->wq ? qlen(c->wq) : 0);
}

static char *udpannounce(struct conv *c, char **argv, int argc)
{
	char *e;
	Udppriv *upriv;

	upriv = c->p->priv;
	e = Fsstdannounce(c, argv, argc);
	if (e != NULL)
		return e;
	Fsconnected(c, NULL);
	iphtadd(&upriv->ht, c);

	return NULL;
}

static void udpcreate(struct conv *c)
{
	c->rq = qopen(64 * 1024, Qmsg, 0, 0);
	c->wq = qbypass(udpkick, c);
}

static void udpclose(struct conv *c)
{
	Udpcb *ucb;
	Udppriv *upriv;

	upriv = c->p->priv;
	iphtrem(&upriv->ht, c);

	c->state = 0;
	qclose(c->rq);
	qclose(c->wq);
	qclose(c->eq);
	ipmove(c->laddr, IPnoaddr);
	ipmove(c->raddr, IPnoaddr);
	c->lport = 0;
	c->rport = 0;

	ucb = (Udpcb *) c->ptcl;
	ucb->headers = 0;

	qunlock(&c->qlock);
}

void udpkick(void *x, struct block *bp)
{
	struct conv *c = x;
	Udp4hdr *uh4;
	Udp6hdr *uh6;
	uint16_t rport;
	uint8_t laddr[IPaddrlen], raddr[IPaddrlen];
	Udpcb *ucb;
	int dlen, ptcllen;
	Udppriv *upriv;
	struct Fs *f;
	int version;
	struct conv *rc;

	upriv = c->p->priv;
	f = c->p->f;

	netlog(c->p->f, Logudp, "udp: kick\n");
	if (bp == NULL)
		return;

	ucb = (Udpcb *) c->ptcl;
	switch (ucb->headers) {
		case 7:
			/* get user specified addresses */
			bp = pullupblock(bp, UDP_USEAD7);
			if (bp == NULL)
				return;
			ipmove(raddr, bp->rp);
			bp->rp += IPaddrlen;
			ipmove(laddr, bp->rp);
			bp->rp += IPaddrlen;
			/* pick interface closest to dest */
			if (ipforme(f, laddr) != Runi)
				findlocalip(f, laddr, raddr);
			bp->rp += IPaddrlen;	/* Ignore ifc address */
			rport = nhgets(bp->rp);
			bp->rp += 2 + 2;	/* Ignore local port */
			break;
		case 6:
			/* get user specified addresses */
			bp = pullupblock(bp, UDP_USEAD6);
			if (bp == NULL)
				return;
			ipmove(raddr, bp->rp);
			bp->rp += IPaddrlen;
			ipmove(laddr, bp->rp);
			bp->rp += IPaddrlen;
			/* pick interface closest to dest */
			if (ipforme(f, laddr) != Runi)
				findlocalip(f, laddr, raddr);
			rport = nhgets(bp->rp);
			bp->rp += 2 + 2;	/* Ignore local port */
			break;
		default:
			rport = 0;
			break;
	}

	if (ucb->headers) {
		if (memcmp(laddr, v4prefix, IPv4off) == 0 ||
			ipcmp(laddr, IPnoaddr) == 0)
			version = V4;
		else
			version = V6;
	} else {
		if ((memcmp(c->raddr, v4prefix, IPv4off) == 0 &&
			 memcmp(c->laddr, v4prefix, IPv4off) == 0)
			|| ipcmp(c->raddr, IPnoaddr) == 0)
			version = V4;
		else
			version = V6;
	}

	dlen = blocklen(bp);

	/* fill in pseudo header and compute checksum */
	switch (version) {
		case V4:
			bp = padblock(bp, UDP4_IPHDR_SZ + UDP_UDPHDR_SZ);
			if (bp == NULL)
				return;

			uh4 = (Udp4hdr *) (bp->rp);
			ptcllen = dlen + UDP_UDPHDR_SZ;
			uh4->Unused = 0;
			uh4->udpproto = IP_UDPPROTO;
			uh4->frag[0] = 0;
			uh4->frag[1] = 0;
			hnputs(uh4->udpplen, ptcllen);
			if (ucb->headers) {
				v6tov4(uh4->udpdst, raddr);
				hnputs(uh4->udpdport, rport);
				v6tov4(uh4->udpsrc, laddr);
				rc = NULL;
			} else {
				v6tov4(uh4->udpdst, c->raddr);
				hnputs(uh4->udpdport, c->rport);
				if (ipcmp(c->laddr, IPnoaddr) == 0)
					findlocalip(f, c->laddr, c->raddr);
				v6tov4(uh4->udpsrc, c->laddr);
				rc = c;
			}
			hnputs(uh4->udpsport, c->lport);
			hnputs(uh4->udplen, ptcllen);
			uh4->udpcksum[0] = 0;
			uh4->udpcksum[1] = 0;
			hnputs(uh4->udpcksum,
				   ptclcsum(bp, UDP4_PHDR_OFF,
							dlen + UDP_UDPHDR_SZ + UDP4_PHDR_SZ));
			uh4->vihl = IP_VER4;
			ipoput4(f, bp, 0, c->ttl, c->tos, rc);
			break;

		case V6:
			bp = padblock(bp, UDP6_IPHDR_SZ + UDP_UDPHDR_SZ);
			if (bp == NULL)
				return;

			// using the v6 ip header to create pseudo header 
			// first then reset it to the normal ip header
			uh6 = (Udp6hdr *) (bp->rp);
			memset(uh6, 0, 8);
			ptcllen = dlen + UDP_UDPHDR_SZ;
			hnputl(uh6->viclfl, ptcllen);
			uh6->hoplimit = IP_UDPPROTO;
			if (ucb->headers) {
				ipmove(uh6->udpdst, raddr);
				hnputs(uh6->udpdport, rport);
				ipmove(uh6->udpsrc, laddr);
				rc = NULL;
			} else {
				ipmove(uh6->udpdst, c->raddr);
				hnputs(uh6->udpdport, c->rport);
				if (ipcmp(c->laddr, IPnoaddr) == 0)
					findlocalip(f, c->laddr, c->raddr);
				ipmove(uh6->udpsrc, c->laddr);
				rc = c;
			}
			hnputs(uh6->udpsport, c->lport);
			hnputs(uh6->udplen, ptcllen);
			uh6->udpcksum[0] = 0;
			uh6->udpcksum[1] = 0;
			hnputs(uh6->udpcksum,
				   ptclcsum(bp, UDP6_PHDR_OFF,
							dlen + UDP_UDPHDR_SZ + UDP6_PHDR_SZ));
			memset(uh6, 0, 8);
			uh6->viclfl[0] = IP_VER6;
			hnputs(uh6->len, ptcllen);
			uh6->nextheader = IP_UDPPROTO;
			ipoput6(f, bp, 0, c->ttl, c->tos, rc);
			break;

		default:
			panic("udpkick: version %d", version);
	}
	upriv->ustats.udpOutDatagrams++;
}

void udpiput(struct Proto *udp, struct Ipifc *ifc, struct block *bp)
{
	int len;
	Udp4hdr *uh4;
	Udp6hdr *uh6;
	struct conv *c;
	Udpcb *ucb;
	uint8_t raddr[IPaddrlen], laddr[IPaddrlen];
	uint16_t rport, lport;
	Udppriv *upriv;
	struct Fs *f;
	int version;
	int ottl, oviclfl, olen;
	uint8_t *p;

	upriv = udp->priv;
	f = udp->f;
	upriv->ustats.udpInDatagrams++;

	uh4 = (Udp4hdr *) (bp->rp);
	version = ((uh4->vihl & 0xF0) == IP_VER6) ? V6 : V4;

	/*
	 * Put back pseudo header for checksum 
	 * (remember old values for icmpnoconv())
	 */
	switch (version) {
		case V4:
			ottl = uh4->Unused;
			uh4->Unused = 0;
			len = nhgets(uh4->udplen);
			olen = nhgets(uh4->udpplen);
			hnputs(uh4->udpplen, len);

			v4tov6(raddr, uh4->udpsrc);
			v4tov6(laddr, uh4->udpdst);
			lport = nhgets(uh4->udpdport);
			rport = nhgets(uh4->udpsport);

			if (nhgets(uh4->udpcksum)) {
				if (ptclcsum(bp, UDP4_PHDR_OFF, len + UDP4_PHDR_SZ)) {
					upriv->ustats.udpInErrors++;
					netlog(f, Logudp, "udp: checksum error %I\n", raddr);
					printd("udp: checksum error %I\n", raddr);
					freeblist(bp);
					return;
				}
			}
			uh4->Unused = ottl;
			hnputs(uh4->udpplen, olen);
			break;
		case V6:
			uh6 = (Udp6hdr *) (bp->rp);
			len = nhgets(uh6->udplen);
			oviclfl = nhgetl(uh6->viclfl);
			olen = nhgets(uh6->len);
			ottl = uh6->hoplimit;
			ipmove(raddr, uh6->udpsrc);
			ipmove(laddr, uh6->udpdst);
			lport = nhgets(uh6->udpdport);
			rport = nhgets(uh6->udpsport);
			memset(uh6, 0, 8);
			hnputl(uh6->viclfl, len);
			uh6->hoplimit = IP_UDPPROTO;
			if (ptclcsum(bp, UDP6_PHDR_OFF, len + UDP6_PHDR_SZ)) {
				upriv->ustats.udpInErrors++;
				netlog(f, Logudp, "udp: checksum error %I\n", raddr);
				printd("udp: checksum error %I\n", raddr);
				freeblist(bp);
				return;
			}
			hnputl(uh6->viclfl, oviclfl);
			hnputs(uh6->len, olen);
			uh6->nextheader = IP_UDPPROTO;
			uh6->hoplimit = ottl;
			break;
		default:
			panic("udpiput: version %d", version);
			return;	/* to avoid a warning */
	}

	qlock(&udp->qlock);

	c = iphtlook(&upriv->ht, raddr, rport, laddr, lport);
	if (c == NULL) {
		/* no converstation found */
		upriv->ustats.udpNoPorts++;
		qunlock(&udp->qlock);
		netlog(f, Logudp, "udp: no conv %I!%d -> %I!%d\n", raddr, rport,
			   laddr, lport);

		switch (version) {
			case V4:
				icmpnoconv(f, bp);
				break;
			case V6:
				icmphostunr(f, ifc, bp, icmp6_port_unreach, 0);
				break;
			default:
				panic("udpiput2: version %d", version);
		}

		freeblist(bp);
		return;
	}
	ucb = (Udpcb *) c->ptcl;

	if (c->state == Announced) {
		if (ucb->headers == 0) {
			/* create a new conversation */
			if (ipforme(f, laddr) != Runi) {
				switch (version) {
					case V4:
						v4tov6(laddr, ifc->lifc->local);
						break;
					case V6:
						ipmove(laddr, ifc->lifc->local);
						break;
					default:
						panic("udpiput3: version %d", version);
				}
			}
			c = Fsnewcall(c, raddr, rport, laddr, lport, version);
			if (c == NULL) {
				qunlock(&udp->qlock);
				freeblist(bp);
				return;
			}
			iphtadd(&upriv->ht, c);
			ucb = (Udpcb *) c->ptcl;
		}
	}

	qlock(&c->qlock);
	qunlock(&udp->qlock);

	/*
	 * Trim the packet down to data size
	 */
	len -= UDP_UDPHDR_SZ;
	switch (version) {
		case V4:
			bp = trimblock(bp, UDP4_IPHDR_SZ + UDP_UDPHDR_SZ, len);
			break;
		case V6:
			bp = trimblock(bp, UDP6_IPHDR_SZ + UDP_UDPHDR_SZ, len);
			break;
		default:
			bp = NULL;
			panic("udpiput4: version %d", version);
	}
	if (bp == NULL) {
		qunlock(&c->qlock);
		netlog(f, Logudp, "udp: len err %I.%d -> %I.%d\n", raddr, rport,
			   laddr, lport);
		upriv->lenerr++;
		return;
	}

	netlog(f, Logudpmsg, "udp: %I.%d -> %I.%d l %d\n", raddr, rport,
		   laddr, lport, len);

	switch (ucb->headers) {
		case 7:
			/* pass the src address */
			bp = padblock(bp, UDP_USEAD7);
			p = bp->rp;
			ipmove(p, raddr);
			p += IPaddrlen;
			ipmove(p, laddr);
			p += IPaddrlen;
			ipmove(p, ifc->lifc->local);
			p += IPaddrlen;
			hnputs(p, rport);
			p += 2;
			hnputs(p, lport);
			break;
		case 6:
			/* pass the src address */
			bp = padblock(bp, UDP_USEAD6);
			p = bp->rp;
			ipmove(p, raddr);
			p += IPaddrlen;
			ipmove(p, ipforme(f, laddr) == Runi ? laddr : ifc->lifc->local);
			p += IPaddrlen;
			hnputs(p, rport);
			p += 2;
			hnputs(p, lport);
			break;
	}

	if (bp->next)
		bp = concatblock(bp);

	if (qfull(c->rq)) {
		qunlock(&c->qlock);
		netlog(f, Logudp, "udp: qfull %I.%d -> %I.%d\n", raddr, rport,
			   laddr, lport);
		freeblist(bp);
		return;
	}

	qpass(c->rq, bp);
	qunlock(&c->qlock);

}

char *udpctl(struct conv *c, char **f, int n)
{
	Udpcb *ucb;

	ucb = (Udpcb *) c->ptcl;
	if (n == 1) {
		if (strcmp(f[0], "oldheaders") == 0) {
			ucb->headers = 6;
			return NULL;
		} else if (strcmp(f[0], "headers") == 0) {
			ucb->headers = 7;
			return NULL;
		}
	}
	return "unknown control request";
}

void udpadvise(struct Proto *udp, struct block *bp, char *msg)
{
	Udp4hdr *h4;
	Udp6hdr *h6;
	uint8_t source[IPaddrlen], dest[IPaddrlen];
	uint16_t psource, pdest;
	struct conv *s, **p;
	int version;

	h4 = (Udp4hdr *) (bp->rp);
	version = ((h4->vihl & 0xF0) == IP_VER6) ? V6 : V4;

	switch (version) {
		case V4:
			v4tov6(dest, h4->udpdst);
			v4tov6(source, h4->udpsrc);
			psource = nhgets(h4->udpsport);
			pdest = nhgets(h4->udpdport);
			break;
		case V6:
			h6 = (Udp6hdr *) (bp->rp);
			ipmove(dest, h6->udpdst);
			ipmove(source, h6->udpsrc);
			psource = nhgets(h6->udpsport);
			pdest = nhgets(h6->udpdport);
			break;
		default:
			panic("udpadvise: version %d", version);
			return;	/* to avoid a warning */
	}

	/* Look for a connection */
	qlock(&udp->qlock);
	for (p = udp->conv; *p; p++) {
		s = *p;
		if (s->rport == pdest)
			if (s->lport == psource)
				if (ipcmp(s->raddr, dest) == 0)
					if (ipcmp(s->laddr, source) == 0) {
						if (s->ignoreadvice)
							break;
						qlock(&s->qlock);
						qunlock(&udp->qlock);
						qhangup(s->rq, msg);
						qhangup(s->wq, msg);
						qunlock(&s->qlock);
						freeblist(bp);
						return;
					}
	}
	qunlock(&udp->qlock);
	freeblist(bp);
}

int udpstats(struct Proto *udp, char *buf, int len)
{
	Udppriv *upriv;

	upriv = udp->priv;
	return snprintf(buf, len,
					"InDatagrams: %lu\nNoPorts: %lu\nInErrors: %lu\nOutDatagrams: %lu\n",
					upriv->ustats.udpInDatagrams, upriv->ustats.udpNoPorts,
					upriv->ustats.udpInErrors, upriv->ustats.udpOutDatagrams);
}

void udpnewconv(struct Proto *udp, struct conv *conv)
{
	/* Fsprotoclone alloc'd our priv struct and attached it to conv already.
	 * Now we need to init it */
	struct Udpcb *ucb = (struct Udpcb *)conv->ptcl;
	qlock_init(&ucb->qlock);
}

void udpinit(struct Fs *fs)
{
	struct Proto *udp;

	udp = kzmalloc(sizeof(struct Proto), 0);
	udp->priv = kzmalloc(sizeof(Udppriv), 0);
	udp->name = "udp";
	udp->connect = udpconnect;
	udp->announce = udpannounce;
	udp->ctl = udpctl;
	udp->state = udpstate;
	udp->create = udpcreate;
	udp->close = udpclose;
	udp->rcv = udpiput;
	udp->advise = udpadvise;
	udp->stats = udpstats;
	udp->ipproto = IP_UDPPROTO;
	udp->nc = Nchans;
	udp->newconv = udpnewconv;
	udp->ptclsize = sizeof(Udpcb);

	Fsproto(fs, udp);
}

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

/*
 *  well known IP addresses
 */
uint8_t IPv4bcast[IPaddrlen] = {
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff
};

uint8_t IPv4allsys[IPaddrlen] = {
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0xff, 0xff,
	0xe0, 0, 0, 0x01
};

uint8_t IPv4allrouter[IPaddrlen] = {
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0xff, 0xff,
	0xe0, 0, 0, 0x02
};

uint8_t IPallbits[IPaddrlen] = {
	0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff
};

uint8_t IPnoaddr[IPaddrlen];

/*
 *  prefix of all v4 addresses
 */
uint8_t v4prefix[IPaddrlen] = {
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0xff, 0xff,
	0, 0, 0, 0
};

char *v6hdrtypes[Maxhdrtype] = {
	[HBH] "HopbyHop",
	[ICMP] "ICMP",
	[IGMP] "IGMP",
	[GGP] "GGP",
	[IPINIP] "IP",
	[ST] "ST",
	[TCP] "TCP",
	[UDP] "UDP",
	[ISO_TP4] "ISO_TP4",
	[RH] "Routinghdr",
	[FH] "Fraghdr",
	[IDRP] "IDRP",
	[RSVP] "RSVP",
	[AH] "Authhdr",
	[ESP] "ESP",
	[ICMPv6] "ICMPv6",
	[NNH] "Nonexthdr",
	[ISO_IP] "ISO_IP",
	[IGRP] "IGRP",
	[OSPF] "OSPF",
};

/*
 *  well known IPv6 addresses
 */
uint8_t v6Unspecified[IPaddrlen] = {
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0
};

uint8_t v6loopback[IPaddrlen] = {
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0x01
};

uint8_t v6linklocal[IPaddrlen] = {
	0xfe, 0x80, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0
};

uint8_t v6linklocalmask[IPaddrlen] = {
	0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff,
	0, 0, 0, 0,
	0, 0, 0, 0
};

int v6llpreflen = 8;			// link-local prefix length
uint8_t v6sitelocal[IPaddrlen] = {
	0xfe, 0xc0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0
};

uint8_t v6sitelocalmask[IPaddrlen] = {
	0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff,
	0, 0, 0, 0,
	0, 0, 0, 0
};

int v6slpreflen = 6;			// site-local prefix length
uint8_t v6glunicast[IPaddrlen] = {
	0x08, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0
};

uint8_t v6multicast[IPaddrlen] = {
	0xff, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0
};

uint8_t v6multicastmask[IPaddrlen] = {
	0xff, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0
};

int v6mcpreflen = 1;			// multicast prefix length
uint8_t v6allnodesN[IPaddrlen] = {
	0xff, 0x01, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0x01
};

uint8_t v6allnodesNmask[IPaddrlen] = {
	0xff, 0xff, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0
};

int v6aNpreflen = 2;			// all nodes (N) prefix
uint8_t v6allnodesL[IPaddrlen] = {
	0xff, 0x02, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0x01
};

uint8_t v6allnodesLmask[IPaddrlen] = {
	0xff, 0xff, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0
};

int v6aLpreflen = 2;			// all nodes (L) prefix
uint8_t v6allroutersN[IPaddrlen] = {
	0xff, 0x01, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0x02
};

uint8_t v6allroutersL[IPaddrlen] = {
	0xff, 0x02, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0x02
};

uint8_t v6allroutersS[IPaddrlen] = {
	0xff, 0x05, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0x02
};

uint8_t v6solicitednode[IPaddrlen] = {
	0xff, 0x02, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0x01,
	0xff, 0, 0, 0
};

uint8_t v6solicitednodemask[IPaddrlen] = {
	0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff,
	0xff, 0x0, 0x0, 0x0
};

int v6snpreflen = 13;

uint16_t ptclcsum(struct block *bp, int offset, int len)
{
	uint8_t *addr;
	uint32_t losum, hisum;
	uint16_t csum;
	int odd, blocklen, x;

	/* Correct to front of data area */
	while (bp != NULL && offset && offset >= BLEN(bp)) {
		offset -= BLEN(bp);
		bp = bp->next;
	}
	if (bp == NULL)
		return 0;

	addr = bp->rp + offset;
	blocklen = BLEN(bp) - offset;

	if (bp->next == NULL) {
		if (blocklen < len)
			len = blocklen;
		return ~ptclbsum(addr, len) & 0xffff;
	}

	losum = 0;
	hisum = 0;

	odd = 0;
	while (len) {
		x = blocklen;
		if (len < x)
			x = len;

		csum = ptclbsum(addr, x);
		if (odd)
			hisum += csum;
		else
			losum += csum;
		odd = (odd + x) & 1;
		len -= x;

		bp = bp->next;
		if (bp == NULL)
			break;
		blocklen = BLEN(bp);
		addr = bp->rp;
	}

	losum += hisum >> 8;
	losum += (hisum & 0xff) << 8;
	while ((csum = losum >> 16) != 0)
		losum = csum + (losum & 0xffff);

	return ~losum & 0xffff;
}

enum {
	Isprefix = 16,
};

static uint8_t prefixvals[256] = {
	[0x00] 0 | Isprefix,
	[0x80] 1 | Isprefix,
	[0xC0] 2 | Isprefix,
	[0xE0] 3 | Isprefix,
	[0xF0] 4 | Isprefix,
	[0xF8] 5 | Isprefix,
	[0xFC] 6 | Isprefix,
	[0xFE] 7 | Isprefix,
	[0xFF] 8 | Isprefix,
};

#define CLASS(p) ((*( uint8_t *)(p))>>6)

extern char *v4parseip(uint8_t * to, char *from)
{
	int i;
	char *p;

	p = from;
	for (i = 0; i < 4 && *p; i++) {
		to[i] = strtoul(p, &p, 0);
		if (*p == '.')
			p++;
	}
	switch (CLASS(to)) {
		case 0:	/* class A - 1 uint8_t net */
		case 1:
			if (i == 3) {
				to[3] = to[2];
				to[2] = to[1];
				to[1] = 0;
			} else if (i == 2) {
				to[3] = to[1];
				to[1] = 0;
			}
			break;
		case 2:	/* class B - 2 uint8_t net */
			if (i == 3) {
				to[3] = to[2];
				to[2] = 0;
			}
			break;
	}
	return p;
}

int isv4(uint8_t * ip)
{
	return memcmp(ip, v4prefix, IPv4off) == 0;
}

/*
 *  the following routines are unrolled with no memset's to speed
 *  up the usual case
 */
void v4tov6(uint8_t * v6, uint8_t * v4)
{
	v6[0] = 0;
	v6[1] = 0;
	v6[2] = 0;
	v6[3] = 0;
	v6[4] = 0;
	v6[5] = 0;
	v6[6] = 0;
	v6[7] = 0;
	v6[8] = 0;
	v6[9] = 0;
	v6[10] = 0xff;
	v6[11] = 0xff;
	v6[12] = v4[0];
	v6[13] = v4[1];
	v6[14] = v4[2];
	v6[15] = v4[3];
}

int v6tov4(uint8_t * v4, uint8_t * v6)
{
	if (v6[0] == 0
		&& v6[1] == 0
		&& v6[2] == 0
		&& v6[3] == 0
		&& v6[4] == 0
		&& v6[5] == 0
		&& v6[6] == 0
		&& v6[7] == 0
		&& v6[8] == 0 && v6[9] == 0 && v6[10] == 0xff && v6[11] == 0xff) {
		v4[0] = v6[12];
		v4[1] = v6[13];
		v4[2] = v6[14];
		v4[3] = v6[15];
		return 0;
	} else {
		memset(v4, 0, 4);
		return -1;
	}
}

uint32_t parseip(uint8_t * to, char *from)
{
	int i, elipsis = 0, v4 = 1;
	uint32_t x;
	char *p, *op;

	memset(to, 0, IPaddrlen);
	p = from;
	for (i = 0; i < 16 && *p; i += 2) {
		op = p;
		x = strtoul(p, &p, 16);
		if (*p == '.' || (*p == 0 && i == 0)) {
			p = v4parseip(to + i, op);
			i += 4;
			break;
		} else {
			to[i] = x >> 8;
			to[i + 1] = x;
		}
		if (*p == ':') {
			v4 = 0;
			if (*++p == ':') {
				elipsis = i + 2;
				p++;
			}
		}
	}
	if (i < 16) {
		memmove(&to[elipsis + 16 - i], &to[elipsis], i - elipsis);
		memset(&to[elipsis], 0, 16 - i);
	}
	if (v4) {
		to[10] = to[11] = 0xff;
		return nhgetl(to + 12);
	} else
		return 6;
}

/*
 *  hack to allow ip v4 masks to be entered in the old
 *  style
 */
uint32_t parseipmask(uint8_t * to, char *from)
{
	uint32_t x;
	int i;
	uint8_t *p;

	if (*from == '/') {
		/* as a number of prefix bits */
		i = atoi(from + 1);
		if (i < 0)
			i = 0;
		if (i > 128)
			i = 128;
		memset(to, 0, IPaddrlen);
		for (p = to; i >= 8; i -= 8)
			*p++ = 0xff;
		if (i > 0)
			*p = ~((1 << (8 - i)) - 1);
		x = nhgetl(to + IPv4off);
	} else {
		/* as a straight bit mask */
		x = parseip(to, from);
		if (memcmp(to, v4prefix, IPv4off) == 0)
			memset(to, 0xff, IPv4off);
	}
	return x;
}

void maskip(uint8_t * from, uint8_t * mask, uint8_t * to)
{
	int i;

	for (i = 0; i < IPaddrlen; i++)
		to[i] = from[i] & mask[i];
}

uint8_t classmask[4][16] = {
	{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	 0xff, 0x00, 0x00, 0x00}
	,
	{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	 0xff, 0x00, 0x00, 0x00}
	,
	{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	 0xff, 0xff, 0x00, 0x00}
	,

	{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	 0xff, 0xff, 0xff, 0x00}
	,
};

uint8_t *defmask(uint8_t * ip)
{
	if (isv4(ip))
		return classmask[ip[IPv4off] >> 6];
	else {
		if (ipcmp(ip, v6loopback) == 0)
			return IPallbits;
		else if (memcmp(ip, v6linklocal, v6llpreflen) == 0)
			return v6linklocalmask;
		else if (memcmp(ip, v6sitelocal, v6slpreflen) == 0)
			return v6sitelocalmask;
		else if (memcmp(ip, v6solicitednode, v6snpreflen) == 0)
			return v6solicitednodemask;
		else if (memcmp(ip, v6multicast, v6mcpreflen) == 0)
			return v6multicastmask;
		return IPallbits;
	}
}

void ipv62smcast(uint8_t * smcast, uint8_t * a)
{
	assert(IPaddrlen == 16);
	memmove(smcast, v6solicitednode, IPaddrlen);
	smcast[13] = a[13];
	smcast[14] = a[14];
	smcast[15] = a[15];
}

/*
 *  parse a hex mac address
 */
int parsemac(uint8_t * to, char *from, int len)
{
	char nip[4];
	char *p;
	int i;

	p = from;
	memset(to, 0, len);
	for (i = 0; i < len; i++) {
		if (p[0] == '\0' || p[1] == '\0')
			break;

		nip[0] = p[0];
		nip[1] = p[1];
		nip[2] = '\0';
		p += 2;

		to[i] = strtoul(nip, 0, 16);
		if (*p == ':')
			p++;
	}
	return i;
}

/*
 *  hashing tcp, udp, ... connections
 *  gcc weirdness: it gave a bogus result until ron split the %= out.
 */
uint32_t iphash(uint8_t * sa, uint16_t sp, uint8_t * da, uint16_t dp)
{
	uint32_t ret;
	ret = (sa[IPaddrlen - 1] << 24) ^ (sp << 16) ^ (da[IPaddrlen - 1] << 8)
		^ dp;
	ret %= Nhash;
	return ret;
}

void iphtadd(struct Ipht *ht, struct conv *c)
{
	uint32_t hv;
	struct Iphash *h;

	hv = iphash(c->raddr, c->rport, c->laddr, c->lport);
	h = kzmalloc(sizeof(*h), 0);
	if (ipcmp(c->raddr, IPnoaddr) != 0)
		h->match = IPmatchexact;
	else {
		if (ipcmp(c->laddr, IPnoaddr) != 0) {
			if (c->lport == 0)
				h->match = IPmatchaddr;
			else
				h->match = IPmatchpa;
		} else {
			if (c->lport == 0)
				h->match = IPmatchany;
			else
				h->match = IPmatchport;
		}
	}
	h->c = c;

	spin_lock(&ht->lock);
	h->next = ht->tab[hv];
	ht->tab[hv] = h;
	spin_unlock(&ht->lock);
}

void iphtrem(struct Ipht *ht, struct conv *c)
{
	uint32_t hv;
	struct Iphash **l, *h;

	hv = iphash(c->raddr, c->rport, c->laddr, c->lport);
	spin_lock(&ht->lock);
	for (l = &ht->tab[hv]; (*l) != NULL; l = &(*l)->next)
		if ((*l)->c == c) {
			h = *l;
			(*l) = h->next;
			kfree(h);
			break;
		}
	spin_unlock(&ht->lock);
}

/* look for a matching conversation with the following precedence
 *	connected && raddr,rport,laddr,lport
 *	announced && laddr,lport
 *	announced && *,lport
 *	announced && laddr,*
 *	announced && *,*
 */
struct conv *iphtlook(struct Ipht *ht, uint8_t * sa, uint16_t sp, uint8_t * da,
					  uint16_t dp)
{
	uint32_t hv;
	struct Iphash *h;
	struct conv *c;

	/* exact 4 pair match (connection) */
	hv = iphash(sa, sp, da, dp);
	spin_lock(&ht->lock);
	for (h = ht->tab[hv]; h != NULL; h = h->next) {
		if (h->match != IPmatchexact)
			continue;
		c = h->c;
		if (sp == c->rport && dp == c->lport
			&& ipcmp(sa, c->raddr) == 0 && ipcmp(da, c->laddr) == 0) {
			spin_unlock(&ht->lock);
			return c;
		}
	}

	/* match local address and port */
	hv = iphash(IPnoaddr, 0, da, dp);
	for (h = ht->tab[hv]; h != NULL; h = h->next) {
		if (h->match != IPmatchpa)
			continue;
		c = h->c;
		if (dp == c->lport && ipcmp(da, c->laddr) == 0) {
			spin_unlock(&ht->lock);
			return c;
		}
	}

	/* match just port */
	hv = iphash(IPnoaddr, 0, IPnoaddr, dp);
	for (h = ht->tab[hv]; h != NULL; h = h->next) {
		if (h->match != IPmatchport)
			continue;
		c = h->c;
		if (dp == c->lport) {
			spin_unlock(&ht->lock);
			return c;
		}
	}

	/* match local address */
	hv = iphash(IPnoaddr, 0, da, 0);
	for (h = ht->tab[hv]; h != NULL; h = h->next) {
		if (h->match != IPmatchaddr)
			continue;
		c = h->c;
		if (ipcmp(da, c->laddr) == 0) {
			spin_unlock(&ht->lock);
			return c;
		}
	}

	/* look for something that matches anything */
	hv = iphash(IPnoaddr, 0, IPnoaddr, 0);
	for (h = ht->tab[hv]; h != NULL; h = h->next) {
		if (h->match != IPmatchany)
			continue;
		c = h->c;
		spin_unlock(&ht->lock);
		return c;
	}
	spin_unlock(&ht->lock);
	return NULL;
}

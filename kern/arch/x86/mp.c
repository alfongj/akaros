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
 * MultiProcessor Specification Version 1.[14].
 */
typedef struct {				/* MP Floating Pointer */
	uint8_t signature[4];		/* "_MP_" */
	uint8_t addr[4];			/* PCMP */
	uint8_t length;				/* 1 */
	uint8_t revision;			/* [14] */
	uint8_t checksum;
	uint8_t feature[5];
} _MP_;

typedef struct {				/* MP Configuration Table */
	uint8_t signature[4];		/* "PCMP" */
	uint8_t length[2];
	uint8_t revision;			/* [14] */
	uint8_t checksum;
	uint8_t string[20];			/* OEM + Product ID */
	uint8_t oaddr[4];			/* OEM table pointer */
	uint8_t olength[2];			/* OEM table length */
	uint8_t entry[2];			/* entry count */
	uint8_t apicpa[4];			/* local APIC address */
	uint8_t xlength[2];			/* extended table length */
	uint8_t xchecksum;			/* extended table checksum */
	uint8_t reserved;

	uint8_t entries[];
} PCMP;

typedef struct {
	char type[6];
	int polarity;				/* default for this bus */
	int trigger;				/* default for this bus */
} Mpbus;

static Mpbus mpbusdef[] = {
	{"PCI   ", IPlow, TMlevel,},
	{"ISA   ", IPhigh, TMedge,},
};

static Mpbus *mpbus[Nbus];
int mpisabusno = -1;

static void mpintrprint(char *s, uint8_t * p)
{
	char buf[128], *b, *e;
	char format[] = " type %d flags %p bus %d IRQ %d APIC %d INTIN %d\n";

	b = buf;
	e = b + sizeof(buf);
/* can't use seprintf yet!
	b = seprintf(b, e, "mpparse: intr:");
	if(s != NULL)
		b = seprintf(b, e, " %s:", s);
	seprintf(b, e, format, p[1], l16get(p+2), p[4], p[5], p[6], p[7]);
	printd(buf);
*/
	printk("mpparse: intr:");
	if (s != NULL)
		printk(" %s:", s);
	printk(format, p[1], l16get(p + 2), p[4], p[5], p[6], p[7]);
}

static uint32_t mpmkintr(uint8_t * p)
{
	uint32_t v;
	struct apic *apic;
	int n, polarity, trigger;

	/*
	 * Check valid bus, interrupt input pin polarity
	 * and trigger mode. If the APIC ID is 0xff it means
	 * all APICs of this type so those checks for useable
	 * APIC and valid INTIN must also be done later in
	 * the appropriate init routine in that case. It's hard
	 * to imagine routing a signal to all IOAPICs, the
	 * usual case is routing NMI and ExtINT to all LAPICs.
	 */
	if (mpbus[p[4]] == NULL) {
		mpintrprint("no source bus", p);
		return 0;
	}
	if (p[6] != 0xff) {
		if (Napic < 256 && p[6] >= Napic) {
			mpintrprint("APIC ID out of range", p);
			return 0;
		}
		switch (p[0]) {
			default:
				mpintrprint("INTIN botch", p);
				return 0;
			case 3:	/* IOINTR */
				apic = &xioapic[p[6]];
				if (!apic->useable) {
					mpintrprint("unuseable ioapic", p);
					return 0;
				}
				if (p[7] >= apic->nrdt) {
					mpintrprint("IO INTIN out of range", p);
					return 0;
				}
				break;
			case 4:	/* LINTR */
				apic = &xlapic[p[6]];
				if (!apic->useable) {
					mpintrprint("unuseable lapic", p);
					return 0;
				}
				if (p[7] >= ARRAY_SIZE(apic->lvt)) {
					mpintrprint("LOCAL INTIN out of range", p);
					return 0;
				}
				break;
		}
	}
	n = l16get(p + 2);
	if ((polarity = (n & 0x03)) == 2 || (trigger = ((n >> 2) & 0x03)) == 2) {
		mpintrprint("invalid polarity/trigger", p);
		return 0;
	}

	/*
	 * Create the low half of the vector table entry (LVT or RDT).
	 * For the NMI, SMI and ExtINT cases, the polarity and trigger
	 * are fixed (but are not always consistent over IA-32 generations).
	 * For the INT case, either the polarity/trigger are given or
	 * it defaults to that of the source bus;
	 * whether INT is Fixed or Lowest Priority is left until later.
	 */
	v = Im;
	switch (p[1]) {
		default:
			mpintrprint("invalid type", p);
			return 0;
		case 0:	/* INT */
			switch (polarity) {
				case 0:
					v |= mpbus[p[4]]->polarity;
					break;
				case 1:
					v |= IPhigh;
					break;
				case 3:
					v |= IPlow;
					break;
			}
			switch (trigger) {
				case 0:
					v |= mpbus[p[4]]->trigger;
					break;
				case 1:
					v |= TMedge;
					break;
				case 3:
					v |= TMlevel;
					break;
			}
			break;
		case 1:	/* NMI */
			v |= TMedge | IPhigh | MTnmi;
			break;
		case 2:	/* SMI */
			v |= TMedge | IPhigh | MTsmi;
			break;
		case 3:	/* ExtINT */
			v |= TMedge | IPhigh | MTei;
			break;
	}

	return v;
}

static int mpparse(PCMP * pcmp, int maxcores)
{
	uint32_t lo;
	uint8_t *e, *p;
	int devno, i, n;

	p = pcmp->entries;
	e = ((uint8_t *) pcmp) + l16get(pcmp->length);
	while (p < e)
		switch (*p) {
			default:
				printd("mpparse: unknown PCMP type %d (e-p %#ld)\n", *p, e - p);
				for (i = 0; p < e; i++) {
					if (i && ((i & 0x0f) == 0))
						printd("\n");
					printd(" %#2.2ux", *p);
					p++;
				}
				printd("\n");
				break;
			case 0:	/* processor */
				/*
				 * Initialise the APIC if it is enabled (p[3] & 0x01).
				 * p[1] is the APIC ID, the memory mapped address comes
				 * from the PCMP structure as the addess is local to the
				 * CPU and identical for all. Indicate whether this is
				 * the bootstrap processor (p[3] & 0x02).
				 */
				printk("mpparse: cpu %d pa %p bp %d\n",
					   p[1], l32get(pcmp->apicpa), p[3] & 0x02);
				if ((p[3] & 0x01) != 0 && maxcores > 0) {
					maxcores--;
					apicinit(p[1], l32get(pcmp->apicpa), p[3] & 0x02);
				}
				p += 20;
				break;
			case 1:	/* bus */
				printk("mpparse: bus: %d type %6.6s\n", p[1], (char *)p + 2);
				if (p[1] >= Nbus) {
					printd("mpparse: bus %d out of range\n", p[1]);
					p += 8;
					break;
				}
				if (mpbus[p[1]] != NULL) {
					printd("mpparse: bus %d already allocated\n", p[1]);
					p += 8;
					break;
				}
				for (i = 0; i < ARRAY_SIZE(mpbusdef); i++) {
					if (memcmp(p + 2, mpbusdef[i].type, 6) != 0)
						continue;
					if (memcmp(p + 2, "ISA   ", 6) == 0) {
						if (mpisabusno != -1) {
							printd("mpparse: bus %d already have ISA bus %d\n",
								   p[1], mpisabusno);
							continue;
						}
						mpisabusno = p[1];
					}
					mpbus[p[1]] = &mpbusdef[i];
					break;
				}
				if (mpbus[p[1]] == NULL)
					printd("mpparse: bus %d type %6.6s unknown\n",
						   p[1], (char *unused_char_p_t)p + 2);

				p += 8;
				break;
			case 2:	/* IOAPIC */
				/*
				 * Initialise the IOAPIC if it is enabled (p[3] & 0x01).
				 * p[1] is the APIC ID, p[4-7] is the memory mapped address.
				 */
				if (p[3] & 0x01)
					ioapicinit(p[1], -1, l32get(p + 4));

				p += 8;
				break;
			case 3:	/* IOINTR */
				/*
				 * p[1] is the interrupt type;
				 * p[2-3] contains the polarity and trigger mode;
				 * p[4] is the source bus;
				 * p[5] is the IRQ on the source bus;
				 * p[6] is the destination APIC;
				 * p[7] is the INITIN pin on the destination APIC.
				 */
				if (p[6] == 0xff) {
					mpintrprint("routed to all IOAPICs", p);
					p += 8;
					break;
				}
				if ((lo = mpmkintr(p)) == 0) {
					p += 8;
					break;
				}
				if (2)
					mpintrprint(NULL, p);

				/*
				 * Always present the device number in the style
				 * of a PCI Interrupt Assignment Entry. For the ISA
				 * bus the IRQ is the device number but unencoded.
				 * May need to handle other buses here in the future
				 * (but unlikely).
				 */
				devno = p[5];
				if (memcmp(mpbus[p[4]]->type, "PCI   ", 6) != 0)
					devno <<= 2;
				void ioapicintrinit(int busno, int apicno, int intin, int devno,
									int lo);
				ioapicintrinit(p[4], p[6], p[7], devno, lo);

				p += 8;
				break;
			case 4:	/* LINTR */
				/*
				 * Format is the same as IOINTR above.
				 */
				if ((lo = mpmkintr(p)) == 0) {
					p += 8;
					break;
				}
				if (2)
					mpintrprint(NULL, p);

				/*
				 * Everything was checked in mpmkintr above.
				 */
				if (p[6] == 0xff) {
					for (i = 0; i < Napic; i++) {
						if (!xlapic[i].useable || xlapic[i].addr)
							continue;
						xlapic[i].lvt[p[7]] = lo;
					}
				} else
					xlapic[p[6]].lvt[p[7]] = lo;
				p += 8;
				break;
		}

	/*
	 * There's nothing of interest in the extended table,
	 * but check it for consistency.
	 */
	p = e;
	e = p + l16get(pcmp->xlength);
	while (p < e)
		switch (*p) {
			default:
				n = p[1];
				printd("mpparse: unknown extended entry %d length %d\n", *p, n);
				for (i = 0; i < n; i++) {
					if (i && ((i & 0x0f) == 0))
						printd("\n");
					printd(" %#2.2ux", *p);
					p++;
				}
				printd("\n");
				break;
			case 128:
				printk("address space mapping\n");
				printk(" bus %d type %d base %#llux length %#llux\n",
					   p[2], p[3], l64get(p + 4), l64get(p + 12));
				p += p[1];
				break;
			case 129:
				printk("bus hierarchy descriptor\n");
				printk(" bus %d sd %d parent bus %d\n", p[2], p[3], p[4]);
				p += p[1];
				break;
			case 130:
				printk("compatibility bus address space modifier\n");
				printk(" bus %d pr %d range list %d\n",
					   p[2], p[3], l32get(p + 4));
				p += p[1];
				break;
		}
	return maxcores;
}

static void *sigsearch(char *signature)
{
	uintptr_t p;
	uint8_t *bda;
	void *r;
#if 0
	/*
	 * Search for the data structure:
	 * 1) in the first KB of the EBDA;
	 * 2) in the last KB of system base memory;
	 * 3) in the BIOS ROM between 0xe0000 and 0xfffff.
	 */
	bda = BIOSSEG(0x40);
	if (memcmp(KADDR(0xfffd9), "EISA", 4) == 0) {
		if ((p = (bda[0x0f] << 8) | bda[0x0e])) {
			if ((r = sigscan(BIOSSEG(p), 1024, signature)) != NULL)
				return r;
		}
	}

	p = ((bda[0x14] << 8) | bda[0x13]) * 1024;
	if ((r = sigscan(KADDR(p - 1024), 1024, signature)) != NULL)
		return r;
#endif
	r = sigscan(KADDR(0xe0000), 0x20000, signature);
	printk("Found mp table at %p\n", r);
	if (r != NULL)
		return r;

	return NULL;
	/* and virtualbox hidden mp tables... */
//  return sigscan(KADDR(0xa0000 - 1024), 1024, signature);
}

int mpsinit(int maxcores)
{
	uint8_t *p;
	int i, n, ncleft = 254;
	_MP_ *mp;
	PCMP *pcmp;

	if ((mp = sigsearch("_MP_")) == NULL) {
		printd("no mp tables\n");
		return ncleft;
	}

	if (2) {
		printk("_MP_ @ %#p, addr %p length %ud rev %d",
			   mp, l32get(mp->addr), mp->length, mp->revision);
		for (i = 0; i < sizeof(mp->feature); i++)
			printk(" %2.2p", mp->feature[i]);
		printk("\n");
	}
	if (mp->revision != 1 && mp->revision != 4)
		return ncleft;
	if (sigchecksum(mp, mp->length * 16) != 0)
		return ncleft;
#define vmap(x,y) KADDR((x))
#define vunmap(x,y)

	if ((pcmp = vmap(l32get(mp->addr), sizeof(PCMP))) == NULL)
		return ncleft;
	if (pcmp->revision != 1 && pcmp->revision != 4) {
		return ncleft;
	}
	n = l16get(pcmp->length) + l16get(pcmp->xlength);
	vunmap(pcmp, sizeof(PCMP));
	if ((pcmp = vmap(l32get(mp->addr), n)) == NULL)
		return ncleft;
	if (sigchecksum(pcmp, l16get(pcmp->length)) != 0) {
		vunmap(pcmp, n);
		return ncleft;
	}
	if (2) {
		printk("PCMP @ %#p length %p revision %d\n",
			   pcmp, l16get(pcmp->length), pcmp->revision);
		printk(" %20.20s oaddr %p olength %p\n",
			   (char *)pcmp->string, l32get(pcmp->oaddr),
			   l16get(pcmp->olength));
		printk(" entry %d apicpa %p\n",
			   l16get(pcmp->entry), l32get(pcmp->apicpa));

		printk(" xlength %p xchecksum %p\n",
			   l16get(pcmp->xlength), pcmp->xchecksum);
	}
	if (pcmp->xchecksum != 0) {
		p = ((uint8_t *) pcmp) + l16get(pcmp->length);
		i = sigchecksum(p, l16get(pcmp->xlength));
		if (((i + pcmp->xchecksum) & 0xff) != 0) {
			printd("extended table checksums to %p\n", i);
			vunmap(pcmp, n);
			return ncleft;
		}
	}

	/*
	 * Parse the PCMP table and set up the datastructures
	 * for later interrupt enabling and application processor
	 * startup.
	 */
	ncleft = mpparse(pcmp, maxcores);
	return ncleft;
//  mpacpi(ncleft);

//  apicdump();
//  ioapicdump();
}

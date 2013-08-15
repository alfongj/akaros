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
#include <fcall.h>

int
statcheck(uint8_t *buf, unsigned nbuf)
{
	uint8_t *ebuf;
	int i;

	ebuf = buf + nbuf;

	if(nbuf < STATFIXLEN || nbuf != BIT16SZ + GBIT16(buf))
		return -1;

	buf += STATFIXLEN - 4 * BIT16SZ;

	for(i = 0; i < 4; i++){
		if(buf + BIT16SZ > ebuf)
			return -1;
		buf += BIT16SZ + GBIT16(buf);
	}

	if(buf != ebuf)
		return -1;

	return 0;
}

static char nullstring[] = "";

unsigned int
  convM2D(uint8_t *buf, unsigned int nbuf, struct dir *d, char *strs)
{
	uint8_t *p, *ebuf;
	char *sv[4];
	int i, ns;

	if(nbuf < STATFIXLEN)
		return 0; 

	p = buf;
	ebuf = buf + nbuf;

	p += BIT16SZ;	/* ignore size */
	d->type = GBIT16(p);
	p += BIT16SZ;
	d->dev = GBIT32(p);
	p += BIT32SZ;
	d->qid.type = GBIT8(p);
	p += BIT8SZ;
	d->qid.vers = GBIT32(p);
	p += BIT32SZ;
	d->qid.path = GBIT64(p);
	p += BIT64SZ;
	d->mode = GBIT32(p);
	p += BIT32SZ;
	d->atime = GBIT32(p);
	p += BIT32SZ;
	d->mtime = GBIT32(p);
	p += BIT32SZ;
	d->length = GBIT64(p);
	p += BIT64SZ;

	for(i = 0; i < 4; i++){
		if(p + BIT16SZ > ebuf)
			return 0;
		ns = GBIT16(p);
		p += BIT16SZ;
		if(p + ns > ebuf)
			return 0;
		if(strs){
			sv[i] = strs;
			memmove(strs, p, ns);
			strs += ns;
			*strs++ = '\0';
		}
		p += ns;
	}

	if(strs){
		d->name = sv[0];
		d->uid = sv[1];
		d->gid = sv[2];
		d->muid = sv[3];
	}else{
		d->name = nullstring;
		d->uid = nullstring;
		d->gid = nullstring;
		d->muid = nullstring;
	}
	
	return p - buf;
}

/* Special akaros edition. */
/* akaros does not (yet) pass as much info as plan 9 does,
 * and it still has stuff I'm not happy about like an inode number.
 */
#if 0
struct kdirent {
        __ino64_t                                       d_ino;          /* inod
e number */
        __off64_t                                       d_off;          /* offs
et to the next dirent */
        unsigned short                          d_reclen;       /* length of th
is record */
        unsigned char                           d_type;
        char                                            d_name[MAX_FILENAME_SZ 
+ 1];   /* filename */
i __attribute__((aligned(8)));

#endif

unsigned int
  convM2kdirent(uint8_t *buf, unsigned int nbuf, struct kdirent *kd)
{
	uint8_t *p, *ebuf;
	char *sv[4];
	int i, ns;
	uint32_t junk;
printd("convM2kdirent >>>>>>>>>nbuf %d STATFIXLEN %d\n", nbuf, STATFIXLEN);
	if(nbuf < STATFIXLEN)
		return 0; 

	p = buf;
	ebuf = buf + nbuf;

	p += BIT16SZ;	/* ignore size */
	kd->d_type = GBIT16(p);
	p += BIT16SZ;
	junk = GBIT32(p);
	p += BIT32SZ;
	junk = GBIT8(p);
	p += BIT8SZ;
	junk = GBIT32(p);
	p += BIT32SZ;
	kd->d_ino = GBIT64(p);
	p += BIT64SZ;
	junk /* mode */= GBIT32(p);
	p += BIT32SZ;
	junk /*d->atime*/ = GBIT32(p);
	p += BIT32SZ;
	junk /*d->mtime*/ = GBIT32(p);
	p += BIT32SZ;
	junk /*d->length */= GBIT64(p);
	p += BIT64SZ;


	/* for now, uids in akaros are ints. Does not
	 * matter; kdirents are limited in what they tell you.
	 * get the name, ignore the rest. Maybe we can
	 * fix this later. 
	 */
	for(i = 0; i < 4; i++){
		if(p + BIT16SZ > ebuf)
			return 0;
		ns = GBIT16(p);
		p += BIT16SZ;
		if(p + ns > ebuf)
			return 0;
		if (i == 0){
			kd->d_reclen = ns;
			memmove(kd->d_name, p, ns);
			kd->d_name[ns] = 0;
		}
		p += ns;
	}

	printd("%s returns %d %s\n", __func__, p-buf, kd->d_name);
	return p - buf;
}

unsigned int
  convM2kstat(uint8_t *buf, unsigned int nbuf, struct kstat *ks)
{
	uint8_t *p, *ebuf;
	char *sv[4];
	int i, ns;
	uint32_t junk;

	if(nbuf < STATFIXLEN)
		return 0; 

	p = buf;
	ebuf = buf + nbuf;

	p += BIT16SZ;	/* ignore size */
	junk /*kd->d_type*/ = GBIT16(p);
	p += BIT16SZ;
	ks->st_rdev = ks->st_dev = GBIT32(p);
	p += BIT32SZ;
	junk /*qid.type*/= GBIT8(p);
	p += BIT8SZ;
	junk /*qid.vers */= GBIT32(p);
	p += BIT32SZ;
	ks->st_ino = GBIT64(p);
	p += BIT64SZ;
	ks->st_mode = GBIT32(p);
	if (ks->st_mode & DMDIR){
		ks->st_mode &= ~DMDIR;
		ks->st_mode |= __S_IFDIR;
	} else {
		ks->st_mode |= __S_IFREG;
	}
	p += BIT32SZ;
	ks->st_atime.tv_sec = GBIT32(p);
	p += BIT32SZ;
	ks->st_mtime.tv_sec = GBIT32(p);
	p += BIT32SZ;
	ks->st_size = GBIT64(p);
	p += BIT64SZ;
	ks->st_blksize = 512;
	ks->st_blocks = ROUNDUP(ks->st_size, ks->st_blksize)/ks->st_blksize;

	ks->st_nlink = 2; // links make no sense any more. 
	ks->st_uid = ks->st_gid = 0;
	return p - buf;
}

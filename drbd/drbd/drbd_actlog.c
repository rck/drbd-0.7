/*
-*- linux-c -*-
   drbd_actlog.c
   Kernel module for 2.2.x/2.4.x Kernels

   This file is part of drbd by Philipp Reisner.

   Copyright (C) 2003, Philipp Reisner <philipp.reisner@gmx.at>.
        main author.

   drbd is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   drbd is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with drbd; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.

 */

#include <linux/slab.h>
#include "drbd.h"
#include "drbd_int.h"

#define AL_EXTENT_SIZE_B 22             // One extent represents 4M Storage
#define AL_EXTENT_SIZE (1<<AL_EXTENT_SIZE_B)
#define AL_EXTENTS_PT 61

struct al_transaction {
	u32       magic;
	u32       tr_number;
	// u32       tr_generation; //TODO
	struct {
		u32 pos;
		u32 extent; } updates[1 + AL_EXTENTS_PT];
	u32       xor_sum;
       // I do not believe that all storage medias can guarantee atomic
       // 512 byte write operations. When the journal is read, only
       // transactions with correct xor_sums are considered.
};     // sizeof() = 512 byte

STATIC void drbd_al_write_transaction(struct Drbd_Conf *,struct lc_element *);
STATIC void drbd_update_on_disk_bm(struct Drbd_Conf *,unsigned int ,int);


STATIC int drbd_al_changing(struct lru_cache* lc, struct lc_element *e, 
			    unsigned int enr)
{
	// This callback is called by lc_get(). 
	// WRITE transaction.... 
	// async:  do lc->flags &= ~LC_DIRTY and  wake_up(&mdev->al_wait);
	// in end of IO hander. Return 0 here.
	// sync: do everything here and return 1.

	struct Drbd_Conf *mdev = (struct Drbd_Conf *) lc->lc_private;
	unsigned long evicted;

	evicted = e->lc_number;
	e->lc_number = enr;
	spin_unlock_irq(&mdev->al_lock);	

	if(mdev->cstate < Connected && evicted != LC_FREE ) {
		drbd_update_on_disk_bm(mdev,evicted,1);
	}
	drbd_al_write_transaction(mdev,e);
	mdev->al_writ_cnt++;

	spin_lock_irq(&mdev->al_lock);	
	clear_bit(__LC_DIRTY,&lc->flags);
	clear_bit(__LC_STARVING,&lc->flags); // Do we need this here ?
	smp_mb__after_clear_bit();
	wake_up(&mdev->al_wait);

	return 1;
}

void drbd_al_init(struct Drbd_Conf *mdev)
{
	lc_init(&mdev->act_log);
	mdev->act_log.notify_on_change = drbd_al_changing;
	mdev->act_log.lc_private = mdev;
}

static inline 
struct lc_element* _al_get(struct Drbd_Conf *mdev, unsigned int enr)
{
	struct lc_element * extent;
	
	spin_lock_irq(&mdev->al_lock);
	extent = lc_get(&mdev->act_log,enr);	
	spin_unlock_irq(&mdev->al_lock);
	if(extent == 0) {
		if(mdev->act_log.flags & LC_STARVING) {
			WARN("Have to wait for LRU element (AL too small?)\n");
		}

		if(mdev->act_log.flags & LC_DIRTY) {
			WARN("Ongoing AL update (AL device too slow?)\n");
		}
	}
	
	return extent;
}

void drbd_al_begin_io(struct Drbd_Conf *mdev, sector_t sector)
{
	unsigned int enr = (sector >> (AL_EXTENT_SIZE_B-9));
	struct lc_element *extent;

	wait_event(mdev->al_wait, (extent = _al_get(mdev,enr)) );
}

void drbd_al_complete_io(struct Drbd_Conf *mdev, sector_t sector)
{
	unsigned int enr = (sector >> (AL_EXTENT_SIZE_B-9));
	struct lc_element *extent;
	unsigned long flags;

	spin_lock_irqsave(&mdev->al_lock,flags);

	extent = lc_find(&mdev->act_log,enr);

	if(!extent) {
		spin_unlock_irqrestore(&mdev->al_lock,flags);
		ERR("drbd_al_complete_io() called on inactive extent\n");
		return;
	}

	if( lc_put(&mdev->act_log,extent) == 0 ) {
		wake_up(&mdev->al_wait);
	}

	spin_unlock_irqrestore(&mdev->al_lock,flags);
}

STATIC void 
drbd_al_write_transaction(struct Drbd_Conf *mdev,struct lc_element *updated)
{
	int i,n,mx;
	unsigned int extent_nr;
	struct al_transaction* buffer;
	sector_t sector;
	u32 xor_sum=0;

	down(&mdev->md_io_mutex); // protects md_io_buffer, al_tr_cycle, ...
	buffer = (struct al_transaction*)bh_kmap(mdev->md_io_bh);

	buffer->magic = __constant_cpu_to_be32(DRBD_MAGIC);
	buffer->tr_number = cpu_to_be32(mdev->al_tr_number);

	n = lc_index_of(&mdev->act_log, updated);

	buffer->updates[0].pos = cpu_to_be32(n);
	buffer->updates[0].extent = cpu_to_be32(updated->lc_number);

#if 0	/* Use this printf with the test_al.pl program */
	ERR("T%03d S%03d=E%06d\n", mdev->al_tr_number,n,updated->lc_number);
#endif

	xor_sum ^= updated->lc_number;

	mx = min_t(int,AL_EXTENTS_PT,
		   mdev->act_log.nr_elements - mdev->al_tr_cycle);
	for(i=0;i<mx;i++) {
		extent_nr = lc_entry(&mdev->act_log,
				     mdev->al_tr_cycle+i)->lc_number;
		buffer->updates[i+1].pos = cpu_to_be32(mdev->al_tr_cycle+i);
		buffer->updates[i+1].extent = cpu_to_be32(extent_nr);
		xor_sum ^= extent_nr;
	}
	for(;i<AL_EXTENTS_PT;i++) {
		buffer->updates[i+1].pos = __constant_cpu_to_be32(-1);
		buffer->updates[i+1].extent = __constant_cpu_to_be32(LC_FREE);
		xor_sum ^= LC_FREE;
	}
	mdev->al_tr_cycle += AL_EXTENTS_PT;
	if(mdev->al_tr_cycle >= mdev->act_log.nr_elements) mdev->al_tr_cycle=0;

	buffer->xor_sum = cpu_to_be32(xor_sum);

	bh_kunmap(mdev->md_io_bh);

	sector = drbd_md_ss(mdev) + MD_AL_OFFSET + mdev->al_tr_pos ;

	drbd_set_md_bh(mdev, mdev->md_io_bh, sector, 512);
	set_bit(BH_Dirty, &mdev->md_io_bh->b_state);
	set_bit(BH_Lock, &mdev->md_io_bh->b_state);
	mdev->md_io_bh->b_end_io = drbd_generic_end_io;
	generic_make_request(WRITE,mdev->md_io_bh);
	wait_on_buffer(mdev->md_io_bh);

	if( ++mdev->al_tr_pos > div_ceil(mdev->act_log.nr_elements,AL_EXTENTS_PT) ) {
		mdev->al_tr_pos=0;
	}
	mdev->al_tr_number++;

	up(&mdev->md_io_mutex);
}

/* In case this function returns 1 == success, the caller must do
		bh_kunmap(mdev->md_io_bh);
		up(&mdev->md_io_mutex);
 */
STATIC int drbd_al_read_tr(struct Drbd_Conf *mdev, 
			   struct al_transaction** bp,
			   int index)
{
	struct al_transaction* buffer;
	sector_t sector;
	int rv,i;
	u32 xor_sum=0;

	down(&mdev->md_io_mutex);
	sector = drbd_md_ss(mdev) + MD_AL_OFFSET + index;

	drbd_set_md_bh(mdev, mdev->md_io_bh, sector, 512);
	clear_bit(BH_Uptodate, &mdev->md_io_bh->b_state);
	set_bit(BH_Lock, &mdev->md_io_bh->b_state);
	mdev->md_io_bh->b_end_io = drbd_generic_end_io;
	generic_make_request(READ,mdev->md_io_bh);
	wait_on_buffer(mdev->md_io_bh);

	buffer = (struct al_transaction*)bh_kmap(mdev->md_io_bh);

	rv = ( be32_to_cpu(buffer->magic) == DRBD_MAGIC );

	for(i=0;i<AL_EXTENTS_PT+1;i++) {
		xor_sum ^= be32_to_cpu(buffer->updates[i].extent);
	}
	rv &= (xor_sum == be32_to_cpu(buffer->xor_sum));

	if(rv) {
		*bp = buffer;
	} else {
		bh_kunmap(mdev->md_io_bh);
		up(&mdev->md_io_mutex);
	}

	return rv;
}

void drbd_al_read_log(struct Drbd_Conf *mdev)
{
	struct al_transaction* buffer;
	int from=-1,to=-1,i,cnr, overflow=0,rv;
	u32 from_tnr=-1, to_tnr=0;
	int active_extents=0;
	int transactions=0;
	int mx;

	mx = div_ceil(mdev->act_log.nr_elements,AL_EXTENTS_PT);

	// Find the valid transaction in the log
	for(i=0;i<=mx;i++) {
		if(!drbd_al_read_tr(mdev,&buffer,i)) continue;
		cnr = be32_to_cpu(buffer->tr_number);
		// INFO("index %d valid tnr=%d\n",i,cnr);
		bh_kunmap(mdev->md_io_bh);
		up(&mdev->md_io_mutex);

		if(cnr == -1) overflow=1;

		if(cnr < from_tnr && !overflow) {
			from = i;
			from_tnr = cnr;
		}
		if(cnr > to_tnr) {
			to = i;
			to_tnr = cnr;
		}
	}

	if(from == -1 || to == -1) {
		WARN("No usable activity log found.\n");
		//TODO set all bits in the bitmap!
		return;
	}

	// Read the valid transactions.
	// INFO("Reading from %d to %d.\n",from,to);

	i=from;
	while(1) {
		int j,pos;
		unsigned int extent_nr;
		unsigned int trn;

		rv = drbd_al_read_tr(mdev,&buffer,i);
		ERR_IF(!rv) goto cancel;

		trn=be32_to_cpu(buffer->tr_number);

		for(j=0;j<AL_EXTENTS_PT+1;j++) {
			pos = be32_to_cpu(buffer->updates[j].pos);
			extent_nr = be32_to_cpu(buffer->updates[j].extent);

			if(extent_nr == LC_FREE) continue;

		       //if(j<3) INFO("T%03d S%03d=E%06d\n",trn,pos,extent_nr);
			spin_lock_irq(&mdev->al_lock);
			lc_set(&mdev->act_log,extent_nr,pos);
			spin_unlock_irq(&mdev->al_lock);
			active_extents++;
		}

		bh_kunmap(mdev->md_io_bh);
		up(&mdev->md_io_mutex);

		transactions++;

	cancel:
		if( i == to) break;
		i++;
		if( i > mx ) i=0;
	}

	mdev->al_tr_number = to_tnr+1;
	mdev->al_tr_pos = to;
	if( ++mdev->al_tr_pos > div_ceil(mdev->act_log.nr_elements,AL_EXTENTS_PT) ) {
		mdev->al_tr_pos=0;
	}

	INFO("Found %d transactions (%d active extents) in activity log.\n",
	     transactions,active_extents);
}

/**
 * drbd_al_to_on_disk_bm: Writes the areas of the bitmap which are covered by
 * the AL.
 */
void drbd_al_to_on_disk_bm(struct Drbd_Conf *mdev)
{
	int i;
	unsigned int enr;

	// Lock down the lru-cache by setting the DIRTY bit...
	wait_event(mdev->al_wait, 
		   test_and_set_bit(__LC_DIRTY,&mdev->act_log.flags) == 0);

	for(i=0;i<mdev->act_log.nr_elements;i++) {
		enr = lc_entry(&mdev->act_log,i)->lc_number;
		if(enr == LC_FREE) continue;
		drbd_update_on_disk_bm(mdev,enr,1);
	}

	clear_bit(__LC_DIRTY,&mdev->act_log.flags);
	smp_mb__after_clear_bit();
	wake_up(&mdev->al_wait);
}

/**
 * drbd_al_apply_to_bm: Sets the bits in the bitmap that are described
 * by the active extents of the AL.
 */
void drbd_al_apply_to_bm(struct Drbd_Conf *mdev)
{
	int i;
	unsigned int enr;
	unsigned long add=0;

	// Lock down the lru-cache by setting the DIRTY bit...
	wait_event(mdev->al_wait, 
		   test_and_set_bit(__LC_DIRTY,&mdev->act_log.flags) == 0);

	for(i=0;i<mdev->act_log.nr_elements;i++) {
		enr = lc_entry(&mdev->act_log,i)->lc_number;
		if(enr == LC_FREE) continue;
		add += bm_set_bit( mdev, enr << (AL_EXTENT_SIZE_B-9), 
				   AL_EXTENT_SIZE, SS_OUT_OF_SYNC );
	}

	clear_bit(__LC_DIRTY,&mdev->act_log.flags);
	smp_mb__after_clear_bit();
	wake_up(&mdev->al_wait);

	INFO("Marked additional %lu KB as out-of-sync based on AL.\n",add/2);

	mdev->rs_total += add;
}

/**
 * drbd_read_bm: Read the whole bitmap from its on disk location.
 */
void drbd_read_bm(struct Drbd_Conf *mdev)
{
	unsigned long * buffer, * bm, word;
	sector_t sector;
	int want,bm_words,bm_i,buf_i;
	unsigned long bits=0;
	int so = 0;

	bm_i = 0;
	bm_words = mdev->mbds_id->size/sizeof(unsigned long);
	bm = mdev->mbds_id->bm;

	down(&mdev->md_io_mutex);

	while (1) {
		want=min_t(int,512/sizeof(long),bm_words-bm_i);
		if(want == 0) break;

		sector = drbd_md_ss(mdev) + MD_BM_OFFSET + so;
		so++;

		drbd_set_md_bh(mdev, mdev->md_io_bh, sector, 512);
		clear_bit(BH_Uptodate, &mdev->md_io_bh->b_state);
		set_bit(BH_Lock, &mdev->md_io_bh->b_state);
		mdev->md_io_bh->b_end_io = drbd_generic_end_io;
		generic_make_request(READ,mdev->md_io_bh);
		wait_on_buffer(mdev->md_io_bh);

		buffer = (unsigned long *)bh_kmap(mdev->md_io_bh);

		for(buf_i=0;buf_i<want;buf_i++) {
			word = lel_to_cpu(buffer[buf_i]);
			bits += parallel_bitcount(word);
			bm[bm_i++] = word;
		}
		bh_kunmap(mdev->md_io_bh);
	}

	up(&mdev->md_io_mutex);

	mdev->rs_total = (bits << (BM_BLOCK_SIZE_B - 9)) + 
		bm_end_of_dev_case(mdev->mbds_id);
	
	INFO("%lu KB marked out-of-sync by on disk bit-map.\n",
	     mdev->rs_total/2);
}

STATIC void drbd_async_eio(struct buffer_head *bh, int uptodate)
{
	struct Drbd_Conf *mdev;

	mdev=drbd_mdev_of_bh(bh);

	mark_buffer_uptodate(bh, uptodate);
	unlock_buffer(bh);
	up(&mdev->md_io_mutex);
}


#define BM_WORDS_PER_EXTENT ( (AL_EXTENT_SIZE/BM_BLOCK_SIZE) / BITS_PER_LONG )
#define BM_BYTES_PER_EXTENT ( (AL_EXTENT_SIZE/BM_BLOCK_SIZE) / 8 )
#define EXTENTS_PER_SECTOR  ( 512 / BM_BYTES_PER_EXTENT )
/**
 * drbd_update_on_disk_bm: Writes a piece of the bitmap to its
 * on disk location. 
 *
 * @enr: The extent number of the bits we should write to disk.
 */
STATIC void drbd_update_on_disk_bm(struct Drbd_Conf *mdev,unsigned int enr,
				       int sync)
{
	unsigned long * buffer, * bm;
	int want,buf_i,bm_words,bm_i;
	sector_t sector;

	enr = (enr & ~(EXTENTS_PER_SECTOR-1) );

	bm = mdev->mbds_id->bm;
	bm_words = mdev->mbds_id->size/sizeof(unsigned long);
	bm_i = enr * BM_WORDS_PER_EXTENT ;
	want=min_t(int,512/sizeof(long),bm_words-bm_i);

	down(&mdev->md_io_mutex); // protects md_io_buffer
	buffer = (unsigned long *)bh_kmap(mdev->md_io_bh);

	for(buf_i=0;buf_i<want;buf_i++) {
		buffer[buf_i] = cpu_to_lel(bm[bm_i++]);
	}

	bh_kunmap(mdev->md_io_bh);

	sector = drbd_md_ss(mdev) + MD_BM_OFFSET + enr/EXTENTS_PER_SECTOR;

	drbd_set_md_bh(mdev, mdev->md_io_bh, sector, 512);
	set_bit(BH_Dirty, &mdev->md_io_bh->b_state);
	set_bit(BH_Lock, &mdev->md_io_bh->b_state);
	mdev->md_io_bh->b_end_io = sync ? drbd_generic_end_io : drbd_async_eio;
	generic_make_request(WRITE,mdev->md_io_bh);
	if(sync) {
		wait_on_buffer(mdev->md_io_bh);
		up(&mdev->md_io_mutex);
	}

	mdev->bm_writ_cnt++;
}
#undef BM_WORDS_PER_EXTENT
#undef EXTENTS_PER_SECTOR

/* ATTENTION. The AL's extents are 4MB each, while the extents in the  *
 * resync LRU-cache are 16MB each.                                     */
#define SM (BM_EXTENT_SIZE / AL_EXTENT_SIZE)
STATIC void drbd_try_clear_on_disk_bm(struct Drbd_Conf *mdev,sector_t sector,
				      int cleared,int may_sleep)
{
	struct bm_extent* ext;
	unsigned long enr;
	unsigned long flags;

	// I simply assume that a sector/size pair never crosses
	// a 16 MB extent border. (Currently this is true...)
	enr = (sector >> (BM_EXTENT_SIZE_B-9));

	spin_lock_irqsave(&mdev->rs_lock,flags);
	ext = (struct bm_extent *) lc_get(&mdev->resync,enr);
	if(ext) {
		if( ext->lce.lc_number == enr) {
			ext->rs_left -= cleared;
			D_ASSERT((long)ext->rs_left >= 0);
		} else {
			ext->lce.lc_number = enr;
			ext->rs_left = bm_count_sectors(mdev->mbds_id,enr);
		}
		lc_put(&mdev->resync,(struct lc_element*)ext);
	} else {
		ERR("lc_get(rsync) failed ?!?\n");
	}
	spin_unlock_irqrestore(&mdev->rs_lock,flags);

	if(may_sleep) {
		struct list_head *le;

		spin_lock(&mdev->al_lock);
	restart:
		list_for_each(le,&mdev->resync.lru) {
			ext=(struct bm_extent *)list_entry(le,struct lc_element,list);
			if(ext->rs_left == 0) {
				spin_unlock(&mdev->al_lock);	       
				drbd_update_on_disk_bm(mdev,enr*SM,1);
				// TODO: reconsider to use the async version
				// of drbd_update_on_disk_bm()
				//INFO("Clearing e# %lu of on disk bm\n",enr);
				spin_lock(&mdev->al_lock);
				lc_del(&mdev->resync,&ext->lce);
				goto restart;
			}
		}
		spin_unlock(&mdev->al_lock);		
	}
}
#undef SM

void drbd_set_in_sync(drbd_dev* mdev, sector_t sector, 
		      int blk_size, int may_sleep)
{
	/* Is called by drbd_dio_end possibly from IRQ context, but
	   from other places in non IRQ */
	unsigned long flags=0;
	int cleared;
	int wake_dsender=0;

	cleared = bm_set_bit(mdev, sector, blk_size, SS_IN_SYNC);

	spin_lock_irqsave(&mdev->rs_lock,flags);
	mdev->rs_left -= cleared;
	D_ASSERT((long)mdev->rs_left >= 0);
	if( cleared && mdev->rs_left == 0 ) {
		spin_lock(&mdev->ee_lock); // IRQ lock already taken by rs_lock
		set_bit(SYNC_FINISHED,&mdev->flags);
		spin_unlock(&mdev->ee_lock);
		wake_dsender=1;
	}

	if(jiffies - mdev->rs_mark_time > HZ*10) {
		mdev->rs_mark_time=jiffies;
		mdev->rs_mark_left=mdev->rs_left;
	}
	spin_unlock_irqrestore(&mdev->rs_lock,flags);

	drbd_try_clear_on_disk_bm(mdev,sector,cleared,may_sleep);
	if(wake_dsender) {// must happen after drbd_try_clear_on_disk_bm();
		wake_up_interruptible(&mdev->dsender_wait); 
	}
}
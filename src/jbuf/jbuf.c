/**
 * @file jbuf.c  Jitter Buffer implementation
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <stdint.h>
#include <re_types.h>
#include <re_fmt.h>
#include <re_list.h>
#include <re_mbuf.h>
#include <re_mem.h>
#include <re_rtp.h>
#include <re_lock.h>
#include <re_tmr.h>
#include <re_jbuf.h>

#include <stdlib.h>

#define DEBUG_MODULE "jbuf"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


#ifndef RELEASE
#define JBUF_STAT 1  /**< Jitter buffer statistics */
#endif


#if JBUF_STAT
#define STAT_ADD(var, value)  (jb->stat.var) += (value) /**< Stats add */
#define STAT_INC(var)         ++(jb->stat.var)          /**< Stats inc */
#else
#define STAT_ADD(var, value)
#define STAT_INC(var)
#endif

#define JBUF_JITTER_PERIOD 512
#define JBUF_JITTER_UP_SPEED 64
#define JBUF_BUFTIME_PERIOD 16
#define JBUF_LO_BOUND 125              /* 125% of jitter */
#define JBUF_HI_BOUND 220              /* 220% of jitter */
#define JBUF_LH_CNT  20

/** Defines a packet frame */
struct frame {
	struct le le;           /**< Linked list element       */
	struct rtp_header hdr;  /**< RTP Header                */
	void *mem;              /**< Reference counted pointer */
};


enum jb_state {
	JS_GOOD = 0,
	JS_LOW,
	JS_HIGH
};


/** Jitter statistics */
struct jitter_stat {
	int32_t jitter;

	uint32_t ts0;        /**< previous timestamp              */
	uint64_t tr0;        /**< pre. time of arrival            */
#if DEBUG_LEVEL >= 6
	uint64_t tr00;       /**< arrival of first packet         */
#endif

	enum jb_state st;    /**< computed jitter buffer state    */

	int32_t avbuftime;   /**< average buffered time           */
	int32_t jtime;       /**< JBUF_JITTER_PERIOD * ptime      */
	int32_t mintime;     /**< minimum buffer time             */

	uint8_t locnt;       /**< hit low border counter          */
	uint8_t hicnt;       /**< hit high border counter         */

};


/**
 * Defines a jitter buffer
 *
 * The jitter buffer is for incoming RTP packets, which are sorted by
 * sequence number.
 */
struct jbuf {
	struct list pooll;   /**< List of free frames in pool               */
	struct list framel;  /**< List of buffered frames                   */
	uint32_t n;          /**< [# frames] Current # of frames in buffer  */
	uint32_t min;        /**< [# frames] Minimum # of frames to buffer  */
	uint32_t max;        /**< [# frames] Maximum # of frames to buffer  */
	uint32_t wish;       /**< [# frames] Startup wish size for buffer   */
	uint32_t ptime;      /**< packet delta in ms                        */
	uint16_t seq_put;    /**< Sequence number for last jbuf_put()       */
	uint32_t ssrc;       /**< Previous ssrc                             */
	bool started;        /**< Jitter buffer is in start phase           */
	bool running;        /**< Jitter buffer is running                  */
	bool silence;        /**< Silence detected. Set externally.         */
	struct jitter_stat jitst;  /**< Jitter statistics.                  */

	struct lock *lock;   /**< Makes jitter buffer thread safe           */
#if JBUF_STAT
	uint16_t seq_get;      /**< Timestamp of last played frame */
	struct jbuf_stat stat; /**< Jitter buffer Statistics       */
#endif
};


/** Is x less than y? */
static inline bool seq_less(uint16_t x, uint16_t y)
{
	return ((int16_t)(x - y)) < 0;
}


/**
 * Get a frame from the pool
 */
static void frame_alloc(struct jbuf *jb, struct frame **f)
{
	struct le *le;

	le = jb->pooll.head;
	if (le) {
		list_unlink(le);
		++jb->n;
	}
	else {
		struct frame *f0;

		/* Steal an old frame */
		le = jb->framel.head;
		f0 = le->data;

		STAT_INC(n_overflow);
		DEBUG_INFO("drop 1 old frame seq=%u (total dropped %u)\n",
			   f0->hdr.seq, jb->stat.n_overflow);

		f0->mem = mem_deref(f0->mem);
		list_unlink(le);
	}

	*f = le->data;
}


/**
 * Release a frame, put it back in the pool
 */
static void frame_deref(struct jbuf *jb, struct frame *f)
{
	f->mem = mem_deref(f->mem);
	list_unlink(&f->le);
	list_append(&jb->pooll, &f->le, f);
	--jb->n;
}


static void jbuf_destructor(void *data)
{
	struct jbuf *jb = data;

	jbuf_flush(jb);

	/* Free all frames in the pool list */
	list_flush(&jb->pooll);
	mem_deref(jb->lock);
}


static void jbuf_init_jitst(struct jbuf *jb)
{
	struct jitter_stat *st = &jb->jitst;
	memset(st, 0, sizeof(jb->jitst));

	st->jtime = (int32_t) jb->ptime * JBUF_JITTER_PERIOD;

	/* We start with wish size. */
	st->avbuftime = jb->wish * st->jtime;

	/* Compute a good start value for jitter fitting to wish size.
	 * Note: JBUF_LO_BOUND and JBUF_HI_BOUND are in percent.
	 *
	 * jitter = buftime * 100% / ( (JBUF_LO_BOUND + JBUF_HI_BOUND) / 2 )
	 *                                                                       */
	st->jitter = st->avbuftime * 100 * 2 / (JBUF_LO_BOUND + JBUF_HI_BOUND);
	st->mintime = jb->min * st->jtime - st->jtime / 3;
}


/**
 * Allocate a new jitter buffer
 *
 * @param jbp    Pointer to returned jitter buffer
 * @param min    Minimum delay in [frames]
 * @param max    Maximum delay in [frames]
 * @param wish   Wish delay in [frames]. Used at start.
 *
 * @return 0 if success, otherwise errorcode
 */
int jbuf_alloc(struct jbuf **jbp, uint32_t min, uint32_t max, uint32_t wish)
{
	struct jbuf *jb;
	uint32_t i;
	int err = 0;

	if (!jbp || ( min > max))
		return EINVAL;

	/* self-test: x < y (also handle wrap around) */
	if (!seq_less(10, 20) || seq_less(20, 10) || !seq_less(65535, 0)) {
		DEBUG_WARNING("seq_less() is broken\n");
		return ENOSYS;
	}

	jb = mem_zalloc(sizeof(*jb), jbuf_destructor);
	if (!jb)
		return ENOMEM;

	list_init(&jb->pooll);
	list_init(&jb->framel);

	/* apply constraints to min, max and wish for a good audio start */
	jb->min   = MAX(min, 1);

	jb->max   = MAX(max, jb->min + 3);
	jb->max   = MAX(jb->max, jb->min * JBUF_HI_BOUND / JBUF_LO_BOUND);

	jb->wish  = wish;
	jb->wish  = MAX(jb->min + 1, MIN(jb->max - 1, jb->wish));

	DEBUG_INFO("alloc: delay min=%u max=%u wish=%u frames\n",
			jb->min, jb->max, jb->wish);

	/* initial value for ptime is only an estimation */
	jb->ptime = 16;
	jbuf_init_jitst(jb);
	err = lock_alloc(&jb->lock);
	if (err)
		goto out;

	/* Allocate all frames now */
	for (i=0; i<jb->max; i++) {
		struct frame *f = mem_zalloc(sizeof(*f), NULL);
		if (!f) {
			err = ENOMEM;
			break;
		}

		list_append(&jb->pooll, &f->le, f);
		DEBUG_INFO("alloc: adding to pool list %u\n", i);
	}

out:
	if (err)
		mem_deref(jb);
	else
		*jbp = jb;

	return err;
}


static uint32_t calc_bufftime(struct jbuf *jb)
{
	struct frame *f0, *f1;
	uint32_t buftime = jb->ptime;
	uint32_t diff;
	uint32_t ptime = jb->ptime;
	struct jitter_stat *st = &jb->jitst;

	if (jb->n) {
		f0 = jb->framel.head->data;
		f1 = jb->framel.tail->data;
		diff = (f1->hdr.ts - f0->hdr.ts) / 8;
		if (diff) {
			/* re-compute ptime */
			ptime = diff / list_count(&jb->framel);
			buftime = diff + ptime;
			if (ptime != jb->ptime) {
				st->jtime = (int32_t) jb->ptime * JBUF_JITTER_PERIOD;
				st->mintime = jb->min * st->jtime - st->jtime / 3;
			}
		} else {
			buftime = jb->ptime;
		}
	}

	return buftime;
}

/** Computes the jitter for packet arrival. Should be called by
 * jbuf_put.
 *
 * @param jb  Jitter buffer
 * @param ts  The timestamp in rtp header.
 * @param seq The sequence number in rtp header.
 *
 */
static void jbuf_jitter_calc(struct jbuf *jb, uint32_t ts)
{
	struct jitter_stat *st = &jb->jitst;
	uint64_t tr = tmr_jiffies();
	int32_t buftime, bufmax, bufmin;
	int32_t d;
	int32_t da;
	int32_t s;
	int32_t djit;

	if (!st->ts0)
		goto out;

	buftime = calc_bufftime(jb) * JBUF_JITTER_PERIOD;
	d = (int32_t) ( ((int64_t) tr - (int64_t) st->tr0) -
					((int64_t) ts - (int64_t) st->ts0) / 8 );

	/* Multiply timebase by JBUF_JITTER_PERIOD in order to avoid float
	 * computing. */
	/* Thus the jitter is expressed in ms multiplied by JBUF_JITTER_PERIOD. */
	da = abs(d)*JBUF_JITTER_PERIOD;
	s = da > st->jitter ? JBUF_JITTER_UP_SPEED : 1;

	djit = (da - st->jitter) * s / JBUF_JITTER_PERIOD;
	st->jitter = st->jitter + djit;
	if (st->jitter < 0)
		st->jitter = 0;

	if (!jb->ptime) {
		st->st = JS_GOOD;
		goto out;
	}

	if (st->avbuftime)
		st->avbuftime += (buftime - (int32_t) st->avbuftime) /
			JBUF_BUFTIME_PERIOD;
	else
		st->avbuftime = buftime;

	bufmin = st->jitter * JBUF_LO_BOUND / 100;
	bufmax = st->jitter * JBUF_HI_BOUND / 100;

	bufmin = MAX(bufmin, st->mintime);
	bufmax = MAX(bufmax, bufmin + 3 * st->jtime);

	if (jb->n < jb->max && st->avbuftime < bufmin) {
		st->hicnt = 0;
		st->locnt++;
		if (st->locnt > JBUF_LH_CNT) {
			st->st = JS_LOW;

			/* early adjustment */
			st->avbuftime = buftime;
		}
	} else if (jb->n > jb->min && st->avbuftime > bufmax) {
		st->hicnt++;
		st->locnt = 0;
		if (st->hicnt > JBUF_LH_CNT) {
			st->st = JS_HIGH;

			/* early adjustment */
			st->avbuftime = buftime;
		}
	} else {
		st->st = JS_GOOD;
		st->locnt = 0;
		st->hicnt = 0;
	}

#if DEBUG_LEVEL >= 6
	if (!st->tr00)
		st->tr00 = tr;

	uint32_t treal = (uint32_t) (tr - st->tr00);
	DEBUG_INFO("%s, %u, %i, %u, %u, %u, %i, %i, %u\n",
			__func__, treal, d,
			st->jitter / JBUF_JITTER_PERIOD,
			buftime / JBUF_JITTER_PERIOD, st->avbuftime / JBUF_JITTER_PERIOD,
			bufmin / JBUF_JITTER_PERIOD, bufmax / JBUF_JITTER_PERIOD,
			st->st);
#endif

out:
	st->ts0 = ts;
	st->tr0 = tr;
}


/** Checks if the number of packets present in the jitter buffer is ok
 * (JS_GOOD), should be increased (JS_LOW) or decremented (JS_HIGH).
 *
 * @param jb Jitter buffer
 *
 * @return JS_GOOD, JS_LOW, JS_HIGH.
 */
/* ------------------------------------------------------------------------- */
static enum jb_state jbuf_state(const struct jbuf *jb)
{
	const struct jitter_stat *st = &jb->jitst;
	return st->st;
}


/**
 * Put one frame into the jitter buffer
 *
 * @param jb   Jitter buffer
 * @param hdr  RTP Header
 * @param mem  Memory pointer - will be referenced
 *
 * @return 0 if success, otherwise errorcode
 */
int jbuf_put(struct jbuf *jb, const struct rtp_header *hdr, void *mem)
{
	struct frame *f;
	struct le *le, *tail;
	uint16_t seq;
	int err = 0;

	if (!jb || !hdr)
		return EINVAL;

	seq = hdr->seq;

	if (jb->ssrc && jb->ssrc != hdr->ssrc) {
		DEBUG_INFO("ssrc changed %u %u\n", jb->ssrc, hdr->ssrc);
		jbuf_flush(jb);
	}

	lock_write_get(jb->lock);
	jb->ssrc = hdr->ssrc;

	if (jb->running) {

		/* Packet arrived too late to be put into buffer */
		if (jb->seq_get && seq_less(seq, jb->seq_get + 1)) {
			STAT_INC(n_late);
			DEBUG_INFO("packet too late: seq=%u (seq_put=%u seq_get=%u)\n",
				   seq, jb->seq_put, jb->seq_get);
			err = ETIMEDOUT;
			goto out;
		}

		if (jb->silence && jb->n > jb->min && (jbuf_state(jb) == JS_HIGH)) {
			jb->jitst.st = JS_GOOD;
			DEBUG_INFO("reducing jitter buffer (jitter=%ums n=%u min=%u)\n",
					jb->jitst.jitter/JBUF_JITTER_PERIOD, jb->n, jb->min);
			goto out;
		}
	}

	STAT_INC(n_put);

	frame_alloc(jb, &f);

	tail = jb->framel.tail;

	/* If buffer is empty -> append to tail
	   Frame is later than tail -> append to tail
	*/
	if (!tail || seq_less(((struct frame *)tail->data)->hdr.seq, seq)) {
		list_append(&jb->framel, &f->le, f);
		goto success;
	}

	/* Out-of-sequence, find right position */
	for (le = tail; le; le = le->prev) {
		const uint16_t seq_le = ((struct frame *)le->data)->hdr.seq;

		if (seq_less(seq_le, seq)) { /* most likely */
			DEBUG_INFO("put: out-of-sequence"
				   " - inserting after seq=%u (seq=%u)\n",
				   seq_le, seq);
			list_insert_after(&jb->framel, le, &f->le, f);
			break;
		}
		else if (seq == seq_le) { /* less likely */
			/* Detect duplicates */
			DEBUG_INFO("duplicate: seq=%u\n", seq);
			STAT_INC(n_dups);
			list_insert_after(&jb->framel, le, &f->le, f);
			frame_deref(jb, f);
			err = EALREADY;
			goto out;
		}

		/* sequence number less than current seq, continue */
	}

	/* no earlier timestamps found, put in head */
	if (!le) {
		DEBUG_INFO("put: out-of-sequence"
			   " - put in head (seq=%u)\n", seq);
		list_prepend(&jb->framel, &f->le, f);
	}

	STAT_INC(n_oos);

 success:
	/* Update last timestamp */
	jb->running = true;
	jb->seq_put = seq;

	/* Success */
	f->hdr = *hdr;
	f->mem = mem_ref(mem);

out:
	if (jb->started)
		jbuf_jitter_calc(jb, hdr->ts);

	lock_rel(jb->lock);
	return err;
}


void jbuf_silence(struct jbuf *jb, bool on)
{
	if (!jb)
		return;

/*    DEBUG_INFO("set silence to %s\n", on ? "on" : "off");*/
	jb->silence = on;
}


/**
 * Get one frame from the jitter buffer
 *
 * @param jb   Jitter buffer
 * @param hdr  Returned RTP Header
 * @param mem  Pointer to memory object storage - referenced on success
 *
 * @return 0 if success, otherwise errorcode
 */
int jbuf_get(struct jbuf *jb, struct rtp_header *hdr, void **mem)
{
	struct frame *f;
	int err = 0;

	if (!jb || !hdr || !mem)
		return EINVAL;

	lock_write_get(jb->lock);

	if (!jb->started) {
		if (jb->n < jb->wish + 1) {
			DEBUG_INFO("not enough buffer frames - wait.. (n=%u wish=%u)\n",
					jb->n, jb->wish);
			err = ENOENT;
			goto out;
		}

		jb->started = true;
	} else if (!jb->framel.head) {
		jb->stat.n_underflow++;
		err = ENOENT;
		DEBUG_INFO("buffer underflow (%u/%u underflows)\n",
				jb->stat.n_underflow, jb->stat.n_get);
		goto out;
	}

	if (jb->silence) {
		if (jb->n < jb->max && jbuf_state(jb) == JS_LOW) {
			jb->jitst.st = JS_GOOD;
			DEBUG_INFO("inc buffer due to high jitter=%ums n=%u max=%u\n",
					jb->jitst.jitter/JBUF_JITTER_PERIOD, jb->n, jb->max);
			err = ENOENT;
			goto out;
		}
	}

	STAT_INC(n_get);

	/* When we get one frame F[i], check that the next frame F[i+1]
	   is present and have a seq no. of seq[i] + 1 !
	   if not, we should consider that packet lost */

	f = jb->framel.head->data;

#if JBUF_STAT
	/* Check timestamp of previously played frame */
	if (jb->seq_get) {
		const int16_t seq_diff = f->hdr.seq - jb->seq_get;
		if (seq_less(f->hdr.seq, jb->seq_get)) {
			DEBUG_WARNING("get: seq=%u too late\n", f->hdr.seq);
		}
		else if (seq_diff > 1) {
			STAT_ADD(n_lost, 1);
			DEBUG_INFO("get: n_lost: diff=%d,seq=%u,seq_get=%u\n",
				   seq_diff, f->hdr.seq, jb->seq_get);
		}
	}

	/* Update sequence number for 'get' */
	jb->seq_get = f->hdr.seq;
#endif

	*hdr = f->hdr;
	*mem = mem_ref(f->mem);

	frame_deref(jb, f);

out:
	lock_rel(jb->lock);
	return err;
}


/**
 * Flush all frames in the jitter buffer
 *
 * @param jb   Jitter buffer
 */
void jbuf_flush(struct jbuf *jb)
{
	struct le *le;
#if JBUF_STAT
	uint32_t n_flush;
#endif

	if (!jb)
		return;

	lock_write_get(jb->lock);
	if (jb->framel.head) {
		DEBUG_INFO("flush: %u frames\n", jb->n);
	}

	/* put all buffered frames back in free list */
	for (le = jb->framel.head; le; le = jb->framel.head) {
		DEBUG_INFO(" flush frame: seq=%u\n",
			   ((struct frame *)(le->data))->hdr.seq);

		frame_deref(jb, le->data);
	}

	jb->n       = 0;
	jb->running = false;

#if JBUF_STAT
	n_flush = STAT_INC(n_flush);
	jb->seq_get = 0;
	memset(&jb->stat, 0, sizeof(jb->stat));
	jb->stat.n_flush = n_flush;
#endif
	jbuf_init_jitst(jb);
	jb->started = false;
	lock_rel(jb->lock);
}


/**
 * Get jitter buffer statistics
 *
 * @param jb    Jitter buffer
 * @param jstat Pointer to statistics storage
 *
 * @return 0 if success, otherwise errorcode
 */
int jbuf_stats(const struct jbuf *jb, struct jbuf_stat *jstat)
{
	if (!jb || !jstat)
		return EINVAL;

#if JBUF_STAT
	*jstat = jb->stat;

	return 0;
#else
	return ENOSYS;
#endif
}


/**
 * Debug the jitter buffer
 *
 * @param pf Print handler
 * @param jb Jitter buffer
 *
 * @return 0 if success, otherwise errorcode
 */
int jbuf_debug(struct re_printf *pf, const struct jbuf *jb)
{
	int err = 0;

	if (!jb)
		return 0;

	err |= re_hprintf(pf, "--- jitter buffer debug---\n");

	err |= re_hprintf(pf, " running=%d", jb->running);
	err |= re_hprintf(pf, " min=%u cur=%u max=%u [frames]\n",
			  jb->min, jb->n, jb->max);
	err |= re_hprintf(pf, " seq_put=%u\n", jb->seq_put);

#if JBUF_STAT
	err |= re_hprintf(pf, " Stat: put=%u", jb->stat.n_put);
	err |= re_hprintf(pf, " get=%u", jb->stat.n_get);
	err |= re_hprintf(pf, " oos=%u", jb->stat.n_oos);
	err |= re_hprintf(pf, " dup=%u", jb->stat.n_dups);
	err |= re_hprintf(pf, " late=%u", jb->stat.n_late);
	err |= re_hprintf(pf, " or=%u", jb->stat.n_overflow);
	err |= re_hprintf(pf, " ur=%u", jb->stat.n_underflow);
	err |= re_hprintf(pf, " flush=%u", jb->stat.n_flush);
	err |= re_hprintf(pf, "       put/get_ratio=%u%%", jb->stat.n_get ?
			  100*jb->stat.n_put/jb->stat.n_get : 0);
	err |= re_hprintf(pf, " lost=%u (%u.%02u%%)\n",
			  jb->stat.n_lost,
			  jb->stat.n_put ?
			  100*jb->stat.n_lost/jb->stat.n_put : 0,
			  jb->stat.n_put ?
			  10000*jb->stat.n_lost/jb->stat.n_put%100 : 0);
#endif

	return err;
}

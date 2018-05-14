/* Handover Decision Algorithm 2 for intra-BSC (inter-BTS) handover, public API for OsmoBSC. */

/* (C) 2009 by Andreas Eversberg <jolly@eversberg.eu>
 * (C) 2017-2018 by sysmocom - s.f.m.c. GmbH <info@sysmocom.de>
 *
 * All Rights Reserved
 *
 * Author: Andreas Eversberg <jolly@eversberg.eu>
 *         Neels Hofmeyr <nhofmeyr@sysmocom.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <errno.h>

#include <osmocom/bsc/debug.h>
#include <osmocom/bsc/gsm_data.h>
#include <osmocom/bsc/handover.h>
#include <osmocom/bsc/handover_decision.h>
#include <osmocom/bsc/handover_decision_2.h>
#include <osmocom/bsc/handover_cfg.h>
#include <osmocom/bsc/bsc_subscriber.h>
#include <osmocom/bsc/chan_alloc.h>
#include <osmocom/bsc/signal.h>
#include <osmocom/bsc/penalty_timers.h>
#include <osmocom/bsc/neighbor_ident.h>

#define LOGPHOBTS(bts, level, fmt, args...) \
	LOGP(DHODEC, level, "(BTS %u) " fmt, bts->nr, ## args)

#define LOGPHOLCHAN(lchan, level, fmt, args...) \
	LOGP(DHODEC, level, "(lchan %u.%u%u%u %s) (subscr %s) " fmt, \
	     lchan->ts->trx->bts->nr, \
	     lchan->ts->trx->nr, \
	     lchan->ts->nr, \
	     lchan->nr, \
	     gsm_pchan_name(lchan->ts->pchan), \
	     bsc_subscr_name(lchan->conn? lchan->conn->bsub : NULL), \
	     ## args)

#define LOGPHOLCHANTOBTS(lchan, new_bts, level, fmt, args...) \
	LOGP(DHODEC, level, "(lchan %u.%u%u%u %s)->(BTS %u) (subscr %s) " fmt, \
	     lchan->ts->trx->bts->nr, \
	     lchan->ts->trx->nr, \
	     lchan->ts->nr, \
	     lchan->nr, \
	     gsm_pchan_name(lchan->ts->pchan), \
	     new_bts->nr, \
	     bsc_subscr_name(lchan->conn? lchan->conn->bsub : NULL), \
	     ## args)

#define REQUIREMENT_A_TCHF	0x01
#define REQUIREMENT_B_TCHF	0x02
#define REQUIREMENT_C_TCHF	0x04
#define REQUIREMENT_A_TCHH	0x10
#define REQUIREMENT_B_TCHH	0x20
#define REQUIREMENT_C_TCHH	0x40
#define REQUIREMENT_TCHF_MASK	(REQUIREMENT_A_TCHF | REQUIREMENT_B_TCHF | REQUIREMENT_C_TCHF)
#define REQUIREMENT_TCHH_MASK	(REQUIREMENT_A_TCHH | REQUIREMENT_B_TCHH | REQUIREMENT_C_TCHH)
#define REQUIREMENT_A_MASK	(REQUIREMENT_A_TCHF | REQUIREMENT_A_TCHH)
#define REQUIREMENT_B_MASK	(REQUIREMENT_B_TCHF | REQUIREMENT_B_TCHH)
#define REQUIREMENT_C_MASK	(REQUIREMENT_C_TCHF | REQUIREMENT_C_TCHH)

struct ho_candidate {
	struct gsm_lchan *lchan;	/* candidate for whom */
	struct gsm_bts *bts;		/* target BTS in local BSS */
	struct gsm0808_cell_id_list2 *cil; /* target cells in remote BSS */
	uint8_t requirements;		/* what is fulfilled */
	int avg;			/* average RX level */
};

enum ho_reason {
	HO_REASON_INTERFERENCE,
	HO_REASON_BAD_QUALITY,
	HO_REASON_LOW_RXLEVEL,
	HO_REASON_MAX_DISTANCE,
	HO_REASON_BETTER_CELL,
	HO_REASON_CONGESTION,
};

static const struct value_string ho_reason_names[] = {
	{ HO_REASON_INTERFERENCE,	"interference (bad quality)" },
	{ HO_REASON_BAD_QUALITY,	"bad quality" },
	{ HO_REASON_LOW_RXLEVEL,	"low rxlevel" },
	{ HO_REASON_MAX_DISTANCE,	"maximum allowed distance" },
	{ HO_REASON_BETTER_CELL,	"better cell" },
	{ HO_REASON_CONGESTION,		"congestion" },
	{0, NULL}
};

static const char *ho_reason_name(int value)
{
        return get_value_string(ho_reason_names, value);
}


static bool hodec2_initialized = false;
static enum ho_reason global_ho_reason;

static void congestion_check_cb(void *arg);

/* This function gets called on ho2 init, whenever the congestion check interval is changed, and also
 * when the timer has fired to trigger again after the next congestion check timeout. */
static void reinit_congestion_timer(struct gsm_network *net)
{
	int congestion_check_interval_s;
	bool was_active;

	/* Don't setup timers from VTY config parsing before the main program has actually initialized
	 * the data structures. */
	if (!hodec2_initialized)
		return;

	was_active = net->hodec2.congestion_check_timer.active;
	if (was_active)
		osmo_timer_del(&net->hodec2.congestion_check_timer);

	congestion_check_interval_s = net->hodec2.congestion_check_interval_s;
	if (congestion_check_interval_s < 1) {
		if (was_active)
			LOGP(DHODEC, LOGL_NOTICE, "HO algorithm 2: Disabling congestion check\n");
		return;
	}

	LOGP(DHODEC, LOGL_DEBUG, "HO algorithm 2: next periodical congestion check in %u seconds\n",
	     congestion_check_interval_s);

	osmo_timer_setup(&net->hodec2.congestion_check_timer,
			 congestion_check_cb, net);
	osmo_timer_schedule(&net->hodec2.congestion_check_timer,
			    congestion_check_interval_s, 0);
}

void hodec2_on_change_congestion_check_interval(struct gsm_network *net, unsigned int new_interval)
{
	net->hodec2.congestion_check_interval_s = new_interval;
	reinit_congestion_timer(net);
}

static void conn_penalty_time_add(struct gsm_subscriber_connection *conn, struct gsm_bts *bts,
				   int penalty_time)
{
	if (!conn->hodec2.penalty_timers) {
		conn->hodec2.penalty_timers = penalty_timers_init(conn);
		OSMO_ASSERT(conn->hodec2.penalty_timers);
	}
	penalty_timers_add(conn->hodec2.penalty_timers, bts, penalty_time);
}

static unsigned int conn_penalty_time_remaining(struct gsm_subscriber_connection *conn,
						struct gsm_bts *bts)
{
	if (!conn->hodec2.penalty_timers)
		return 0;
	return penalty_timers_remaining(conn->hodec2.penalty_timers, bts);
}

/* did we get a RXLEV for a given cell in the given report? Mark matches as MRC_F_PROCESSED. */
static struct gsm_meas_rep_cell *cell_in_rep(struct gsm_meas_rep *mr, uint16_t arfcn, uint8_t bsic)
{
	int i;

	for (i = 0; i < mr->num_cell; i++) {
		struct gsm_meas_rep_cell *mrc = &mr->cell[i];

		if (mrc->arfcn != arfcn)
			continue;
		if (mrc->bsic != bsic)
			continue;

		return mrc;
	}
	return NULL;
}

/* obtain averaged rxlev for given neighbor */
static int neigh_meas_avg(struct neigh_meas_proc *nmp, int window)
{
	unsigned int i, idx;
	int avg = 0;

	/* reduce window to the actual number of existing measurements */
	if (window > nmp->rxlev_cnt)
		window = nmp->rxlev_cnt;
	/* this should never happen */
	if (window <= 0)
		return 0;

	idx = calc_initial_idx(ARRAY_SIZE(nmp->rxlev),
			       nmp->rxlev_cnt % ARRAY_SIZE(nmp->rxlev),
			       window);

	for (i = 0; i < window; i++) {
		int j = (idx+i) % ARRAY_SIZE(nmp->rxlev);

		avg += nmp->rxlev[j];
	}

	return avg / window;
}

/* Find empty slot or the worst neighbor. */
static struct neigh_meas_proc *find_unused_or_worst_neigh(struct gsm_lchan *lchan)
{
	struct neigh_meas_proc *nmp_worst = NULL;
	int worst;
	int j;

	/* First try to find an empty/unused slot. */
	for (j = 0; j < ARRAY_SIZE(lchan->neigh_meas); j++) {
		struct neigh_meas_proc *nmp = &lchan->neigh_meas[j];
		if (!nmp->arfcn)
			return nmp;
	}

	/* No empty slot found. Return worst neighbor to be evicted. */
	worst = 0; /* (overwritten on first loop, but avoid compiler warning) */
	for (j = 0; j < ARRAY_SIZE(lchan->neigh_meas); j++) {
		struct neigh_meas_proc *nmp = &lchan->neigh_meas[j];
		int avg = neigh_meas_avg(nmp, MAX_WIN_NEIGH_AVG);
		if (nmp_worst && avg >= worst)
			continue;
		worst = avg;
		nmp_worst = nmp;
	}

	return nmp_worst;
}

/* process neighbor cell measurement reports */
static void process_meas_neigh(struct gsm_meas_rep *mr)
{
	int i, j, idx;

	/* For each reported cell, try to update measurements we already have from previous reports. */
	for (j = 0; j < ARRAY_SIZE(mr->lchan->neigh_meas); j++) {
		struct neigh_meas_proc *nmp = &mr->lchan->neigh_meas[j];
		unsigned int idx;
		struct gsm_meas_rep_cell *mrc;

		/* skip unused entries */
		if (!nmp->arfcn)
			continue;

		mrc = cell_in_rep(mr, nmp->arfcn, nmp->bsic);
		idx = nmp->rxlev_cnt % ARRAY_SIZE(nmp->rxlev);
		if (mrc) {
			nmp->rxlev[idx] = mrc->rxlev;
			nmp->last_seen_nr = mr->nr;
			LOGPHOLCHAN(mr->lchan, LOGL_DEBUG, "neigh %u rxlev=%d last_seen_nr=%u\n",
				    nmp->arfcn, mrc->rxlev, nmp->last_seen_nr);
			mrc->flags |= MRC_F_PROCESSED;
		} else {
			nmp->rxlev[idx] = 0;
			LOGPHOLCHAN(mr->lchan, LOGL_DEBUG, "neigh %u not in report (last_seen_nr=%u)\n",
				    nmp->arfcn, nmp->last_seen_nr);
		}
		nmp->rxlev_cnt++;
	}

	/* Add cells that we don't know about yet, if necessary overwriting previous records that reflect
	 * cells with worse receive levels */
	for (i = 0; i < mr->num_cell; i++) {
		struct gsm_meas_rep_cell *mrc = &mr->cell[i];
		struct neigh_meas_proc *nmp;

		if (mrc->flags & MRC_F_PROCESSED)
			continue;

		nmp = find_unused_or_worst_neigh(mr->lchan);

		nmp->arfcn = mrc->arfcn;
		nmp->bsic = mrc->bsic;

		nmp->rxlev_cnt = 0;
		idx = nmp->rxlev_cnt % ARRAY_SIZE(nmp->rxlev);
		nmp->rxlev[idx] = mrc->rxlev;
		nmp->rxlev_cnt++;
		nmp->last_seen_nr = mr->nr;
		LOGPHOLCHAN(mr->lchan, LOGL_DEBUG, "neigh %u new in report rxlev=%d last_seen_nr=%u\n",
			    nmp->arfcn, mrc->rxlev, nmp->last_seen_nr);

		mrc->flags |= MRC_F_PROCESSED;
	}
}

static bool codec_type_is_supported(struct gsm_subscriber_connection *conn,
				    enum gsm0808_speech_codec_type type)
{
	int i;
	struct gsm0808_speech_codec_list *clist = &conn->codec_list;

	if (!conn->codec_list_present) {
		/* We don't have a list of supported codecs. This should never happen. */
		LOGPHOLCHAN(conn->lchan, LOGL_ERROR,
			    "No Speech Codec List present, accepting all codecs\n");
		return true;
	}

	for (i = 0; i < clist->len; i++) {
		if (clist->codec[i].type == type) {
			LOGPHOLCHAN(conn->lchan, LOGL_DEBUG, "%s supported\n",
				    gsm0808_speech_codec_type_name(type));
			return true;
		}
	}
	LOGPHOLCHAN(conn->lchan, LOGL_DEBUG, "Codec not supported by MS or not allowed by MSC: %s\n",
		    gsm0808_speech_codec_type_name(type));
	return false;
}

/*
 * Check what requirements the given cell fulfills.
 * A bit mask of fulfilled requirements is returned.
 *
 * Target cell requirement A -- ability to service the call
 *
 * In order to successfully handover/assign to a better cell, the target cell
 * must be able to continue the current call. Therefore the cell must fulfill
 * the following criteria:
 *
 *  * The handover must be enabled for the target cell, if it differs from the
 *    originating cell.
 *  * The assignment must be enabled for the cell, if it equals the current
 *    cell.
 *  * The handover penalty timer must not run for the cell.
 *  * If FR, EFR or HR codec is used, the cell must support this codec.
 *  * If FR or EFR codec is used, the cell must have a TCH/F slot type
 *    available.
 *  * If HR codec is used, the cell must have a TCH/H slot type available.
 *  * If AMR codec is used, the cell must have a TCH/F slot available, if AFS
 *    is supported by mobile and BTS.
 *  * If AMR codec is used, the cell must have a TCH/H slot available, if AHS
 *    is supported by mobile and BTS.
 *  * osmo-nitb with built-in MNCC application:
 *     o If AMR codec is used, the cell must support AMR codec with equal codec
 *       rate or rates. (not meaning TCH types)
 *  * If defined, the number of maximum unsynchronized handovers to this cell
 *    may not be exceeded. (This limits processing load for random access
 *    bursts.)
 *
 *
 * Target cell requirement B -- avoid congestion
 *
 * In order to prevent congestion of a target cell, the cell must fulfill the
 * requirement A, but also:
 *
 *  * The minimum free channels, that are defined for that cell must be
 *    maintained after handover/assignment.
 *  * The minimum free channels are defined for TCH/F and TCH/H slot types
 *    individually.
 *
 *
 * Target cell requirement C -- balance congestion
 *
 * In order to balance congested cells, the target cell must fulfill the
 * requirement A, but also:
 *
 *  * The target cell (which is congested also) must have more or equal free
 *    slots after handover/assignment.
 *  * The number of free slots are checked for TCH/F and TCH/H slot types
 *    individually.
 */
static uint8_t check_requirements(struct gsm_lchan *lchan, struct gsm_bts *bts, int tchf_count, int tchh_count)
{
	int count;
	uint8_t requirement = 0;
	unsigned int penalty_time;
	struct gsm_bts *current_bts = lchan->ts->trx->bts;

	/* Requirement A */

	/* the handover/assignment must not be disabled */
	if (current_bts == bts) {
		if (!ho_get_hodec2_as_active(bts->ho)) {
			LOGPHOLCHAN(lchan, LOGL_DEBUG, "Assignment disabled\n");
			return 0;
		}
	} else {
		if (!ho_get_ho_active(bts->ho)) {
			LOGPHOLCHANTOBTS(lchan, bts, LOGL_DEBUG,
					 "not a candidate, handover is disabled in target BTS\n");
			return 0;
		}
	}

	/* the handover penalty timer must not run for this bts */
	penalty_time = conn_penalty_time_remaining(lchan->conn, bts);
	if (penalty_time) {
		LOGPHOLCHANTOBTS(lchan, bts, LOGL_DEBUG, "not a candidate, target BTS still in penalty time"
				 " (%u seconds left)\n", penalty_time);
		return 0;
	}

	LOGPHOLCHANTOBTS(lchan, bts, LOGL_DEBUG, "tch_mode='%s' type='%s'\n",
			 get_value_string(gsm48_chan_mode_names, lchan->tch_mode),
			 gsm_lchant_name(lchan->type));

	/* compatibility check for codecs.
	 * if so, the candidates for full rate and half rate are selected */
	switch (lchan->tch_mode) {
	case GSM48_CMODE_SPEECH_V1:
		switch (lchan->type) {
		case GSM_LCHAN_TCH_F: /* mandatory */
			requirement |= REQUIREMENT_A_TCHF;
			LOGPHOLCHANTOBTS(lchan, bts, LOGL_DEBUG, "tch_mode='%s' type='%s' supported\n",
					 get_value_string(gsm48_chan_mode_names, lchan->tch_mode),
					 gsm_lchant_name(lchan->type));
			break;
		case GSM_LCHAN_TCH_H:
			if (!bts->codec.hr) {
				LOGPHOLCHANTOBTS(lchan, bts, LOGL_DEBUG,
						 "tch_mode='%s' type='%s' not supported\n",
						 get_value_string(gsm48_chan_mode_names,
								  lchan->tch_mode),
						 gsm_lchant_name(lchan->type));
				break;
			}
			if (codec_type_is_supported(lchan->conn, GSM0808_SCT_HR1))
				requirement |= REQUIREMENT_A_TCHH;
			break;
		default:
			LOGPHOLCHAN(lchan, LOGL_ERROR, "Unexpected channel type: neither TCH/F nor TCH/H for %s\n",
				    get_value_string(gsm48_chan_mode_names, lchan->tch_mode));
			return 0;
		}
		break;
	case GSM48_CMODE_SPEECH_EFR:
		if (!bts->codec.efr) {
			LOGPHOBTS(bts, LOGL_DEBUG, "EFR not supported\n");
			break;
		}
		if (codec_type_is_supported(lchan->conn, GSM0808_SCT_FR2))
			requirement |= REQUIREMENT_A_TCHF;
		break;
	case GSM48_CMODE_SPEECH_AMR:
		if (!bts->codec.amr) {
			LOGPHOBTS(bts, LOGL_DEBUG, "AMR not supported\n");
			break;
		}
		if (codec_type_is_supported(lchan->conn, GSM0808_SCT_FR3))
			requirement |= REQUIREMENT_A_TCHF;
		if (codec_type_is_supported(lchan->conn, GSM0808_SCT_HR3))
			requirement |= REQUIREMENT_A_TCHH;
		break;
	default:
		LOGPHOLCHANTOBTS(lchan, bts, LOGL_DEBUG, "Not even considering: src is not a SPEECH mode lchan\n");
		return 0;
	}

	/* no candidate, because new cell is incompatible */
	if (!requirement) {
		LOGPHOLCHANTOBTS(lchan, bts, LOGL_DEBUG, "not a candidate, because codec of MS and BTS are incompatible\n");
		return 0;
	}

	/* remove slot types that are not available */
	if (!tchf_count && requirement & REQUIREMENT_A_TCHF) {
		LOGPHOLCHANTOBTS(lchan, bts, LOGL_DEBUG,
				 "removing TCH/F, since all TCH/F lchans are in use\n");
		requirement &= ~(REQUIREMENT_A_TCHF);
	}
	if (!tchh_count && requirement & REQUIREMENT_A_TCHH) {
		LOGPHOLCHANTOBTS(lchan, bts, LOGL_DEBUG,
				 "removing TCH/H, since all TCH/H lchans are in use\n");
		requirement &= ~(REQUIREMENT_A_TCHH);
	}

	if (!requirement) {
		LOGPHOLCHANTOBTS(lchan, bts, LOGL_DEBUG, "not a candidate, because no suitable slots available\n");
		return 0;
	}

	/* omit same channel type on same BTS (will not change anything) */
	if (bts == current_bts) {
		switch (lchan->type) {
		case GSM_LCHAN_TCH_F:
			LOGPHOLCHANTOBTS(lchan, bts, LOGL_DEBUG,
					 "removing TCH/F, already on TCH/F in this cell\n");
			requirement &= ~(REQUIREMENT_A_TCHF);
			break;
		case GSM_LCHAN_TCH_H:
			LOGPHOLCHANTOBTS(lchan, bts, LOGL_DEBUG,
					 "removing TCH/H, already on TCH/H in this cell\n");
			requirement &= ~(REQUIREMENT_A_TCHH);
			break;
		default:
			break;
		}

		if (!requirement) {
			LOGPHOLCHAN(lchan, LOGL_DEBUG,
				    "Reassignment within cell not an option, no differing channel types available\n");
			return 0;
		}
	}

#ifdef LEGACY
	// This was useful in osmo-nitb. We're in osmo-bsc now and have no idea whether the osmo-msc does
	// internal or external call control. Maybe a future config switch wants to add this behavior?
	/* Built-in call control requires equal codec rates. Remove rates that are not equal. */
	if (lchan->tch_mode == GSM48_CMODE_SPEECH_AMR
	    && current_bts->network->mncc_recv != mncc_sock_from_cc) {
		switch (lchan->type) {
		case GSM_LCHAN_TCH_F:
			if ((requirement & REQUIREMENT_A_TCHF)
			    && !!memcmp(&current_bts->mr_full, &bts->mr_full,
					sizeof(struct amr_multirate_conf)))
				requirement &= ~(REQUIREMENT_A_TCHF);
			if ((requirement & REQUIREMENT_A_TCHH)
			    && !!memcmp(&current_bts->mr_full, &bts->mr_half,
					sizeof(struct amr_multirate_conf)))
				requirement &= ~(REQUIREMENT_A_TCHH);
			break;
		case GSM_LCHAN_TCH_H:
			if ((requirement & REQUIREMENT_A_TCHF)
			    && !!memcmp(&current_bts->mr_half, &bts->mr_full,
					sizeof(struct amr_multirate_conf)))
				requirement &= ~(REQUIREMENT_A_TCHF);
			if ((requirement & REQUIREMENT_A_TCHH)
			    && !!memcmp(&current_bts->mr_half, &bts->mr_half,
					sizeof(struct amr_multirate_conf)))
				requirement &= ~(REQUIREMENT_A_TCHH);
			break;
		default:
			break;
		}

		if (!requirement) {
			LOGPHOLCHANTOBTS(lchan, bts, LOGL_DEBUG,
					 "not a candidate, cannot provide identical codec rate\n");
			return 0;
		}
	}
#endif

	/* the maximum number of unsynchonized handovers must no be exceeded */
	if (current_bts != bts
	    && bsc_ho_count(bts, true) >= ho_get_hodec2_ho_max(bts->ho)) {
		LOGPHOLCHANTOBTS(lchan, bts, LOGL_DEBUG,
				 "not a candidate, number of allowed handovers (%d) would be exceeded\n",
				 ho_get_hodec2_ho_max(bts->ho));
		return 0;
	}

	/* Requirement B */

	/* the minimum free timeslots that are defined for this cell must
	 * be maintained _after_ handover/assignment */
	if (requirement & REQUIREMENT_A_TCHF) {
		if (tchf_count - 1 >= ho_get_hodec2_tchf_min_slots(bts->ho)) {
			LOGPHOLCHANTOBTS(lchan, bts, LOGL_DEBUG,
					 "TCH/F would not be congested after HO\n");
			requirement |= REQUIREMENT_B_TCHF;
		} else {
			LOGPHOLCHANTOBTS(lchan, bts, LOGL_DEBUG,
					 "TCH/F would be congested after HO\n");
		}
	}
	if (requirement & REQUIREMENT_A_TCHH) {
		if (tchh_count - 1 >= ho_get_hodec2_tchh_min_slots(bts->ho)) {
			LOGPHOLCHANTOBTS(lchan, bts, LOGL_DEBUG,
					 "TCH/H would not be congested after HO\n");
			requirement |= REQUIREMENT_B_TCHH;
		} else {
			LOGPHOLCHANTOBTS(lchan, bts, LOGL_DEBUG,
					 "TCH/H would be congested after HO\n");
		}
	}

	/* Requirement C */

	/* the nr of free timeslots of the target cell must be >= the
	 * free slots of the current cell _after_ handover/assignment */
	count = bts_count_free_ts(current_bts,
				  (lchan->type == GSM_LCHAN_TCH_H) ?
				  	GSM_PCHAN_TCH_H : GSM_PCHAN_TCH_F);
	if (requirement & REQUIREMENT_A_TCHF) {
		if (tchf_count - 1 >= count + 1) {
			LOGPHOLCHANTOBTS(lchan, bts, LOGL_DEBUG,
					 "TCH/F would be less congested in target than source cell after HO\n");
			requirement |= REQUIREMENT_C_TCHF;
		} else {
			LOGPHOLCHANTOBTS(lchan, bts, LOGL_DEBUG,
					 "TCH/F would not be less congested in target than source cell after HO\n");
		}
	}
	if (requirement & REQUIREMENT_A_TCHH) {
		if (tchh_count - 1 >= count + 1) {
			LOGPHOLCHANTOBTS(lchan, bts, LOGL_DEBUG,
					 "TCH/H would be less congested in target than source cell after HO\n");
			requirement |= REQUIREMENT_C_TCHH;
		} else {
			LOGPHOLCHANTOBTS(lchan, bts, LOGL_DEBUG,
					 "TCH/H would not be less congested in target than source cell after HO\n");
		}
	}

	LOGPHOLCHANTOBTS(lchan, bts, LOGL_DEBUG, "requirements=0x%x\n", requirement);

	/* return mask of fulfilled requirements */
	return requirement;
}

/* Trigger handover or assignment depending on the target BTS */
static int trigger_handover_or_assignment(struct gsm_lchan *lchan, struct gsm_bts *new_bts, uint8_t requirements)
{
	struct gsm_bts *current_bts = lchan->ts->trx->bts;
	int afs_bias = 0;
	bool full_rate = false;

	if (current_bts == new_bts)
		LOGPHOLCHAN(lchan, LOGL_NOTICE, "Triggering Assignment\n");
	else
		LOGPHOLCHANTOBTS(lchan, new_bts, LOGL_NOTICE, "Triggering Handover\n");

	/* afs_bias becomes > 0, if AFS is used and is improved */
	if (lchan->tch_mode == GSM48_CMODE_SPEECH_AMR)
		afs_bias = ho_get_hodec2_afs_bias_rxlev(new_bts->ho);

	/* select TCH rate, prefer TCH/F if AFS is improved */
	switch (lchan->type) {
	case GSM_LCHAN_TCH_F:
		/* keep on full rate, if TCH/F is a candidate */
		if ((requirements & REQUIREMENT_TCHF_MASK)) {
			if (current_bts == new_bts) {
				LOGPHOLCHAN(lchan, LOGL_INFO, "Not performing assignment: Already on target type\n");
				return 0;
			}
			full_rate = true;
			break;
		}
		/* change to half rate */
		if (!(requirements & REQUIREMENT_TCHH_MASK)) {
			LOGPHOLCHANTOBTS(lchan, new_bts, LOGL_ERROR,
					 "neither TCH/F nor TCH/H requested, aborting ho/as\n");
			return -EINVAL;
		}
		break;
	case GSM_LCHAN_TCH_H:
		/* change to full rate if AFS is improved and a candidate */
		if (afs_bias > 0 && (requirements & REQUIREMENT_TCHF_MASK)) {
			full_rate = true;
			LOGPHOLCHAN(lchan, LOGL_DEBUG, "[Improve AHS->AFS]\n");
			break;
		}
		/* change to full rate if the only candidate */
		if ((requirements & REQUIREMENT_TCHF_MASK)
		    && !(requirements & REQUIREMENT_TCHH_MASK)) {
			full_rate = true;
			break;
		}
		/* keep on half rate */
		if (!(requirements & REQUIREMENT_TCHH_MASK)) {
			LOGPHOLCHANTOBTS(lchan, new_bts, LOGL_ERROR,
					 "neither TCH/F nor TCH/H requested, aborting ho/as\n");
			return -EINVAL;
		}
		if (current_bts == new_bts) {
			LOGPHOLCHAN(lchan, LOGL_INFO, "Not performing assignment: Already on target type\n");
			return 0;
		}
		break;
	default:
		LOGPHOLCHANTOBTS(lchan, new_bts, LOGL_ERROR, "lchan is neither TCH/F nor TCH/H, aborting ho/as\n");
		return -EINVAL;
	}

	/* trigger handover or assignment */
	if  (current_bts == new_bts)
		LOGPHOLCHAN(lchan, LOGL_NOTICE, "Triggering assignment to %s, due to %s\n",
			    full_rate ? "TCH/F" : "TCH/H",
			    ho_reason_name(global_ho_reason));
	else
		LOGPHOLCHANTOBTS(lchan, new_bts, LOGL_NOTICE,
				 "Triggering handover to %s, due to %s\n",
				 full_rate ? "TCH/F" : "TCH/H",
				 ho_reason_name(global_ho_reason));

	return handover_to_neighbor_ident(HODEC2, lchan, bts_ident_key(new_bts),
					  full_rate? GSM_LCHAN_TCH_F : GSM_LCHAN_TCH_H);
}

/* debug collected candidates */
static inline void debug_candidate(struct ho_candidate *candidate,
	int neighbor, int8_t rxlev, int tchf_count, int tchh_count)
{
	if (neighbor)
		LOGP(DHODEC, LOGL_DEBUG, " - neighbor BTS %d, RX level "
			"%d -> %d\n", candidate->bts->nr, rxlev2dbm(rxlev),
			rxlev2dbm(candidate->avg));
	else
		LOGP(DHODEC, LOGL_DEBUG, " - current BTS %d, RX level %d\n",
			candidate->bts->nr, rxlev2dbm(candidate->avg));

	LOGP(DHODEC, LOGL_DEBUG, "   o free TCH/F slots %d, minimum required "
		"%d\n", tchf_count, ho_get_hodec2_tchf_min_slots(candidate->bts->ho));
	LOGP(DHODEC, LOGL_DEBUG, "   o free TCH/H slots %d, minimum required "
		"%d\n", tchh_count, ho_get_hodec2_tchh_min_slots(candidate->bts->ho));

	if ((candidate->requirements & REQUIREMENT_TCHF_MASK))
		LOGP(DHODEC, LOGL_DEBUG, "   o requirement ");
	else
		LOGP(DHODEC, LOGL_DEBUG, "   o no requirement ");
	if ((candidate->requirements & REQUIREMENT_A_TCHF))
		LOGPC(DHODEC, LOGL_DEBUG, "A ");
	if ((candidate->requirements & REQUIREMENT_B_TCHF))
		LOGPC(DHODEC, LOGL_DEBUG, "B ");
	if ((candidate->requirements & REQUIREMENT_C_TCHF))
		LOGPC(DHODEC, LOGL_DEBUG, "C ");
	LOGPC(DHODEC, LOGL_DEBUG, "fulfilled for TCHF");
	if (!(candidate->requirements & REQUIREMENT_TCHF_MASK)) /* nothing */
		LOGPC(DHODEC, LOGL_DEBUG, " (no %s possible)\n",
			(neighbor) ? "handover" : "assignment");
	else if ((candidate->requirements & REQUIREMENT_TCHF_MASK)
					== REQUIREMENT_A_TCHF) /* only A */
		LOGPC(DHODEC, LOGL_DEBUG, " (more congestion after %s)\n",
			(neighbor) ? "handover" : "assignment");
	else if ((candidate->requirements & REQUIREMENT_B_TCHF)) /* B incl. */
		LOGPC(DHODEC, LOGL_DEBUG, " (not congested after %s)\n",
			(neighbor) ? "handover" : "assignment");
	else /* so it must include C */
		LOGPC(DHODEC, LOGL_DEBUG, " (less or equally congested after "
			"%s)\n", (neighbor) ? "handover" : "assignment");

	if ((candidate->requirements & REQUIREMENT_TCHH_MASK))
		LOGP(DHODEC, LOGL_DEBUG, "   o requirement ");
	else
		LOGP(DHODEC, LOGL_DEBUG, "   o no requirement ");
	if ((candidate->requirements & REQUIREMENT_A_TCHH))
		LOGPC(DHODEC, LOGL_DEBUG, "A ");
	if ((candidate->requirements & REQUIREMENT_B_TCHH))
		LOGPC(DHODEC, LOGL_DEBUG, "B ");
	if ((candidate->requirements & REQUIREMENT_C_TCHH))
		LOGPC(DHODEC, LOGL_DEBUG, "C ");
	LOGPC(DHODEC, LOGL_DEBUG, "fulfilled for TCHH");
	if (!(candidate->requirements & REQUIREMENT_TCHH_MASK)) /* nothing */
		LOGPC(DHODEC, LOGL_DEBUG, " (no %s possible)\n",
			(neighbor) ? "handover" : "assignment");
	else if ((candidate->requirements & REQUIREMENT_TCHH_MASK)
					== REQUIREMENT_A_TCHH) /* only A */
		LOGPC(DHODEC, LOGL_DEBUG, " (more congestion after %s)\n",
			(neighbor) ? "handover" : "assignment");
	else if ((candidate->requirements & REQUIREMENT_B_TCHH)) /* B incl. */
		LOGPC(DHODEC, LOGL_DEBUG, " (not congested after %s)\n",
			(neighbor) ? "handover" : "assignment");
	else /* so it must include C */
		LOGPC(DHODEC, LOGL_DEBUG, " (less or equally congested after "
			"%s)\n", (neighbor) ? "handover" : "assignment");
}

/* add candidate for re-assignment within the current cell */
static void collect_assignment_candidate(struct gsm_lchan *lchan, struct ho_candidate *clist,
					 unsigned int *candidates, int av_rxlev)
{
	struct gsm_bts *bts = lchan->ts->trx->bts;
	int tchf_count, tchh_count;
	struct ho_candidate *c;

	tchf_count = bts_count_free_ts(bts, GSM_PCHAN_TCH_F);
	tchh_count = bts_count_free_ts(bts, GSM_PCHAN_TCH_H);

	c = &clist[*candidates];
	c->lchan = lchan;
	c->bts = bts;
	c->requirements = check_requirements(lchan, bts, tchf_count, tchh_count);
	c->avg = av_rxlev;
	debug_candidate(c, 0, 0, tchf_count, tchh_count);
	(*candidates)++;
}

/* add candidates for handover to all neighbor cells */
static void collect_handover_candidate(struct gsm_lchan *lchan, struct neigh_meas_proc *nmp,
				       struct ho_candidate *clist, unsigned int *candidates,
				       bool include_weaker_rxlev, int av_rxlev,
				       int *neighbors_count)
{
	struct gsm_bts *bts = lchan->ts->trx->bts;
	int tchf_count, tchh_count;
	struct gsm_bts *neighbor_bts;
	struct gsm0808_cell_id_list2 *neighbor_cil;
	struct neighbor_ident_key ni = {
		.arfcn = nmp->arfcn,
		.bsic_kind = BSIC_6BIT,
		.bsic = nmp->bsic,
	};
	int avg;
	struct ho_candidate *c;
	int min_rxlev;

	/* skip empty slots */
	if (nmp->arfcn == 0)
		return;

	if (neighbors_count)
		(*neighbors_count)++;

	/* skip if measurement report is old */
	if (nmp->last_seen_nr != lchan->meas_rep_last_seen_nr) {
		LOGPHOLCHAN(lchan, LOGL_DEBUG, "neighbor ARFCN %u BSIC %u measurement report is old"
			    " (nmp->last_seen_nr=%u lchan->meas_rep_last_seen_nr=%u)\n",
			    nmp->arfcn, nmp->bsic, nmp->last_seen_nr, lchan->meas_rep_last_seen_nr);
		return;
	}

	neighbor_bts = bts_by_neighbor_ident(bts->network, &ni);
	if (!neighbor_bts) {
		neighbor_cil = neighbor_ident_get(bts->network->neighbor_bss_cells, &ni);
		if (neighbor_cil) {
			LOGPHOBTS(bts, LOGL_DEBUG, "neighbor ARFCN %u BSIC %u does not belong to this BSS\n",
				  nmp->arfcn, nmp->bsic);
			LOGPHOBTS(bts, LOGL_ERROR, "neighbor ARFCN %u BSIC %u does not belong to this BSS,"
				  " would handover to neighbor BSS but"
				  " inter-BSC handover for handover algorithm 2 not implemented!\n",
				  nmp->arfcn, nmp->bsic);
			/* FIXME */
			return;
		}

		LOGPHOBTS(bts, LOGL_DEBUG, "neighbor ARFCN %u BSIC %u does not belong to this network\n",
			  nmp->arfcn, nmp->bsic);
		return;
	}

	/* in case we have measurements of our bts, due to misconfiguration */
	if (neighbor_bts == bts) {
		LOGPHOBTS(bts, LOGL_ERROR, "Configuration error: this BTS appears as its own neighbor\n");
		return;
	}

	/* calculate average rxlev for this cell over the window */
	avg = neigh_meas_avg(nmp, ho_get_hodec2_rxlev_neigh_avg_win(bts->ho));

	/* Heed rxlev hysteresis only if the RXLEV/RXQUAL/TA levels of the MS aren't critically bad and
	 * we're just looking for an improvement. If levels are critical, we desperately need a handover
	 * and thus skip the hysteresis check. */
	if (!include_weaker_rxlev) {
		unsigned int pwr_hyst = ho_get_hodec2_pwr_hysteresis(bts->ho);
		if (avg <= (av_rxlev + pwr_hyst)) {
			LOGPHOLCHAN(lchan, LOGL_DEBUG,
				    "BTS %d is not a candidate, because RX level (%d) is lower"
				    " or equal than current RX level (%d) + hysteresis (%d)\n",
				    neighbor_bts->nr, rxlev2dbm(avg), rxlev2dbm(av_rxlev), pwr_hyst);
			return;
		}
	}

	/* if the minimum level is not reached */
	min_rxlev = ho_get_hodec2_min_rxlev(neighbor_bts->ho);
	if (rxlev2dbm(avg) < min_rxlev) {
		LOGPHOLCHAN(lchan, LOGL_DEBUG,
			    "BTS %d is not a candidate, because RX level (%d) is lower"
			    " than its minimum required RX level (%d)\n",
			    neighbor_bts->nr, rxlev2dbm(avg), min_rxlev);
		return;
	}

	tchf_count = bts_count_free_ts(neighbor_bts, GSM_PCHAN_TCH_F);
	tchh_count = bts_count_free_ts(neighbor_bts, GSM_PCHAN_TCH_H);
	c = &clist[*candidates];
	c->lchan = lchan;
	c->bts = neighbor_bts;
	c->requirements = check_requirements(lchan, neighbor_bts, tchf_count,
					     tchh_count);
	c->avg = avg;
	debug_candidate(c, 1, av_rxlev, tchf_count, tchh_count);
	(*candidates)++;
}

static void collect_candidates_for_lchan(struct gsm_lchan *lchan,
					 struct ho_candidate *clist, unsigned int *candidates,
					 int *_av_rxlev, bool include_weaker_rxlev)
{
	struct gsm_bts *bts = lchan->ts->trx->bts;
	int av_rxlev;
	unsigned int candidates_was;
	bool assignment;
	bool handover;
	int neighbors_count = 0;
	unsigned int rxlev_avg_win = ho_get_hodec2_rxlev_avg_win(bts->ho);

	OSMO_ASSERT(candidates);
	candidates_was = *candidates;

	/* caculate average rxlev for this cell over the window */
	av_rxlev = get_meas_rep_avg(lchan,
				    ho_get_hodec2_full_tdma(bts->ho) ?
				    MEAS_REP_DL_RXLEV_FULL : MEAS_REP_DL_RXLEV_SUB,
				    rxlev_avg_win);
	if (_av_rxlev)
		*_av_rxlev = av_rxlev;

	/* in case there is no measurment report (yet) */
	if (av_rxlev < 0) {
		LOGPHOLCHAN(lchan, LOGL_DEBUG, "Not collecting candidates, not enough measurements"
			    " (got %d, want %u)\n",
			    lchan->meas_rep_count, rxlev_avg_win);
		return;
	}

	assignment = ho_get_hodec2_as_active(bts->ho);
	handover = ho_get_ho_active(bts->ho);

	LOGPHOLCHAN(lchan, LOGL_DEBUG, "Collecting candidates for%s%s%s\n",
		    assignment ? " Assignment" : "",
		    assignment && handover ? " and" : "",
		    handover ? " Handover" : "");

	if (assignment)
		collect_assignment_candidate(lchan, clist, candidates, av_rxlev);

	if (handover) {
		int i;
		for (i = 0; i < ARRAY_SIZE(lchan->neigh_meas); i++) {
			collect_handover_candidate(lchan, &lchan->neigh_meas[i],
						   clist, candidates,
						   include_weaker_rxlev, av_rxlev, &neighbors_count);
		}
	}

	LOGPHOLCHAN(lchan, LOGL_DEBUG, "adding %u candidates from %u neighbors, total %u\n",
		    *candidates - candidates_was, neighbors_count, *candidates);
}

/*
 * Search for a alternative / better cell.
 *
 * Do not trigger handover/assignment on slots which have already ongoing
 * handover/assignment processes. If no AFS improvement offset is given, try to
 * maintain the same TCH rate, if available.
 * Do not perform this process, if handover and assignment are disabled for
 * the current cell.
 * Do not perform handover, if the minimum acceptable RX level
 * is not reched for this cell.
 *
 * If one or more 'better cells' are available, check the current and neighbor
 * cell measurements in descending order of their RX levels (down-link):
 *
 *  * Select the best candidate that fulfills requirement B (no congestion
 *    after handover/assignment) and trigger handover or assignment.
 *  * If no candidate fulfills requirement B, select the best candidate that
 *    fulfills requirement C (less or equally congested cells after handover)
 *    and trigger handover or assignment.
 *  * If no candidate fulfills requirement C, do not perform handover nor
 *    assignment.
 *
 * If the RX level (down-link) or RX quality (down-link) of the current cell is
 * below minimum acceptable level, or if the maximum allowed timing advance is
 * reached or exceeded, check the RX levels (down-link) of the current and
 * neighbor cells in descending order of their levels: (bad BTS case)
 *
 *  * Select the best candidate that fulfills requirement B (no congestion after
 *    handover/assignment) and trigger handover or assignment.
 *  * If no candidate fulfills requirement B, select the best candidate that
 *    fulfills requirement C (less or equally congested cells after handover)
 *    and trigger handover or assignment.
 *  * If no candidate fulfills requirement C, select the best candidate that
 *    fulfills requirement A (ignore congestion after handover or assignment)
 *    and trigger handover or assignment.
 *  * If no candidate fulfills requirement A, do not perform handover nor
 *    assignment.
 *
 * RX levels (down-link) of current and neighbor cells:
 *
 *  * The RX levels of the current cell and neighbor cells are improved by a
 *    given offset, if AFS (AMR on TCH/F) is used or is a candidate for
 *    handover/assignment.
 *  * If AMR is used, the requirement for handover is checked for TCH/F and
 *    TCH/H. Both results (if any) are used as a candidate.
 *  * If AMR is used, the requirement for assignment to a different TCH slot
 *    rate is checked. The result (if available) is used as a candidate.
 *
 * If minimum RXLEV, minimum RXQUAL or maximum TA are exceeded, the caller should pass
 * include_weaker_rxlev=true so that handover is performed despite congestion.
 */
static int find_alternative_lchan(struct gsm_lchan *lchan, bool include_weaker_rxlev)
{
	struct gsm_bts *bts = lchan->ts->trx->bts;
	int ahs = (lchan->tch_mode == GSM48_CMODE_SPEECH_AMR
		   && lchan->type == GSM_LCHAN_TCH_H);
	int av_rxlev;
	struct ho_candidate clist[1 + ARRAY_SIZE(lchan->neigh_meas)];
	unsigned int candidates = 0;
	int i;
	struct ho_candidate *best_cand = NULL;
	unsigned int best_better_db;
	bool best_applied_afs_bias = false;
	int better;

	/* check for disabled handover/assignment at the current cell */
	if (!ho_get_hodec2_as_active(bts->ho)
	    && !ho_get_ho_active(bts->ho)) {
		LOGP(DHODEC, LOGL_INFO, "Skipping, Handover and Assignment both disabled in this cell\n");
		return 0;
	}

	collect_candidates_for_lchan(lchan, clist, &candidates, &av_rxlev, include_weaker_rxlev);

	/* If assignment is disabled and no neighbor cell report exists, or no neighbor cell qualifies,
	 * we may not even have any candidates. */
	if (!candidates)
		goto no_candidates;

	/* select best candidate that fulfills requirement B: no congestion after HO */
	best_better_db = 0;
	for (i = 0; i < candidates; i++) {
		int afs_bias;
		if (!(clist[i].requirements & REQUIREMENT_B_MASK))
			continue;

		better = clist[i].avg - av_rxlev;
		/* Apply AFS bias? */
		afs_bias = 0;
		if (ahs && (clist[i].requirements & REQUIREMENT_B_TCHF))
			afs_bias = ho_get_hodec2_afs_bias_rxlev(clist[i].bts->ho);
		better += afs_bias;
		if (better > best_better_db) {
			best_cand = &clist[i];
			best_better_db = better;
			best_applied_afs_bias = afs_bias? true : false;
		}
	}

	/* perform handover, if there is a candidate */
	if (best_cand) {
		LOGPHOLCHANTOBTS(lchan, best_cand->bts, LOGL_INFO, "Best candidate, RX level %d%s\n",
				 rxlev2dbm(best_cand->avg),
				 best_applied_afs_bias ? " (applied AHS -> AFS rxlev bias)" : "");
		return trigger_handover_or_assignment(lchan, best_cand->bts,
						      best_cand->requirements & REQUIREMENT_B_MASK);
	}

	/* select best candidate that fulfills requirement C: less or equal congestion after HO */
	best_better_db = 0;
	for (i = 0; i < candidates; i++) {
		int afs_bias;
		if (!(clist[i].requirements & REQUIREMENT_C_MASK))
			continue;

		better = clist[i].avg - av_rxlev;
		/* Apply AFS bias? */
		afs_bias = 0;
		if (ahs && (clist[i].requirements & REQUIREMENT_C_TCHF))
			afs_bias = ho_get_hodec2_afs_bias_rxlev(clist[i].bts->ho);
		better += afs_bias;
		if (better > best_better_db) {
			best_cand = &clist[i];
			best_better_db = better;
			best_applied_afs_bias = afs_bias? true : false;
		}
	}

	/* perform handover, if there is a candidate */
	if (best_cand) {
		LOGPHOLCHANTOBTS(lchan, best_cand->bts, LOGL_INFO, "Best candidate, RX level %d%s\n",
				 rxlev2dbm(best_cand->avg),
				 best_applied_afs_bias? " (applied AHS -> AFS rxlev bias)" : "");
		return trigger_handover_or_assignment(lchan, best_cand->bts,
						      best_cand->requirements & REQUIREMENT_C_MASK);
	}

	/* we are done in case the MS RXLEV/RXQUAL/TA aren't critical and we're avoiding congestion. */
	if (!include_weaker_rxlev)
		goto no_candidates;

	/* Select best candidate that fulfills requirement A: can service the call.
	 * From above we know that there are no options that avoid congestion. Here we're trying to find
	 * *any* free lchan that has no critically low RXLEV and is able to handle the MS. */
	best_better_db = 0;
	for (i = 0; i < candidates; i++) {
		int afs_bias;
		if (!(clist[i].requirements & REQUIREMENT_A_MASK))
			continue;

		better = clist[i].avg - av_rxlev;
		/* Apply AFS bias? */
		afs_bias = 0;
		if (ahs && (clist[i].requirements & REQUIREMENT_A_TCHF))
			afs_bias = ho_get_hodec2_afs_bias_rxlev(clist[i].bts->ho);
		better += afs_bias;
		if (better > best_better_db) {
			best_cand = &clist[i];
			best_better_db = better;
			best_applied_afs_bias = afs_bias? true : false;
		}
	}

	/* perform handover, if there is a candidate */
	if (best_cand) {
		LOGPHOLCHANTOBTS(lchan, best_cand->bts, LOGL_INFO, "Best candidate, RX level %d"
			" with greater congestion found%s\n",
			rxlev2dbm(best_cand->avg),
			best_applied_afs_bias ? " (applied AHS -> AFS rxlev bias)" : "");
		return trigger_handover_or_assignment(lchan, best_cand->bts,
						      best_cand->requirements & REQUIREMENT_A_MASK);
	}

	/* Damn, all is congested, has too low RXLEV or cannot service the voice call due to codec
	 * restrictions or because all lchans are taken. */

no_candidates:
	if (include_weaker_rxlev)
		LOGPHOLCHAN(lchan, LOGL_INFO, "No alternative lchan found\n");
	else
		LOGPHOLCHAN(lchan, LOGL_INFO, "No better/less congested neighbor cell found\n");

	return 0;
}

/*
 * Handover/assignment check, if measurement report is received
 *
 * Do not trigger handover/assignment on slots which have already ongoing
 * handover/assignment processes.
 *
 * In case of handover triggered because maximum allowed timing advance is
 * exceeded, the handover penalty timer is started for the originating cell.
 *
 */
static void on_measurement_report(struct gsm_meas_rep *mr)
{
	struct gsm_lchan *lchan = mr->lchan;
	struct gsm_bts *bts = lchan->ts->trx->bts;
	int av_rxlev = -EINVAL, av_rxqual = -EINVAL;
	unsigned int pwr_interval;

	/* we currently only do handover for TCH channels */
	switch (mr->lchan->type) {
	case GSM_LCHAN_TCH_F:
	case GSM_LCHAN_TCH_H:
		break;
	default:
		return;
	}

	if (log_check_level(DHODEC, LOGL_DEBUG)) {
		int i;
		LOGPHOLCHAN(lchan, LOGL_DEBUG, "MEASUREMENT REPORT (%d neighbors)\n",
			    mr->num_cell);
		for (i = 0; i < mr->num_cell; i++) {
			struct gsm_meas_rep_cell *mrc = &mr->cell[i];
			LOGPHOLCHAN(lchan, LOGL_DEBUG,
				    "  %d: arfcn=%u bsic=%u neigh_idx=%u rxlev=%u flags=%x\n",
				    i, mrc->arfcn, mrc->bsic, mrc->neigh_idx, mrc->rxlev, mrc->flags);
		}
	}

	/* parse actual neighbor cell info */
	if (mr->num_cell > 0 && mr->num_cell < 7)
		process_meas_neigh(mr);

	/* check for ongoing handover/assignment */
	if (!lchan->conn) {
		LOGPHOLCHAN(lchan, LOGL_ERROR, "Skipping, No subscriber connection???\n");
		return;
	}
	if (lchan->conn->secondary_lchan) {
		LOGPHOLCHAN(lchan, LOGL_INFO, "Skipping, Initial Assignment is still ongoing\n");
		return;
	}
	if (lchan->conn->ho) {
		LOGPHOLCHAN(lchan, LOGL_INFO, "Skipping, Handover already triggered\n");
		return;
	}

	LOGPHOLCHAN(lchan, LOGL_DEBUG, "HODEC2: evaluating measurement report\n");

	/* get average levels. if not enought measurements yet, value is < 0 */
	av_rxlev = get_meas_rep_avg(lchan,
				    ho_get_hodec2_full_tdma(bts->ho) ?
				    MEAS_REP_DL_RXLEV_FULL : MEAS_REP_DL_RXLEV_SUB,
				    ho_get_hodec2_rxlev_avg_win(bts->ho));
	av_rxqual = get_meas_rep_avg(lchan,
				     ho_get_hodec2_full_tdma(bts->ho) ?
				     MEAS_REP_DL_RXQUAL_FULL : MEAS_REP_DL_RXQUAL_SUB,
				     ho_get_hodec2_rxqual_avg_win(bts->ho));
	if (av_rxlev < 0 && av_rxqual < 0) {
		LOGPHOLCHAN(lchan, LOGL_INFO, "Skipping, Not enough recent measurements\n");
		return;
	}
	if (av_rxlev >= 0) {
		LOGPHOLCHAN(lchan, LOGL_DEBUG, "Measurement report: average RX level = %d\n",
			    rxlev2dbm(av_rxlev));
	}
	if (av_rxqual >= 0) {
		LOGPHOLCHAN(lchan, LOGL_DEBUG, "Measurement report: average RX quality = %d\n",
			    av_rxqual);
	}

	/* improve levels in case of AFS, if defined */
	if (lchan->type == GSM_LCHAN_TCH_F
	 && lchan->tch_mode == GSM48_CMODE_SPEECH_AMR) {
		int rxlev_bias = ho_get_hodec2_afs_bias_rxlev(bts->ho);
		int rxqual_bias = ho_get_hodec2_afs_bias_rxqual(bts->ho);
		if (av_rxlev >= 0 && rxlev_bias) {
			int imp = av_rxlev + rxlev_bias;
			LOGPHOLCHAN(lchan, LOGL_INFO, "Virtually improving RX level from %d to %d,"
				    " due to AFS bias\n", rxlev2dbm(av_rxlev), rxlev2dbm(imp));
			av_rxlev = imp;
		}
		if (av_rxqual >= 0 && rxqual_bias) {
			int imp = av_rxqual - rxqual_bias;
			if (imp < 0)
				imp = 0;
			LOGPHOLCHAN(lchan, LOGL_INFO, "Virtually improving RX quality from %d to %d,"
				    " due to AFS bias\n", rxlev2dbm(av_rxqual), rxlev2dbm(imp));
			av_rxqual = imp;
		}
	}

	/* Bad Quality */
	if (av_rxqual >= 0 && av_rxqual > ho_get_hodec2_min_rxqual(bts->ho)) {
		if (rxlev2dbm(av_rxlev) > -85) {
			global_ho_reason = HO_REASON_INTERFERENCE;
			LOGPHOLCHAN(lchan, LOGL_INFO, "Trying handover/assignment"
				    " due to interference (bad quality)\n");
		} else {
			global_ho_reason = HO_REASON_BAD_QUALITY;
			LOGPHOLCHAN(lchan, LOGL_INFO, "Trying handover/assignment due to bad quality\n");
		}
		find_alternative_lchan(lchan, true);
		return;
	}

	/* Low Level */
	if (av_rxlev >= 0 && rxlev2dbm(av_rxlev) < ho_get_hodec2_min_rxlev(bts->ho)) {
		global_ho_reason = HO_REASON_LOW_RXLEVEL;
		LOGPHOLCHAN(lchan, LOGL_INFO, "Attempting handover/assignment due to low rxlev\n");
		find_alternative_lchan(lchan, true);
		return;
	}

	/* Max Distance */
	if (lchan->meas_rep_count > 0
	    && lchan->rqd_ta > ho_get_hodec2_max_distance(bts->ho)) {
		global_ho_reason = HO_REASON_MAX_DISTANCE;
		LOGPHOLCHAN(lchan, LOGL_INFO, "Attempting handover due to high TA\n");
		/* start penalty timer to prevent comming back too
		 * early. it must be started before selecting a better cell,
		 * so there is no assignment selected, due to running
		 * penalty timer. */
		conn_penalty_time_add(lchan->conn, bts, ho_get_hodec2_penalty_max_dist(bts->ho));
		find_alternative_lchan(lchan, true);
		return;
	}

	/* pwr_interval's range is 1-99, clarifying that no div-zero shall happen in modulo below: */
	pwr_interval = ho_get_hodec2_pwr_interval(bts->ho);
	OSMO_ASSERT(pwr_interval);

	/* try handover to a better cell */
	if (av_rxlev >= 0 && (mr->nr % pwr_interval) == 0) {
		LOGPHOLCHAN(lchan, LOGL_INFO, "Looking whether a cell has better RXLEV\n");
		global_ho_reason = HO_REASON_BETTER_CELL;
		find_alternative_lchan(lchan, false);
	}
}

/*
 * Handover/assignment check after timer timeout:
 *
 * Even if handover process tries to prevent a congestion, a cell might get
 * congested due to new call setups or handovers to prevent loss of radio link.
 * A cell is congested, if not the minimum number of free slots are available.
 * The minimum number can be defined for TCH/F and TCH/H individually.
 *
 * Do not perform congestion check, if no minimum free slots are defined for
 * a cell.
 * Do not trigger handover/assignment on slots which have already ongoing
 * handover/assignment processes. If no AFS improvement offset is given, try to
 * maintain the same TCH rate, if available.
 * Do not perform this process, if handover and assignment are disabled for
 * the current cell.
 * Do not perform handover, if the minimum acceptable RX level
 * is not reched for this cell.
 * Only check candidates that will solve/reduce congestion.
 *
 * If a cell is congested, all slots are checked for all their RX levels
 * (down-link) of the current and neighbor cell measurements in descending
 * order of their RX levels:
 *
 *  * Select the best candidate that fulfills requirement B (no congestion after
 *    handover/assignment), trigger handover or assignment. Candidates that will
 *    cause an assignment from AHS (AMR on TCH/H) to AFS (AMR on TCH/F) are
 *    omitted.
 *     o This process repeated until the minimum required number of free slots
 *       are restored or if all cell measurements are checked. The process ends
 *       then, otherwise:
 *  * Select the worst candidate that fulfills requirement B, trigger
 *    assignment. Note that only assignment candidates for changing from AHS to
 *    AFS are left.
 *     o This process repeated until the minimum required number of free slots
 *       are restored or if all cell measurements are checked. The process ends
 *       then, otherwise:
 *  * Select the best candidates that fulfill requirement C (less or equally
 *    congested cells after handover/assignment), trigger handover or
 *    assignment. Candidates that will cause an assignment from AHS (AMR on
 *    TCH/H) to AFS (AMR on TCH/F) are omitted.
 *     o This process repeated until the minimum required number of free slots
 *       are restored or if all cell measurements are checked. The process ends
 *       then, otherwise:
 *  * Select the worst candidate that fulfills requirement C, trigger
 *    assignment. Note that only assignment candidates for changing from AHS to
 *    AFS are left.
 *     o This process repeated until the minimum required number of free slots
 *       are restored or if all cell measurements are checked.
 */
static int bts_resolve_congestion(struct gsm_bts *bts, int tchf_congestion, int tchh_congestion)
{
	struct gsm_lchan *lc;
	struct gsm_bts_trx *trx;
	struct gsm_bts_trx_ts *ts;
	int i, j;
	struct ho_candidate *clist;
	unsigned int candidates;
	struct ho_candidate *best_cand = NULL, *worst_cand = NULL;
	struct gsm_lchan *delete_lchan = NULL;
	unsigned int best_avg_db, worst_avg_db;
	int avg;
	int rc = 0;
	int any_ho = 0;
	int is_improved = 0;

	if (tchf_congestion < 0)
		tchf_congestion = 0;
	if (tchh_congestion < 0)
		tchh_congestion = 0;

	LOGPHOBTS(bts, LOGL_INFO, "congested: %d TCH/F and %d TCH/H should be moved\n",
		  tchf_congestion, tchh_congestion);

	/* allocate array of all bts */
	clist = talloc_zero_array(tall_bsc_ctx, struct ho_candidate,
		bts->num_trx * 8 * 2 * (1 + ARRAY_SIZE(lc->neigh_meas)));
	if (!clist)
		return 0;

	candidates = 0;

	/* loop through all active lchan and collect candidates */
	llist_for_each_entry(trx, &bts->trx_list, list) {
		if (!trx_is_usable(trx))
			continue;

		for (i = 0; i < 8; i++) {
			ts = &trx->ts[i];
			if (!ts_is_usable(ts))
				continue;

			/* (Do not consider dynamic TS that are in PDCH mode) */
			switch (ts_pchan(ts)) {
			case GSM_PCHAN_TCH_F:
				lc = &ts->lchan[0];
				/* omit if channel not active */
				if (lc->type != GSM_LCHAN_TCH_F
				    || lc->state != LCHAN_S_ACTIVE)
					break;
				/* omit if there is an ongoing ho/as */
				if (!lc->conn || lc->conn->secondary_lchan
				    || lc->conn->ho)
					break;
				/* We desperately want to resolve congestion, ignore rxlev when
				 * collecting candidates by passing include_weaker_rxlev=true. */
				collect_candidates_for_lchan(lc, clist, &candidates, NULL, true);
				break;
			case GSM_PCHAN_TCH_H:
				for (j = 0; j < 2; j++) {
					lc = &ts->lchan[j];
					/* omit if channel not active */
					if (lc->type != GSM_LCHAN_TCH_H
					    || lc->state != LCHAN_S_ACTIVE)
						continue;
					/* omit of there is an ongoing ho/as */
					if (!lc->conn
					    || lc->conn->secondary_lchan
					    || lc->conn->ho)
						continue;
					/* We desperately want to resolve congestion, ignore rxlev when
					 * collecting candidates by passing include_weaker_rxlev=true. */
					collect_candidates_for_lchan(lc, clist, &candidates, NULL, true);
				}
				break;
			default:
				break;
			}
		}
	}

	if (!candidates) {
		LOGPHOBTS(bts, LOGL_DEBUG, "No neighbor cells qualify to solve congestion\n");
		goto exit;
	}
	if (log_check_level(DHODEC, LOGL_DEBUG)) {
		LOGPHOBTS(bts, LOGL_DEBUG, "Considering %u candidates to solve congestion:\n", candidates);
		for (i = 0; i < candidates; i++) {
			LOGPHOLCHANTOBTS(clist[i].lchan, clist[i].bts, LOGL_DEBUG,
					 "#%d: req=0x%x avg-rxlev=%d\n",
					 i, clist[i].requirements, clist[i].avg);
		}
	}

#if 0
next_b1:
#endif
	/* select best candidate that fulfills requirement B,
	 * omit change from AHS to AFS */
	best_avg_db = 0;
	for (i = 0; i < candidates; i++) {
		/* delete subscriber that just have handovered */
		if (clist[i].lchan == delete_lchan)
			clist[i].lchan = NULL;
		/* omit all subscribers that are handovered */
		if (!clist[i].lchan)
			continue;

		if (!(clist[i].requirements & REQUIREMENT_B_MASK))
			continue;
		/* omit assignment from AHS to AFS */
		if (clist[i].lchan->ts->trx->bts == clist[i].bts
		 && clist[i].lchan->type == GSM_LCHAN_TCH_H
		 && (clist[i].requirements & REQUIREMENT_B_TCHF))
			continue;
		/* omit candidates that will not solve/reduce congestion */
		if (clist[i].lchan->type == GSM_LCHAN_TCH_F
		 && tchf_congestion <= 0)
			continue;
		if (clist[i].lchan->type == GSM_LCHAN_TCH_H
		 && tchh_congestion <= 0)
			continue;

		avg = clist[i].avg;
		/* improve AHS */
		if (clist[i].lchan->tch_mode == GSM48_CMODE_SPEECH_AMR
		 && clist[i].lchan->type == GSM_LCHAN_TCH_H
		 && (clist[i].requirements & REQUIREMENT_B_TCHF)) {
			avg += ho_get_hodec2_afs_bias_rxlev(clist[i].bts->ho);
			is_improved = 1;
		} else
			is_improved = 0;
		LOGP(DHODEC, LOGL_DEBUG, "candidate %d: avg=%d best_avg_db=%d\n", i, avg, best_avg_db);
		if (avg > best_avg_db) {
			best_cand = &clist[i];
			best_avg_db = avg;
		}
	}

	/* perform handover, if there is a candidate */
	if (best_cand) {
		any_ho = 1;
		LOGPHOLCHAN(best_cand->lchan, LOGL_INFO,
			    "Best candidate BTS %u (RX level %d) without congestion found\n",
			    best_cand->bts->nr, rxlev2dbm(best_cand->avg));
		if (is_improved)
			LOGP(DHODEC, LOGL_INFO, "(is improved due to "
				"AHS -> AFS)\n");
		trigger_handover_or_assignment(best_cand->lchan, best_cand->bts,
			best_cand->requirements & REQUIREMENT_B_MASK);
#if 0
		/* if there is still congestion, mark lchan as deleted
		 * and redo this process */
		if (best_cand->lchan->type == GSM_LCHAN_TCH_H)
			tchh_congestion--;
		else
			tchf_congestion--;
		if (tchf_congestion > 0 || tchh_congestion > 0) {
			delete_lchan = best_cand->lchan;
			best_cand = NULL;
			goto next_b1;
		}
#else
		/* must exit here, because triggering handover/assignment
		 * will cause change in requirements. more check for this
		 * bts is performed in the next iteration.
		 */
#endif
		goto exit;
	}

	LOGPHOBTS(bts, LOGL_DEBUG, "Did not find a best candidate that fulfills requirement B"
		  " (omitting change from AHS to AFS)\n");

#if 0
next_b2:
#endif
	/* select worst candidate that fulfills requirement B,
	 * select candidates that change from AHS to AFS only */
	if (tchh_congestion > 0) {
		/* since this will only check half rate channels, it will
		 * only need to be checked, if tchh is congested */
		worst_avg_db = 999;
		for (i = 0; i < candidates; i++) {
			/* delete subscriber that just have handovered */
			if (clist[i].lchan == delete_lchan)
				clist[i].lchan = NULL;
			/* omit all subscribers that are handovered */
			if (!clist[i].lchan)
				continue;

			if (!(clist[i].requirements & REQUIREMENT_B_MASK))
				continue;
			/* omit all but assignment from AHS to AFS */
			if (clist[i].lchan->ts->trx->bts != clist[i].bts
			 || clist[i].lchan->type != GSM_LCHAN_TCH_H
			 || !(clist[i].requirements & REQUIREMENT_B_TCHF))
				continue;

			avg = clist[i].avg;
			/* improve AHS */
			if (clist[i].lchan->tch_mode == GSM48_CMODE_SPEECH_AMR
			 && clist[i].lchan->type == GSM_LCHAN_TCH_H) {
				avg += ho_get_hodec2_afs_bias_rxlev(clist[i].bts->ho);
				is_improved = 1;
			} else
				is_improved = 0;
			LOGP(DHODEC, LOGL_DEBUG, "candidate %d: avg=%d worst_avg_db=%d\n", i, avg,
			     worst_avg_db);
			if (avg < worst_avg_db) {
				worst_cand = &clist[i];
				worst_avg_db = avg;
			}
		}
	}

	/* perform handover, if there is a candidate */
	if (worst_cand) {
		any_ho = 1;
		LOGP(DHODEC, LOGL_INFO, "Worst candidate for assignment "
			"(RX level %d) from TCH/H -> TCH/F without congestion "
			"found\n", rxlev2dbm(worst_cand->avg));
		if (is_improved)
			LOGP(DHODEC, LOGL_INFO, "(is improved due to "
				"AHS -> AFS)\n");
		trigger_handover_or_assignment(worst_cand->lchan,
			worst_cand->bts,
			worst_cand->requirements & REQUIREMENT_B_MASK);
#if 0
		/* if there is still congestion, mark lchan as deleted
		 * and redo this process */
		tchh_congestion--;
		if (tchh_congestion > 0) {
			delete_lchan = worst_cand->lchan;
			best_cand = NULL;
			goto next_b2;
		}
#else
		/* must exit here, because triggering handover/assignment
		 * will cause change in requirements. more check for this
		 * bts is performed in the next iteration.
		 */
#endif
		goto exit;
	}

	LOGPHOBTS(bts, LOGL_DEBUG, "Did not find a worst candidate that fulfills requirement B,"
		  " selecting candidates that change from AHS to AFS only\n");

#if 0
next_c1:
#endif
	/* select best candidate that fulfills requirement C,
	 * omit change from AHS to AFS */
	best_avg_db = 0;
	for (i = 0; i < candidates; i++) {
		/* delete subscriber that just have handovered */
		if (clist[i].lchan == delete_lchan)
			clist[i].lchan = NULL;
		/* omit all subscribers that are handovered */
		if (!clist[i].lchan)
			continue;

		if (!(clist[i].requirements & REQUIREMENT_C_MASK))
			continue;
		/* omit assignment from AHS to AFS */
		if (clist[i].lchan->ts->trx->bts == clist[i].bts
		 && clist[i].lchan->type == GSM_LCHAN_TCH_H
		 && (clist[i].requirements & REQUIREMENT_C_TCHF))
			continue;
		/* omit candidates that will not solve/reduce congestion */
		if (clist[i].lchan->type == GSM_LCHAN_TCH_F
		 && tchf_congestion <= 0)
			continue;
		if (clist[i].lchan->type == GSM_LCHAN_TCH_H
		 && tchh_congestion <= 0)
			continue;

		avg = clist[i].avg;
		/* improve AHS */
		if (clist[i].lchan->tch_mode == GSM48_CMODE_SPEECH_AMR
		 && clist[i].lchan->type == GSM_LCHAN_TCH_H
		 && (clist[i].requirements & REQUIREMENT_C_TCHF)) {
			avg += ho_get_hodec2_afs_bias_rxlev(clist[i].bts->ho);
			is_improved = 1;
		} else
			is_improved = 0;
		LOGP(DHODEC, LOGL_DEBUG, "candidate %d: avg=%d best_avg_db=%d\n", i, avg, best_avg_db);
		if (avg > best_avg_db) {
			best_cand = &clist[i];
			best_avg_db = avg;
		}
	}

	/* perform handover, if there is a candidate */
	if (best_cand) {
		any_ho = 1;
		LOGP(DHODEC, LOGL_INFO, "Best candidate BTS %d (RX level %d) "
			"with less or equal congestion found\n",
			best_cand->bts->nr, rxlev2dbm(best_cand->avg));
		if (is_improved)
			LOGP(DHODEC, LOGL_INFO, "(is improved due to "
				"AHS -> AFS)\n");
		trigger_handover_or_assignment(best_cand->lchan, best_cand->bts,
			best_cand->requirements & REQUIREMENT_C_MASK);
#if 0
		/* if there is still congestion, mark lchan as deleted
		 * and redo this process */
		if (best_cand->lchan->type == GSM_LCHAN_TCH_H)
			tchh_congestion--;
		else
			tchf_congestion--;
		if (tchf_congestion > 0 || tchh_congestion > 0) {
			delete_lchan = best_cand->lchan;
			best_cand = NULL;
			goto next_c1;
		}
#else
		/* must exit here, because triggering handover/assignment
		 * will cause change in requirements. more check for this
		 * bts is performed in the next iteration.
		 */
#endif
		goto exit;
	}

	LOGPHOBTS(bts, LOGL_DEBUG, "Did not find a best candidate that fulfills requirement C"
		  " (omitting change from AHS to AFS)\n");

#if 0
next_c2:
#endif
	/* select worst candidate that fulfills requirement C,
	 * select candidates that change from AHS to AFS only */
	if (tchh_congestion > 0) {
		/* since this will only check half rate channels, it will
		 * only need to be checked, if tchh is congested */
		worst_avg_db = 999;
		for (i = 0; i < candidates; i++) {
			/* delete subscriber that just have handovered */
			if (clist[i].lchan == delete_lchan)
				clist[i].lchan = NULL;
			/* omit all subscribers that are handovered */
			if (!clist[i].lchan)
				continue;

			if (!(clist[i].requirements & REQUIREMENT_C_MASK))
				continue;
			/* omit all but assignment from AHS to AFS */
			if (clist[i].lchan->ts->trx->bts != clist[i].bts
			 || clist[i].lchan->type != GSM_LCHAN_TCH_H
			 || !(clist[i].requirements & REQUIREMENT_C_TCHF))
				continue;

			avg = clist[i].avg;
			/* improve AHS */
			if (clist[i].lchan->tch_mode == GSM48_CMODE_SPEECH_AMR
			 && clist[i].lchan->type == GSM_LCHAN_TCH_H) {
				avg += ho_get_hodec2_afs_bias_rxlev(clist[i].bts->ho);
				is_improved = 1;
			} else
				is_improved = 0;
			LOGP(DHODEC, LOGL_DEBUG, "candidate %d: avg=%d worst_avg_db=%d\n", i, avg,
			     worst_avg_db);
			if (avg < worst_avg_db) {
				worst_cand = &clist[i];
				worst_avg_db = avg;
			}
		}
	}

	/* perform handover, if there is a candidate */
	if (worst_cand) {
		any_ho = 1;
		LOGP(DHODEC, LOGL_INFO, "Worst candidate for assignment "
			"(RX level %d) from TCH/H -> TCH/F with less or equal "
			"congestion found\n", rxlev2dbm(worst_cand->avg));
		if (is_improved)
			LOGP(DHODEC, LOGL_INFO, "(is improved due to "
				"AHS -> AFS)\n");
		trigger_handover_or_assignment(worst_cand->lchan,
			worst_cand->bts,
			worst_cand->requirements & REQUIREMENT_C_MASK);
#if 0
		/* if there is still congestion, mark lchan as deleted
		 * and redo this process */
		tchh_congestion--;
		if (tchh_congestion > 0) {
			delete_lchan = worst_cand->lchan;
			worst_cand = NULL;
			goto next_c2;
		}
#else
		/* must exit here, because triggering handover/assignment
		 * will cause change in requirements. more check for this
		 * bts is performed in the next iteration.
		 */
#endif
		goto exit;
	}
	LOGPHOBTS(bts, LOGL_DEBUG, "Did not find a worst candidate that fulfills requirement C,"
		  " selecting candidates that change from AHS to AFS only\n");


exit:
	/* free array */
	talloc_free(clist);

	if (tchf_congestion <= 0 && tchh_congestion <= 0)
		LOGP(DHODEC, LOGL_INFO, "Congestion at BTS %d solved!\n",
			bts->nr);
	else if (any_ho)
		LOGP(DHODEC, LOGL_INFO, "Congestion at BTS %d reduced!\n",
			bts->nr);
	else
		LOGP(DHODEC, LOGL_INFO, "Congestion at BTS %d can't be reduced/solved!\n", bts->nr);

	return rc;
}

static void bts_congestion_check(struct gsm_bts *bts)
{
	int min_free_tchf, min_free_tchh;
	int tchf_count, tchh_count;

	global_ho_reason = HO_REASON_CONGESTION;

	/* only check BTS if TRX 0 is usable */
	if (!trx_is_usable(bts->c0)) {
		LOGPHOBTS(bts, LOGL_DEBUG, "No congestion check: TRX 0 not usable\n");
		return;
	}

	/* only check BTS if handover or assignment is enabled */
	if (!ho_get_hodec2_as_active(bts->ho)
	    && !ho_get_ho_active(bts->ho)) {
		LOGPHOBTS(bts, LOGL_DEBUG, "No congestion check: Assignment and Handover both disabled\n");
		return;
	}

	min_free_tchf = ho_get_hodec2_tchf_min_slots(bts->ho);
	min_free_tchh = ho_get_hodec2_tchh_min_slots(bts->ho);

	/* only check BTS with congestion level set */
	if (!min_free_tchf && !min_free_tchh) {
		LOGPHOBTS(bts, LOGL_DEBUG, "No congestion check: no minimum for free TCH/F nor TCH/H set\n");
		return;
	}

	tchf_count = bts_count_free_ts(bts, GSM_PCHAN_TCH_F);
	tchh_count = bts_count_free_ts(bts, GSM_PCHAN_TCH_H);
	LOGPHOBTS(bts, LOGL_INFO, "Congestion check: (free/want-free) TCH/F=%d/%d TCH/H=%d/%d\n",
		  tchf_count, min_free_tchf, tchh_count, min_free_tchh);

	/* only check BTS if congested */
	if (tchf_count >= min_free_tchf && tchh_count >= min_free_tchh) {
		LOGPHOBTS(bts, LOGL_DEBUG, "Not congested\n");
		return;
	}

	LOGPHOBTS(bts, LOGL_DEBUG, "Attempting to resolve congestion...\n");
	bts_resolve_congestion(bts, min_free_tchf - tchf_count, min_free_tchh - tchh_count);
}

void hodec2_congestion_check(struct gsm_network *net)
{
	struct gsm_bts *bts;

	llist_for_each_entry(bts, &net->bts_list, list)
		bts_congestion_check(bts);
}

static void congestion_check_cb(void *arg)
{
	struct gsm_network *net = arg;
	hodec2_congestion_check(net);
	reinit_congestion_timer(net);
}

void on_ho_chan_activ_nack(struct bsc_handover *ho)
{
	struct gsm_bts *new_bts = ho->new_lchan->ts->trx->bts;

	LOGPHO(ho, LOGL_ERROR, "Channel Activate Nack for %s, starting penalty timer\n", ho->inter_cell? "Handover" : "Assignment");

	/* if channel failed, wait 10 seconds before allowing to retry handover */
	conn_penalty_time_add(ho->old_lchan->conn, new_bts, 10); /* FIXME configurable */
}

void on_ho_failure(struct bsc_handover *ho)
{
	struct gsm_bts *old_bts = ho->old_lchan->ts->trx->bts;
	struct gsm_bts *new_bts = ho->new_lchan->ts->trx->bts;
	struct gsm_subscriber_connection *conn = ho->old_lchan->conn;

	if (!conn) {
		LOGPHO(ho, LOGL_ERROR, "HO failure, but no conn");
		return;
	}

	if (conn->hodec2.failures >= ho_get_hodec2_retries(old_bts->ho)) {
		int penalty = ho->inter_cell
			? ho_get_hodec2_penalty_failed_ho(old_bts->ho)
			: ho_get_hodec2_penalty_failed_as(old_bts->ho);
		LOGPHO(ho, LOGL_NOTICE, "%s failed, starting penalty timer (%d s)\n",
		       ho->inter_cell ? "Handover" : "Assignment",
		       penalty);
		conn->hodec2.failures = 0;
		conn_penalty_time_add(conn, new_bts, penalty);
	} else {
		conn->hodec2.failures++;
		LOGPHO(ho, LOGL_NOTICE, "%s failed, allowing handover decision to try again"
		       " (%d/%d attempts)\n",
		       ho->inter_cell ? "Handover" : "Assignment",
		       conn->hodec2.failures, ho_get_hodec2_retries(old_bts->ho));
	}
}

struct handover_decision_callbacks hodec2_callbacks = {
	.hodec_id = 2,
	.on_measurement_report = on_measurement_report,
	.on_ho_chan_activ_nack = on_ho_chan_activ_nack,
	.on_ho_failure = on_ho_failure,
};

void hodec2_init(struct gsm_network *net)
{
	handover_decision_callbacks_register(&hodec2_callbacks);
	hodec2_initialized = true;
	reinit_congestion_timer(net);
}

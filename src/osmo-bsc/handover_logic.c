/* Handover Logic for Inter-BTS (Intra-BSC) Handover.  This does not
 * actually implement the handover algorithm/decision, but executes a
 * handover decision */

/* (C) 2009 by Harald Welte <laforge@gnumonks.org>
 *
 * All Rights Reserved
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
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <netinet/in.h>

#include <osmocom/core/msgb.h>
#include <osmocom/bsc/debug.h>
#include <osmocom/bsc/gsm_data.h>
#include <osmocom/gsm/gsm_utils.h>
#include <osmocom/bsc/abis_rsl.h>
#include <osmocom/bsc/lchan_select.h>
#include <osmocom/bsc/signal.h>
#include <osmocom/core/talloc.h>
#include <osmocom/bsc/bsc_subscriber.h>
#include <osmocom/bsc/gsm_04_08_utils.h>
#include <osmocom/bsc/handover.h>
#include <osmocom/bsc/handover_cfg.h>
#include <osmocom/bsc/bsc_subscr_conn_fsm.h>
#include <osmocom/bsc/neighbor_ident.h>
#include <osmocom/bsc/abis_nm.h>
#include <osmocom/gsm/gsm0808.h>
#include <osmocom/gsm/gsm0808_utils.h>

const struct value_string handover_scope_names[] = {
	{ HO_NO_HANDOVER, "No Handover" },
	{ HO_INTRA_CELL, "Assignment" },
	{ HO_INTRA_BSC, "Handover" },
	{ HO_INTER_BSC_MO, "Inter-BSC-Handover (MO)" },
	{ HO_INTER_BSC_MT, "Inter-BSC-Handover (MT)" },
	{ HO_SCOPE_ALL, "Any Handover" },
	{}
};

const struct value_string handover_result_names[] = {
	{ HO_RESULT_OK, "Complete" },
	{ HO_RESULT_FAIL_NO_CHANNEL, "Failure (no channel could be allocated)" },
	{ HO_RESULT_FAIL_RR_HO_FAIL, "Failure (MS sent RR Handover Failure)" },
	{ HO_RESULT_FAIL_TIMEOUT, "Failure (timeout)" },
	{ HO_RESULT_CONN_RELEASE, "Connection released" },
	{ HO_RESULT_ERROR, "Failure" },
	{}
};

static LLIST_HEAD(handover_decision_callbacks);

void handover_decision_callbacks_register(struct handover_decision_callbacks *hdc)
{
	llist_add_tail(&hdc->entry, &handover_decision_callbacks);
}

struct handover_decision_callbacks *handover_decision_callbacks_get(int hodec_id)
{
	struct handover_decision_callbacks *hdc;
	llist_for_each_entry(hdc, &handover_decision_callbacks, entry) {
		if (hdc->hodec_id == hodec_id)
			return hdc;
	}
	return NULL;
}

#if 0
static void handover_start_inter_bsc_mo(struct handover *ho,
					const struct gsm0808_cell_id_list2 *target_cells,
					enum gsm_chan_t new_lchan_type)
{
	int rc;
	struct gsm_lchan *old_lchan = ho->mo.old_lchan;
	struct gsm0808_handover_required ho_required_params = {
		.cause = GSM0808_CAUSE_BETTER_CELL,
		.cil = *target_cells,
		.current_channel_type_1_present = true,
		.current_channel_type_1 = gsm0808_current_channel_type_1(old_lchan->type),
	};

	ho->scope = HO_INTER_BSC_MO;

	LOGPHO(ho, LOGL_INFO, "Starting\n");

	if (osmo_fsm_inst_dispatch(ho->conn->fi, GSCON_EV_INTER_BSC_HO_MO_START, NULL)) {
		handover_end(ho, HO_RESULT_ERROR);
		return;
	}

	switch (old_lchan->type) {
	case GSM_LCHAN_TCH_F:
	case GSM_LCHAN_TCH_H:
		ho_required_params.speech_version_used_present = true;
		ho_required_params.speech_version_used = gsm0808_permitted_speech(old_lchan->type,
										  old_lchan->tch_mode);
		if (!ho_required_params.speech_version_used) {
			LOGPHO(ho, LOGL_ERROR,
			       "Cannot encode Speech Version (Used) for HANDOVER REQUIRED message\n");
			handover_end(ho, HO_RESULT_ERROR);
			return;
		}
		break;
	default:
		break;
	}

	rc = bsc_send_handover_required(ho->mo.old_lchan, &ho_required_params);
	if (rc) {
		LOGPHO(ho, LOGL_ERROR, "Failed to send Handover Required (rc=%d)\n", rc);
		handover_end(ho, HO_RESULT_ERROR);
	}
}

#endif



static void ho_meas_rep(struct gsm_meas_rep *mr)
{
	struct handover_decision_callbacks *hdc;
	enum hodec_id hodec_id = ho_get_algorithm(mr->lchan->ts->trx->bts->ho);

	hdc = handover_decision_callbacks_get(hodec_id);
	if (!hdc || !hdc->on_measurement_report)
		return;
	hdc->on_measurement_report(mr);
}

/* Count ongoing handovers within the given BTS.
 * ho_scopes is an OR'd combination of enum handover_scope values to include in the count. */
int handover_count(struct gsm_bts *bts, int ho_scopes)
{
	struct gsm_bts_trx *trx;
	int count = 0;

	llist_for_each_entry(trx, &bts->trx_list, list) {
		int i;
		for (i = 0; i < ARRAY_SIZE(trx->ts); i++) {
			struct gsm_bts_trx_ts *ts = &trx->ts[i];
			struct gsm_lchan *lchan;

			/* skip administratively deactivated timeslots */
			if (!nm_is_running(&ts->mo.nm_state))
				continue;

			ts_for_each_lchan(lchan, ts) {
				if (!lchan->conn)
					continue;
				if (!lchan->conn->ho.fi)
					continue;
				if (lchan->conn->ho.scope & ho_scopes)
					count++;
			}
		}
	}

	return count;
}

struct gsm_bts *bts_by_neighbor_ident(const struct gsm_network *net,
				      const struct neighbor_ident_key *search_for)
{
	struct gsm_bts *found = NULL;
	struct gsm_bts *bts;
	struct gsm_bts *wildcard_match = NULL;

	llist_for_each_entry(bts, &net->bts_list, list) {
		struct neighbor_ident_key entry = {
			.from_bts = NEIGHBOR_IDENT_KEY_ANY_BTS,
			.arfcn = bts->c0->arfcn,
			.bsic_kind = BSIC_6BIT,
			.bsic = bts->bsic,
		};
		if (neighbor_ident_key_match(&entry, search_for, true)) {
			if (found) {
				LOGP(DHO, LOGL_ERROR, "CONFIG ERROR: Multiple BTS match %s: %d and %d\n",
				     neighbor_ident_key_name(search_for),
				     found->nr, bts->nr);
				return found;
			}
			found = bts;
		}
		if (neighbor_ident_key_match(&entry, search_for, false))
			wildcard_match = bts;
	}

	if (found)
		return found;

	return wildcard_match;
}

struct neighbor_ident_key *bts_ident_key(const struct gsm_bts *bts)
{
	static struct neighbor_ident_key key;
	key = (struct neighbor_ident_key){
		.arfcn = bts->c0->arfcn,
		.bsic_kind = BSIC_6BIT,
		.bsic = bts->bsic,
	};
	return &key;
}

static int ho_logic_sig_cb(unsigned int subsys, unsigned int signal,
			   void *handler_data, void *signal_data)
{
	struct lchan_signal_data *lchan_data;
	struct gsm_lchan *lchan;

	lchan_data = signal_data;
	switch (subsys) {
	case SS_LCHAN:
		OSMO_ASSERT(lchan_data);
		lchan = lchan_data->lchan;
		OSMO_ASSERT(lchan);

		switch (signal) {
		case S_LCHAN_MEAS_REP:
			ho_meas_rep(lchan_data->mr);
			break;
		}

	default:
		break;
	}
	return 0;
}

static __attribute__((constructor)) void on_dso_load_ho_logic(void)
{
	osmo_signal_register_handler(SS_LCHAN, ho_logic_sig_cb, NULL);
}

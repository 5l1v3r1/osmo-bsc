/* (C) 2017 by Harald Welte <laforge@gnumonks.org>
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

#include <osmocom/core/fsm.h>
#include <osmocom/core/logging.h>
#include <osmocom/gsm/gsm0808.h>
#include <osmocom/sigtran/sccp_sap.h>
#include <osmocom/gsm/gsm0808_utils.h>

#include <osmocom/bsc/debug.h>
#include <osmocom/bsc/bsc_api.h>
#include <osmocom/bsc/gsm_data.h>
#include <osmocom/bsc/handover.h>
#include <osmocom/bsc/chan_alloc.h>
#include <osmocom/bsc/bsc_subscriber.h>
#include <osmocom/bsc/osmo_bsc_sigtran.h>
#include <osmocom/bsc/bsc_subscr_conn_fsm.h>
#include <osmocom/bsc/osmo_bsc.h>
#include <osmocom/bsc/penalty_timers.h>
#include <osmocom/mgcp_client/mgcp_client_fsm.h>
#include <osmocom/core/byteswap.h>

#define S(x)	(1 << (x))

#define MGCP_MGW_TIMEOUT 4	/* in seconds */
#define MGCP_MGW_TIMEOUT_TIMER_NR 1

#define MGCP_MGW_HO_TIMEOUT 4	/* in seconds */
#define MGCP_MGW_HO_TIMEOUT_TIMER_NR 2

#define GSM0808_T10_TIMER_NR 10
#define GSM0808_T10_VALUE 6

#define ENDPOINT_ID "rtpbridge/*@mgw"

enum gscon_fsm_states {
	ST_INIT,
	/* waiting for CC from MSC */
	ST_WAIT_CC,
	/* active connection */
	ST_ACTIVE,
	/* when allocating an lchan, it may be necessary to deactivate PDCH on a dyn ts first. */
	ST_WAIT_DYN_TS_SWITCHOVER,
	/* during assignment; waiting for ASS_CMPL */
	ST_WAIT_ASS_CMPL,
	/* during assignment; waiting for MODE_MODIFY_ACK */
	ST_WAIT_MODE_MODIFY_ACK,
	/* BSSMAP CLEAR has been received */
	ST_CLEARING,

/* MGW handling */
	/* during assignment; waiting for MGW response to CRCX for BTS */
	ST_WAIT_CRCX_BTS,
	/* during assignment; waiting for MGW response to MDCX for BTS */
	ST_WAIT_MDCX_BTS,
	/* during assignment; waiting for MGW response to CRCX for MSC */
	ST_WAIT_CRCX_MSC,

/* MT (inbound) handover */
	/* Wait for Handover Access from MS/BTS */
	ST_WAIT_MT_HO_ACC,
	/* Wait for RR Handover Complete from MS/BTS */
	ST_WAIT_MT_HO_COMPL,

/* MO (outbound) handover */
	/* Wait for Handover Command / Handover Required Reject from MSC */
	ST_WAIT_MO_HO_CMD,
	/* Wait for Clear Command from MSC */
	ST_MO_HO_PROCEEDING,

/* Internal HO handling */
	/* Wait for the handover logic to complete the handover */
	ST_WAIT_HO_COMPL,
	/* during handover; waiting for MGW response to MDCX for BTS */
	ST_WAIT_MDCX_BTS_HO,
};

static const struct value_string gscon_fsm_event_names[] = {
	{GSCON_EV_A_CONN_IND, "MT-CONNECT.ind"},
	{GSCON_EV_A_CONN_REQ, "MO-CONNECT.req"},
	{GSCON_EV_A_CONN_CFM, "MO-CONNECT.cfm"},
	{GSCON_EV_A_ASSIGNMENT_CMD, "ASSIGNMENT_CMD"},
	{GSCON_EV_A_CLEAR_CMD, "CLEAR_CMD"},
	{GSCON_EV_A_DISC_IND, "DISCONNET.ind"},
	{GSCON_EV_A_HO_REQ, "HANDOVER_REQUEST"},

	{GSCON_EV_RR_ASS_COMPL, "RR_ASSIGN_COMPL"},
	{GSCON_EV_RR_ASS_FAIL, "RR_ASSIGN_FAIL"},
	{GSCON_EV_RR_MODE_MODIFY_ACK, "RR_MODE_MODIFY_ACK"},
	{GSCON_EV_RLL_REL_IND, "RLL_RELEASE.ind"},
	{GSCON_EV_RSL_CONN_FAIL, "RSL_CONN_FAIL.ind"},
	{GSCON_EV_RSL_CLEAR_COMPL, "RSL_CLEAR_COMPLETE"},

	{GSCON_EV_MO_DTAP, "MO-DTAP"},
	{GSCON_EV_MT_DTAP, "MT-DTAP"},
	{GSCON_EV_TX_SCCP, "TX_SCCP"},

	{GSCON_EV_MGW_FAIL_BTS, "MGW_FAILURE_BTS"},
	{GSCON_EV_MGW_FAIL_MSC, "MGW_FAILURE_MSC"},
	{GSCON_EV_MGW_CRCX_RESP_BTS, "MGW_CRCX_RESPONSE_BTS"},
	{GSCON_EV_MGW_MDCX_RESP_BTS, "MGW_MDCX_RESPONSE_BTS"},
	{GSCON_EV_MGW_CRCX_RESP_MSC, "MGW_CRCX_RESPONSE_MSC"},

	{GSCON_EV_HO_START, "HO_START"},
	{GSCON_EV_HO_TIMEOUT, "HO_TIMEOUT"},
	{GSCON_EV_HO_FAIL, "HO_FAIL"},
	{GSCON_EV_HO_COMPL, "HO_COMPL"},

	{0, NULL}
};

/* Send data SCCP message through SCCP connection. All sigtran messages
 * that are send from this FSM must use this function. Never use
 * osmo_bsc_sigtran_send() directly since this would defeat the checks
 * provided by this function. */
static void sigtran_send(struct gsm_subscriber_connection *conn, struct msgb *msg, struct osmo_fsm_inst *fi)
{
	int rc;

	/* Make sure that we only attempt to send SCCP messages if we have
	 * a life SCCP connection. Otherwise drop the message. */
	if (fi->state == ST_INIT || fi->state == ST_WAIT_CC) {
		LOGPFSML(fi, LOGL_ERROR, "No active SCCP connection, dropping message!\n");
		msgb_free(msg);
		return;
	}

	rc = osmo_bsc_sigtran_send(conn, msg);
	if (rc < 0)
		LOGPFSML(fi, LOGL_ERROR, "Unable to deliver SCCP message!\n");
}


/* See TS 48.008 3.2.2.11 Channel Type Octet 5 */
static int bssap_speech_from_lchan(const struct gsm_lchan *lchan)
{
	switch (lchan->type) {
	case GSM_LCHAN_TCH_H:
		switch (lchan->tch_mode) {
		case GSM48_CMODE_SPEECH_V1:
			return 0x05;
		case GSM48_CMODE_SPEECH_AMR:
			return 0x25;
		default:
			return -1;
		}
		break;
	case GSM_LCHAN_TCH_F:
		switch (lchan->tch_mode) {
		case GSM48_CMODE_SPEECH_V1:
			return 0x01;
		case GSM48_CMODE_SPEECH_EFR:
			return 0x11;
		case GSM48_CMODE_SPEECH_AMR:
			return 0x21;
		default:
			return -1;
		}
		break;
	default:
		return -1;
	}
}

/* GSM 08.08 3.2.2.33 */
static uint8_t lchan_to_chosen_channel(struct gsm_lchan *lchan)
{
	uint8_t channel_mode = 0, channel = 0;

	switch (lchan->tch_mode) {
	case GSM48_CMODE_SPEECH_V1:
	case GSM48_CMODE_SPEECH_EFR:
	case GSM48_CMODE_SPEECH_AMR:
		channel_mode = 0x9;
		break;
	case GSM48_CMODE_SIGN:
		channel_mode = 0x8;
		break;
	case GSM48_CMODE_DATA_14k5:
		channel_mode = 0xe;
		break;
	case GSM48_CMODE_DATA_12k0:
		channel_mode = 0xb;
		break;
	case GSM48_CMODE_DATA_6k0:
		channel_mode = 0xc;
		break;
	case GSM48_CMODE_DATA_3k6:
		channel_mode = 0xd;
		break;
	}

	switch (lchan->type) {
	case GSM_LCHAN_NONE:
		channel = 0x0;
		break;
	case GSM_LCHAN_SDCCH:
		channel = 0x1;
		break;
	case GSM_LCHAN_TCH_F:
		channel = 0x8;
		break;
	case GSM_LCHAN_TCH_H:
		channel = 0x9;
		break;
	case GSM_LCHAN_UNKNOWN:
	default:
		LOGP(DMSC, LOGL_ERROR, "Unknown lchan type: %p\n", lchan);
		break;
	}

	return channel_mode << 4 | channel;
}

/* Generate and send assignment complete message */
static void send_ass_compl(struct gsm_lchan *lchan, struct osmo_fsm_inst *fi, bool voice)
{
	struct msgb *resp;
	struct gsm0808_speech_codec sc;
	struct gsm0808_speech_codec *sc_ptr = NULL;
	struct gsm_subscriber_connection *conn;
	struct sockaddr_storage *addr_local = NULL;
	int perm_spch = 0;

	conn = lchan->conn;
	OSMO_ASSERT(conn);

	LOGPFSML(fi, LOGL_DEBUG, "Sending assignment complete message... (id=%i)\n", conn->sccp.conn_id);

	/* Generate voice related fields */
	if (voice) {
		OSMO_ASSERT(lchan->abis_ip.ass_compl.valid);
		perm_spch = bssap_speech_from_lchan(lchan);
		addr_local = &conn->user_plane.aoip_rtp_addr_local;

		/* Extrapolate speech codec from speech mode */
		gsm0808_speech_codec_from_chan_type(&sc, perm_spch);
		sc_ptr = &sc;

		/* FIXME: AMR codec configuration must be derived from lchan1! */
	}

	/* Generate message */
	resp = gsm0808_create_ass_compl(lchan->abis_ip.ass_compl.rr_cause,
					lchan_to_chosen_channel(lchan),
					lchan->encr.alg_id, perm_spch,
					addr_local, sc_ptr, NULL);

	if (!resp) {
		LOGPFSML(fi, LOGL_ERROR, "Failed to generate assignment completed message! (id=%i)\n",
			 conn->sccp.conn_id);
	}

	sigtran_send(conn, resp, fi);
}

/* forward MT DTAP from BSSAP side to RSL side */
static void submit_dtap(struct gsm_subscriber_connection *conn, struct msgb *msg, struct osmo_fsm_inst *fi)
{
	int rc;
	struct msgb *resp = NULL;

	OSMO_ASSERT(fi);
	OSMO_ASSERT(msg);
	OSMO_ASSERT(conn);

	rc = gsm0808_submit_dtap(conn, msg, OBSC_LINKID_CB(msg), 1);
	if (rc != 0) {
		LOGPFSML(fi, LOGL_ERROR, "Tx BSSMAP CLEAR REQUEST to MSC\n");
		resp = gsm0808_create_clear_rqst(GSM0808_CAUSE_EQUIPMENT_FAILURE);
		sigtran_send(conn, resp, fi);
		osmo_fsm_inst_state_chg(fi, ST_ACTIVE, 0, 0);
		return;
	}
}

/* forward MO DTAP from RSL side to BSSAP side */
static void forward_dtap(struct gsm_subscriber_connection *conn, struct msgb *msg, struct osmo_fsm_inst *fi)
{
	struct msgb *resp = NULL;

	OSMO_ASSERT(msg);
	OSMO_ASSERT(conn);

	resp = gsm0808_create_dtap(msg, OBSC_LINKID_CB(msg));
	sigtran_send(conn, resp, fi);
}

/* In case there are open MGCP connections, toss
 * those connections */
static void toss_mgcp_conn(struct gsm_subscriber_connection *conn, struct osmo_fsm_inst *fi)
{
	LOGPFSML(fi, LOGL_ERROR, "tossing all MGCP connections...\n");

	if (conn->user_plane.fi_bts) {
		mgcp_conn_delete(conn->user_plane.fi_bts);
		conn->user_plane.fi_bts = NULL;
	}

	if (conn->user_plane.fi_msc) {
		mgcp_conn_delete(conn->user_plane.fi_msc);
		conn->user_plane.fi_msc = NULL;
	}

	if (conn->user_plane.mgw_endpoint) {
		talloc_free(conn->user_plane.mgw_endpoint);
		conn->user_plane.mgw_endpoint = NULL;
	}
}

static void gscon_fsm_init(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	struct gsm_subscriber_connection *conn = fi->priv;
	struct osmo_scu_prim *scu_prim = NULL;
	struct msgb *msg = NULL;
	int rc;

	switch (event) {
	case GSCON_EV_A_CONN_REQ:
		/* RLL ESTABLISH IND with initial L3 Message */
		msg = data;
		/* FIXME: Extract Mobile ID and update FSM using osmo_fsm_inst_set_id()
		 * i.e. we will probably extract the mobile identity earlier, where the
		 * imsi filter code is. Then we could just use it here.
		 * related: OS#2969 */

		rc = osmo_bsc_sigtran_open_conn(conn, msg);
		if (rc < 0) {
			osmo_fsm_inst_term(fi, OSMO_FSM_TERM_ERROR, NULL);
		} else {
			/* SCCP T(conn est) is 1-2 minutes, way too long. The MS will timeout
			 * using T3210 (20s), T3220 (5s) or T3230 (10s) */
			osmo_fsm_inst_state_chg(fi, ST_WAIT_CC, 20, 993210);
		}
		break;
	case GSCON_EV_A_CONN_IND:
		scu_prim = data;
		if (!conn->sccp.msc) {
			LOGPFSML(fi, LOGL_NOTICE, "N-CONNECT.ind from unknown MSC %s\n",
				 osmo_sccp_addr_dump(&scu_prim->u.connect.calling_addr));
			osmo_sccp_tx_disconn(conn->sccp.msc->a.sccp_user, scu_prim->u.connect.conn_id,
					     &scu_prim->u.connect.called_addr, 0);
			osmo_fsm_inst_term(fi, OSMO_FSM_TERM_REGULAR, NULL);
		}
		/* FIXME: Extract optional IMSI and update FSM using osmo_fsm_inst_set_id()
		 * related: OS2969 (same as above) */

		LOGPFSML(fi, LOGL_NOTICE, "No support for MSC-originated SCCP Connections yet\n");
		osmo_sccp_tx_disconn(conn->sccp.msc->a.sccp_user, scu_prim->u.connect.conn_id,
				     &scu_prim->u.connect.called_addr, 0);
		osmo_fsm_inst_term(fi, OSMO_FSM_TERM_REGULAR, NULL);
		break;
	default:
		OSMO_ASSERT(false);
		break;
	}
}

/* We've sent the CONNECTION.req to the SCCP provider and are waiting for CC from MSC */
static void gscon_fsm_wait_cc(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	switch (event) {
	case GSCON_EV_A_CONN_CFM:
		/* MSC has confirmed the connection, we now change into the
		 * active state and wait there for further operations */
		osmo_fsm_inst_state_chg(fi, ST_ACTIVE, 0, 0);
		/* if there's user payload, forward it just like EV_MT_DTAP */
		/* FIXME: Question: if there's user payload attached to the CC, forward it like EV_MT_DTAP? */
		break;
	default:
		OSMO_ASSERT(false);
		break;
	}
}

#define assignment_failed(fi, cause) \
	_assignment_failed(fi, cause, __FILE__, __LINE__)
static void _assignment_failed(struct osmo_fsm_inst *fi, enum gsm0808_cause cause,
			       const char *file, int line)
{
	struct gsm_subscriber_connection *conn = fi->priv;
	struct msgb *resp = NULL;

	LOGPFSMLSRC(fi, LOGL_ERROR, file, line, "Assignment failed: %s\n", gsm0808_cause_name(cause));

	resp = gsm0808_create_assignment_failure(cause, NULL);
	sigtran_send(conn, resp, fi);
	if (fi->state != ST_ACTIVE)
		osmo_fsm_inst_state_chg(fi, ST_ACTIVE, 0, 0);
}

/* We're on an active subscriber connection, passing DTAP back and forth */
static void gscon_fsm_active(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	struct gsm_subscriber_connection *conn = fi->priv;
	struct msgb *resp = NULL;
	struct mgcp_conn_peer conn_peer;
	int rc;

	switch (event) {
	case GSCON_EV_A_ASSIGNMENT_CMD:
		/* MSC requests us to perform assignment, this code section is
		 * triggered via signal GSCON_EV_A_ASSIGNMENT_CMD from
		 * bssmap_handle_assignm_req() in osmo_bsc_bssap.c, which does
		 * the parsing of incoming assignment requests. */

		LOGPFSML(fi, LOGL_NOTICE, "Channel assignment: chan_mode=%s, full_rate=%i\n",
			 get_value_string(gsm48_chan_mode_names, conn->user_plane.chan_mode),
			 conn->user_plane.full_rate);

		/* FIXME: We need to check if current channel is sufficient. If
		 * yes, do MODIFY. If not, do assignment (see commented lines below) */

		switch (conn->user_plane.chan_mode) {
		case GSM48_CMODE_SPEECH_V1:
		case GSM48_CMODE_SPEECH_EFR:
		case GSM48_CMODE_SPEECH_AMR:
			/* A voice channel is requested, so we run down the
			 * mgcp-ass-mgcp state-chain (see FIXME above) */
			memset(&conn_peer, 0, sizeof(conn_peer));
			conn_peer.call_id = conn->sccp.conn_id;
			osmo_strlcpy(conn_peer.endpoint, ENDPOINT_ID, sizeof(conn_peer.endpoint));

			/* (Pre)Change state and create the connection */
			osmo_fsm_inst_state_chg(fi, ST_WAIT_CRCX_BTS, MGCP_MGW_TIMEOUT, MGCP_MGW_TIMEOUT_TIMER_NR);
			conn->user_plane.fi_bts =
			    mgcp_conn_create(conn->network->mgw.client, fi, GSCON_EV_MGW_FAIL_BTS,
					     GSCON_EV_MGW_CRCX_RESP_BTS, &conn_peer);
			if (!conn->user_plane.fi_bts) {
				assignment_failed(fi, GSM0808_CAUSE_EQUIPMENT_FAILURE);
				return;
			}
			break;
		case GSM48_CMODE_SIGN:
			/* A signalling channel is requested, so we perform the
			 * channel assignment directly without performing any
			 * MGCP actions. ST_WAIT_ASS_CMPL will see by the
			 * conn->user_plane.chan_mode parameter that this
			 * assignment is for a signalling channel and will then
			 * change back to ST_ACTIVE (here) immediately. */
			rc = gsm0808_assign_req(conn, conn->user_plane.chan_mode,
						conn->user_plane.full_rate);
			if (rc != 0) {
				assignment_failed(fi, GSM0808_CAUSE_EQUIPMENT_FAILURE);
				return;
			}

			osmo_fsm_inst_state_chg(fi, ST_WAIT_ASS_CMPL, GSM0808_T10_VALUE, GSM0808_T10_TIMER_NR);
			break;
		default:
			/* An unsupported channel is requested, so we have to
			 * reject this request by sending an assignment failure
			 * message immediately */
			LOGPFSML(fi, LOGL_ERROR, "Requested channel mode is not supported! chan_mode=%s full_rate=%d\n",
				 get_value_string(gsm48_chan_mode_names, conn->user_plane.chan_mode),
				 conn->user_plane.full_rate);

			/* The requested channel mode is not supported  */
			assignment_failed(fi, GSM0808_CAUSE_REQ_CODEC_TYPE_OR_CONFIG_NOT_SUPP);
			break;
		}
		break;
	case GSCON_EV_HO_START:
		rc = bsc_handover_start_gscon(conn);
		if (rc) {
			resp = gsm0808_create_clear_rqst(GSM0808_CAUSE_EQUIPMENT_FAILURE);
			sigtran_send(conn, resp, fi);
			osmo_fsm_inst_state_chg(fi, ST_CLEARING, 0, 0);
			return;
		}

		/* Note: No timeout is set here, T3103 in handover_logic.c
		 * will generate a GSCON_EV_HO_TIMEOUT event should the
		 * handover time out, so we do not need another timeout
		 * here (maybe its worth to think about giving GSCON
		 * more power over the actual handover process). */
		osmo_fsm_inst_state_chg(fi, ST_WAIT_HO_COMPL, 0, 0);
		break;
	case GSCON_EV_A_HO_REQ:
		/* FIXME: reject any handover requests with HO FAIL until implemented */
		break;
	case GSCON_EV_MO_DTAP:
		forward_dtap(conn, (struct msgb *)data, fi);
		break;
	case GSCON_EV_MT_DTAP:
		submit_dtap(conn, (struct msgb *)data, fi);
		break;
	case GSCON_EV_TX_SCCP:
		sigtran_send(conn, (struct msgb *)data, fi);
		break;
	default:
		OSMO_ASSERT(false);
		break;
	}
}

/*
 * Start a new assignment and make sure that it is completed within T10 either
 * positively, negatively or by the timeout.
 *
 *  1.) allocate a new lchan
 *  2.) copy the encryption key and other data from the
 *      old to the new channel.
 *  3.) RSL Channel Activate this channel and wait
 *
 * -> Signal handler for the LCHAN
 *  4.) Send GSM 04.08 assignment command to the MS
 *
 * -> Assignment Complete/Assignment Failure
 *  5.) Release the SDCCH, continue signalling on the new link
 */
static int handle_new_assignment(struct gsm_subscriber_connection *conn, int chan_mode, int full_rate)
{
	struct gsm_lchan *new_lchan;
	enum gsm_chan_t chan_type;

	chan_type = full_rate ? GSM_LCHAN_TCH_F : GSM_LCHAN_TCH_H;


	if (!new_lchan) {
		LOGP(DMSC, LOGL_NOTICE, "No free channel.\n");
		return -1;
	}

	/* copy old data to the new channel */
	memcpy(&new_lchan->encr, &conn->lchan->encr, sizeof(new_lchan->encr));
	new_lchan->ms_power = conn->lchan->ms_power;
	new_lchan->bs_power = conn->lchan->bs_power;
	new_lchan->rqd_ta = conn->lchan->rqd_ta;

	/* copy new data to it */
	new_lchan->tch_mode = chan_mode;
	new_lchan->rsl_cmode = (chan_mode == GSM48_CMODE_SIGN) ?
					RSL_CMOD_SPD_SIGN : RSL_CMOD_SPD_SPEECH;

	/* handle AMR correctly */
	if (chan_mode == GSM48_CMODE_SPEECH_AMR)
		handle_mr_config(conn, new_lchan, full_rate);


	if (rsl_chan_activate_lchan(new_lchan, RSL_ACT_INTRA_NORM_ASS, 0) < 0) {
		LOGP(DHO, LOGL_ERROR, "could not activate channel\n");
		lchan_free(new_lchan);
		return -1;
	}

	/* remember that we have the channel */
	conn->secondary_lchan = new_lchan;
	new_lchan->conn = conn;

	rsl_lchan_set_state(new_lchan, LCHAN_S_ACT_REQ);
	return 0;
}

static void allocate_lchan_done(struct osmo_fsm_inst *fi, enum gsm0808_cause cause_or_zero)
{
	struct gsm_subscriber_connection *conn = fi->priv;

	osmo_fsm_inst_state_chg(fi,
				conn->lchan_alloc.next_state,
				conn->lchan_alloc.timeout_secs,
				conn->lchan_alloc.T);

	if (cause_or_zero) {
		if (conn->lchan_alloc.new_lchan) {
			lchan_free(conn->lchan_alloc.new_lchan);
			conn->lchan_alloc.new_lchan = NULL;
		}
		osmo_fsm_inst_dispatch(fi, conn->lchan_alloc.failure_event, &cause);
	} else
		osmo_fsm_inst_dispatch(fi, conn->lchan_alloc.success_event, NULL);
}

static void allocate_lchan_activate(struct osmo_fsm_inst *fi)
{
	struct gsm_subscriber_connection *conn = fi->priv;
	struct gsm_lchan *new_lchan = conn->lchan_alloc.new_lchan;
	enum gsm48_chan_mode;

	if (!new_lchan) {
		LOGPFSML(fi, LOGL_ERROR, "cannot activate new lchan: it is NULL\n");
		allocate_lchan_done(fi, GSM0808_CAUSE_EQUIPMENT_FAILURE);
	}

	/* copy old data to the new channel */
	if (conn->lchan) {
		memcpy(&new_lchan->encr, &conn->lchan->encr, sizeof(new_lchan->encr));
		new_lchan->ms_power = conn->lchan->ms_power;
		new_lchan->bs_power = conn->lchan->bs_power;
		new_lchan->rqd_ta = conn->lchan->rqd_ta;
	}

	/* copy new data to it */
	chan_mode = conn->lchan_alloc.chan_mode;
	new_lchan->tch_mode = chan_mode;
	new_lchan->rsl_cmode = (chan_mode == GSM48_CMODE_SIGN) ?
					RSL_CMOD_SPD_SIGN : RSL_CMOD_SPD_SPEECH;

	/* handle AMR correctly */
	if (chan_mode == GSM48_CMODE_SPEECH_AMR)
		handle_mr_config(conn, new_lchan, conn->lchan_alloc.full_rate ? 1 : 0);

	if (rsl_chan_activate_lchan(new_lchan, RSL_ACT_INTRA_NORM_ASS, 0) < 0) {
		LOGP(DHO, LOGL_ERROR, "could not activate channel\n");
		lchan_free(new_lchan);
		return -1;
	}

	/* remember that we have the channel */
	conn->secondary_lchan = new_lchan;
	new_lchan->conn = conn;

	rsl_lchan_set_state(new_lchan, LCHAN_S_ACT_REQ);

	allocate_lchan_done(fi, 0);
}

/* If necessary, release PDCH on dynamic TS. The switch-on of PDCH is handled when a voice call ends, not
 * here. Return true if a dyn TS is in PDCH mode, i.e. it is still active and we need to release and
 * wait for the release ack. Return false if the caller can carry on right away.
 */
static bool allocate_lchan_disable_dyn_ts_pdch(struct osmo_fsm_inst *fi)
{
	struct gsm_subscriber_connection *conn = fi->priv;
	struct gsm_lchan *lchan = conn->lchan_alloc.new_lchan;
	bool deactivating_pdch = false;
	int rc = 0;

	switch (lchan->ts->pchan) {
	case GSM_PCHAN_TCH_F_PDCH:
		if (lchan->ts->flags & TS_F_PDCH_ACTIVE) {
			deactivating_pdch = true;
			rc = rsl_ipacc_pdch_activate(lchan->ts, 0);
		}
		break;

	case GSM_PCHAN_TCH_F_TCH_H_PDCH:

		if (lchan->ts->dyn.pchan_is == GSM_PCHAN_PDCH) {
			deactivating_pdch = true;

			if (lchan->ts->dyn.pchan_is != lchan->ts->dyn.pchan_want) {
				LOGPFSML(fi, LOGL_ERROR,
					 "%s cannot deactivate PDCH, dyn TS already in transition\n",
					 gsm_lchan_name(lchan));
				rc = -EINVAL;
				break;
			}

			rc = dyn_ts_switchover_start(lchan->ts, pchan_for_lchant(lchan->type));
		}
		break;

	default:
		break;
	}

	if (!deactivating_pdch)
		return false;

	if (rc) {
		allocate_lchan_done(fi, GSM0808_CAUSE_EQUIPMENT_FAILURE);
		return true;
	}

	osmo_fsm_inst_state_chg(fi,
				ST_WAIT_DYN_TS_SWITCHOVER,
				conn->lchan_alloc.timeout_secs,
				conn->lchan_alloc.T);
}

static void allocate_lchan(struct osmo_fsm_inst *fi,
			   struct gsm_bts *on_bts,
			   enum gsm48_chan_mode chan_mode,
			   bool full_rate,
			   bool differ_from_current_lchan,
			   enum gscon_fsm_states next_state,
			   unsigned int timeout_secs, int T,
			   enum gscon_fsm_event success_event,
			   enum gscon_fsm_event failure_event)
{
	struct gsm_subscriber_connection *conn = fi->priv;
	struct gsm_lchan *new_lchan;
	enum gsm0808_cause cause = GSM0808_CAUSE_EQUIPMENT_FAILURE;
	enum gsm_chan_t chan_type = full_rate ? GSM_LCHAN_TCH_F : GSM_LCHAN_TCH_H;

	if (conn->lchan_alloc.new_lchan) {
		LOGPFSML(fi, LOGL_ERROR, "lchan allocation already busy on %s, cannot start another\n",
			 gsm_lchan_name(conn->lchan_alloc.new_lchan));
		return;
	}

	conn->lchan_alloc.next_state = next_state;
	conn->lchan_alloc.timeout_secs = timeout_secs;
	conn->lchan_alloc.T = T;
	conn->lchan_alloc.success_event = success_event;
	conn->lchan_alloc.failure_event = failure_event;

	conn->lchan_alloc.chan_mode = chan_mode;
	conn->lchan_alloc.full_rate = full_rate;

	switch (chan_mode) {
	case GSM48_CMODE_SPEECH_V1:
	case GSM48_CMODE_SPEECH_EFR:
	case GSM48_CMODE_SPEECH_AMR:
		break;
	default:
		LOGPFSML(fi, LOGL_ERROR, "this code path is not capable of allocating %s\n",
			 get_value_string(gsm48_chan_mode_names, chan_mode));
		goto failure;
	}

	/* About allow_bigger: currently this is only used for TCH, and allow_bigger is about
	 * allocating SDCCH. So just pass 0. */
	new_lchan = lchan_alloc(on_bts, chan_type, 0);

	if (!new_lchan) {
		LOGPFSML(fi, LOGL_NOTICE, "No lchan available for %s\n",
			 gsm_lchant_name(chan_type));
		cause = GSM0808_CAUSE_NO_RADIO_RESOURCE_AVAILABLE;
		goto failure;
	}

	if (differ_from_current_lchan
	    && conn->lchan
	    && conn->lchan->ts->trx->bts == new_lchan->ts->trx->bts
	    && conn->lchan->type == new_lchan->type) {
		LOGPFSML(fi, LOGL_NOTICE,
			 "%s -> %s Will not re-assign to identical channel type,"
			 " %s was requested\n",
			 gsm_lchan_name(conn->lchan), gsm_lchan_name(new_lchan),
			 gsm_lchant_name(chan_type));
		lchan_free(new_lchan);
		cause = GSM0808_CAUSE_NO_RADIO_RESOURCE_AVAILABLE;
		goto failure;
	}

	conn->lchan_alloc.new_lchan = new_lchan;

	if (allocate_lchan_disable_dyn_ts_pdch(fi))
		return;

	allocate_lchan_activate(fi);
	return;

failure:
	osmo_fsm_inst_state_chg(fi, next_state, timeout_secs, T);
	osmo_fsm_inst_dispatch(fi, failure_event, &cause);
}

static void assignment_request(struct osmo_fsm_inst *fi)
{
	struct gsm_subscriber_connection *conn = fi->priv;
	int chan_mode = conn->user_plane.chan_mode;
	int full_rate = conn->user_plane.full_rate;
	enum gsm0808_cause cause = GSM0808_CAUSE_EQUIPMENT_FAILURE;

	if (conn->lchan && chan_compat_with_mode(conn->lchan, chan_mode, full_rate)) {
		/* Already got a compatible lchan, just modify speech */
		if (chan_mode == GSM48_CMODE_SPEECH_AMR)
			handle_mr_config(conn, conn->lchan, full_rate);

		LOGPFSML(fi, LOGL_INFO,
			 "%s: Sending ChanModify for speech: %s\n",
			 gsm_lchan_name(conn->lchan),
			 get_value_string(gsm48_chan_mode_names, chan_mode));

		if (gsm48_lchan_modify(conn->lchan, chan_mode)) {
			LOGPFSML(fi, LOGL_ERROR, "Sending ChanModify failed\n");
			goto error;
		}
		osmo_fsm_inst_state_chg(fi, ST_WAIT_ASS_CMPL, GSM0808_T10_VALUE, GSM0808_T10_TIMER_NR);
		osmo_fsm_inst_dispatch(fi, GSCON_EV_RR_ASS_COMPL, NULL);
		return;
	}

	/* No (suitable) channel available, allocate */
	allocate_lchan(fi, ST_WAIT_ASS_CMPL, GSM0808_T10_VALUE, GSM0808_T10_TIMER_NR,
		       GSCON_EV_RR_ASS_FAIL);
	return;

error:
	osmo_fsm_inst_state_chg(fi, ST_WAIT_ASS_CMPL, GSM0808_T10_VALUE, GSM0808_T10_TIMER_NR);
	osmo_fsm_inst_dispatch(fi, GSCON_EV_RR_ASS_FAIL, &cause);
	return -1;
}


/* Before we may start the channel assignment we need to get an IP/Port for the
 * RTP connection from the MGW */
static void gscon_fsm_wait_crcx_bts(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	struct gsm_subscriber_connection *conn = fi->priv;
	struct mgcp_conn_peer *conn_peer = NULL;
	int rc;

	switch (event) {
	case GSCON_EV_MGW_CRCX_RESP_BTS:
		conn_peer = data;

		/* Check if the MGW has assigned an enpoint to us, otherwise we
		 * can not proceed. */
		if (strlen(conn_peer->endpoint) <= 0) {
			assignment_failed(fi, GSM0808_CAUSE_EQUIPMENT_FAILURE);
			return;
		}

		/* Memorize the endpoint name we got assigned from the MGW.
		 * When the BTS sided connection is done, we need to create
		 * a second connection on that same endpoint, so we need
		 * to know its ID */
		if (!conn->user_plane.mgw_endpoint)
			conn->user_plane.mgw_endpoint = talloc_zero_size(conn, MGCP_ENDPOINT_MAXLEN);
		OSMO_ASSERT(conn->user_plane.mgw_endpoint);
		osmo_strlcpy(conn->user_plane.mgw_endpoint, conn_peer->endpoint, MGCP_ENDPOINT_MAXLEN);

		/* Store the IP-Address and the port the MGW assigned to us,
		 * then start the channel assignment. */
		conn->user_plane.rtp_port = conn_peer->port;
		conn->user_plane.rtp_ip = osmo_ntohl(inet_addr(conn_peer->addr));

		allocate_lchan(fi, ST_WAIT_ASS_CMPL);
		break;
	case GSCON_EV_MO_DTAP:
		forward_dtap(conn, (struct msgb *)data, fi);
		break;
	case GSCON_EV_MT_DTAP:
		submit_dtap(conn, (struct msgb *)data, fi);
		break;
	case GSCON_EV_TX_SCCP:
		sigtran_send(conn, (struct msgb *)data, fi);
		break;
	default:
		OSMO_ASSERT(false);
		break;
	}
}

/* We're waiting for an ASSIGNMENT COMPLETE from MS */
static void gscon_fsm_wait_ass_cmpl(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	struct gsm_subscriber_connection *conn = fi->priv;
	struct gsm_lchan *lchan = conn->lchan;
	struct mgcp_conn_peer conn_peer;
	struct in_addr addr;
	int rc;

	switch (event) {
	case GSCON_EV_RR_ASS_COMPL:
		switch (conn->user_plane.chan_mode) {
		case GSM48_CMODE_SPEECH_V1:
		case GSM48_CMODE_SPEECH_EFR:
		case GSM48_CMODE_SPEECH_AMR:
			/* FIXME: What if we are using SCCP-Lite? */

			/* We are dealing with a voice channel, so we can not
			 * confirm the assignment directly. We must first do
			 * some final steps on the MGCP side. */

			/* Prepare parameters with the information we got during the assignment */
			memset(&conn_peer, 0, sizeof(conn_peer));
			addr.s_addr = osmo_ntohl(lchan->abis_ip.bound_ip);
			osmo_strlcpy(conn_peer.addr, inet_ntoa(addr), sizeof(conn_peer.addr));
			conn_peer.port = lchan->abis_ip.bound_port;

			/* (Pre)Change state and modify the connection */
			osmo_fsm_inst_state_chg(fi, ST_WAIT_MDCX_BTS, MGCP_MGW_TIMEOUT, MGCP_MGW_TIMEOUT_TIMER_NR);
			rc = mgcp_conn_modify(conn->user_plane.fi_bts, GSCON_EV_MGW_MDCX_RESP_BTS, &conn_peer);
			if (rc != 0) {
				assignment_failed(fi, GSM0808_CAUSE_EQUIPMENT_FAILURE);
				return;
			}
			break;
		case GSM48_CMODE_SIGN:
			/* Confirm the successful assignment on BSSMAP and
			 * change back into active state */
			send_ass_compl(lchan, fi, false);
			osmo_fsm_inst_state_chg(fi, ST_ACTIVE, 0, 0);
			break;
		default:
			/* Unsupported modes should have been already filtered
			 * by gscon_fsm_active(). If we reach the default
			 * section here anyway than some unsupported mode must
			 * have made it into the FSM, this would be a bug, so
			 * we fire an assertion here */
			OSMO_ASSERT(false);
			break;
		}

		break;
	case GSCON_EV_RR_ASS_FAIL:
		{
			enum gsm0808_cause cause = GSM0808_CAUSE_RQSTED_TERRESTRIAL_RESOURCE_UNAVAILABLE;
			if (data)
				cause = *((enum gsm0808_cause*)data);
			assignment_failed(fi, cause);
		}
		break;
	case GSCON_EV_MO_DTAP:
		forward_dtap(conn, (struct msgb *)data, fi);
		break;
	case GSCON_EV_MT_DTAP:
		submit_dtap(conn, (struct msgb *)data, fi);
		break;
	case GSCON_EV_TX_SCCP:
		sigtran_send(conn, (struct msgb *)data, fi);
		break;
	default:
		OSMO_ASSERT(false);
		break;
	}
}

/* We are waiting for the MGW response to the MDCX */
static void gscon_fsm_wait_mdcx_bts(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	struct gsm_subscriber_connection *conn = fi->priv;
	struct mgcp_conn_peer conn_peer;
	struct sockaddr_in *sin = NULL;

	switch (event) {
	case GSCON_EV_MGW_MDCX_RESP_BTS:

		/* Prepare parameters with the connection information we got
		 * with the assignment command */
		memset(&conn_peer, 0, sizeof(conn_peer));
		conn_peer.call_id = conn->sccp.conn_id;
		sin = (struct sockaddr_in *)&conn->user_plane.aoip_rtp_addr_remote;
		conn_peer.port = osmo_ntohs(sin->sin_port);
		osmo_strlcpy(conn_peer.addr, inet_ntoa(sin->sin_addr), sizeof(conn_peer.addr));

		/* Make sure we use the same endpoint where we created the
		 * BTS connection. */
		osmo_strlcpy(conn_peer.endpoint, conn->user_plane.mgw_endpoint, sizeof(conn_peer.endpoint));

		/* (Pre)Change state and create the connection */
		osmo_fsm_inst_state_chg(fi, ST_WAIT_CRCX_MSC, MGCP_MGW_TIMEOUT, MGCP_MGW_TIMEOUT_TIMER_NR);
		conn->user_plane.fi_msc =
		    mgcp_conn_create(conn->network->mgw.client, fi, GSCON_EV_MGW_FAIL_MSC, GSCON_EV_MGW_CRCX_RESP_MSC,
				     &conn_peer);
		if (!conn->user_plane.fi_bts) {
			assignment_failed(fi, GSM0808_CAUSE_EQUIPMENT_FAILURE);
			return;
		}

		break;
	case GSCON_EV_MO_DTAP:
		forward_dtap(conn, (struct msgb *)data, fi);
		break;
	case GSCON_EV_MT_DTAP:
		submit_dtap(conn, (struct msgb *)data, fi);
		break;
	case GSCON_EV_TX_SCCP:
		sigtran_send(conn, (struct msgb *)data, fi);
		break;
	default:
		OSMO_ASSERT(false);
		break;
	}
}

static void gscon_fsm_wait_crcx_msc(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	struct gsm_subscriber_connection *conn = fi->priv;
	struct mgcp_conn_peer *conn_peer = NULL;
	struct gsm_lchan *lchan = conn->lchan;
	struct sockaddr_in *sin = NULL;

	switch (event) {
	case GSCON_EV_MGW_CRCX_RESP_MSC:
		conn_peer = data;

		/* Store address information we got in response from the CRCX command. */
		sin = (struct sockaddr_in *)&conn->user_plane.aoip_rtp_addr_local;
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = inet_addr(conn_peer->addr);
		sin->sin_port = osmo_ntohs(conn_peer->port);

		/* Send assignment complete message to the MSC */
		send_ass_compl(lchan, fi, true);

		osmo_fsm_inst_state_chg(fi, ST_ACTIVE, 0, 0);

		break;
	case GSCON_EV_MO_DTAP:
		forward_dtap(conn, (struct msgb *)data, fi);
		break;
	case GSCON_EV_MT_DTAP:
		submit_dtap(conn, (struct msgb *)data, fi);
		break;
	case GSCON_EV_TX_SCCP:
		sigtran_send(conn, (struct msgb *)data, fi);
		break;
	default:
		OSMO_ASSERT(false);
		break;
	}
}

/* We're waiting for a MODE MODIFY ACK from MS + BTS */
static void gscon_fsm_wait_mode_modify_ack(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	struct gsm_subscriber_connection *conn = fi->priv;
	struct gsm_lchan *lchan = conn->lchan;

	switch (event) {
	case GSCON_EV_RR_MODE_MODIFY_ACK:
		/* we assume that not only have we received the RR MODE_MODIFY_ACK, but
		 * actually that also the BTS side of the channel mode has been changed accordingly */
		osmo_fsm_inst_state_chg(fi, ST_ACTIVE, 0, 0);

		/* FIXME: Check if this requires special handling. For now I assume that the send_ass_compl()
		 * can be used. But I am not sure. */
		send_ass_compl(lchan, fi, false);

		break;
		/* FIXME: Do we need to handle DTAP traffic in this state? Maybe yes? Needs to be checked. */
	case GSCON_EV_MO_DTAP:
		forward_dtap(conn, (struct msgb *)data, fi);
		break;
	case GSCON_EV_MT_DTAP:
		submit_dtap(conn, (struct msgb *)data, fi);
		break;
	case GSCON_EV_TX_SCCP:
		sigtran_send(conn, (struct msgb *)data, fi);
		break;
	default:
		OSMO_ASSERT(false);
		break;
	}
}

static void gscon_fsm_clearing(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	struct gsm_subscriber_connection *conn = fi->priv;
	struct msgb *resp;

	switch (event) {
	case GSCON_EV_RSL_CLEAR_COMPL:
		resp = gsm0808_create_clear_complete();
		sigtran_send(conn, resp, fi);
		osmo_fsm_inst_term(fi, OSMO_FSM_TERM_REGULAR, data);
		break;
	default:
		OSMO_ASSERT(false);
		break;
	}
}

/* Wait for the handover logic to tell us whether the handover completed,
 * failed or has timed out */
static void gscon_fsm_wait_ho_compl(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	struct gsm_subscriber_connection *conn = fi->priv;
	struct mgcp_conn_peer conn_peer;
	struct gsm_lchan *lchan = conn->lchan;
	struct in_addr addr;
	struct msgb *resp;
	int rc;

	switch (event) {
	case GSCON_EV_HO_COMPL:
		/* The handover logic informs us that the handover has been
		 * completet. Now we have to tell the MGW the IP/Port on the
		 * new BTS so that the uplink RTP traffic can be redirected
		 * there. */

		/* Prepare parameters with the information we got during the
		 * handover procedure (via IPACC) */
		memset(&conn_peer, 0, sizeof(conn_peer));
		addr.s_addr = osmo_ntohl(lchan->abis_ip.bound_ip);
		osmo_strlcpy(conn_peer.addr, inet_ntoa(addr), sizeof(conn_peer.addr));
		conn_peer.port = lchan->abis_ip.bound_port;

		/* (Pre)Change state and modify the connection */
		osmo_fsm_inst_state_chg(fi, ST_WAIT_MDCX_BTS_HO, MGCP_MGW_TIMEOUT, MGCP_MGW_HO_TIMEOUT_TIMER_NR);
		rc = mgcp_conn_modify(conn->user_plane.fi_bts, GSCON_EV_MGW_MDCX_RESP_BTS, &conn_peer);
		if (rc != 0) {
			resp = gsm0808_create_clear_rqst(GSM0808_CAUSE_EQUIPMENT_FAILURE);
			sigtran_send(conn, resp, fi);
			osmo_fsm_inst_state_chg(fi, ST_CLEARING, 0, 0);
			return;
		}
		break;
	case GSCON_EV_HO_TIMEOUT:
	case GSCON_EV_HO_FAIL:
		/* The handover logic informs us that the handover failed for
		 * some reason. This means the phone stays on the TS/BTS on
		 * which it currently is. We will change back to the active
		 * state again as there are no further operations needed */
		osmo_fsm_inst_state_chg(fi, ST_ACTIVE, 0, 0);
		break;
	default:
		OSMO_ASSERT(false);
		break;
	}
}

/* Wait for the MGW to confirm handover related modification of the connection
 * parameters */
static void gscon_fsm_wait_mdcx_bts_ho(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	struct gsm_subscriber_connection *conn = fi->priv;

	switch (event) {
	case GSCON_EV_MGW_MDCX_RESP_BTS:
		/* The MGW has confirmed the handover MDCX, and the handover
		 * is now also done on the RTP side. We may now change back
		 * to the active state. */
		osmo_fsm_inst_state_chg(fi, ST_ACTIVE, 0, 0);
		break;
	case GSCON_EV_MO_DTAP:
		forward_dtap(conn, (struct msgb *)data, fi);
		break;
	case GSCON_EV_MT_DTAP:
		submit_dtap(conn, (struct msgb *)data, fi);
		break;
	case GSCON_EV_TX_SCCP:
		sigtran_send(conn, (struct msgb *)data, fi);
		break;
	default:
		OSMO_ASSERT(false);
		break;
	}
}

#define EV_TRANSPARENT_SCCP S(GSCON_EV_TX_SCCP) | S(GSCON_EV_MO_DTAP) | S(GSCON_EV_MT_DTAP)

static const struct osmo_fsm_state gscon_fsm_states[] = {
	[ST_INIT] = {
		.name = OSMO_STRINGIFY(INIT),
		.in_event_mask = S(GSCON_EV_A_CONN_REQ) | S(GSCON_EV_A_CONN_IND),
		.out_state_mask = S(ST_WAIT_CC),
		.action = gscon_fsm_init,
	 },
	[ST_WAIT_CC] = {
		.name = OSMO_STRINGIFY(WAIT_CC),
		.in_event_mask = S(GSCON_EV_A_CONN_CFM),
		.out_state_mask = S(ST_ACTIVE),
		.action = gscon_fsm_wait_cc,
	},
	[ST_ACTIVE] = {
		.name = OSMO_STRINGIFY(ACTIVE),
		.in_event_mask = EV_TRANSPARENT_SCCP | S(GSCON_EV_A_ASSIGNMENT_CMD) |
				 S(GSCON_EV_A_HO_REQ) | S(GSCON_EV_HO_START),
		.out_state_mask = S(ST_CLEARING) | S(ST_WAIT_CRCX_BTS) | S(ST_WAIT_ASS_CMPL) |
				  S(ST_WAIT_MODE_MODIFY_ACK) | S(ST_WAIT_MO_HO_CMD) | S(ST_WAIT_HO_COMPL),
		.action = gscon_fsm_active,
	},
	[ST_WAIT_CRCX_BTS] = {
		.name = OSMO_STRINGIFY(WAIT_CRCX_BTS),
		.in_event_mask = EV_TRANSPARENT_SCCP | S(GSCON_EV_MGW_CRCX_RESP_BTS),
		.out_state_mask = S(ST_ACTIVE) | S(ST_WAIT_ASS_CMPL),
		.action = gscon_fsm_wait_crcx_bts,
	},
	[ST_WAIT_ASS_CMPL] = {
		.name = OSMO_STRINGIFY(WAIT_ASS_CMPL),
		.in_event_mask = EV_TRANSPARENT_SCCP | S(GSCON_EV_RR_ASS_COMPL) | S(GSCON_EV_RR_ASS_FAIL),
		.out_state_mask = S(ST_ACTIVE) | S(ST_WAIT_MDCX_BTS),
		.action = gscon_fsm_wait_ass_cmpl,
	},
	[ST_WAIT_MDCX_BTS] = {
		.name = OSMO_STRINGIFY(WAIT_MDCX_BTS),
		.in_event_mask = EV_TRANSPARENT_SCCP | S(GSCON_EV_MGW_MDCX_RESP_BTS),
		.out_state_mask = S(ST_ACTIVE) | S(ST_WAIT_CRCX_MSC),
		.action = gscon_fsm_wait_mdcx_bts,
	},
	[ST_WAIT_CRCX_MSC] = {
		.name = OSMO_STRINGIFY(WAIT_CRCX_MSC),
		.in_event_mask = EV_TRANSPARENT_SCCP | S(GSCON_EV_MGW_CRCX_RESP_MSC),
		.out_state_mask = S(ST_ACTIVE),
		.action = gscon_fsm_wait_crcx_msc,
	},
	[ST_WAIT_MODE_MODIFY_ACK] = {
		.name = OSMO_STRINGIFY(WAIT_MODE_MODIFY_ACK),
		.in_event_mask = EV_TRANSPARENT_SCCP | S(GSCON_EV_RR_MODE_MODIFY_ACK),
		.out_state_mask = S(ST_ACTIVE) | S(ST_CLEARING),
		.action = gscon_fsm_wait_mode_modify_ack,
	},
	[ST_CLEARING] = {
		.name = OSMO_STRINGIFY(CLEARING),
		.in_event_mask = S(GSCON_EV_RSL_CLEAR_COMPL),
		.action = gscon_fsm_clearing,
	 },

	/* TODO: external handover, probably it makes sense to break up the
	 * program flow in handover_logic.c a bit and handle some of the logic
	 * here? */
	[ST_WAIT_MT_HO_ACC] = {
		.name = OSMO_STRINGIFY(WAIT_MT_HO_ACC),
	},
	[ST_WAIT_MT_HO_COMPL] = {
		 .name = OSMO_STRINGIFY(WAIT_MT_HO_COMPL),
	},
	[ST_WAIT_MO_HO_CMD] = {
		.name = OSMO_STRINGIFY(WAIT_MO_HO_CMD),
	},
	[ST_MO_HO_PROCEEDING] = {
		 .name = OSMO_STRINGIFY(MO_HO_PROCEEDING),
	},

	/* Internal handover */
	[ST_WAIT_HO_COMPL] = {
		.name = OSMO_STRINGIFY(WAIT_HO_COMPL),
		.in_event_mask = S(GSCON_EV_HO_COMPL) | S(GSCON_EV_HO_FAIL) | S(GSCON_EV_HO_TIMEOUT),
		.out_state_mask = S(ST_ACTIVE) | S(ST_WAIT_MDCX_BTS_HO) | S(ST_CLEARING),
		.action = gscon_fsm_wait_ho_compl,
	},
	[ST_WAIT_MDCX_BTS_HO] = {
		.name = OSMO_STRINGIFY(WAIT_MDCX_BTS_HO),
		.in_event_mask = EV_TRANSPARENT_SCCP | S(GSCON_EV_MGW_MDCX_RESP_BTS),
		.action = gscon_fsm_wait_mdcx_bts_ho,
		.out_state_mask = S(ST_ACTIVE),
	},
};

static void gscon_fsm_allstate(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	struct gsm_subscriber_connection *conn = fi->priv;
	struct msgb *resp = NULL;

	/* When a connection on the MGW fails, make sure that the reference
	 * in our book-keeping is erased. */
	switch (event) {
	case GSCON_EV_MGW_FAIL_BTS:
		conn->user_plane.fi_bts = NULL;
		break;
	case GSCON_EV_MGW_FAIL_MSC:
		conn->user_plane.fi_msc = NULL;
		break;
	}

	/* Regular allstate event processing */
	switch (event) {
	case GSCON_EV_MGW_FAIL_BTS:
	case GSCON_EV_MGW_FAIL_MSC:
		/* Note: An MGW connection die per definition at any time.
		 * However, if it dies during the assignment we must return
		 * with an assignment failure */
		OSMO_ASSERT(fi->state != ST_INIT && fi->state != ST_WAIT_CC);
		if (fi->state == ST_WAIT_CRCX_BTS || fi->state == ST_WAIT_ASS_CMPL || fi->state == ST_WAIT_MDCX_BTS
		    || fi->state == ST_WAIT_CRCX_MSC) {
			resp = gsm0808_create_assignment_failure(GSM0808_CAUSE_EQUIPMENT_FAILURE, NULL);
			sigtran_send(conn, resp, fi);
			osmo_fsm_inst_state_chg(fi, ST_ACTIVE, 0, 0);
		}
		break;
	case GSCON_EV_A_CLEAR_CMD:
		/* MSC tells us to cleanly shut down */
		osmo_fsm_inst_state_chg(fi, ST_CLEARING, 0, 0);
		gsm0808_clear(conn);
		/* FIXME: Release all terestrial resources in ST_CLEARING */
		/* According to 3GPP 48.008 3.1.9.1. "The BSS need not wait for the radio channel
		 * release to be completed or for the guard timer to expire before returning the
		 * CLEAR COMPLETE message" */

		/* Close MGCP connections */
		toss_mgcp_conn(conn, fi);

		/* FIXME: Question: Is this a hack to force a clear complete from internel?
		 * nobody seems to send the event from outside? */
		osmo_fsm_inst_dispatch(conn->fi, GSCON_EV_RSL_CLEAR_COMPL, NULL);
		break;
	case GSCON_EV_A_DISC_IND:
		/* MSC or SIGTRAN network has hard-released SCCP connection,
		 * terminate the FSM now. */
		osmo_fsm_inst_term(fi, OSMO_FSM_TERM_REGULAR, data);
		break;
	case GSCON_EV_RLL_REL_IND:
		/* BTS reports that one of the LAPDm data links was released */
		/* send proper clear request to MSC */
		LOGPFSML(fi, LOGL_DEBUG, "Tx BSSMAP CLEAR REQUEST to MSC\n");
		resp = gsm0808_create_clear_rqst(GSM0808_CAUSE_RADIO_INTERFACE_MESSAGE_FAILURE);
		sigtran_send(conn, resp, fi);
		break;
	case GSCON_EV_RSL_CONN_FAIL:
		LOGPFSML(fi, LOGL_DEBUG, "Tx BSSMAP CLEAR REQUEST to MSC\n");
		resp = gsm0808_create_clear_rqst(GSM0808_CAUSE_RADIO_INTERFACE_FAILURE);
		sigtran_send(conn, resp, fi);
		break;
	default:
		OSMO_ASSERT(false);
		break;
	}
}

void ho_dtap_cache_flush(struct gsm_subscriber_connection *conn, int send);

static void gscon_cleanup(struct osmo_fsm_inst *fi, enum osmo_fsm_term_cause cause)
{
	struct gsm_subscriber_connection *conn = fi->priv;

	if (conn->ho) {
		LOGPFSML(fi, LOGL_DEBUG, "Releasing handover state\n");
		bsc_clear_handover(conn, 1);
		conn->ho = NULL;
	}

	if (conn->secondary_lchan) {
		LOGPFSML(fi, LOGL_DEBUG, "Releasing secondary_lchan\n");
		lchan_release(conn->secondary_lchan, 0, RSL_REL_LOCAL_END);
		conn->secondary_lchan = NULL;
	}
	if (conn->lchan) {
		LOGPFSML(fi, LOGL_DEBUG, "Releasing lchan\n");
		lchan_release(conn->lchan, 0, RSL_REL_LOCAL_END);
		conn->lchan = NULL;
	}

	if (conn->sccp.state != SUBSCR_SCCP_ST_NONE) {
		LOGPFSML(fi, LOGL_DEBUG, "Disconnecting SCCP\n");
		struct bsc_msc_data *msc = conn->sccp.msc;
		/* FIXME: include a proper cause value / error message? */
		osmo_sccp_tx_disconn(msc->a.sccp_user, conn->sccp.conn_id, &msc->a.bsc_addr, 0);
		conn->sccp.state = SUBSCR_SCCP_ST_NONE;
	}

	/* drop pending messages */
	ho_dtap_cache_flush(conn, 0);

	penalty_timers_free(&conn->hodec2.penalty_timers);

	if (conn->bsub) {
		LOGPFSML(fi, LOGL_DEBUG, "Putting bsc_subscr\n");
		bsc_subscr_put(conn->bsub);
		conn->bsub = NULL;
	}

	llist_del(&conn->entry);
	talloc_free(conn);
	fi->priv = NULL;
}

static void gscon_pre_term(struct osmo_fsm_inst *fi, enum osmo_fsm_term_cause cause)
{
	struct gsm_subscriber_connection *conn = fi->priv;

	/* Make sure all possibly still open MGCP connections get closed */
	toss_mgcp_conn(conn, fi);
}

static int gscon_timer_cb(struct osmo_fsm_inst *fi)
{
	struct gsm_subscriber_connection *conn = fi->priv;
	struct msgb *resp = NULL;

	switch (fi->T) {
	case 993210:
		/* MSC has not responded/confirmed connection with CC, this
		 * could indicate a bad SCCP connection. We now inform the the
		 * FSM that controls the BSSMAP reset about the event. Maybe
		 * a BSSMAP reset is necessary. */
		a_reset_conn_fail(conn->sccp.msc->a.reset);

		/* Since we could not reach the MSC, we give up and terminate
		 * the FSM instance now (N-DISCONNET.req is sent in
		 * gscon_cleanup() above) */
		osmo_fsm_inst_term(fi, OSMO_FSM_TERM_REGULAR, NULL);
		break;
	case GSM0808_T10_TIMER_NR:	/* Assignment Failed */
		resp = gsm0808_create_assignment_failure(GSM0808_CAUSE_RADIO_INTERFACE_FAILURE, NULL);
		sigtran_send(conn, resp, fi);
		osmo_fsm_inst_state_chg(fi, ST_ACTIVE, 0, 0);
		break;
	case MGCP_MGW_TIMEOUT_TIMER_NR:	/* Assignment failed (no response from MGW) */
		resp = gsm0808_create_assignment_failure(GSM0808_CAUSE_EQUIPMENT_FAILURE, NULL);
		sigtran_send(conn, resp, fi);
		osmo_fsm_inst_state_chg(fi, ST_ACTIVE, 0, 0);
		break;
	case MGCP_MGW_HO_TIMEOUT_TIMER_NR:	/* Handover failed (no response from MGW) */
		osmo_fsm_inst_state_chg(fi, ST_ACTIVE, 0, 0);
		break;
	default:
		OSMO_ASSERT(false);
	}
	return 0;
}

static struct osmo_fsm gscon_fsm = {
	.name = "SUBSCR_CONN",
	.states = gscon_fsm_states,
	.num_states = ARRAY_SIZE(gscon_fsm_states),
	.allstate_event_mask = S(GSCON_EV_A_DISC_IND) | S(GSCON_EV_A_CLEAR_CMD) | S(GSCON_EV_RSL_CONN_FAIL) |
	    S(GSCON_EV_RLL_REL_IND) | S(GSCON_EV_MGW_FAIL_BTS) | S(GSCON_EV_MGW_FAIL_MSC),
	.allstate_action = gscon_fsm_allstate,
	.cleanup = gscon_cleanup,
	.pre_term = gscon_pre_term,
	.timer_cb = gscon_timer_cb,
	.log_subsys = DMSC,
	.event_names = gscon_fsm_event_names,
};

/* Allocate a subscriber connection and its associated FSM */
struct gsm_subscriber_connection *bsc_subscr_con_allocate(struct gsm_network *net)
{
	struct gsm_subscriber_connection *conn;
	static bool g_initialized = false;

	if (!g_initialized) {
		osmo_fsm_register(&gscon_fsm);
		g_initialized = true;
	}

	conn = talloc_zero(net, struct gsm_subscriber_connection);
	if (!conn)
		return NULL;

	conn->network = net;
	INIT_LLIST_HEAD(&conn->ho_dtap_cache);
	/* BTW, penalty timers will be initialized on-demand. */
	conn->sccp.conn_id = -1;

	/* don't allocate from 'conn' context, as gscon_cleanup() will call talloc_free(conn) before
	 * libosmocore will call talloc_free(conn->fi), i.e. avoid use-after-free during cleanup */
	conn->fi = osmo_fsm_inst_alloc(&gscon_fsm, net, conn, LOGL_NOTICE, NULL);
	if (!conn->fi) {
		talloc_free(conn);
		return NULL;
	}

	llist_add_tail(&conn->entry, &net->subscr_conns);
	return conn;
}

/*
 * (C) 2017 by sysmocom - s.f.m.c. GmbH <info@sysmocom.de>
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
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <osmocom/core/application.h>

#include <osmocom/bsc/debug.h>
#include <osmocom/bsc/osmo_bsc.h>
#include <osmocom/bsc/signal.h>
#include <osmocom/bsc/bsc_subscriber.h>
#include <osmocom/bsc/bsc_msc_data.h>
#include <osmocom/bsc/osmo_bsc_rf.h>
#include <osmocom/bsc/bss.h>

struct msgb *msgb_from_hex(const char *label, uint16_t size, const char *hex)
{
	struct msgb *msg = msgb_alloc(size, label);
	unsigned char *rc;
	msg->l2h = msg->l3h = msg->head;
	rc = msgb_put(msg, osmo_hexparse(hex, msg->head, msgb_tailroom(msg)));
	OSMO_ASSERT(rc == msg->l2h);
	return msg;
}

uint16_t gl_expect_lac = 0;

/* override, requires '-Wl,--wrap=bsc_grace_paging_request' */
int __real_bsc_grace_paging_request(enum signal_rf rf_policy, struct bsc_subscr *subscr, int chan_needed,
				    struct bsc_msc_data *msc, struct gsm_bts *bts);
int __wrap_bsc_grace_paging_request(enum signal_rf rf_policy, struct bsc_subscr *subscr, int chan_needed,
				    struct bsc_msc_data *msc, struct gsm_bts *bts)
{
	if (subscr->lac == GSM_LAC_RESERVED_ALL_BTS)
		fprintf(stderr, "BSC paging started on entire BSS (%u)\n", subscr->lac);
	else
		fprintf(stderr, "BSC paging started with LAC %u\n", subscr->lac);
	OSMO_ASSERT(gl_expect_lac == subscr->lac);
	return 1; /* pretend one BTS was paged */
}

struct {
	const char *msg;
	uint16_t expect_lac;
	int expect_rc;
} cell_identifier_tests[] = {
	{
		"001652080859512069000743940904010844601a03050065",
		/*                                         ^^^^^^ Cell Identifier List: LAC */
		0x65, 0
	},
	{
		"001452080859512069000743940904010844601a0106",
		/*                                         ^^ Cell Identifier List: BSS */
		GSM_LAC_RESERVED_ALL_BTS, 0
	},
	{
		"001952080859512069000743940904010844601a060415f5490065",
		/*                                         ^^^^^^^^^^^^ Cell Identifier List: LAI */
		GSM_LAC_RESERVED_ALL_BTS, 0
	},
	{
		"001952080859512069000743940904010844601a060400f1100065",
		/*                                         ^^^^^^^^^^^^ Cell Identifier List: LAI */
		0x65, 0
	},
};

struct gsm_network *bsc_gsmnet = NULL;

void test_cell_identifier()
{
	int i;
	int rc;
	struct bsc_msc_data *msc;
	struct gsm_bts *bts;

	bsc_network_alloc();
	bsc_gsmnet->bsc_data->rf_ctrl = talloc_zero(NULL, struct osmo_bsc_rf);
	bsc_gsmnet->bsc_data->rf_ctrl->policy = S_RF_ON;

	msc = talloc_zero(bsc_gsmnet, struct bsc_msc_data);
	msc->network = bsc_gsmnet;

	bts = gsm_bts_alloc_register(bsc_gsmnet, GSM_BTS_TYPE_UNKNOWN, 0);
	if (bts == NULL) {
		fprintf(stderr, "gsm_bts_alloc_register() returned NULL\n");
		return;
	}

	log_set_log_level(osmo_stderr_target, LOGL_DEBUG);

	for (i = 0; i < ARRAY_SIZE(cell_identifier_tests); i++) {
		struct msgb *msg;
		fprintf(stderr, "\n%d:\n", i);
		msg = msgb_from_hex("test_cell_identifier", 1024, cell_identifier_tests[i].msg);

		gl_expect_lac = cell_identifier_tests[i].expect_lac;
		bts->location_area_code = (gl_expect_lac == GSM_LAC_RESERVED_ALL_BTS ? 0 : gl_expect_lac);
		rc = bsc_handle_udt(msc, msg, msgb_l2len(msg));

		fprintf(stderr, "bsc_handle_udt() returned %d\n", rc);
		OSMO_ASSERT(rc == cell_identifier_tests[i].expect_rc);

		msgb_free(msg);
	}
}

static const struct log_info_cat log_categories[] = {
	[DMSC] = {
		.name = "DMSC",
		.description = "Mobile Switching Center",
		.enabled = 1, .loglevel = LOGL_NOTICE,
	},
	[DREF] = {
		.name = "DREF",
		.description = "Reference Counting",
		.enabled = 0, .loglevel = LOGL_DEBUG,
	},
};

static const struct log_info log_info = {
	.cat = log_categories,
	.num_cat = ARRAY_SIZE(log_categories),
};

int main(int argc, char **argv)
{
	void *tall_ctx = talloc_named_const(NULL, 1, "bssap_test");
	msgb_talloc_ctx_init(tall_ctx, 0);
	osmo_init_logging2(tall_ctx, &log_info);
	log_set_use_color(osmo_stderr_target, 0);
	log_set_print_timestamp(osmo_stderr_target, 0);
	log_set_print_filename(osmo_stderr_target, 0);
	log_set_print_category(osmo_stderr_target, 1);

	test_cell_identifier();

	return 0;
}

struct gsm_subscriber_connection *bsc_subscr_con_allocate(struct gsm_network *net) {
	OSMO_ASSERT(0);
}

int bsc_sccplite_rx_ctrl(struct osmo_ss7_asp *asp, struct msgb *msg) {
	OSMO_ASSERT(0);
}

int bsc_sccplite_rx_mgcp(struct osmo_ss7_asp *asp, struct msgb *msg) {
	OSMO_ASSERT(0);
}

int bsc_msg_filter_initial(struct gsm48_hdr *hdr48, size_t hdr48_len,
			struct bsc_filter_request *req,
			int *con_type,
			char **imsi, struct bsc_filter_reject_cause *cause)
{ return 0; }

int bsc_msg_filter_data(struct gsm48_hdr *hdr48, size_t len,
		struct bsc_filter_request *req,
		struct bsc_filter_state *state,
		struct bsc_filter_reject_cause *cause)
{ return 0; }

struct llist_head *bsc_access_lists(void)
{ return NULL; }

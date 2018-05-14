/* Manage identity of neighboring BSS cells for inter-BSC handover.
 *
 * Measurement reports tell us about neighbor ARFCN and BSIC. If that ARFCN and BSIC is not managed by
 * this local BSS, we need to tell the MSC a cell identity, like CGI, LAC+CI, etc. -- hence we need a
 * mapping from ARFCN+BSIC to Cell Identifier List, which needs to be configured by the user.
 */
/* (C) 2018 by sysmocom - s.f.m.c. GmbH <info@sysmocom.de>
 *
 * All Rights Reserved
 *
 * Author: Neels Hofmeyr <nhofmeyr@sysmocom.de>
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

#include <errno.h>

#include <osmocom/core/linuxlist.h>
#include <osmocom/core/utils.h>
#include <osmocom/gsm/gsm0808.h>

#include <osmocom/bsc/neighbor_ident.h>

struct neighbor_ident_list {
	struct llist_head list;
};

struct neighbor_ident {
	struct llist_head entry;

	struct neighbor_ident_key key;
	struct gsm0808_cell_id_list2 val;
};

const char *neighbor_ident_key_name(const struct neighbor_ident_key *ni_key)
{
	static char buf[32];
	switch (ni_key->bsic_kind) {
	default:
	case BSIC_NONE:
		snprintf(buf, sizeof(buf), "ARFCN %u (any BSIC)",
			 ni_key->arfcn);
		break;
	case BSIC_6BIT:
		snprintf(buf, sizeof(buf), "ARFCN %u BSIC %u",
			 ni_key->arfcn, ni_key->bsic & 0x3f);
		break;
	case BSIC_9BIT:
		snprintf(buf, sizeof(buf), "ARFCN %u BSIC %u(9bit)",
			 ni_key->arfcn, ni_key->bsic & 0x1ff);
		break;
	}
	return buf;
}

struct neighbor_ident_list *neighbor_ident_init(void *talloc_ctx)
{
	struct neighbor_ident_list *nil = talloc_zero(talloc_ctx, struct neighbor_ident_list);
	OSMO_ASSERT(nil);
	INIT_LLIST_HEAD(&nil->list);
	return nil;
}

void neighbor_ident_free(struct neighbor_ident_list *nil)
{
	if (!nil)
		return;
	talloc_free(nil);
}

/* Return true when the entry matches the search_for requirements.
 * If exact_bsic_kind is false, a BSIC_NONE entry acts as wildcard to match any search_for on that ARFCN,
 * and a BSIC_NONE in search_for likewise returns any one entry that matches the ARFCN.
 * If exact_bsic_kind is true, only identical bsic_kind values return a match.
 * Note, typically wildcard BSICs are only in entry, e.g. the user configured list, and search_for
 * contains a specific BSIC, e.g. as received from a Measurement Report. */
bool neighbor_ident_key_match(const struct neighbor_ident_key *entry,
			      const struct neighbor_ident_key *search_for,
			      bool exact_bsic_kind)
{
	uint16_t bsic_mask;

	if (entry->arfcn != search_for->arfcn)
		return false;

	switch (entry->bsic_kind) {
	default:
		return false;
	case BSIC_NONE:
		if (!exact_bsic_kind) {
			/* The neighbor identifier list entry matches any BSIC for this ARFCN. */
			return true;
		}
		/* Match exact entry */
		bsic_mask = 0;
		break;
	case BSIC_6BIT:
		bsic_mask = 0x3f;
		break;
	case BSIC_9BIT:
		bsic_mask = 0x1ff;
		break;
	}
	if (!exact_bsic_kind && search_for->bsic_kind == BSIC_NONE) {
		/* The search is looking only for an ARFCN with any BSIC */
		return true;
	}
	if (search_for->bsic_kind == entry->bsic_kind
	    && (search_for->bsic & bsic_mask) == (entry->bsic & bsic_mask))
		return true;
	return false;
}

static struct neighbor_ident *_neighbor_ident_get(const struct neighbor_ident_list *nil,
						  const struct neighbor_ident_key *key,
						  bool exact_bsic_kind)
{
	struct neighbor_ident *ni;
	struct neighbor_ident *wildcard_match = NULL;

	/* Do both exact-bsic and wildcard matching in the same iteration:
	 * Any exact match returns immediately, while for a wildcard match we still go through all
	 * remaining items in case an exact match exists. */
	llist_for_each_entry(ni, &nil->list, entry) {
		if (neighbor_ident_key_match(&ni->key, key, true))
			return ni;
		if (!exact_bsic_kind) {
			if (neighbor_ident_key_match(&ni->key, key, false))
				wildcard_match = ni;
		}
	}
	return wildcard_match;
}

static void _neighbor_ident_free(struct neighbor_ident *ni)
{
	llist_del(&ni->entry);
	talloc_free(ni);
}

/*! Add Cell Identifiers to an ARFCN+BSIC entry.
 * Exactly one kind of identifier is allowed per ARFCN+BSIC entry, and any number of entries of that kind
 * may be added up to the capacity of gsm0808_cell_id_list2, by one or more calls to this function. To
 * replace an existing entry, first call neighbor_ident_del(nil, key).
 * \returns number of entries in the resulting identifier list, or negative on error:
 *   see gsm0808_cell_id_list_add() for the meaning of returned error codes;
 *   return -ENOMEM when the list is not initialized, -ERANGE when the BSIC value is too large. */
int neighbor_ident_add(struct neighbor_ident_list *nil, const struct neighbor_ident_key *key,
		       const struct gsm0808_cell_id_list2 *val)
{
	struct neighbor_ident *ni;
	int rc;

	if (!nil)
		return -ENOMEM;

	switch (key->bsic_kind) {
	case BSIC_6BIT:
		if (key->bsic > 0x3f)
			return -ERANGE;
		break;
	case BSIC_9BIT:
		if (key->bsic > 0x1ff)
			return -ERANGE;
		break;
	default:
		break;
	}

	ni = _neighbor_ident_get(nil, key, true);
	if (!ni) {
		ni = talloc_zero(nil, struct neighbor_ident);
		OSMO_ASSERT(ni);
		*ni = (struct neighbor_ident){
			.key = *key,
			.val = *val,
		};
		llist_add_tail(&ni->entry, &nil->list);
		return ni->val.id_list_len;
	}

	rc = gsm0808_cell_id_list_add(&ni->val, val);

	if (rc < 0)
		return rc;

	return ni->val.id_list_len;
}

/*! Find cell identity for given ARFCN and BSIC, as previously added by neighbor_ident_add().
 */
const struct gsm0808_cell_id_list2 *neighbor_ident_get(const struct neighbor_ident_list *nil,
						       const struct neighbor_ident_key *key)
{
	struct neighbor_ident *ni;
	if (!nil)
		return NULL;
	ni = _neighbor_ident_get(nil, key, false);
	if (!ni)
		return NULL;
	return &ni->val;
}

bool neighbor_ident_del(struct neighbor_ident_list *nil, const struct neighbor_ident_key *key)
{
	struct neighbor_ident *ni;
	if (!nil)
		return false;
	ni = _neighbor_ident_get(nil, key, true);
	if (!ni)
		return false;
	_neighbor_ident_free(ni);
	return true;
}

void neighbor_ident_clear(struct neighbor_ident_list *nil)
{
	struct neighbor_ident *ni;
	while ((ni = llist_first_entry_or_null(&nil->list, struct neighbor_ident, entry)))
		_neighbor_ident_free(ni);
}

void neighbor_ident_iter(const struct neighbor_ident_list *nil,
			 bool (* iter_cb )(const struct neighbor_ident_key *key,
					   const struct gsm0808_cell_id_list2 *val,
					   void *cb_data),
			 void *cb_data)
{
	struct neighbor_ident *ni, *ni_next;
	if (!nil)
		return;
	llist_for_each_entry_safe(ni, ni_next, &nil->list, entry) {
		if (!iter_cb(&ni->key, &ni->val, cb_data))
			return;
	}
}

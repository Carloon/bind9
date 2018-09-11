/*
 * Copyright (C) Internet Systems Consortium, Inc. ("ISC")
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * See the COPYRIGHT file distributed with this work for additional
 * information regarding copyright ownership.
 */

/*! \file */

#include <config.h>

#include <isc/hash.h>
#include <isc/lib.h>
#include <isc/mem.h>
#include <isc/result.h>
#include <isc/util.h>

#include <isccfg/aclconf.h>
#include <isccfg/grammar.h>
#include <isccfg/namedconf.h>

#include <dns/acl.h>
#include <dns/result.h>

#include <ns/client.h>
#include <ns/hooks.h>
#include <ns/log.h>
#include <ns/query.h>

#define CHECK(r) \
	do { \
		result = (r); \
		if (result != ISC_R_SUCCESS) \
			goto cleanup; \
	} while (0)

/*
 * Set up in the register function.
 */
static ns_hook_querydone_t query_done;
static ns_hook_queryrecurse_t query_recurse;
static int module_id;

/*
 * Hook data pool.
 */
static isc_mempool_t *datapool = NULL;

/*
 * Per-client flags set by this module
 */
#define FILTER_AAAA_RECURSING	0x0001	/* Recursing for A */
#define FILTER_AAAA_FILTERED	0x0002	/* AAAA was removed from answer */

/*
 * Client attribute tests.
 */
#define WANTDNSSEC(c)	(((c)->attributes & NS_CLIENTATTR_WANTDNSSEC) != 0)
#define RECURSIONOK(c)	(((c)->query.attributes & \
				  NS_QUERYATTR_RECURSIONOK) != 0)

/*
 * Hook registration structures: pointers to these structures will
 * be added to a hook table when this module is registered.
 */
static bool
filter_qctx_initialize(void *hookdata, void *cbdata, isc_result_t *resp);
static ns_hook_t filter_init = {
	.callback = filter_qctx_initialize,
};

static bool
filter_respond_begin(void *hookdata, void *cbdata, isc_result_t *resp);
static ns_hook_t filter_respbegin = {
	.callback = filter_respond_begin,
};

static bool
filter_respond_any_found(void *hookdata, void *cbdata, isc_result_t *resp);
static ns_hook_t filter_respanyfound = {
	.callback = filter_respond_any_found,
};

static bool
filter_prep_response_begin(void *hookdata, void *cbdata, isc_result_t *resp);
static ns_hook_t filter_prepresp = {
	.callback = filter_prep_response_begin,
};

static bool
filter_query_done_send(void *hookdata, void *cbdata, isc_result_t *resp);
static ns_hook_t filter_donesend = {
	.callback = filter_query_done_send,
};

static bool
filter_qctx_destroy(void *hookdata, void *cbdata, isc_result_t *resp);
ns_hook_t filter_destroy = {
	.callback = filter_qctx_destroy,
};

/**
 ** Support for parsing of parameters and configuration of the module.
 **/

/*
 * Possible values for the settings of filter-aaaa-on-v4 and
 * filter-aaaa-on-v6: "no" is NONE, "yes" is FILTER, "break-dnssec"
 * is BREAK_DNSSEC.
 */
typedef enum {
	NONE = 0,
	FILTER = 1,
	BREAK_DNSSEC = 2
} filter_aaaa_t;

/*
 * Values configured when the module is loaded.
 */
static filter_aaaa_t v4_aaaa = NONE;
static filter_aaaa_t v6_aaaa = NONE;
static dns_acl_t *aaaa_acl = NULL;

/*
 * Support for parsing of parameters.
 */
static const char *filter_aaaa_enums[] = { "break-dnssec", NULL };

static isc_result_t
parse_filter_aaaa(cfg_parser_t *pctx, const cfg_type_t *type, cfg_obj_t **ret) {
	return (cfg_parse_enum_or_other(pctx, type, &cfg_type_boolean, ret));
}

static void
doc_filter_aaaa(cfg_printer_t *pctx, const cfg_type_t *type) {
	cfg_doc_enum_or_other(pctx, type, &cfg_type_boolean);
}

static cfg_type_t cfg_type_filter_aaaa = {
	"filter_aaaa", parse_filter_aaaa, cfg_print_ustring,
	doc_filter_aaaa, &cfg_rep_string, filter_aaaa_enums,
};

static cfg_clausedef_t param_clauses[] = {
	{ "filter-aaaa", &cfg_type_bracketed_aml, 0 },
	{ "filter-aaaa-on-v4", &cfg_type_filter_aaaa, 0 },
	{ "filter-aaaa-on-v6", &cfg_type_filter_aaaa, 0 },
};

static cfg_clausedef_t *param_clausesets[] = {
	param_clauses,
	NULL
};

static cfg_type_t cfg_type_parameters = {
	"filter-aaaa-params", cfg_parse_mapbody, cfg_print_mapbody,
	cfg_doc_mapbody, &cfg_rep_map, param_clausesets
};

static isc_result_t
parse_parameters(const char *parameters, const void *cfg,
		 void *actx, ns_hookctx_t *hctx)
{
	isc_result_t result = ISC_R_SUCCESS;
	cfg_parser_t *parser = NULL;
	cfg_obj_t *param_obj = NULL;
	const cfg_obj_t *obj = NULL;
	isc_buffer_t b;

	CHECK(cfg_parser_create(hctx->mctx, hctx->lctx, &parser));

	isc_buffer_constinit(&b, parameters, strlen(parameters));
	isc_buffer_add(&b, strlen(parameters));
	CHECK(cfg_parse_buffer(parser, &b, &cfg_type_parameters,
			       &param_obj));

	obj = NULL;
	result = cfg_map_get(param_obj, "filter-aaaa-on-v4", &obj);
	if (result == ISC_R_SUCCESS && cfg_obj_isboolean(obj)) {
		if (cfg_obj_asboolean(obj)) {
			v4_aaaa = FILTER;
		} else {
			v4_aaaa = NONE;
		}
	} else if (result == ISC_R_SUCCESS) {
		const char *aaaastr = cfg_obj_asstring(obj);
		if (strcasecmp(aaaastr, "break-dnssec") == 0) {
			v4_aaaa = BREAK_DNSSEC;
		} else {
			return (ISC_R_UNEXPECTED);
		}
	}

	obj = NULL;
	result = cfg_map_get(param_obj, "filter-aaaa-on-v6", &obj);
	if (result == ISC_R_SUCCESS && cfg_obj_isboolean(obj)) {
		if (cfg_obj_asboolean(obj)) {
			v6_aaaa = FILTER;
		} else {
			v6_aaaa = NONE;
		}
	} else if (result == ISC_R_SUCCESS) {
		const char *aaaastr = cfg_obj_asstring(obj);
		if (strcasecmp(aaaastr, "break-dnssec") == 0) {
			v6_aaaa = BREAK_DNSSEC;
		} else {
			return (ISC_R_UNEXPECTED);
		}
	}

	obj = NULL;
	result = cfg_map_get(param_obj, "filter-aaaa", &obj);
	if (result != ISC_R_SUCCESS) {
		CHECK(dns_acl_any(hctx->mctx, &aaaa_acl));
	}
	CHECK(cfg_acl_fromconfig(obj, (const cfg_obj_t *) cfg, hctx->lctx,
				 (cfg_aclconfctx_t *) actx,
				 hctx->mctx, 0, &aaaa_acl));

 cleanup:
	if (param_obj != NULL) {
		cfg_obj_destroy(parser, &param_obj);
	}
	if (parser != NULL) {
		cfg_parser_destroy(&parser);
	}
	return (result);
}

/**
 ** Mandatory hook API functions.
 **/

/*
 * Prototypes for the hook module API functions defined below.
 */
ns_hook_destroy_t hook_destroy;
ns_hook_register_t hook_register;
ns_hook_version_t hook_version;

/*
 * Called by ns_hookmodule_load() to register hook functions into
 * a hook table.
 */
isc_result_t
hook_register(const unsigned int modid, const char *parameters,
	      const char *file, unsigned long line,
	      const void *cfg, void *actx,
	      ns_hookctx_t *hctx, ns_hooktable_t *hooktable)
{
	isc_result_t result;

	module_id = modid;

	/*
	 * Depending on how dlopen() works on the current platform, we
	 * may not have access to named's global namespace, in which
	 * case we need to initialize libisc/libdns/libns. We compare
	 * the address of a global reference variable to its address
	 * in the calling program to determine whether this is necessary.
	 */
	if (hctx->refvar != &isc_bind9) {
		isc_lib_register();
		isc_log_setcontext(hctx->lctx);
		dns_log_setcontext(hctx->lctx);
		ns_log_setcontext(hctx->lctx);
	}

	isc_hash_set_initializer(hctx->hashinit);

	query_done = hctx->query_done;
	query_recurse = hctx->query_recurse;

	if (parameters != NULL) {
		isc_log_write(hctx->lctx, NS_LOGCATEGORY_GENERAL,
			      NS_LOGMODULE_HOOKS, ISC_LOG_INFO,
			      "loading params for 'filter-aaaa' "
			      "module from %s:%lu",
			      file, line);

		CHECK(parse_parameters(parameters, cfg, actx, hctx));
	} else {
		isc_log_write(hctx->lctx, NS_LOGCATEGORY_GENERAL,
			      NS_LOGMODULE_HOOKS, ISC_LOG_INFO,
			      "loading 'filter-aaaa' "
			      "module from %s:%lu, no parameters",
			      file, line);
	}

	ns_hook_add(hooktable, NS_QUERY_QCTX_INITIALIZED, &filter_init);
	ns_hook_add(hooktable, NS_QUERY_RESPOND_BEGIN, &filter_respbegin);
	ns_hook_add(hooktable, NS_QUERY_RESPOND_ANY_FOUND,
		    &filter_respanyfound);
	ns_hook_add(hooktable, NS_QUERY_PREP_RESPONSE_BEGIN, &filter_prepresp);
	ns_hook_add(hooktable, NS_QUERY_DONE_SEND, &filter_donesend);
	ns_hook_add(hooktable, NS_QUERY_QCTX_DESTROYED, &filter_destroy);

	CHECK(isc_mempool_create(hctx->mctx, sizeof(filter_aaaa_t),
				 &datapool));

	/*
	 * Fill the mempool with 1K filter_aaaa state objects at
	 * a time; ideally after a single allocation, the mempool will
	 * have enough to handle all the simultaneous queries the system
	 * requires and it won't be necessary to allocate more.
	 *
	 * We don't set any limit on the number of free state objects
	 * so that they'll always be returned to the pool and not
	 * freed until the pool is destroyed on shutdown.
	 */
	isc_mempool_setfillcount(datapool, 1024);
	isc_mempool_setfreemax(datapool, UINT_MAX);

 cleanup:
	if (result != ISC_R_SUCCESS) {
		if (datapool != NULL) {
			isc_mempool_destroy(&datapool);
		}
	}
	return (result);
}

/*
 * Called by ns_hookmodule_cleanup(); frees memory allocated by
 * the module when it was registered.
 */
void
hook_destroy(void) {
	if (datapool != NULL) {
		isc_mempool_destroy(&datapool);
	}
	if (aaaa_acl != NULL) {
		dns_acl_detach(&aaaa_acl);
	}

	return;
}

/*
 * Returns hook module API version for compatibility checks.
 */
int
hook_version(unsigned int *flags) {
	UNUSED(flags);

	return (NS_HOOK_VERSION);
}

/**
 ** "filter-aaaa" feature implementation begins here
 **/

/*
 * Check whether this is a V4 client.
 */
static bool
is_v4_client(ns_client_t *client) {
	if (isc_sockaddr_pf(&client->peeraddr) == AF_INET)
		return (true);
	if (isc_sockaddr_pf(&client->peeraddr) == AF_INET6 &&
	    IN6_IS_ADDR_V4MAPPED(&client->peeraddr.type.sin6.sin6_addr))
		return (true);
	return (false);
}

/*
 * Check whether this is a V6 client.
 */
static bool
is_v6_client(ns_client_t *client) {
	if (isc_sockaddr_pf(&client->peeraddr) == AF_INET6 &&
	    !IN6_IS_ADDR_V4MAPPED(&client->peeraddr.type.sin6.sin6_addr))
		return (true);
	return (false);
}

/*
 * Shorthand to refer to the persistent stored by this module in
 * the query context structure.
 */
#define QFA ((filter_aaaa_t **) &qctx->hookdata[module_id])

/*
 * Initialize hook data in the query context, fetching from a memory
 * pool.
 */
static bool
filter_qctx_initialize(void *hookdata, void *cbdata, isc_result_t *resp) {
	query_ctx_t *qctx = (query_ctx_t *) hookdata;

	UNUSED(cbdata);

	*QFA = isc_mempool_get(datapool);
	**QFA = NONE;

	*resp = ISC_R_UNSET;
	return (false);
}

/*
 * Determine whether this client should have AAAA filtered nor not,
 * based on the client address family and the settings of
 * filter-aaaa-on-v4 and filter-aaaa-on-v6.
 */
static bool
filter_prep_response_begin(void *hookdata, void *cbdata, isc_result_t *resp) {
	query_ctx_t *qctx = (query_ctx_t *) hookdata;
	isc_result_t result;

	UNUSED(cbdata);

	if (v4_aaaa != NONE || v6_aaaa != NONE) {
		result = ns_client_checkaclsilent(qctx->client, NULL,
						  aaaa_acl, true);
		if (result == ISC_R_SUCCESS &&
		    v4_aaaa != NONE &&
		    is_v4_client(qctx->client))
		{
			**QFA = v4_aaaa;
		} else if (result == ISC_R_SUCCESS &&
			   v6_aaaa != NONE &&
			   is_v6_client(qctx->client))
		{
			**QFA = v6_aaaa;
		}
	}

	*resp = ISC_R_UNSET;
	return (false);
}

/*
 * Hide AAAA rrsets if there is a matching A. Trigger recursion if
 * necessary to find out whether an A exists.
 *
 * (This version is for processing answers to explicit AAAA
 * queries; ANY queries are handled in query_filter_aaaa_any().)
 */
static bool
filter_respond_begin(void *hookdata, void *cbdata, isc_result_t *resp) {
	query_ctx_t *qctx = (query_ctx_t *) hookdata;
	isc_result_t result = ISC_R_UNSET;

	UNUSED(cbdata);

	if (**QFA != BREAK_DNSSEC &&
	    (**QFA != FILTER ||
	     (WANTDNSSEC(qctx->client) && qctx->sigrdataset != NULL &&
	      dns_rdataset_isassociated(qctx->sigrdataset))))
	{
		*resp = result;
		return (false);
	}

	if (qctx->qtype == dns_rdatatype_aaaa) {
		dns_rdataset_t *trdataset;
		trdataset = ns_client_newrdataset(qctx->client);
		result = dns_db_findrdataset(qctx->db, qctx->node,
					     qctx->version,
					     dns_rdatatype_a, 0,
					     qctx->client->now,
					     trdataset, NULL);
		if (dns_rdataset_isassociated(trdataset)) {
			dns_rdataset_disassociate(trdataset);
		}
		ns_client_putrdataset(qctx->client, &trdataset);

		/*
		 * We have an AAAA but if the A is not in our
		 * cache, any result other than DNS_R_DELEGATION
		 * or ISC_R_NOTFOUND means there is no A and
		 * so AAAAs are ok.
		 *
		 * Assume there is no A if we can't recurse
		 * for this client, although that could be
		 * the wrong answer. What else can we do?
		 * Besides, that we have the AAAA and are using
		 * this mechanism suggests that we care more
		 * about As than AAAAs and would have cached
		 * the A if it existed.
		 */
		if (result == ISC_R_SUCCESS) {
			qctx->rdataset->attributes |=
				DNS_RDATASETATTR_RENDERED;
			if (qctx->sigrdataset != NULL &&
			    dns_rdataset_isassociated(qctx->sigrdataset))
			{
				qctx->sigrdataset->attributes |=
					DNS_RDATASETATTR_RENDERED;
			}

			qctx->client->hookflags[module_id] |=
				FILTER_AAAA_FILTERED;
		} else if (qctx->authoritative ||
			   !RECURSIONOK(qctx->client) ||
			   (result != DNS_R_DELEGATION &&
			    result != ISC_R_NOTFOUND))
		{
			qctx->rdataset->attributes &=
				~DNS_RDATASETATTR_RENDERED;
			if (qctx->sigrdataset != NULL &&
			    dns_rdataset_isassociated(qctx->sigrdataset))
			{
				qctx->sigrdataset->attributes &=
					~DNS_RDATASETATTR_RENDERED;
			}
		} else {
			/*
			 * This is an ugly kludge to recurse
			 * for the A and discard the result.
			 *
			 * Continue to add the AAAA now.
			 * We'll make a note to not render it
			 * if the recursion for the A succeeds.
			 */
			result = (*query_recurse)(qctx->client,
						  dns_rdatatype_a,
						  qctx->client->query.qname,
						  NULL, NULL, qctx->resuming);
			if (result == ISC_R_SUCCESS) {
				qctx->client->hookflags[module_id] |=
					FILTER_AAAA_RECURSING;
				qctx->client->query.attributes |=
					NS_QUERYATTR_RECURSING;
			}
		}
	} else if (qctx->qtype == dns_rdatatype_a &&
		   ((qctx->client->hookflags[module_id] &
		     FILTER_AAAA_RECURSING) != 0))
	{

		dns_rdataset_t *mrdataset = NULL;
		dns_rdataset_t *sigrdataset = NULL;

		result = dns_message_findname(qctx->client->message,
					      DNS_SECTION_ANSWER, qctx->fname,
					      dns_rdatatype_aaaa, 0,
					      NULL, &mrdataset);
		if (result == ISC_R_SUCCESS) {
			mrdataset->attributes |= DNS_RDATASETATTR_RENDERED;
		}

		result = dns_message_findname(qctx->client->message,
					      DNS_SECTION_ANSWER, qctx->fname,
					      dns_rdatatype_rrsig,
					      dns_rdatatype_aaaa,
					      NULL, &sigrdataset);
		if (result == ISC_R_SUCCESS) {
			sigrdataset->attributes |= DNS_RDATASETATTR_RENDERED;
		}

		qctx->client->hookflags[module_id] &= ~FILTER_AAAA_RECURSING;

		result = (*query_done)(qctx);

		*resp = result;

		return (true);
	}

	*resp = result;
	return (false);
}

/*
 * When answering an ANY query, remove AAAA if A is present.
 */
static bool
filter_respond_any_found(void *hookdata, void *cbdata, isc_result_t *resp) {
	query_ctx_t *qctx = (query_ctx_t *) hookdata;
	dns_name_t *name = NULL;
	dns_rdataset_t *aaaa = NULL, *aaaa_sig = NULL;
	dns_rdataset_t *a = NULL;
	bool have_a = true;

	UNUSED(cbdata);

	if (**QFA == NONE) {
		*resp = ISC_R_UNSET;
		return (false);
	}

	dns_message_findname(qctx->client->message, DNS_SECTION_ANSWER,
			     (qctx->fname != NULL)
			      ? qctx->fname
			      : qctx->tname,
			     dns_rdatatype_any, 0, &name, NULL);

	/*
	 * If we're not authoritative, just assume there's an
	 * A even if it wasn't in the cache and therefore isn't
	 * in the message.  But if we're authoritative, then
	 * if there was an A, it should be here.
	 */
	if (qctx->authoritative && name != NULL) {
		dns_message_findtype(name, dns_rdatatype_a, 0, &a);
		if (a == NULL) {
			have_a = false;
		}
	}

	if (name != NULL) {
		dns_message_findtype(name, dns_rdatatype_aaaa, 0, &aaaa);
		dns_message_findtype(name, dns_rdatatype_rrsig,
				     dns_rdatatype_aaaa, &aaaa_sig);
	}

	if (have_a && aaaa != NULL &&
	    (aaaa_sig == NULL || !WANTDNSSEC(qctx->client) ||
	     **QFA == BREAK_DNSSEC))
	{
		aaaa->attributes |= DNS_RDATASETATTR_RENDERED;
		if (aaaa_sig != NULL) {
			aaaa_sig->attributes |= DNS_RDATASETATTR_RENDERED;
		}
	}

	*resp = ISC_R_UNSET;
	return (false);
}

/*
 * Hide AAAA rrsets in the additional section if there is a matching A,
 * and hide NS in the additional section if AAAA was filtered in the answer
 * section.
 */
static bool
filter_query_done_send(void *hookdata, void *cbdata, isc_result_t *resp) {
	query_ctx_t *qctx = (query_ctx_t *) hookdata;
	isc_result_t result;

	UNUSED(cbdata);

	if (**QFA == NONE) {
		*resp = ISC_R_UNSET;
		return (false);
	}

	result = dns_message_firstname(qctx->client->message,
				       DNS_SECTION_ADDITIONAL);
	while (result == ISC_R_SUCCESS) {
		dns_name_t *name = NULL;
		dns_rdataset_t *aaaa = NULL, *aaaa_sig = NULL;
		dns_rdataset_t *a = NULL;

		dns_message_currentname(qctx->client->message,
					DNS_SECTION_ADDITIONAL,
					&name);

		result = dns_message_nextname(qctx->client->message,
					      DNS_SECTION_ADDITIONAL);

		dns_message_findtype(name, dns_rdatatype_a, 0, &a);
		if (a == NULL) {
			continue;
		}

		dns_message_findtype(name, dns_rdatatype_aaaa, 0,
				     &aaaa);
		if (aaaa == NULL) {
			continue;
		}

		dns_message_findtype(name, dns_rdatatype_rrsig,
				     dns_rdatatype_aaaa, &aaaa_sig);

		if (aaaa_sig == NULL || !WANTDNSSEC(qctx->client) ||
		    **QFA == BREAK_DNSSEC)
		{
			aaaa->attributes |= DNS_RDATASETATTR_RENDERED;
			if (aaaa_sig != NULL) {
				aaaa_sig->attributes |=
					DNS_RDATASETATTR_RENDERED;
			}
		}
	}

	if ((qctx->client->hookflags[module_id] & FILTER_AAAA_FILTERED) != 0) {
		result = dns_message_firstname(qctx->client->message,
					       DNS_SECTION_AUTHORITY);
		while (result == ISC_R_SUCCESS) {
			dns_name_t *name = NULL;
			dns_rdataset_t *ns = NULL, *ns_sig = NULL;

			dns_message_currentname(qctx->client->message,
						DNS_SECTION_AUTHORITY,
						&name);

			result = dns_message_findtype(name, dns_rdatatype_ns,
						      0, &ns);
			if (result == ISC_R_SUCCESS) {
				ns->attributes |= DNS_RDATASETATTR_RENDERED;
			}

			result = dns_message_findtype(name, dns_rdatatype_rrsig,
						      dns_rdatatype_ns,
						      &ns_sig);
			if (result == ISC_R_SUCCESS) {
				ns_sig->attributes |= DNS_RDATASETATTR_RENDERED;
			}

			result = dns_message_nextname(qctx->client->message,
						      DNS_SECTION_AUTHORITY);
		}
	}

	*resp = ISC_R_UNSET;
	return (false);
}

/*
 * Return hook data to the mempool.
 */
static bool
filter_qctx_destroy(void *hookdata, void *cbdata, isc_result_t *resp) {
	query_ctx_t *qctx = (query_ctx_t *) hookdata;

	UNUSED(cbdata);

	if (*QFA != NULL) {
		isc_mempool_put(datapool, *QFA);
		*QFA = NULL;
	}

	*resp = ISC_R_UNSET;
	return (false);
}
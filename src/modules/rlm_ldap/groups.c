/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or (at
 *   your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file groups.c
 * @brief LDAP module group functions.
 *
 * @author Arran Cudbard-Bell (a.cudbardb@freeradius.org)
 *
 * @copyright 2013 Network RADIUS SAS (legal@networkradius.com)
 * @copyright 2013-2015 The FreeRADIUS Server Project.
 */
RCSID("$Id$")

USES_APPLE_DEPRECATED_API

#include <freeradius-devel/util/debug.h>
#include <freeradius-devel/server/rcode.h>
#include <freeradius-devel/unlang/action.h>

#define LOG_PREFIX "rlm_ldap groups"

#include "rlm_ldap.h"

static char const *null_attrs[] = { NULL };

/** Context to use when resolving group membership from the user object.
 *
 */
typedef struct {
	rlm_ldap_t const	*inst;					//!< Module instance.
	fr_value_box_t		*base_dn;				//!< The base DN to search for groups in.
	fr_ldap_thread_trunk_t	*ttrunk;				//!< Trunk on which to perform additional queries.
	fr_pair_list_t		groups;					//!< Temporary list to hold pairs.
	TALLOC_CTX		*list_ctx;				//!< In which to allocate pairs.
	char			*group_name[LDAP_MAX_CACHEABLE + 1];	//!< List of group names which need resolving.
	unsigned int		name_cnt;				//!< How many names need resolving.
	char			*group_dn[LDAP_MAX_CACHEABLE + 1];	//!< List of group DNs which need resolving.
	char			**dn;					//!< Current DN being resolved.
	char const		*attrs[2];				//!< For resolving name from DN.
	fr_ldap_query_t		*query;					//!< Current query performing group resolution.
} ldap_group_userobj_ctx_t;

/** Context to use when looking up group membership using group objects.
 *
 */
typedef struct {
	rlm_ldap_t const	*inst;					//!< Module instance.
	fr_value_box_t		*base_dn;				//!< The base DN to search for groups in.
	fr_ldap_thread_trunk_t	*ttrunk;				//!< Trunk on which to perform additional queries.
	tmpl_t			*filter_tmpl;				//!< Tmpl to expand into LDAP filter.
	fr_value_box_list_t	expanded_filter;			//!< Values produced by expanding filter xlat.
	char const		*attrs[2];				//!< For retrieving the group name.
	fr_ldap_query_t		*query;					//!< Current query performing group lookup.
	void			*uctx;					//!< Optional context for use in results parsing.
} ldap_group_groupobj_ctx_t;

/** Context to use when evaluating group membership from the user object in an xlat
 *
 */
typedef struct {
	ldap_group_xlat_ctx_t	*xlat_ctx;		//!< Xlat context being evaluated.
	char const			*attrs[2];		//!< For retrieving the group name.
	struct berval			**values;		//!< Values of the membership attribute to check.
	int				count;			//!< How many entries there are in values.
	int				value_no;		//!< The current entry in values being processed.
	char const			*lookup_dn;		//!< The DN currently being looked up, when resolving DN to name.
	char				*group_name;		//!< Result of resolving the provided group DN as to a name.
	fr_ldap_query_t			*query;			//!< Current query doing a DN to name resolution.
	bool				resolving_value;	//!< Is the current query resolving a DN from values.
} ldap_group_userobj_dyn_ctx_t;

/** Cancel a pending group lookup query
 *
 */
static void ldap_group_userobj_cancel(UNUSED request_t *request, UNUSED fr_signal_t action, void *uctx)
{
	ldap_group_userobj_ctx_t	*group_ctx = talloc_get_type_abort(uctx, ldap_group_userobj_ctx_t);

	/*
	 *	If the query is not in flight, just return.
	 */
	if (!group_ctx->query || !(group_ctx->query->treq)) return;

	trunk_request_signal_cancel(group_ctx->query->treq);
}

/** Convert multiple group names into a DNs
 *
 * Given an array of group names, builds a filter matching all names, then retrieves all group objects
 * and stores the DN associated with each group object.
 *
 * @param[out] p_result		The result of trying to resolve a group name to a dn.
 * @param[in] request		Current request.
 * @param[in] uctx		Group lookup context.
 * @return One of the RLM_MODULE_* values.
 */
static unlang_action_t ldap_group_name2dn_start(unlang_result_t *p_result, request_t *request, void *uctx)
{
	ldap_group_userobj_ctx_t	*group_ctx = talloc_get_type_abort(uctx, ldap_group_userobj_ctx_t);
	rlm_ldap_t const		*inst = group_ctx->inst;
	char				**name = group_ctx->group_name;
	char				buffer[LDAP_MAX_GROUP_NAME_LEN + 1];
	char				*filter;

	if (!inst->group.obj_name_attr) {
		REDEBUG("Told to convert group names to DNs but missing 'group.name_attribute' directive");
		RETURN_UNLANG_INVALID;
	}
	if (group_ctx->base_dn->type != FR_TYPE_STRING) {
		REDEBUG("Missing group base_dn");
		RETURN_UNLANG_INVALID;
	}

	RDEBUG2("Converting group name(s) to group DN(s)");

	/*
	 *	It'll probably only save a few ms in network latency, but it means we can send a query
	 *	for the entire group list at once.
	 */
	filter = talloc_typed_asprintf(group_ctx, "%s%s%s",
				 inst->group.obj_filter ? "(&" : "",
				 inst->group.obj_filter ? inst->group.obj_filter : "",
				 group_ctx->group_name[0] && group_ctx->group_name[1] ? "(|" : "");
	while (*name) {
		fr_ldap_uri_escape_func(request, buffer, sizeof(buffer), *name++, NULL);
		filter = talloc_asprintf_append_buffer(filter, "(%s=%s)", inst->group.obj_name_attr, buffer);

		group_ctx->name_cnt++;
	}
	filter = talloc_asprintf_append_buffer(filter, "%s%s",
					       inst->group.obj_filter ? ")" : "",
					       group_ctx->group_name[0] && group_ctx->group_name[1] ? ")" : "");

	return fr_ldap_trunk_search(group_ctx, &group_ctx->query, request, group_ctx->ttrunk,
				    group_ctx->base_dn->vb_strvalue, inst->group.obj_scope, filter,
				    null_attrs, NULL, NULL);
}

/** Process the results of looking up group DNs from names
 *
 * @param[out] p_result		The result of trying to resolve a group name to a dn.
 * @param[in] request		Current request.
 * @param[in] uctx		Group lookup context.
 * @return One of the RLM_MODULE_* values.
 */
static unlang_action_t ldap_group_name2dn_resume(unlang_result_t *p_result, request_t *request, void *uctx)
{
	ldap_group_userobj_ctx_t	*group_ctx = talloc_get_type_abort(uctx, ldap_group_userobj_ctx_t);
	fr_ldap_query_t			*query = talloc_get_type_abort(group_ctx->query, fr_ldap_query_t);
	rlm_ldap_t const		*inst = group_ctx->inst;
	rlm_rcode_t			rcode = RLM_MODULE_OK;
	unsigned int			entry_cnt;
	LDAPMessage			*entry;
	int				ldap_errno;
	char				*dn;
	fr_pair_t			*vp;

	switch (query->ret) {
	case LDAP_RESULT_SUCCESS:
		break;

	case LDAP_RESULT_NO_RESULT:
	case LDAP_RESULT_BAD_DN:
		RDEBUG2("Tried to resolve group name(s) to DNs but got no results");
		goto finish;

	default:
		rcode = RLM_MODULE_FAIL;
		goto finish;
	}

	entry_cnt = ldap_count_entries(query->ldap_conn->handle, query->result);
	if (entry_cnt > group_ctx->name_cnt) {
		REDEBUG("Number of DNs exceeds number of names, group and/or dn should be more restrictive");
		rcode = RLM_MODULE_INVALID;

		goto finish;
	}

	if (entry_cnt < group_ctx->name_cnt) {
		RWDEBUG("Got partial mapping of group names (%i) to DNs (%i), membership information may be incomplete",
			group_ctx->name_cnt, entry_cnt);
	}

	entry = ldap_first_entry(query->ldap_conn->handle, query->result);
	if (!entry) {
		ldap_get_option(query->ldap_conn->handle, LDAP_OPT_RESULT_CODE, &ldap_errno);
		REDEBUG("Failed retrieving entry: %s", ldap_err2string(ldap_errno));

		rcode = RLM_MODULE_FAIL;
		goto finish;
	}

	do {
		dn = ldap_get_dn(query->ldap_conn->handle, entry);
		if (!dn) {
			ldap_get_option(query->ldap_conn->handle, LDAP_OPT_RESULT_CODE, &ldap_errno);
			REDEBUG("Retrieving object DN from entry failed: %s", ldap_err2string(ldap_errno));

			rcode = RLM_MODULE_FAIL;
			goto finish;
		}
		fr_ldap_util_normalise_dn(dn, dn);

		RDEBUG2("Got group DN \"%s\"", dn);
		MEM(vp = fr_pair_afrom_da(group_ctx->list_ctx, inst->group.cache_da));
		fr_pair_value_bstrndup(vp, dn, strlen(dn), true);
		fr_pair_append(&group_ctx->groups, vp);
		ldap_memfree(dn);
	} while((entry = ldap_next_entry(query->ldap_conn->handle, entry)));

finish:
	/*
	 *	Remove pointer to group name to resolve so we don't
	 *	try to do it again
	 */
	*group_ctx->group_name = NULL;
	talloc_free(group_ctx->query);

	RETURN_UNLANG_RCODE(rcode);
}

/** Initiate an LDAP search to turn a group DN into it's name
 *
 * Unlike the inverse conversion of a name to a DN, most LDAP directories don't allow filtering by DN,
 * so we need to search for each DN individually.
 *
 * @param[out] p_result		The result of trying to resolve a dn to a group name..
 * @param[in] request		Current request.
 * @param[in] uctx		The group resolution context.
 * @return One of the RLM_MODULE_* values.
 */
static unlang_action_t ldap_group_dn2name_start(unlang_result_t *p_result, request_t *request, void *uctx)
{
	ldap_group_userobj_ctx_t	*group_ctx = talloc_get_type_abort(uctx, ldap_group_userobj_ctx_t);
	rlm_ldap_t const		*inst = group_ctx->inst;

	if (!inst->group.obj_name_attr) {
		REDEBUG("Told to resolve group DN to name but missing 'group.name_attribute' directive");
		RETURN_UNLANG_INVALID;
	}

	RDEBUG2("Resolving group DN \"%s\" to group name", *group_ctx->dn);

	return fr_ldap_trunk_search(group_ctx, &group_ctx->query, request, group_ctx->ttrunk, *group_ctx->dn,
				    LDAP_SCOPE_BASE, NULL, group_ctx->attrs, NULL, NULL);
}

/** Process the results of a group DN -> name lookup.
 *
 * The retrieved value is added as a value pair to the
 * temporary list in the group resolution context.
 *
 * @param[out] p_result		The result of trying to resolve a dn to a group name.
 * @param[in] request		Current request.
 * @param[in] uctx		The group resolution context.
 * @return One of the RLM_MODULE_* values.
 */
static unlang_action_t ldap_group_dn2name_resume(unlang_result_t *p_result, request_t *request, void *uctx)
{
	ldap_group_userobj_ctx_t	*group_ctx = talloc_get_type_abort(uctx, ldap_group_userobj_ctx_t);
	fr_ldap_query_t			*query = talloc_get_type_abort(group_ctx->query, fr_ldap_query_t);
	rlm_ldap_t const		*inst = group_ctx->inst;
	LDAPMessage			*entry;
	struct berval			**values = NULL;
	int				ldap_errno;
	rlm_rcode_t			rcode = RLM_MODULE_OK;
	fr_pair_t			*vp;

	switch (query->ret) {
	case LDAP_RESULT_SUCCESS:
		break;

	case LDAP_RESULT_NO_RESULT:
	case LDAP_RESULT_BAD_DN:
		REDEBUG("Group DN \"%s\" did not resolve to an object", *group_ctx->dn);
		rcode = (inst->group.allow_dangling_refs ? RLM_MODULE_NOOP : RLM_MODULE_INVALID);
		goto finish;

	default:
		rcode = RLM_MODULE_FAIL;
		goto finish;
	}

	entry = ldap_first_entry(query->ldap_conn->handle, query->result);
	if (!entry) {
		ldap_get_option(query->ldap_conn->handle, LDAP_OPT_RESULT_CODE, &ldap_errno);
		REDEBUG("Failed retrieving entry: %s", ldap_err2string(ldap_errno));
		rcode = RLM_MODULE_INVALID;
		goto finish;
	}

	values = ldap_get_values_len(query->ldap_conn->handle, entry, inst->group.obj_name_attr);
	if (!values) {
		REDEBUG("No %s attributes found in object", inst->group.obj_name_attr);
		rcode = RLM_MODULE_INVALID;
		goto finish;
	}

	MEM(vp = fr_pair_afrom_da(group_ctx->list_ctx, inst->group.cache_da));
	fr_pair_value_bstrndup(vp, values[0]->bv_val, values[0]->bv_len, true);
	fr_pair_append(&group_ctx->groups, vp);
	RDEBUG2("Group DN \"%s\" resolves to name \"%pV\"", *group_ctx->dn, &vp->data);

finish:
	/*
	 *	Walk the pointer to the DN being resolved forward
	 *	ready for the next resolution.
	 */
	group_ctx->dn++;

	if (values) ldap_value_free_len(values);
	talloc_free(query);

	RETURN_UNLANG_RCODE(rcode);
}

/** Move user object group attributes to the control list
 *
 * @param p_result	The result of adding user object group attributes
 * @param request	Current request.
 * @param group_ctx	Context used to evaluate group attributes
 * @return RLM_MODULE_OK
 */
static unlang_action_t ldap_cacheable_userobj_store(unlang_result_t *p_result, request_t *request,
						    ldap_group_userobj_ctx_t *group_ctx)
{
	fr_pair_t		*vp;
	fr_pair_list_t		*list;

	list = tmpl_list_head(request, request_attr_control);
	fr_assert(list != NULL);

	RDEBUG2("Adding cacheable user object memberships");
	RINDENT();
	if (RDEBUG_ENABLED) {
		for (vp = fr_pair_list_head(&group_ctx->groups);
		     vp;
		     vp = fr_pair_list_next(&group_ctx->groups, vp)) {
			RDEBUG2("control.%s += \"%pV\"", group_ctx->inst->group.cache_da->name, &vp->data);
		}
	}

	fr_pair_list_append(list, &group_ctx->groups);
	REXDENT();

	talloc_free(group_ctx);
	RETURN_UNLANG_OK;
}

/** Initiate DN to name and name to DN group lookups
 *
 * Called repeatedly until there are no more lookups to perform
 * or an unresolved lookup causes the module to fail.
 *
 * @param p_result	The result of the previous expansion.
 * @param request	Current request.
 * @param uctx		The group context being processed.
 * @return One of the RLM_MODULE_* values.
 */
static unlang_action_t ldap_cacheable_userobj_resolve(unlang_result_t *p_result, request_t *request, void *uctx)
{
	ldap_group_userobj_ctx_t	*group_ctx = talloc_get_type_abort(uctx, ldap_group_userobj_ctx_t);

	/*
	 *	If we've previously failed to expand, fail the group section
	 */
	switch (p_result->rcode) {
	case RLM_MODULE_FAIL:
	case RLM_MODULE_INVALID:
		talloc_free(group_ctx);
		return UNLANG_ACTION_CALCULATE_RESULT;
	default:
		break;
	}

	/*
	 *	Are there any DN to resolve to names?
	 *	These are resolved one at a time as most directories don't allow for
	 *	filters on the DN.
	 */
	if (*group_ctx->dn) {
		if (unlang_function_repeat_set(request, ldap_cacheable_userobj_resolve) < 0) RETURN_UNLANG_FAIL;
		if (unlang_function_push_with_result(/* both start and resume provide an rcode */p_result, request,
						     ldap_group_dn2name_start,
						     ldap_group_dn2name_resume,
						     ldap_group_userobj_cancel, ~FR_SIGNAL_CANCEL,
						     UNLANG_SUB_FRAME,
						     group_ctx) < 0) RETURN_UNLANG_FAIL;
		return UNLANG_ACTION_PUSHED_CHILD;
	}

	/*
	 *	Are there any names to resolve to DN?
	 */
	if (*group_ctx->group_name) {
		if (unlang_function_repeat_set(request, ldap_cacheable_userobj_resolve) < 0) RETURN_UNLANG_FAIL;
		if (unlang_function_push_with_result(/* both start and resume provide an rcode */p_result, request,
						     ldap_group_name2dn_start,
						     ldap_group_name2dn_resume,
						     ldap_group_userobj_cancel, ~FR_SIGNAL_CANCEL,
						     UNLANG_SUB_FRAME,
						     group_ctx) < 0) RETURN_UNLANG_FAIL;
		return UNLANG_ACTION_PUSHED_CHILD;
	}

	/*
	 *	Nothing left to resolve, move the resulting attributes to
	 *	the control list.
	 */
	return ldap_cacheable_userobj_store(p_result, request, group_ctx);
}

/** Convert group membership information into attributes
 *
 * This may just be able to parse attribute values in the user object
 * or it may need to yield to other LDAP searches depending on what was
 * returned and what is set to be cached.
 *
 * @param[out] p_result		The result of trying to resolve a dn to a group name.
 * @param[in] request		Current request.
 * @param[in] autz_ctx		LDAP authorization context being processed.
 * @param[in] attr		membership attribute to look for in the entry.
 * @return One of the RLM_MODULE_* values.
 */
unlang_action_t rlm_ldap_cacheable_userobj(unlang_result_t *p_result, request_t *request, ldap_autz_ctx_t *autz_ctx,
					   char const *attr)
{
	rlm_ldap_t const		*inst = autz_ctx->inst;
	LDAPMessage			*entry = autz_ctx->entry;
	fr_ldap_thread_trunk_t		*ttrunk = autz_ctx->ttrunk;
	ldap_group_userobj_ctx_t	*group_ctx;
	struct berval			**values;
	char				**name_p;
	char				**dn_p;
	fr_pair_t			*vp;
	int				is_dn, i, count, name2dn = 0, dn2name = 0;

	fr_assert(entry);
	fr_assert(attr);

	/*
	 *	Parse the membership information we got in the initial user query.
	 */
	values = ldap_get_values_len(fr_ldap_handle_thread_local(), entry, attr);
	if (!values) {
		RDEBUG2("No cacheable group memberships found in user object");

		RETURN_UNLANG_OK;
	}
	count = ldap_count_values_len(values);

	/*
	 *	Set up context for managing group membership attribute resolution.
	 */
	MEM(group_ctx = talloc_zero(unlang_interpret_frame_talloc_ctx(request), ldap_group_userobj_ctx_t));
	group_ctx->inst = inst;
	group_ctx->ttrunk = ttrunk;
	group_ctx->base_dn = &autz_ctx->call_env->group_base;
	group_ctx->list_ctx = tmpl_list_ctx(request, request_attr_control);
	fr_assert(group_ctx->list_ctx != NULL);

	/*
	 *	Set up pointers to entries in arrays of names / DNs to resolve.
	 */
	name_p = group_ctx->group_name;
	group_ctx->dn = dn_p = group_ctx->group_dn;

	/*
	 *	Temporary list to hold new group VPs, will be merged
	 *	once all group info has been gathered/resolved
	 *	successfully.
	 */
	fr_pair_list_init(&group_ctx->groups);

	for (i = 0; (i < count); i++) {
		is_dn = fr_ldap_util_is_dn(values[i]->bv_val, values[i]->bv_len);

		if (inst->group.cacheable_dn) {
			/*
			 *	The easy case, we're caching DNs and we got a DN.
			 */
			if (is_dn) {
				MEM(vp = fr_pair_afrom_da(group_ctx->list_ctx, inst->group.cache_da));
				fr_pair_value_bstrndup(vp, values[i]->bv_val, values[i]->bv_len, true);
				fr_pair_append(&group_ctx->groups, vp);
			/*
			 *	We were told to cache DNs but we got a name, we now need to resolve
			 *	this to a DN. Store all the group names in an array so we can do one query.
			 */
			} else {
				if (++name2dn > LDAP_MAX_CACHEABLE) {
					REDEBUG("Too many groups require name to DN resolution");
				invalid:
					ldap_value_free_len(values);
					talloc_free(group_ctx);
					RETURN_UNLANG_INVALID;
				}
				*name_p++ = fr_ldap_berval_to_string(group_ctx, values[i]);
			}
		}

		if (inst->group.cacheable_name) {
			/*
			 *	The easy case, we're caching names and we got a name.
			 */
			if (!is_dn) {
				MEM(vp = fr_pair_afrom_da(group_ctx->list_ctx, inst->group.cache_da));
				fr_pair_value_bstrndup(vp, values[i]->bv_val, values[i]->bv_len, true);
				fr_pair_append(&group_ctx->groups, vp);
			/*
			 *	We were told to cache names but we got a DN, we now need to resolve
			 *	this to a name.  Store group DNs which need resolving to names.
			 */
			} else {
				if (++dn2name > LDAP_MAX_CACHEABLE) {
					REDEBUG("Too many groups require DN to name resolution");
					goto invalid;
				}
				*dn_p++ = fr_ldap_berval_to_string(group_ctx, values[i]);
			}
		}
	}

	ldap_value_free_len(values);

	/*
	 *	We either have group names which need converting to DNs or
	 *	DNs which need resolving to names.  Push a function which will
	 *	do the resolution.
	 */
	if ((name_p != group_ctx->group_name) || (dn_p != group_ctx->group_dn)) {
		group_ctx->attrs[0] = inst->group.obj_name_attr;
		if (unlang_function_push_with_result(p_result, request,
						     ldap_cacheable_userobj_resolve,
						     NULL,
						     ldap_group_userobj_cancel,
						     ~FR_SIGNAL_CANCEL, UNLANG_SUB_FRAME,
						     group_ctx) < 0) {
			talloc_free(group_ctx);
			RETURN_UNLANG_FAIL;
		}
		return UNLANG_ACTION_PUSHED_CHILD;
	}

	/*
	 *	No additional queries needed, just process the context to
	 *	move any generated pairs into the correct list.
	 */
	return ldap_cacheable_userobj_store(p_result, request, group_ctx);
}

/** Initiate an LDAP search for group membership looking at the group objects
 *
 * @param[out] p_result		Result of submitting LDAP search
 * @param[in] request		Current request.
 * @param[in] uctx		Group lookup context.
 * @return One of the RLM_MODULE_* values.
 */
static unlang_action_t ldap_cacheable_groupobj_start(unlang_result_t *p_result, request_t *request, void *uctx)
{
	ldap_group_groupobj_ctx_t	*group_ctx = talloc_get_type_abort(uctx, ldap_group_groupobj_ctx_t);
	rlm_ldap_t const		*inst = group_ctx->inst;
	fr_value_box_t			*filter;

	filter = fr_value_box_list_head(&group_ctx->expanded_filter);

	if (!filter || filter->type != FR_TYPE_STRING) RETURN_UNLANG_FAIL;

	group_ctx->attrs[0] = inst->group.obj_name_attr;
	return fr_ldap_trunk_search(group_ctx, &group_ctx->query, request, group_ctx->ttrunk,
				    group_ctx->base_dn->vb_strvalue, inst->group.obj_scope,
				    filter->vb_strvalue, group_ctx->attrs, NULL, NULL);
}

/** Cancel a pending group object lookup.
 *
 */
static void ldap_group_groupobj_cancel(UNUSED request_t *request, UNUSED fr_signal_t action, void *uctx)
{
	ldap_group_groupobj_ctx_t	*group_ctx = talloc_get_type_abort(uctx, ldap_group_groupobj_ctx_t);

	/*
	 *	If the query is not in flight, just return
	 */
	if (!group_ctx->query || !group_ctx->query->treq) return;

	trunk_request_signal_cancel(group_ctx->query->treq);
}

/** Process the results of a group object lookup.
 *
 * @param[out] p_result		Result of processing group lookup.
 * @param[in] request		Current request.
 * @param[in] uctx		Group lookup context.
 * @return One of the RLM_MODULE_* values.
 */
static unlang_action_t ldap_cacheable_groupobj_resume(unlang_result_t *p_result, request_t *request, void *uctx)
{
	ldap_group_groupobj_ctx_t	*group_ctx = talloc_get_type_abort(uctx, ldap_group_groupobj_ctx_t);
	rlm_ldap_t const		*inst = group_ctx->inst;
	fr_ldap_query_t			*query = group_ctx->query;
	rlm_rcode_t			rcode = RLM_MODULE_OK;
	LDAPMessage			*entry;
	int				ldap_errno;
	char				*dn;
	fr_pair_t			*vp;

	switch (query->ret) {
	case LDAP_SUCCESS:
		break;

	case LDAP_RESULT_NO_RESULT:
	case LDAP_RESULT_BAD_DN:
		RDEBUG2("No cacheable group memberships found in group objects");
		rcode = RLM_MODULE_NOTFOUND;
		goto finish;

	default:
		rcode = RLM_MODULE_FAIL;
		goto finish;
	}

	entry = ldap_first_entry(query->ldap_conn->handle, query->result);
	if (!entry) {
		ldap_get_option(query->ldap_conn->handle, LDAP_OPT_RESULT_CODE, &ldap_errno);
		REDEBUG("Failed retrieving entry: %s", ldap_err2string(ldap_errno));

		goto finish;
	}

	RDEBUG2("Adding cacheable group object memberships");
	do {
		if (inst->group.cacheable_dn) {
			dn = ldap_get_dn(query->ldap_conn->handle, entry);
			if (!dn) {
				ldap_get_option(query->ldap_conn->handle, LDAP_OPT_RESULT_CODE, &ldap_errno);
				REDEBUG("Retrieving object DN from entry failed: %s", ldap_err2string(ldap_errno));

				goto finish;
			}
			fr_ldap_util_normalise_dn(dn, dn);

			MEM(pair_append_control(&vp, inst->group.cache_da) == 0);
			fr_pair_value_strdup(vp, dn, false);

			RINDENT();
			RDEBUG2("control.%pP", vp);
			REXDENT();
			ldap_memfree(dn);
		}

		if (inst->group.cacheable_name) {
			struct berval **values;

			values = ldap_get_values_len(query->ldap_conn->handle, entry, inst->group.obj_name_attr);
			if (!values) continue;

			MEM(pair_append_control(&vp, inst->group.cache_da) == 0);
			fr_pair_value_bstrndup(vp, values[0]->bv_val, values[0]->bv_len, true);

			RINDENT();
			RDEBUG2("control.%pP", vp);
			REXDENT();

			ldap_value_free_len(values);
		}
	} while ((entry = ldap_next_entry(query->ldap_conn->handle, entry)));

finish:
	talloc_free(group_ctx);

	RETURN_UNLANG_RCODE(rcode);
}

/** Convert group membership information into attributes
 *
 * @param[out] p_result		The result of trying to resolve a dn to a group name.
 * @param[in] request		Current request.
 * @param[in] autz_ctx		Authentication context being processed.
 * @return One of the RLM_MODULE_* values.
 */
unlang_action_t rlm_ldap_cacheable_groupobj(unlang_result_t *p_result, request_t *request, ldap_autz_ctx_t *autz_ctx)
{
	rlm_ldap_t const		*inst = autz_ctx->inst;
	ldap_group_groupobj_ctx_t	*group_ctx;

	if (!inst->group.obj_membership_filter) {
		RDEBUG2("Skipping caching group objects as directive 'group.membership_filter' is not set");
		RETURN_UNLANG_OK;
	}

	if (autz_ctx->call_env->group_base.type != FR_TYPE_STRING) {
		REDEBUG("Missing group base_dn");
		RETURN_UNLANG_INVALID;
	}

	MEM(group_ctx = talloc_zero(unlang_interpret_frame_talloc_ctx(request), ldap_group_groupobj_ctx_t));
	group_ctx->inst = inst;
	group_ctx->ttrunk = autz_ctx->ttrunk;
	group_ctx->base_dn = &autz_ctx->call_env->group_base;
	fr_value_box_list_init(&group_ctx->expanded_filter);

	if (unlang_function_push_with_result(p_result,
					     request,
					     ldap_cacheable_groupobj_start,
					     ldap_cacheable_groupobj_resume,
					     ldap_group_groupobj_cancel, ~FR_SIGNAL_CANCEL,
					     UNLANG_SUB_FRAME,
					     group_ctx) < 0) {
	error:
		talloc_free(group_ctx);
		RETURN_UNLANG_FAIL;
	}

	if (unlang_tmpl_push(group_ctx, NULL, &group_ctx->expanded_filter, request, autz_ctx->call_env->group_filter, NULL, UNLANG_SUB_FRAME) < 0) goto error;

	return UNLANG_ACTION_PUSHED_CHILD;
}

/** Process the results of a group object lookup.
 *
 * @param[out] p_result		Result of processing group lookup.
 * @param[in] request		Current request.
 * @param[in] uctx		Group lookup context.
 * @return One of the RLM_MODULE_* values.
 */
static unlang_action_t ldap_check_groupobj_resume(unlang_result_t *p_result, request_t *request, void *uctx)
{
	ldap_group_groupobj_ctx_t	*group_ctx = talloc_get_type_abort(uctx, ldap_group_groupobj_ctx_t);
	ldap_group_xlat_ctx_t		*xlat_ctx = talloc_get_type_abort(group_ctx->uctx, ldap_group_xlat_ctx_t);
	fr_ldap_query_t			*query = group_ctx->query;
	rlm_rcode_t			rcode = RLM_MODULE_OK;

	switch (query->ret) {
	case LDAP_SUCCESS:
		xlat_ctx->found = true;
		if (RDEBUG_ENABLED2) {
			LDAPMessage	*entry = NULL;
			char		*dn = NULL;
			entry = ldap_first_entry(query->ldap_conn->handle, query->result);
			if (entry) {
				dn = ldap_get_dn(query->ldap_conn->handle, entry);
				RDEBUG2("User found in group object \"%pV\"", fr_box_strvalue(dn));
				ldap_memfree(dn);
			}
		}
		break;

	case LDAP_RESULT_NO_RESULT:
	case LDAP_RESULT_BAD_DN:
		rcode = RLM_MODULE_NOTFOUND;
		break;

	default:
		rcode = RLM_MODULE_FAIL;
		break;
	}

	talloc_free(group_ctx);
	RETURN_UNLANG_RCODE(rcode);
}

/** Initiate an LDAP search to determine group membership, querying group objects
 *
 * Used by LDAP group membership xlat
 *
 * @param p_result	Current module result code.
 * @param request	Current request.
 * @param xlat_ctx	xlat context being processed.
 */
unlang_action_t rlm_ldap_check_groupobj_dynamic(unlang_result_t *p_result, request_t *request,
						ldap_group_xlat_ctx_t *xlat_ctx)
{
	rlm_ldap_t const		*inst = xlat_ctx->inst;
	ldap_group_groupobj_ctx_t	*group_ctx;

	MEM(group_ctx = talloc(unlang_interpret_frame_talloc_ctx(request), ldap_group_groupobj_ctx_t));
	*group_ctx = (ldap_group_groupobj_ctx_t) {
		.inst = inst,
		.ttrunk = xlat_ctx->ttrunk,
		.uctx = xlat_ctx
	};
	fr_value_box_list_init(&group_ctx->expanded_filter);

	if (fr_ldap_util_is_dn(xlat_ctx->group->vb_strvalue, xlat_ctx->group->vb_length)) {
		group_ctx->filter_tmpl = xlat_ctx->env_data->group_filter;
		group_ctx->base_dn = xlat_ctx->group;
	} else {
		char		name_filter[LDAP_MAX_FILTER_STR_LEN];
		char const	*filters[] = { name_filter, inst->group.obj_filter, inst->group.obj_membership_filter };
		tmpl_rules_t	t_rules;

		if (!inst->group.obj_name_attr) {
			REDEBUG("Told to search for group by name, but missing 'group.name_attribute' "
				"directive");
		invalid:
			talloc_free(group_ctx);
			RETURN_UNLANG_INVALID;
		}

		t_rules = (tmpl_rules_t){
			.attr = {
				.dict_def = request->proto_dict,
				.list_def = request_attr_request,
			},
			.xlat = {
				.runtime_el = unlang_interpret_event_list(request),
			},
			.at_runtime = true,
			.escape.box_escape = (fr_value_box_escape_t) {
				.func = fr_ldap_box_escape,
				.safe_for = (fr_value_box_safe_for_t)fr_ldap_box_escape,
				.always_escape = false,
			},
			.escape.mode = TMPL_ESCAPE_PRE_CONCAT,
			.literals_safe_for = (fr_value_box_safe_for_t)fr_ldap_box_escape,
			.cast = FR_TYPE_STRING,
		};

		snprintf(name_filter, sizeof(name_filter), "(%s=%s)",
			 inst->group.obj_name_attr, xlat_ctx->group->vb_strvalue);

		if (fr_ldap_filter_to_tmpl(group_ctx, &t_rules, filters, NUM_ELEMENTS(filters),
					   &group_ctx->filter_tmpl) < 0) goto invalid;

		fr_assert(xlat_ctx->env_data);
		group_ctx->base_dn = &xlat_ctx->env_data->group_base;
	}

	if (unlang_function_push_with_result(p_result,
					     request,
					     ldap_cacheable_groupobj_start,
					     ldap_check_groupobj_resume,
					     ldap_group_groupobj_cancel, ~FR_SIGNAL_CANCEL,
					     UNLANG_SUB_FRAME,
					     group_ctx) < 0) {
	error:
		talloc_free(group_ctx);
		RETURN_UNLANG_FAIL;
	}

	if (unlang_tmpl_push(group_ctx, NULL, &group_ctx->expanded_filter, request, group_ctx->filter_tmpl, NULL, UNLANG_SUB_FRAME) < 0) goto error;

	return UNLANG_ACTION_PUSHED_CHILD;
}

/** Initiate resolving a group DN to its name
 *
 */
static unlang_action_t ldap_dn2name_start(unlang_result_t *p_result, request_t *request, void *uctx)
{
	ldap_group_userobj_dyn_ctx_t	*group_ctx = talloc_get_type_abort(uctx, ldap_group_userobj_dyn_ctx_t);
	ldap_group_xlat_ctx_t		*xlat_ctx = group_ctx->xlat_ctx;
	rlm_ldap_t const		*inst = xlat_ctx->inst;

	if (!inst->group.obj_name_attr) {
		REDEBUG("Told to resolve group DN to name but missing 'group.name_attribute' directive");
		RETURN_UNLANG_INVALID;
	}

	RDEBUG2("Resolving group DN \"%pV\" to group name", fr_box_strvalue_buffer(group_ctx->lookup_dn));

	return fr_ldap_trunk_search(group_ctx, &group_ctx->query, request, xlat_ctx->ttrunk,
				    group_ctx->lookup_dn, LDAP_SCOPE_BASE, NULL, group_ctx->attrs,
				    NULL, NULL);
}

/** Cancel an in-progress DN to name lookup.
 *
 */
static void ldap_dn2name_cancel(UNUSED request_t *request, UNUSED fr_signal_t action, void *uctx)
{
	ldap_group_userobj_dyn_ctx_t	*group_ctx = talloc_get_type_abort(uctx, ldap_group_userobj_dyn_ctx_t);

	if (!group_ctx->query || !group_ctx->query->treq) return;

	trunk_request_signal_cancel(group_ctx->query->treq);
}

/** Initiate a user lookup to check membership.
 *
 * Used when the user's DN is already known but cached group membership has not been stored
 *
 */
static unlang_action_t ldap_check_userobj_start(UNUSED unlang_result_t *p_result,
						request_t *request, void *uctx)
{
	ldap_group_userobj_dyn_ctx_t	*group_ctx = talloc_get_type_abort(uctx, ldap_group_userobj_dyn_ctx_t);
	ldap_group_xlat_ctx_t	*xlat_ctx = talloc_get_type_abort(group_ctx->xlat_ctx, ldap_group_xlat_ctx_t);

	return fr_ldap_trunk_search(xlat_ctx, &xlat_ctx->query, request, xlat_ctx->ttrunk, xlat_ctx->dn,
				    LDAP_SCOPE_BASE, NULL, xlat_ctx->attrs, NULL, NULL);
}

/** Process the results of evaluating a user object when checking group membership
 *
 * Any information relating to the user's group memberships should be evailable in the group_ctx
 * structure before this is called.
 */
static unlang_action_t ldap_check_userobj_resume(unlang_result_t *p_result, request_t *request, void *uctx)
{
	ldap_group_userobj_dyn_ctx_t	*group_ctx = talloc_get_type_abort(uctx, ldap_group_userobj_dyn_ctx_t);
	ldap_group_xlat_ctx_t		*xlat_ctx = talloc_get_type_abort(group_ctx->xlat_ctx, ldap_group_xlat_ctx_t);
	rlm_ldap_t const		*inst = xlat_ctx->inst;
	fr_ldap_query_t			*query = xlat_ctx->query;
	LDAPMessage			*entry;
	int				ldap_errno;
	bool				value_is_dn = false;
	fr_value_box_t			*group = xlat_ctx->group;
	char				*value_name = NULL;

	/*
	 *	If group_ctx->values is not populated, this is the first call
	 *	- extract the returned values if any.
	 */
	if (!group_ctx->values) {
		entry = ldap_first_entry(query->ldap_conn->handle, query->result);
		if (!entry) {
			ldap_get_option(query->ldap_conn->handle, LDAP_OPT_RESULT_CODE, &ldap_errno);
			REDEBUG("Failed retrieving entry: %s", ldap_err2string(ldap_errno));
			RETURN_UNLANG_FAIL;
		}

		group_ctx->values = ldap_get_values_len(query->ldap_conn->handle, entry, inst->group.userobj_membership_attr);
		if (!group_ctx->values) {
			RDEBUG2("User object contains no group membership information in attribute \"%s\"",
				inst->group.userobj_membership_attr);
			RETURN_UNLANG_REJECT;
		}

		/*
		 *	To avoid re-assessing after each call out to do a DN -> name
		 *	lookup, cache this.
		 */
		group_ctx->count = ldap_count_values_len(group_ctx->values);
	}

	/*
	 *	Following a call out to do a DN -> name lookup, group_ctx->query will be
	 *	populated - process the results.
	 */
	if (group_ctx->query) {
		char		*buff;
		struct berval	**values = NULL;

		switch (group_ctx->query->ret) {
		case LDAP_RESULT_SUCCESS:
			break;

		case LDAP_RESULT_NO_RESULT:
		case LDAP_RESULT_BAD_DN:
			REDEBUG("Group DN \"%pV\" did not resolve to an object",
				fr_box_strvalue_buffer(group_ctx->lookup_dn));
			RETURN_UNLANG_INVALID;

		default:
			RETURN_UNLANG_FAIL;
		}

		entry = ldap_first_entry(group_ctx->query->ldap_conn->handle, group_ctx->query->result);
		if (!entry) {
			ldap_get_option(group_ctx->query->ldap_conn->handle, LDAP_OPT_RESULT_CODE, &ldap_errno);
			REDEBUG("Failed retrieving entry: %s", ldap_err2string(ldap_errno));
			RETURN_UNLANG_INVALID;
		}

		values = ldap_get_values_len(group_ctx->query->ldap_conn->handle, entry, inst->group.obj_name_attr);
		if (!values) {
			REDEBUG("No %s attributes found in object", inst->group.obj_name_attr);
			RETURN_UNLANG_INVALID;
		}

		MEM(buff = talloc_bstrndup(group_ctx, values[0]->bv_val, values[0]->bv_len));
		RDEBUG2("Group DN \"%pV\" resolves to name \"%pV\"", fr_box_strvalue_buffer(group_ctx->lookup_dn),
			fr_box_strvalue_len(values[0]->bv_val, values[0]->bv_len));
		ldap_value_free_len(values);

		if (group_ctx->resolving_value) {
			value_name = buff;
		} else {
			group_ctx->group_name = buff;
		}
	}

	/*
	 *	Loop over the list of groups the user is a member of, looking for a match.
	 */
	while (group_ctx->value_no < group_ctx->count) {
		struct berval	*value = group_ctx->values[group_ctx->value_no];

		/*
		 *	We have come back from resolving a membership DN to its name,
		 *	compare to the provided name.
		 */
		if (value_name && group_ctx->resolving_value) {
			if (((talloc_array_length(value_name) - 1) == group->vb_length) &&
			    (memcmp(group->vb_strvalue, value_name, group->vb_length) == 0)) {
				RDEBUG2("User found in group \"%pV\". Comparison between membership: name "
				       "(resolved from DN \"%pV\"), check: name", group,
				       fr_box_strvalue_buffer(group_ctx->lookup_dn));
				talloc_free(value_name);
				goto found;
			}
			talloc_const_free(group_ctx->lookup_dn);
			TALLOC_FREE(value_name);
			group_ctx->resolving_value = false;
			group_ctx->value_no++;
			continue;
		}

		value_is_dn = fr_ldap_util_is_dn(value->bv_val, value->bv_len);

		RDEBUG2("Processing %s value \"%pV\" as a %s", inst->group.userobj_membership_attr,
			fr_box_strvalue_len(value->bv_val, value->bv_len),
			value_is_dn ? "DN" : "group name");

		/*
		 *	Both literal group names, do case sensitive comparison
		 */
		if (!xlat_ctx->group_is_dn && !value_is_dn) {
			if ((group->vb_length == value->bv_len) &&
			    (memcmp(value->bv_val, group->vb_strvalue, value->bv_len) == 0)) {
				RDEBUG2("User found in group \"%pV\". Comparison between membership: name, check: name",
				       group);
				goto found;
			}
			group_ctx->value_no++;
			continue;
		}

		/*
		 *	Both DNs, do case insensitive, binary safe comparison
		 */
		if (xlat_ctx->group_is_dn && value_is_dn) {
			if (fr_ldap_berval_strncasecmp(value, group->vb_strvalue, group->vb_length) == 0) {
				RDEBUG2("User found in group DN \"%pV\". "
				       "Comparison between membership: dn, check: dn", group);
				goto found;
			}
			group_ctx->value_no++;
			continue;
		}

		/*
		 *	If the value is not a DN, and the name we were given is a dn
		 *	convert the value to a DN and do a comparison.
		 */
		if (!value_is_dn && xlat_ctx->group_is_dn) {
			/*
			 *	So we only do the DN -> name lookup once, regardless of how many
			 *	group values we have to check, the resolved name is put in group_ctx->group_name
			 */
			if (!group_ctx->group_name) {
				group_ctx->lookup_dn = group->vb_strvalue;

				if (unlang_function_repeat_set(request, ldap_check_userobj_resume) < 0) RETURN_UNLANG_FAIL;

				/* Need to push this for the custom cancellation function */
				return unlang_function_push_with_result(p_result,
									request,
									ldap_dn2name_start,
									NULL,
									ldap_dn2name_cancel, ~FR_SIGNAL_CANCEL,
									UNLANG_SUB_FRAME,
									group_ctx);
			}

			if (((talloc_array_length(group_ctx->group_name) - 1) == value->bv_len) &&
			    (memcmp(value->bv_val, group_ctx->group_name, value->bv_len) == 0)) {
				RDEBUG2("User found in group \"%pV\". Comparison between membership: "
					"name, check: name (resolved from DN \"%pV\")",
			       		fr_box_strvalue_len(value->bv_val, value->bv_len), group);
				goto found;
			}
			group_ctx->value_no++;
			continue;
		}

		/*
		 *	We have a value which is a DN, and a check item which specifies the name of a group,
		 *	convert the value to a name so we can do a comparison.
		 */
		if (value_is_dn && !xlat_ctx->group_is_dn) {
			group_ctx->lookup_dn = fr_ldap_berval_to_string(group_ctx, value);
			group_ctx->resolving_value = true;

			if (unlang_function_repeat_set(request, ldap_check_userobj_resume) < 0) RETURN_UNLANG_FAIL;

			/* Need to push this for the custom cancellation function */
			return unlang_function_push_with_result(p_result,
								request,
								ldap_dn2name_start,
								NULL,
								ldap_dn2name_cancel, ~FR_SIGNAL_CANCEL,
								UNLANG_SUB_FRAME, group_ctx);
		}

		fr_assert(0);
	}
	RETURN_UNLANG_NOTFOUND;

found:
	xlat_ctx->found = true;
	RETURN_UNLANG_OK;
}

/** Ensure retrieved LDAP values are cleared up
 *
 */
static int userobj_dyn_free(ldap_group_userobj_dyn_ctx_t *group_ctx)
{
	if (group_ctx->values) ldap_value_free_len(group_ctx->values);
	return 0;
}

/** Query the LDAP directory to check if a user object is a member of a group
 *
 * @param[out] p_result		Result of calling the module.
 * @param[in] request		Current request.
 * @param[in] xlat_ctx		Context of the xlat being evaluated.
 */
unlang_action_t rlm_ldap_check_userobj_dynamic(unlang_result_t *p_result, request_t *request,
					       ldap_group_xlat_ctx_t *xlat_ctx)
{
	rlm_ldap_t const		*inst = xlat_ctx->inst;
	ldap_group_userobj_dyn_ctx_t	*group_ctx;

	MEM(group_ctx = talloc(unlang_interpret_frame_talloc_ctx(request), ldap_group_userobj_dyn_ctx_t));
	talloc_set_destructor(group_ctx, userobj_dyn_free);

	*group_ctx = (ldap_group_userobj_dyn_ctx_t) {
		.xlat_ctx = xlat_ctx,
		.attrs = { inst->group.obj_name_attr, NULL }
	};

	RDEBUG2("Checking user object's %s attributes", inst->group.userobj_membership_attr);

	/*
	 *	If a previous query was required to find the user DN, that will have
	 *	retrieved the user object membership attribute and the resulting values
	 *	can be checked.
	 *	If not then a query is needed to retrieve the user object.
	 */
	if (unlang_function_push_with_result(p_result,
					     request,
					     xlat_ctx->query ? NULL : ldap_check_userobj_start,
					     ldap_check_userobj_resume,
					     ldap_group_userobj_cancel, ~FR_SIGNAL_CANCEL,
					     UNLANG_SUB_FRAME,
					     group_ctx) < 0) {
		talloc_free(group_ctx);
		RETURN_UNLANG_FAIL;
	}

	return UNLANG_ACTION_PUSHED_CHILD;
}

/** Check group membership attributes to see if a user is a member.
 *
 * @param[out] p_result		Result of calling the module.
 * @param[in] inst		rlm_ldap configuration.
 * @param[in] request		Current request.
 * @param[in] check		vb containing the group value (name or dn).
 */
unlang_action_t rlm_ldap_check_cached(unlang_result_t *p_result,
				      rlm_ldap_t const *inst, request_t *request, fr_value_box_t const *check)
{
	fr_pair_t	*vp;
	int		ret;
	fr_dcursor_t	cursor;

	/*
	 *	We return RLM_MODULE_INVALID here as an indication
	 *	the caller should try a dynamic group lookup instead.
	 */
	vp =  fr_pair_dcursor_by_da_init(&cursor, &request->control_pairs, inst->group.cache_da);
	if (!vp) RETURN_UNLANG_INVALID;

	for (vp = fr_dcursor_current(&cursor);
	     vp;
	     vp = fr_dcursor_next(&cursor)) {
		ret = fr_value_box_cmp_op(T_OP_CMP_EQ, &vp->data, check);
		if (ret == 1) {
			RDEBUG2("User found. Matched cached membership");
			RETURN_UNLANG_OK;
		}

		if (ret < -1) RETURN_UNLANG_FAIL;
	}

	RDEBUG2("Cached membership not found");

	RETURN_UNLANG_NOTFOUND;
}

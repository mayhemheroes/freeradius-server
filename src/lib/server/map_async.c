/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
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

/*
 * $Id$
 *
 * @brief map / template functions
 * @file src/lib/server/map.c
 *
 * @ingroup AVP
 *
 * @copyright 2013 The FreeRADIUS server project
 * @copyright 2013 Alan DeKok (aland@freeradius.org)
 */

RCSID("$Id$")

#include <freeradius-devel/server/exec.h>
#include <freeradius-devel/server/map.h>
#include <freeradius-devel/server/paircmp.h>
#include <freeradius-devel/server/tmpl_dcursor.h>

#include <freeradius-devel/util/pair_legacy.h>

#include <freeradius-devel/protocol/radius/rfc2865.h>
#include <freeradius-devel/protocol/freeradius/freeradius.internal.h>


static inline vp_list_mod_t *list_mod_alloc(TALLOC_CTX *ctx)
{
	vp_list_mod_t *mod;
	mod = talloc_zero(ctx, vp_list_mod_t);
	map_list_init(&mod->mod);
	return mod;
}

static inline map_t *map_alloc(TALLOC_CTX *ctx)
{
	map_t *map;
	map = talloc_zero(ctx, map_t);
	map_list_init(&map->child);
	return map;
}

/** Allocate a 'generic' #vp_list_mod_t
 *
 * This covers most cases, where we need to allocate a #vp_list_mod_t with a single
 * modification map, with an attribute ref LHS, and a boxed value RHS.
 *
 * @param[in] ctx	to allocate #vp_list_mod_t in.
 * @param[in] original	The map from the update section.
 * @param[in] mutated	The original map but with a altered dst (LHS).
 *			If the LHS of the original map was not expanded, this should be
 *			the same as original.
 * @return
 *	- A new vlm structure on success.
 *	- NULL on failure.
 */
static inline vp_list_mod_t *list_mod_generic_afrom_map(TALLOC_CTX *ctx,
							map_t const *original, map_t const *mutated)
{
	vp_list_mod_t *n;
	map_t *mod;

	n = list_mod_alloc(ctx);
	if (!n) return NULL;

	n->map = original;

	mod = map_alloc(n);
	if (!mod) return NULL;
	mod->lhs = mutated->lhs;
	mod->op = mutated->op;
	mod->rhs = tmpl_alloc(mod, TMPL_TYPE_DATA, T_BARE_WORD, NULL, 0);
	if (!mod->rhs) {
		talloc_free(n);
		return NULL;
	}
	map_list_insert_tail(&n->mod, mod);

	return n;
}

/** Allocate a 'delete' #vp_list_mod_t
 *
 * This will cause the dst (LHS) to be deleted when applied.  This is intended to be
 * used where the RHS expansion is NULL, and we're doing a := assignment, so need to
 * delete the LHS.
 *
 * @param[in] ctx	to allocate #vp_list_mod_t in.
 * @param[in] original	The map from the update section.
 * @param[in] mutated	The original map but with a altered dst (LHS).
 *			If the LHS of the original map was not expanded, this should be
 *			the same as original.
 *
 * @return
 *	- A new vlm structure on success.
 *	- NULL on failure.
 */
static inline vp_list_mod_t *list_mod_delete_afrom_map(TALLOC_CTX *ctx,
						       map_t const *original, map_t const *mutated)
{
	vp_list_mod_t *n;
	map_t *mod;

	n = list_mod_alloc(ctx);
	if (!n) return NULL;

	n->map = original;

	mod = map_alloc(n);
	if (!mod) return NULL;

	mod->lhs = mutated->lhs;
	mod->op = T_OP_CMP_FALSE;	/* Means delete the LHS */
	mod->rhs = tmpl_alloc(mod, TMPL_TYPE_DATA, T_BARE_WORD, "ANY", 3);
	if (!mod->rhs) {
		talloc_free(n);
		return NULL;
	}
	map_list_insert_tail(&n->mod, mod);

	return n;
}

/** Allocate an 'empty_string' #vp_list_mod_t
 *
 * This shallow copies the mutated map, but sets the RHS to be an empty string.
 *
 * @param[in] ctx	to allocate #vp_list_mod_t in.
 * @param[in] original	The map from the update section.
 * @param[in] mutated	The original map but with a altered dst (LHS).
 *			If the LHS of the original map was not expanded, this should be
 *			the same as original.
 *
 * @return
 *	- A new vlm structure on success.
 *	- NULL on failure.
 */
static inline vp_list_mod_t *list_mod_empty_string_afrom_map(TALLOC_CTX *ctx,
							     map_t const *original, map_t const *mutated)
{
	vp_list_mod_t		*n;
	map_t			*mod;
	fr_value_box_t		empty_string = *fr_box_strvalue_len("", 0);

	n = list_mod_alloc(ctx);
	if (!n) return NULL;

	n->map = original;

	mod = map_alloc(n);
	if (!mod) return NULL;

	mod->lhs = mutated->lhs;
	mod->op = mutated->op;
	mod->rhs = tmpl_alloc(mod, TMPL_TYPE_DATA, T_DOUBLE_QUOTED_STRING, NULL, -1);
	if (!mod->rhs) {
		talloc_free(n);
		return NULL;
	}

	/*
	 *	For consistent behaviour we don't try and guess
	 *	what value we should assign, we try and cast a
	 *	zero length string to the specified type and
	 *	see what happens...
	 */
	if (fr_value_box_cast(mod->rhs, tmpl_value(mod->rhs),
			      mutated->cast ? mutated->cast : tmpl_attr_tail_da(mutated->lhs)->type,
			      tmpl_attr_tail_da(mutated->lhs), &empty_string) < 0) {
		talloc_free(n);
		return NULL;
	}
	map_list_insert_tail(&n->mod, mod);

	return n;
}

/** Check that the destination list is currently value
 *
 * @param[in] request	to resolve in the list in.
 * @param[in] map	to check
 * @param[in] src_dst	a lhs or rhs tmpl to check.
 * @return
 *	- destination list if list is valid.
 *	- NULL if destination list is invalid.
 */
static inline fr_pair_list_t *map_check_src_or_dst(request_t *request, map_t const *map, tmpl_t const *src_dst)
{
	request_t		*context = request;
	fr_pair_list_t		*list;
	fr_dict_attr_t const	*list_ref;

	if (tmpl_request_ptr(&context, tmpl_request(src_dst)) < 0) {
		RPEDEBUG("Mapping \"%.*s\" -> \"%.*s\" cannot be performed",
			(int)map->rhs->len, map->rhs->name, (int)map->lhs->len, map->lhs->name);
		return NULL;
	}

	list_ref = tmpl_list(src_dst);
	list = tmpl_list_head(context, list_ref);
	if (!list) {
		REDEBUG("Mapping \"%.*s\" -> \"%.*s\" cannot be performed due to to invalid list qualifier \"%s\"",
			(int)map->rhs->len, map->rhs->name, (int)map->lhs->len, map->lhs->name,
			tmpl_list_name(list_ref, "<INVALID>"));
		return NULL;
	}

	return list;
}

/** Evaluate a map creating a new map with #TMPL_TYPE_ATTR LHS and #TMPL_TYPE_DATA RHS
 *
 * This function creates maps for consumption by map_to_request.
 *
 * @param[in,out] ctx		to allocate modification maps in.
 * @param[out] out		Where to write the #fr_pair_t (s), which may be NULL if not found
 * @param[in] request		The current request.
 * @param[in] original		the map. The LHS (dst) has to be #TMPL_TYPE_ATTR.
 * @param[in] lhs_result	of previous stack based rhs evaluation.
 *				Must be provided for rhs types:
 *				- TMPL_TYPE_XLAT
 *				- TMPL_TYPE_EXEC (in future)
 * @param[in] rhs_result	of previous stack based rhs evaluation.
 *				Must be provided for rhs types:
 *				- TMPL_TYPE_XLAT
 *				- TMPL_TYPE_EXEC (in future)
 *				Once this function returns result will be invalidated even
 *				if this function errors.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int map_to_list_mod(TALLOC_CTX *ctx, vp_list_mod_t **out,
		    request_t *request, map_t const *original,
		    fr_value_box_list_t *lhs_result, fr_value_box_list_t *rhs_result)
{
	vp_list_mod_t	*n = NULL;
	map_t		map_tmp;
	map_t const	*mutated = original;

	fr_dcursor_t	values;
	fr_value_box_list_t	head;
	TALLOC_CTX	*tmp_ctx = NULL;

	MAP_VERIFY(original);

	if (!fr_cond_assert(original->lhs != NULL)) return -1;
	if (!fr_cond_assert(original->rhs != NULL)) return -1;

	fr_assert(tmpl_is_attr(original->lhs) ||
		  tmpl_is_xlat(original->lhs));

	*out = NULL;
	fr_value_box_list_init(&head);

	/*
	 *	Preprocessing of the LHS of the map.
	 */
	switch (original->lhs->type) {
	/*
	 *	Already in the correct form.
	 */
	case TMPL_TYPE_ATTR:
		break;

	/*
	 *	Everything else gets expanded, then re-parsed as an attribute reference.
	 *
	 *	This allows the syntax like:
	 *	- "Attr-%{number}" := "value"
	 */
	case TMPL_TYPE_EXEC:
	case TMPL_TYPE_XLAT:
	{
		ssize_t slen;
		fr_value_box_t *lhs_result_head = fr_value_box_list_head(lhs_result);

		/*
		 *	Get our own mutable copy of the original so we can
		 *	dynamically expand the LHS.
		 */
		memcpy(&map_tmp, original, sizeof(map_tmp));
		mutated = &map_tmp;

		tmp_ctx = talloc_new(NULL);

		fr_assert(!fr_value_box_list_empty(lhs_result));

		/*
		 *	This should always be a noop, but included
		 *	here for robustness.
		 */
		if (fr_value_box_list_concat_in_place(lhs_result_head,
						      lhs_result_head, lhs_result, FR_TYPE_STRING,
						      FR_VALUE_BOX_LIST_FREE, true,
						      SIZE_MAX) < 0) {
			RPEDEBUG("Left side expansion failed");
			fr_value_box_list_talloc_free(lhs_result);
			goto error;
		}

		slen = tmpl_afrom_attr_str(tmp_ctx, NULL, &map_tmp.lhs, lhs_result_head->vb_strvalue,
					   &(tmpl_rules_t){
					   	.attr = {
							.dict_def = request->local_dict,
							.list_def = request_attr_request,
				   		}
					   });
		if (slen <= 0) {
			RPEDEBUG("Left side expansion result \"%s\" is not an attribute reference",
				 lhs_result_head->vb_strvalue);
			fr_value_box_list_talloc_free(lhs_result);
			goto error;
		}
		fr_assert(tmpl_is_attr(mutated->lhs));
	}
		break;

	default:
		fr_assert(0);
		break;
	}

	/*
	 *	Special case for !*, we don't need to parse RHS as this is a unary operator.
	 */
	if (mutated->op == T_OP_CMP_FALSE) {
		map_t *mod;
		n = list_mod_alloc(ctx);
		if (!n) goto error;

		n->map = original;
		mod = map_alloc(n);	/* Need to duplicate input map, so next pointer is NULL */
		mod->lhs = mutated->lhs;
		mod->op = mutated->op;
		mod->rhs = mutated->rhs;
		map_list_insert_tail(&n->mod, mod);
		goto finish;
	}

	/*
	 *	List to list copy.
	 */
	if (tmpl_is_attr(mutated->lhs) && tmpl_is_attr(mutated->rhs) &&
	    tmpl_attr_tail_da_is_structural(mutated->lhs) && tmpl_attr_tail_da_is_structural(mutated->rhs)) {
		fr_pair_list_t	*list = NULL;
		fr_pair_t	*vp = NULL;

		/*
		 *	Check source list
		 */
		list = map_check_src_or_dst(request, mutated, mutated->rhs);
		if (!list) goto error;

		vp = fr_pair_list_head(list);
		/*
		 *	No attributes found on LHS.
		 */
		if (!vp) {
			/*
			 *	Special case for := if RHS was NULL.
			 *	Should delete all LHS attributes.
			 */
			if (mutated->op == T_OP_SET) n = list_mod_delete_afrom_map(ctx, original, mutated);
			goto finish;
		}

		n = list_mod_alloc(ctx);
		n->map = original;

		/*
		 *	Iterate over all attributes in that list
		 */
		do {
			map_t 	*n_mod;

			n_mod = map_alloc(n);
			if (!n_mod) goto error;

			n_mod->op = mutated->op;

			/*
			 *	For the LHS we need to create a reference to
			 *	the attribute, with the same destination list
			 *	as the current LHS map.
			 */
			n_mod->lhs = tmpl_alloc(n, TMPL_TYPE_ATTR, T_BARE_WORD, mutated->lhs->name, mutated->lhs->len);
			if (!n_mod->lhs) goto error;

			if (tmpl_attr_copy(n_mod->lhs, mutated->lhs) < 0) goto error;

			tmpl_attr_set_leaf_da(n_mod->lhs, vp->da);

			/*
			 *	For the RHS we copy the value of the attribute
			 *	we just found, creating data (literal) tmpl.
			 */
			n_mod->rhs = tmpl_alloc(n_mod, TMPL_TYPE_DATA,
					        vp->data.type == FR_TYPE_STRING ? T_DOUBLE_QUOTED_STRING : T_BARE_WORD,
					        NULL, 0);
			if (!n_mod->rhs) goto error;

			/*
			 *	Have to do a full copy, as the attribute we're
			 *	getting the buffer value from may be freed
			 *	before this map is applied.
			 */
			if (unlikely(fr_value_box_copy(n_mod->rhs, tmpl_value(n_mod->rhs), &vp->data) < 0)) goto error;
			map_list_insert_tail(&n->mod, n_mod);

			MAP_VERIFY(n_mod);
		} while ((vp = fr_pair_list_next(list, vp)));

		goto finish;
	}

	/*
	 *	Unparsed.  These are easy because they
	 *	can only have a single value.
	 */
	if (tmpl_is_data_unresolved(mutated->rhs)) {
		fr_type_t type = tmpl_attr_tail_da(mutated->lhs)->type;

		fr_assert(tmpl_is_attr(mutated->lhs));
		fr_assert(tmpl_attr_tail_da(mutated->lhs));	/* We need to know which attribute to create */

		n = list_mod_generic_afrom_map(ctx, original, mutated);
		if (!n) goto error;

		fr_dcursor_init(&values, fr_value_box_list_dlist_head(&head));

		if (fr_value_box_from_str(map_list_head(&n->mod),
					  tmpl_value(map_list_head(&n->mod)->rhs), type,
					  tmpl_attr_tail_da(mutated->lhs),
					  mutated->rhs->name, mutated->rhs->len,
					  fr_value_unescape_by_quote[(uint8_t)mutated->rhs->quote])) {
			RPEDEBUG("Assigning value to \"%s\" failed", tmpl_attr_tail_da(mutated->lhs)->name);
			goto error;
		}
		goto finish;
	}

	/*
	 *	Check destination list
	 */
	if (!map_check_src_or_dst(request, mutated, mutated->lhs)) goto error;

	(void)fr_dcursor_init(&values, fr_value_box_list_dlist_head(&head));

	switch (mutated->rhs->type) {
	case TMPL_TYPE_XLAT:
	{
		fr_dcursor_t	from;
		fr_value_box_t	*vb, *n_vb;

		fr_assert(tmpl_xlat(mutated->rhs) != NULL);

	assign_values:
		fr_assert(tmpl_is_attr(mutated->lhs));
		fr_assert(tmpl_attr_tail_da(mutated->lhs));		/* We need to know which attribute to create */

		/*
		 *	Empty value - Try and cast an empty string
		 *	to the destination type, and see what
		 *	happens.  This is only for XLATs and in future
		 *	EXECs.
		 */
		if (fr_value_box_list_empty(rhs_result)) {
			n = list_mod_empty_string_afrom_map(ctx, original, mutated);
			if (!n) {
				RPEDEBUG("Assigning value to \"%s\" failed", tmpl_attr_tail_da(mutated->lhs)->name);
			xlat_error:
				fr_dcursor_head(&values);
				fr_dcursor_free_list(&values);
				goto error;
			}
			goto finish;
		}

		/*
		 *	Non-Empty value
		 */
		n = list_mod_generic_afrom_map(ctx, original, mutated);
		if (!n) goto error;

		(void)fr_dcursor_init(&from, fr_value_box_list_dlist_head(rhs_result));
		while ((vb = fr_dcursor_remove(&from))) {
			if (vb->type != tmpl_attr_tail_da(mutated->lhs)->type) {
				n_vb = fr_value_box_alloc_null(map_list_head(&n->mod)->rhs);
				if (!n_vb) {
					fr_dcursor_head(&from);
					fr_dcursor_free_list(&from);
					goto xlat_error;
				}

				if (fr_value_box_cast(n_vb, n_vb,
						      mutated->cast ? mutated->cast : tmpl_attr_tail_da(mutated->lhs)->type,
						      tmpl_attr_tail_da(mutated->lhs), vb) < 0) {
					RPEDEBUG("Assigning value to \"%s\" failed", tmpl_attr_tail_da(mutated->lhs)->name);

					fr_dcursor_head(&from);
					fr_dcursor_free_list(&from);
					goto xlat_error;
				}
				talloc_free(vb);
			} else {
				n_vb = talloc_steal(n, vb);	/* Should already be in ctx of n's parent */
			}
			fr_dcursor_append(&values, n_vb);
		}
	}
		break;

	case TMPL_TYPE_ATTR:
	{
		fr_dcursor_t		from;
		tmpl_dcursor_ctx_t	cc_attr;
		fr_pair_t		*vp;
		fr_value_box_t		*n_vb;
		int			err;

		fr_assert(fr_value_box_list_empty(rhs_result));
		fr_assert(tmpl_is_attr(mutated->lhs) && tmpl_attr_tail_da(mutated->lhs));

		/*
		 *	Check source list
		 */
		if (!map_check_src_or_dst(request, mutated, mutated->rhs)) goto error;

		/*
		 *	Check we have pairs to copy *before*
		 *	doing any expensive allocations.
		 */
		vp = tmpl_dcursor_init(&err, request, &cc_attr, &from, request, mutated->rhs);
		if (!vp) switch (err) {
		default:
			break;

		case -1:		/* No input pairs */
			RDEBUG3("No matching pairs found for \"%s\"", tmpl_attr_tail_da(mutated->rhs)->name);
			/*
			 *	Special case for := if RHS had no attributes
			 *	we should delete all LHS attributes.
			 */
			if (mutated->op == T_OP_SET) n = list_mod_delete_afrom_map(ctx, original, mutated);
			tmpl_dcursor_clear(&cc_attr);
			goto finish;

		case -2:		/* No matching list */
		case -3:		/* No request context */
		case -4:		/* memory allocation error */
			RPEDEBUG("Failed resolving attribute source");
			tmpl_dcursor_clear(&cc_attr);
			goto error;
		}

		n = list_mod_generic_afrom_map(ctx, original, mutated);
		if (!n) {
			tmpl_dcursor_clear(&cc_attr);
			goto error;
		}

		vp = fr_dcursor_current(&from);
		fr_assert(vp);		/* Should have errored out */
		do {
			n_vb = fr_value_box_alloc_null(map_list_head(&n->mod)->rhs);
			if (!n_vb) {
			attr_error:
				fr_dcursor_head(&values);
				fr_dcursor_free_list(&values);
				tmpl_dcursor_clear(&cc_attr);
				goto error;
			}

			if (vp->data.type != tmpl_attr_tail_da(mutated->lhs)->type) {
				if (fr_value_box_cast(n_vb, n_vb,
						      mutated->cast ? mutated->cast : tmpl_attr_tail_da(mutated->lhs)->type,
						      tmpl_attr_tail_da(mutated->lhs), &vp->data) < 0) {
				assign_error:
					RPEDEBUG("Assigning value to \"%s\" failed", tmpl_attr_tail_da(mutated->lhs)->name);

					goto attr_error;
				}
			} else {
				if (unlikely(fr_value_box_copy(n_vb, n_vb, &vp->data) < 0)) goto assign_error;
			}
			fr_dcursor_append(&values, n_vb);
		} while ((vp = fr_dcursor_next(&from)));

		tmpl_dcursor_clear(&cc_attr);
	}
		break;

	case TMPL_TYPE_DATA:
	{
		fr_value_box_t	*vb, *n_vb;

		fr_assert(fr_value_box_list_empty(rhs_result));
		fr_assert(tmpl_attr_tail_da(mutated->lhs));
		fr_assert(tmpl_is_attr(mutated->lhs));

		n = list_mod_generic_afrom_map(ctx, original, mutated);
		if (!n) goto error;

		vb = tmpl_value(mutated->rhs);

		n_vb = fr_value_box_alloc_null(map_list_head(&n->mod)->rhs);
		if (!n_vb) {
		data_error:
			fr_dcursor_head(&values);
			fr_dcursor_free_list(&values);
			goto error;
		}
		/*
		 *	This should be optimised away by the map
		 *	parser, but in case we're applying runtime
		 *	maps we still need to check if we need to
		 *	cast.
		 */
		if (tmpl_attr_tail_da(mutated->lhs)->type != tmpl_value_type(mutated->rhs)) {
			if (fr_value_box_cast(n_vb, n_vb,
					      mutated->cast ? mutated->cast : tmpl_attr_tail_da(mutated->lhs)->type,
					      tmpl_attr_tail_da(mutated->lhs), vb) < 0) {
				RPEDEBUG("Assigning value to \"%s\" failed", tmpl_attr_tail_da(mutated->lhs)->name);
				goto data_error;
			}
		} else {
			/*
			 *	We need to do a full copy, as shallow
			 *	copy would increase the reference count
			 *	on the static/global buffers and possibly
			 *	lead to threading issues.
			 */
			if (unlikely(fr_value_box_copy(n_vb, n_vb, vb) < 0)) goto data_error;
		}
		fr_dcursor_append(&values, n_vb);
	}
		break;

	/*
	 *	The result of an exec is a value if the LHS is an
	 *	attribute, or a set of VPs, if the LHS is a list.
	 *
	 *	@todo - we should just create maps from the RHS
	 *	instead of VPs, and then converting them to maps.
	 */
	case TMPL_TYPE_EXEC:
	{
		fr_dcursor_t	from;
		fr_pair_list_t	vp_head;
		fr_pair_t	*vp;
		fr_value_box_t	*rhs_result_head = fr_value_box_list_head(rhs_result);

		fr_pair_list_init(&vp_head);
		/*
		 *	If the LHS is an attribute, we just do the
		 *	same thing as an xlat expansion.
		 */
		if (tmpl_is_attr(mutated->lhs)) goto assign_values;

		fr_assert(tmpl_is_list(mutated->lhs));

		/*
		 *	Empty value - Try and cast an empty string
		 *	to the destination type, and see what
		 *	happens.  This is only for XLATs and in future
		 *	EXECs.
		 */
		if (fr_value_box_list_empty(rhs_result)) {
			RPEDEBUG("Cannot assign empty value to \"%s\"", mutated->lhs->name);
			goto error;
		}

		/*
		 *	This should always be a noop, but included
		 *	here for robustness.
		 */
		if (fr_value_box_list_concat_in_place(rhs_result_head,
						      rhs_result_head, rhs_result, FR_TYPE_STRING,
						      FR_VALUE_BOX_LIST_FREE, true,
						      SIZE_MAX) < 0) {
			RPEDEBUG("Right side expansion failed");
			fr_value_box_list_talloc_free(rhs_result);
			goto error;
		}

		n = list_mod_alloc(ctx);
		if (!n) goto error;

		n->map = original;

		/*
		 *	Parse the VPs from the RHS.
		 */
		fr_pair_list_afrom_box(ctx, &vp_head, request->proto_dict, rhs_result_head);
		if (fr_pair_list_empty(&vp_head)) {
			talloc_free(n);
			RDEBUG2("No pairs returned by exec");
			return 0;	/* No pairs returned */
		}

		(void)fr_pair_dcursor_init(&from, &vp_head);
		while ((vp = fr_dcursor_remove(&from))) {
			map_t *mod;
			tmpl_rules_t rules = {
				.attr = {
					.request_def = tmpl_request(mutated->lhs),
					.list_def = tmpl_list(mutated->lhs)
				}
			};

			if (map_afrom_vp(n, &mod, vp, &rules) < 0) {
				RPEDEBUG("Failed converting VP to map");
				fr_dcursor_head(&from);
				fr_dcursor_free_item(&from);
				goto error;
			}

			if (tmpl_is_exec(mod->lhs) || tmpl_is_exec(mod->rhs)) {
				RPEDEBUG("Program output cannot request execution of another program for attribute %s", vp->da->name);
				fr_dcursor_head(&from);
				fr_dcursor_free_item(&from);
				goto error;
			}


			if ((vp->op == T_OP_REG_EQ) || (vp->op == T_OP_REG_NE)) {
				RPEDEBUG("Program output cannot request regular expression matching for attribute %s", vp->da->name);
				fr_dcursor_head(&from);
				fr_dcursor_free_item(&from);
				goto error;
			}

			mod->op = vp->op;
			map_list_insert_tail(&n->mod, mod);
		}

	}
		goto finish;

	default:
		fr_assert(0);	/* Should have been caught at parse time */
		goto error;
	}

	fr_assert(!fr_value_box_list_empty(&head) || !n);

	/*
	 *	FIXME: This is only required because
	 *	tmpls allocate space for a value.
	 *
	 *	If tmpl_value were a pointer we could
	 *	assign values directly.
	 */
	if (unlikely(fr_value_box_copy(map_list_head(&n->mod)->rhs, tmpl_value(map_list_head(&n->mod)->rhs),
				       fr_value_box_list_head(&head)) < 0)) {
		goto error;
	}

	/*
	 *	value boxes in tmpls cannot now be the head of a list
	 *
	 *tmpl_value(map_list_head(&n->mod)->rhs)->next = head->next;
	 */
	fr_value_box_list_talloc_free(&head);

finish:
	if (n) {
		MAP_VERIFY(n->map);
		*out = n;
	}

	/*
	 *	Reparent ephemeral LHS to the vp_list_mod_t.
	 */
	if (tmp_ctx) {
		if (talloc_parent(mutated->lhs) == tmp_ctx) talloc_steal(n, mutated->lhs);
		talloc_free(tmp_ctx);
	}
	return 0;

error:
	talloc_free(tmp_ctx);
	talloc_free(n);	/* Frees all mod maps too */
	return -1;
}

static inline fr_pair_t *map_list_mod_to_vp(TALLOC_CTX *ctx, tmpl_t const *attr, fr_value_box_t const *value)
{
	fr_pair_t *vp;

	MEM(vp = fr_pair_afrom_da(ctx, tmpl_attr_tail_da(attr)));
	if (unlikely(fr_value_box_copy(vp, &vp->data, value) < 0)) {
		talloc_free(vp);
		return NULL;
	}
	PAIR_VERIFY(vp);		/* Check we created something sane */

	return vp;
}

/** Allocate one or more fr_pair_ts from a #vp_list_mod_t
 *
 */
static void map_list_mod_to_vps(TALLOC_CTX *ctx, fr_pair_list_t *list, vp_list_mod_t const *vlm)
{
	map_t	*mod;

	fr_assert(!map_list_empty(&vlm->mod));

	/*
	 *	Fast path...
	 */
	mod = map_list_head(&vlm->mod);
	if (map_list_num_elements(&vlm->mod) == 1) {
		fr_pair_t *vp;
		vp = map_list_mod_to_vp(ctx, mod->lhs, tmpl_value(mod->rhs));
		fr_pair_append(list, vp);
		return;
	}

	/*
	 *	Slow path.  This may generate multiple attributes.
	 */
	for (;
	     mod;
	     mod = map_list_next(&vlm->mod, mod)) {
		fr_pair_t	*vp;

		vp = map_list_mod_to_vp(ctx, mod->lhs, tmpl_value(mod->rhs));
		if (!vp) {
			fr_pair_list_free(list);
			return;
		}
		fr_pair_append(list, vp);
	}
}

/** Print debug for a modification map
 *
 * @param[in] request	being modified.
 * @param[in] map	The original map.
 * @param[in] mod	The ephemeral map which describes the change.
 * @param[in] vb	The value in the ephemeral map.
 */
static inline void map_list_mod_debug(request_t *request,
				      map_t const *map, map_t const *mod, fr_value_box_t const *vb)
{
	char *rhs = NULL;
	char const *quote = "";

	if (!fr_cond_assert(map->lhs != NULL)) return;
	if (!fr_cond_assert(map->rhs != NULL)) return;

	fr_assert(mod);

	if (vb && (vb->type == FR_TYPE_STRING)) quote = "\"";

	/*
	 *	If it's an exec, ignore the list
	 */
	if (tmpl_is_exec(map->rhs)) {
		RDEBUG2("%s %s %s%pV%s", mod->lhs->name, fr_table_str_by_value(fr_tokens_table, mod->op, "<INVALID>"),
		        quote, vb, quote);
		return;
	}

	switch (map->rhs->type) {
	/*
	 *	Just print the value being assigned
	 */
	default:
	case TMPL_TYPE_XLAT:
	case TMPL_TYPE_DATA_UNRESOLVED:
	case TMPL_TYPE_DATA:
		rhs = fr_asprintf(request, "%s%pV%s", quote, vb, quote);
		break;

	case TMPL_TYPE_ATTR:
		rhs = fr_asprintf(request, "%s -> %s%pV%s", map->rhs->name, quote, vb, quote);
		break;
	}

	switch (map->lhs->type) {
	case TMPL_TYPE_ATTR:
		RDEBUG2("%s %s %s", map->lhs->name, fr_table_str_by_value(fr_tokens_table, mod->op, "<INVALID>"), rhs);
		break;

	default:
		break;
	}

	/*
	 *	Must be LIFO free order so we don't leak pool memory
	 */
	talloc_free(rhs);
}

/** Apply the output of #map_to_list_mod to a request
 *
 * @param request	to modify.
 * @param vlm		VP List Modification to apply.
 */
int map_list_mod_apply(request_t *request, vp_list_mod_t const *vlm)
{
	int			rcode = 0;

	map_t const		*map = vlm->map, *mod = NULL;
	fr_pair_list_t		*vp_list;
	fr_pair_t		*found;
	request_t		*context;
	TALLOC_CTX		*parent;

	fr_dcursor_t		list;
	tmpl_dcursor_ctx_t	cc;

	memset(&cc, 0, sizeof(cc));

	MAP_VERIFY(map);
	fr_assert(!map_list_empty(&vlm->mod));

	/*
	 *	Print debug information for the mods being applied
	 */
	while ((mod = map_list_next(&vlm->mod, mod))) {
		MAP_VERIFY(mod);

		fr_assert(mod->lhs != NULL);
		fr_assert(mod->rhs != NULL);

		fr_assert(tmpl_is_attr(mod->lhs));

		/*
		 *	map_list_mod_debug()
		 */
		if (RDEBUG_ENABLED2) {
			map_list_mod_debug(request, map, mod, tmpl_value(mod->rhs));
		}
	}
	mod = map_list_head(&vlm->mod);	/* Reset */

	/*
	 *	All this has been checked by #map_to_list_mod
	 */
	context = request;
	if (!fr_cond_assert(mod && tmpl_request_ptr(&context, tmpl_request(mod->lhs)) == 0)) return -1;

	vp_list = tmpl_list_head(context, tmpl_list(mod->lhs));
	if (!fr_cond_assert(vp_list)) return -1;

	parent = tmpl_list_ctx(context, tmpl_list(mod->lhs));
	fr_assert(parent);

	/*
	 *	The destination is a list (which is a completely different set of operations)
	 */
	if (tmpl_is_list(map->lhs)) {
		switch (mod->op) {
		case T_OP_CMP_FALSE:
			fr_pair_list_free(vp_list);				/* Clear the entire list */
			goto finish;

		case T_OP_SET:
		{
			fr_pair_list_t tmp_list;
			fr_pair_list_init(&tmp_list);
			fr_pair_list_free(vp_list);				/* Clear the existing list */
			map_list_mod_to_vps(parent, &tmp_list, vlm);		/* Replace with a new list */
			fr_pair_list_append(vp_list, &tmp_list);
			goto finish;
		}

		/*
		 *	Ugh... exponential... Fixme? Build a tree if number
		 *	of attribute in to is > n?
		 */
		case T_OP_EQ:
		{
			bool		exists = false;
			fr_pair_list_t	vp_from, vp_to_insert;
			fr_pair_t	*vp;

			fr_pair_list_init(&vp_from);
			fr_pair_list_init(&vp_to_insert);
			map_list_mod_to_vps(parent, &vp_from, vlm);
			if (fr_pair_list_empty(&vp_from)) goto finish;

			while ((vp = fr_pair_remove(&vp_from, fr_pair_list_head(&vp_from)))) {
				fr_pair_list_foreach(vp_list, vp_to) {
					if (fr_pair_cmp_by_da(vp_to, vp) == 0) {
						exists = true;
						break;
					}
				}

				if (exists) {
					talloc_free(vp);	/* Don't overwrite */
				} else {
					fr_pair_append(&vp_to_insert, vp);
				}
			}

			fr_pair_list_append(vp_list, &vp_to_insert);	/* Do this last so we don't expand the 'to' set */
		}
			goto finish;

		case T_OP_ADD_EQ:
		{
			fr_pair_list_t	vp_from;

			fr_pair_list_init(&vp_from);
			map_list_mod_to_vps(parent, &vp_from, vlm);
			fr_assert(!fr_pair_list_empty(&vp_from));

			fr_pair_list_append(vp_list, &vp_from);
		}
			goto finish;

		case T_OP_PREPEND:
		{
			fr_pair_list_t	vp_from;

			fr_pair_list_init(&vp_from);
			map_list_mod_to_vps(parent, &vp_from, vlm);
			fr_assert(!fr_pair_list_empty(&vp_from));

			fr_pair_list_prepend(vp_list, &vp_from);

			goto finish;
		}

		default:
			rcode = -1;
			goto finish;
		}
	}

	fr_assert(!map_list_next(&vlm->mod, mod));

	/*
	 *	Find the destination attribute.  We leave with either
	 *	the list and vp pointing to the attribute or the VP
	 *	being NULL (no attribute at that index).
	 */
	found = tmpl_dcursor_init(NULL, request, &cc, &list, request, mod->lhs);

	/*
	 *	The destination is an attribute
	 */
	switch (mod->op) {
	/*
	 * 	!* - Remove all attributes which match the LHS attribute.
	 */
	case T_OP_CMP_FALSE:
		if (!found) goto finish;

		/*
		 *	The cursor was set to the Nth one.  Delete it, and only it.
		 */
		if (tmpl_attr_tail_num(map->lhs) != NUM_ALL) {
			fr_dcursor_free_item(&list);
		/*
		 *	Wildcard: delete all of the matching ones
		 */
		} else {
			fr_dcursor_free_list(&list);		/* Remember, we're using a custom iterator */
		}

		/*
		 *	Check that the User-Name and User-Password
		 *	caches point to the correct attribute.
		 */
		goto finish;

	/*
	 *	-= - Delete attributes in the found list which match any of the
	 *	src_list attributes.
	 *
	 *	This operation has two modes:
	 *	- If tmpl_attr_tail_num(map->lhs) > 0, we check each of the src_list attributes against
	 *	  the found attribute, to see if any of their values match.
	 *	- If tmpl_attr_tail_num(map->lhs) == NUM_UNSPEC, we compare all instances of the found attribute
	 *	  against each of the src_list attributes.
	 */
	case T_OP_SUB_EQ:
	{
		/* We didn't find any attributes earlier */
		if (!found) goto finish;

		/*
		 *	Instance specific[n] delete
		 *
		 *	i.e. Remove this single instance if it matches
		 *	any of these values.
		 */
		if (tmpl_attr_tail_num(map->lhs) != NUM_ALL) {
			fr_value_box_t	*vb = tmpl_value(map_list_head(&vlm->mod)->rhs);

			if (fr_value_box_cmp(vb, &found->data) == 0) {
				fr_dcursor_free_item(&list);
				goto finish;
			}

			goto finish;	/* Wasn't found */
		}

		/*
		 *	All instances[*] delete
		 *
		 *	i.e. Remove any instance of this attribute which
		 *	matches any of these values.
		 */
		do {
		     	fr_value_box_t	*vb = tmpl_value(map_list_head(&vlm->mod)->rhs);

			if (fr_value_box_cmp(vb, &found->data) == 0) {
				fr_dcursor_free_item(&list);
				break;
			}
		} while ((found = fr_dcursor_next(&list)));
	}
		goto finish;

	/*
	 *	+= - Add all attributes to the destination
	 */
	case T_OP_ADD_EQ:
	do_add:
	{
		fr_pair_list_t	vp_from;

		fr_pair_list_init(&vp_from);
		map_list_mod_to_vps(parent, &vp_from, vlm);
		if (fr_pair_list_empty(&vp_from)) goto finish;

		fr_pair_list_append(vp_list, &vp_from);
	}
		goto finish;

	case T_OP_PREPEND:
	{
		fr_pair_list_t	vp_from;

		fr_pair_list_init(&vp_from);
		map_list_mod_to_vps(parent, &vp_from, vlm);
		fr_assert(!fr_pair_list_empty(&vp_from));

		fr_pair_list_prepend(vp_list, &vp_from);

		goto finish;
	}

	/*
	 *	= - Set only if not already set
	 */
	case T_OP_EQ:
		if (found) {
			RDEBUG3("Refusing to overwrite (use :=)");
			goto finish;
		}
		goto do_add;

	/*
	 *	:= - Overwrite existing attribute with last src_list attribute
	 */
	case T_OP_SET:
		if (!found) goto do_add;

		/*
		 *	Instance specific[n] overwrite
		 */
		if (tmpl_attr_tail_num(map->lhs) != NUM_ALL) {
			fr_dcursor_t	from;
			fr_pair_list_t	vp_from;

			fr_pair_list_init(&vp_from);
			map_list_mod_to_vps(parent, &vp_from, vlm);
			if (fr_pair_list_empty(&vp_from)) goto finish;

			fr_pair_dcursor_init(&from, &vp_from);

			fr_dcursor_merge(&list, &from);	/* Merge first (insert after current attribute) */
			fr_dcursor_free_item(&list);	/* Then free the current attribute */
			goto finish;
		}

		/*
		 *	All instances[*] overwrite
		 */
		fr_dcursor_free_list(&list);		/* Remember, we're using a custom iterator */
		goto do_add;

	/*
	 *	!=, ==, >=, >, <=, < - Filter operators
	 */
	case T_OP_NE:
	case T_OP_CMP_EQ:
	case T_OP_GE:
	case T_OP_GT:
	case T_OP_LE:
	case T_OP_LT:
	{
		if (!found) goto finish;

		/*
		 *	Instance specific[n] filter
		 */
		if (tmpl_attr_tail_num(map->lhs) != NUM_ALL) {
			fr_value_box_t	*vb = tmpl_value(mod->rhs);
			bool		remove = true;

			if (fr_value_box_cmp_op(mod->op, &found->data, vb) == 1) remove = false;

			if (remove) fr_dcursor_free_item(&list);
			goto finish;
		}

		/*
		 *	All instances[*] filter
		 */
		do {
			fr_value_box_t	*vb = tmpl_value(mod->rhs);
			bool		remove = true;

			if (fr_value_box_cmp_op(mod->op, &found->data, vb) == 1) remove = false;

			if (remove) {
				fr_dcursor_free_item(&list);
			} else {
				fr_dcursor_next(&list);
			}
		} while ((found = fr_dcursor_current(&list)));
	}
		goto finish;

	default:
		fr_assert(0);	/* Should have been caught be the caller */
		rcode = -1;
		goto finish;
	}

finish:
	tmpl_dcursor_clear(&cc);
	return rcode;
}

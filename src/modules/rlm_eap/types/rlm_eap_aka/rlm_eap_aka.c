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
 * @file rlm_eap_aka.c
 * @brief Implements EAP-AKA
 *
 * @author Arran Cudbard-Bell (a.cudbardb@freeradius.org)
 *
 * @copyright 2021 Arran Cudbard-Bell (a.cudbardb@freeradius.org)
 * @copyright 2016 The FreeRADIUS server project
 * @copyright 2016 Network RADIUS SAS (legal.com)
 */
RCSID("$Id$")

#include <freeradius-devel/eap/base.h>
#include <freeradius-devel/eap_aka_sim/attrs.h>
#include <freeradius-devel/eap_aka_sim/base.h>
#include <freeradius-devel/eap_aka_sim/module.h>
#include <freeradius-devel/server/virtual_servers.h>
#include <freeradius-devel/unlang/module.h>
#include <freeradius-devel/util/rand.h>
#include <freeradius-devel/util/debug.h>

static conf_parser_t submodule_config[] = {
	{ FR_CONF_OFFSET_TYPE_FLAGS("virtual_server", FR_TYPE_VOID, 0, eap_aka_sim_module_conf_t, virtual_server),
				    .func = virtual_server_cf_parse,
				    .uctx = &(virtual_server_cf_parse_uctx_t){
					    .process_module_name = "eap_aka",
				    } },
	{ FR_CONF_OFFSET_IS_SET("prefer_aka_prime", FR_TYPE_BOOL, 0, eap_aka_sim_module_conf_t, aka.send_at_bidding_prefer_prime ), .dflt = "no" },

	CONF_PARSER_TERMINATOR
};

extern rlm_eap_submodule_t rlm_eap_aka;

static unlang_action_t mod_session_init(unlang_result_t *p_result, module_ctx_t const *mctx, request_t *request)
{
	eap_session_t			*eap_session = eap_session_get(request->parent);
	eap_aka_sim_mod_session_t	*mod_session;

	MEM(mod_session = talloc_zero(eap_session, eap_aka_sim_mod_session_t));
	mod_session->id = (uint8_t)(fr_rand() & 0xff);
	mod_session->ctx.hmac_md = mod_session->ctx.checkcode_md = EVP_sha1();
	eap_session->opaque = mod_session;
	eap_session->process = eap_aka_sim_process;

	return eap_session->process(p_result, mctx, request);
}

static int mod_instantiate(module_inst_ctx_t const *mctx)
{
	eap_aka_sim_module_conf_t	*inst = talloc_get_type_abort(mctx->mi->data, eap_aka_sim_module_conf_t);
	CONF_SECTION			*conf = mctx->mi->conf;

	inst->type = rlm_eap_aka.provides[0];

	/*
	 *	If the user didn't specify a bidding value
	 *	infer whether we need to send the bidding
	 *	attribute, by whether the EAP module has
	 *	has the AKA-Prime module enabled.
	 */
	if (!inst->aka.send_at_bidding_prefer_prime_is_set) {
		CONF_SECTION	*parent = cf_item_to_section(cf_parent(conf));
		CONF_PAIR	*cp = NULL;

		while ((cp = cf_pair_find_next(parent, cp, "type"))) {
			if (strcmp(cf_pair_value(cp), "aka-prime") == 0) {
				cf_log_debug(conf, "Setting 'prefer_aka_prime = yes', as EAP-AKA-Prime is enabled");
				inst->aka.send_at_bidding_prefer_prime = true;
				inst->aka.send_at_bidding_prefer_prime_is_set = true;
				break;
			}
		}
	}

	return 0;
}

static eap_type_t mod_type_identity(UNUSED void *instance, char const *id, size_t len)
{
	if (fr_aka_sim_id_to_eap_type(id, len) == FR_EAP_METHOD_AKA) return FR_EAP_METHOD_AKA;

	return FR_EAP_METHOD_INVALID;
}

static int mod_load(void)
{
	if (fr_aka_sim_init() < 0) return -1;

	fr_aka_sim_xlat_func_register();

	return 0;
}

static void mod_unload(void)
{
	fr_aka_sim_xlat_func_unregister();

	fr_aka_sim_free();
}

/*
 *	The module name should be the only globally exported symbol.
 *	That is, everything else should be 'static'.
 */
rlm_eap_submodule_t rlm_eap_aka = {
	.common = {
		.magic		= MODULE_MAGIC_INIT,
		.name		= "eap_aka",

		.inst_size	= sizeof(eap_aka_sim_module_conf_t),
		.inst_type	= "eap_aka_sim_module_conf_t",
		.config		= submodule_config,

		.onload		= mod_load,
		.unload		= mod_unload,

		.instantiate	= mod_instantiate,
	},
	.provides	= { FR_EAP_METHOD_AKA },
	.type_identity	= mod_type_identity,
	.session_init	= mod_session_init,
	.namespace	= &dict_eap_aka_sim
};

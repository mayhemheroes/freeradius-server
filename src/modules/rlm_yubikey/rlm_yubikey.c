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
 * @file rlm_yubikey.c
 * @brief Authentication for yubikey OTP tokens.
 *
 * @author Arran Cudbard-Bell (a.cudbardb@networkradius.com)
 * @copyright 2013 The FreeRADIUS server project
 * @copyright 2013 Network RADIUS (legal@networkradius.com)
 */
RCSID("$Id$")

#include <freeradius-devel/radius/radius.h>
#include <freeradius-devel/unlang/xlat_func.h>
#include <freeradius-devel/server/rcode.h>
#include <freeradius-devel/unlang/action.h>
#include "rlm_yubikey.h"

#ifdef HAVE_YKCLIENT
static const conf_parser_t validation_config[] = {
	{ FR_CONF_OFFSET("client_id", rlm_yubikey_t, client_id), .dflt = 0 },
	{ FR_CONF_OFFSET_FLAGS("api_key", CONF_FLAG_SECRET, rlm_yubikey_t, api_key) },
	CONF_PARSER_TERMINATOR
};
#endif

static const conf_parser_t module_config[] = {
	{ FR_CONF_OFFSET("id_length", rlm_yubikey_t, id_len), .dflt = "12" },
	{ FR_CONF_OFFSET("split", rlm_yubikey_t, split), .dflt = "yes" },
	{ FR_CONF_OFFSET("decrypt", rlm_yubikey_t, decrypt), .dflt = "no" },
	{ FR_CONF_OFFSET("validate", rlm_yubikey_t, validate), .dflt = "no" },
#ifdef HAVE_YKCLIENT
	{ FR_CONF_POINTER("validation", 0, CONF_FLAG_SUBSECTION, NULL), .subcs = (void const *) validation_config },
#endif
	CONF_PARSER_TERMINATOR
};

static fr_dict_t const *dict_freeradius;
static fr_dict_t const *dict_radius;

extern fr_dict_autoload_t rlm_yubikey_dict[];
fr_dict_autoload_t rlm_yubikey_dict[] = {
	{ .out = &dict_freeradius, .proto = "freeradius" },
	{ .out = &dict_radius, .proto = "radius" },
	{ NULL }
};

fr_dict_attr_t const *attr_auth_type;
fr_dict_attr_t const *attr_user_password;
fr_dict_attr_t const *attr_yubikey_key;
fr_dict_attr_t const *attr_yubikey_public_id;
fr_dict_attr_t const *attr_yubikey_private_id;
fr_dict_attr_t const *attr_yubikey_counter;
fr_dict_attr_t const *attr_yubikey_timestamp;
fr_dict_attr_t const *attr_yubikey_random;
fr_dict_attr_t const *attr_yubikey_otp;

extern fr_dict_attr_autoload_t rlm_yubikey_dict_attr[];
fr_dict_attr_autoload_t rlm_yubikey_dict_attr[] = {
	{ .out = &attr_auth_type, .name = "Auth-Type", .type = FR_TYPE_UINT32, .dict = &dict_freeradius },
	{ .out = &attr_user_password, .name = "User-Password", .type = FR_TYPE_STRING, .dict = &dict_radius },
	{ .out = &attr_yubikey_key, .name = "Vendor-Specific.Yubico.Yubikey-Key", .type = FR_TYPE_OCTETS, .dict = &dict_radius },
	{ .out = &attr_yubikey_public_id, .name = "Vendor-Specific.Yubico.Yubikey-Public-ID", .type = FR_TYPE_STRING, .dict = &dict_radius },
	{ .out = &attr_yubikey_private_id, .name = "Vendor-Specific.Yubico.Yubikey-Private-ID", .type = FR_TYPE_OCTETS, .dict = &dict_radius },
	{ .out = &attr_yubikey_counter, .name = "Vendor-Specific.Yubico.Yubikey-Counter", .type = FR_TYPE_UINT32, .dict = &dict_radius },
	{ .out = &attr_yubikey_timestamp, .name = "Vendor-Specific.Yubico.Yubikey-Timestamp", .type = FR_TYPE_UINT32, .dict = &dict_radius },
	{ .out = &attr_yubikey_random, .name = "Vendor-Specific.Yubico.Yubikey-Random", .type = FR_TYPE_UINT32, .dict = &dict_radius },
	{ .out = &attr_yubikey_otp, .name = "Vendor-Specific.Yubico.Yubikey-OTP", .type = FR_TYPE_STRING, .dict = &dict_radius },
	{ NULL }
};

static char const modhextab[] = "cbdefghijklnrtuv";
static char const hextab[] = "0123456789abcdef";

#define is_modhex(x) (memchr(modhextab, tolower(x), 16))

/** Convert yubikey modhex to normal hex
 *
 * The same buffer may be passed as modhex and hex to convert the modhex in place.
 *
 * Modhex and hex must be the same size.
 *
 * @param[in] modhex data.
 * @param[in] len of input and output buffers.
 * @param[out] hex where to write the standard hexits.
 * @return
 *	- The number of bytes written to the output buffer.
 *	- -1 on failure.
 */
static ssize_t modhex2hex(char const *modhex, char *hex, size_t len)
{
	size_t i;
	char *c1, *c2;

	for (i = 0; i < len; i += 2) {
		if (modhex[i] == '\0') {
			break;
		}

		/*
		 *	We only deal with whole bytes
		 */
		if (modhex[i + 1] == '\0')
			return -1;

		if (!(c1 = memchr(modhextab, tolower((uint8_t) modhex[i]), 16)) ||
		    !(c2 = memchr(modhextab, tolower((uint8_t) modhex[i + 1]), 16)))
			return -1;

		hex[i] = hextab[c1 - modhextab];
		hex[i + 1] = hextab[c2 - modhextab];
	}

	return i;
}

static xlat_arg_parser_t const modhex_to_hex_xlat_arg[] = {
	{ .required = true, .concat = true, .type = FR_TYPE_STRING },
	XLAT_ARG_PARSER_TERMINATOR
};

/** Xlat to convert Yubikey modhex to standard hex
 *
 * Example:
@verbatim
%modhextohex('vvrbuctetdhc') == "ffc1e0d3d260"
@endverbatim
 *
 * @ingroup xlat_functions
 */
static xlat_action_t modhex_to_hex_xlat(UNUSED TALLOC_CTX *ctx, fr_dcursor_t * out,
					UNUSED xlat_ctx_t const *xctx, request_t *request,
					fr_value_box_list_t *in)
{
	ssize_t 	len;
	fr_value_box_t	*arg = fr_value_box_list_pop_head(in);
	char		*p = UNCONST(char *, arg->vb_strvalue);

	/*
	 *	mod2hex allows conversions in place
	 */
	len = modhex2hex(p, p, arg->vb_length);
	if (len <= 0) {
		REDEBUG("Modhex string invalid");
		talloc_free(arg);
		return XLAT_ACTION_FAIL;
	}

	fr_dcursor_append(out, arg);
	return XLAT_ACTION_DONE;
}

static int mod_load(void)
{
	xlat_t		*xlat;

	if (fr_dict_autoload(rlm_yubikey_dict) < 0) {
		PERROR("%s", __FUNCTION__);
		return -1;
	}

	if (fr_dict_attr_autoload(rlm_yubikey_dict_attr) < 0) {
		PERROR("%s", __FUNCTION__);
		fr_dict_autofree(rlm_yubikey_dict);
		return -1;
	}

	if (unlikely(!(xlat = xlat_func_register(NULL, "modhextohex", modhex_to_hex_xlat, FR_TYPE_STRING)))) return -1;
	xlat_func_args_set(xlat, modhex_to_hex_xlat_arg);
	xlat_func_flags_set(xlat, XLAT_FUNC_FLAG_PURE);

	return 0;
}

static void mod_unload(void)
{
	xlat_func_unregister("modhextohex");
	fr_dict_autofree(rlm_yubikey_dict);
}

#ifndef HAVE_YUBIKEY
static int mod_bootstrap(module_inst_ctx_t const *mctx)
{
	rlm_yubikey_t const *inst = talloc_get_type_abort(mctx->mi->data, rlm_yubikey_t);

	if (inst->decrypt) {
		cf_log_err(mctx->mi->conf, "Requires libyubikey for OTP decryption");
		return -1;
	}
	return 0;
}
#endif

/*
 *	Do any per-module initialization that is separate to each
 *	configured instance of the module.  e.g. set up connections
 *	to external databases, read configuration files, set up
 *	dictionary entries, etc.
 *
 *	If configuration information is given in the config section
 *	that must be referenced in later calls, store a handle to it
 *	in *instance otherwise put a null pointer there.
 */
static int mod_instantiate(module_inst_ctx_t const *mctx)
{
	rlm_yubikey_t	*inst = talloc_get_type_abort(mctx->mi->data, rlm_yubikey_t);
	CONF_SECTION    *conf = mctx->mi->conf;

	inst->name = mctx->mi->name;

	inst->auth_type = fr_dict_enum_by_name(attr_auth_type, inst->name, -1);
	if (!inst->auth_type) {
		WARN("Failed to find 'authenticate %s {...}' section.  Yubikey authentication will likely not work",
		     mctx->mi->name);
	}

	if (inst->validate) {
#ifdef HAVE_YKCLIENT
		CONF_SECTION *cs;

		cs = cf_section_find(conf, "validation", CF_IDENT_ANY);
		if (!cs) {
			cf_log_err(conf, "Missing validation section");
			return -1;
		}

		if (rlm_yubikey_ykclient_init(cs, inst) < 0) {
			return -1;
		}
#else
		cf_log_err(conf, "Requires libykclient for OTP validation against Yubicloud servers");
		return -1;
#endif
	}

	return 0;
}

#ifdef HAVE_YKCLIENT
static int mod_detach(module_detach_ctx_t const *mctx)
{
	rlm_yubikey_ykclient_detach(talloc_get_type_abort(mctx->mi->data, rlm_yubikey_t));
	return 0;
}
#endif

static int CC_HINT(nonnull) otp_string_valid(rlm_yubikey_t const *inst, char const *otp, size_t len)
{
	size_t i;

	for (i = inst->id_len; i < len; i++) {
		if (!is_modhex(otp[i])) return -i;
	}

	return 1;
}


/*
 *	Find the named user in this modules database.  Create the set
 *	of attribute-value pairs to check and reply with for this user
 *	from the database. The authentication code only needs to check
 *	the password, the rest is done here.
 */
static unlang_action_t CC_HINT(nonnull) mod_authorize(unlang_result_t *p_result, module_ctx_t const *mctx, request_t *request)
{
	rlm_yubikey_t const	*inst = talloc_get_type_abort_const(mctx->mi->data, rlm_yubikey_t);
	char const		*passcode;
	size_t			len;
	fr_pair_t		*vp, *password;
	char const		*otp;
	size_t			password_len;
	int			ret;

	/*
	 *	Can't do yubikey auth if there's no password.
	 */
	password = fr_pair_find_by_da(&request->request_pairs, NULL, attr_user_password);
	if (!password) {
		/*
		 *	Don't print out debugging messages if we know
		 *	they're useless.
		 */
		if ((request->proto_dict == dict_radius) && request->packet->code != FR_RADIUS_CODE_ACCESS_CHALLENGE) {
			RDEBUG2("No cleartext password in the request. Can't do Yubikey authentication");
		}

		RETURN_UNLANG_NOOP;
	}

	passcode = password->vp_strvalue;
	len = password->vp_length;

	/*
	 *	Now see if the passcode is the correct length (in its raw
	 *	modhex encoded form).
	 *
	 *	<public_id (6-16 bytes)> + <aes-block (32 bytes)>
	 *
	 */
	if (len < (inst->id_len + YUBIKEY_TOKEN_LEN)) {
		RDEBUG2("User-Password value is not the correct length, expected at least %u bytes, got %zu bytes",
			inst->id_len + YUBIKEY_TOKEN_LEN, len);
		RETURN_UNLANG_NOOP;
	}

	password_len = (len - (inst->id_len + YUBIKEY_TOKEN_LEN));
	otp = passcode + password_len;
	ret = otp_string_valid(inst, otp, (inst->id_len + YUBIKEY_TOKEN_LEN));
	if (ret <= 0) {
		if (RDEBUG_ENABLED3) {
			RDMARKER(otp, -(ret), "User-Password (aes-block) value contains non modhex chars");
		} else {
			RDEBUG2("User-Password (aes-block) value contains non modhex chars");
		}
		RETURN_UNLANG_NOOP;
	}

	/* May be a concatenation, check the last 32 bytes are modhex */
	if (inst->split) {
		/*
		 *	Insert a new request attribute just containing the OTP
		 *	portion.
		 */
		MEM(pair_update_request(&vp, attr_yubikey_otp) >= 0);
		fr_pair_value_strdup(vp, otp, password->vp_tainted);

		/*
		 *	Replace the existing string buffer for the password
		 *	attribute with one just containing the password portion.
		 */
		MEM(fr_pair_value_bstr_realloc(password, NULL, password_len) == 0);

		RDEBUG2("request.%pP", vp);
		RDEBUG2("request.%pP", password);

		/*
		 *	So the ID split code works on the non password portion.
		 */
		passcode = vp->vp_strvalue;
	}

	/*
	 *	Split out the Public ID in case another module in authorize
	 *	needs to verify it's associated with the user.
	 *
	 *	It's left up to the user if they want to decode it or not.
	 */
	if (inst->id_len) {
		MEM(pair_update_request(&vp, attr_yubikey_public_id) >= 0);
		fr_pair_value_bstrndup(vp, passcode, inst->id_len, true);
		RDEBUG2("request.%pP", vp);
	}

	if (!inst->auth_type) {
		WARN("No 'authenticate %s {...}' section or 'Auth-Type = %s' set.  Cannot setup Yubikey authentication",
		     mctx->mi->name, mctx->mi->name);
		RETURN_UNLANG_NOOP;
	}

	if (!module_rlm_section_type_set(request, attr_auth_type, inst->auth_type)) RETURN_UNLANG_NOOP;

	RETURN_UNLANG_OK;
}


/*
 *	Authenticate the user with the given password.
 */
static unlang_action_t CC_HINT(nonnull) mod_authenticate(unlang_result_t *p_result, module_ctx_t const *mctx, request_t *request)
{
	rlm_yubikey_t const	*inst = talloc_get_type_abort_const(mctx->mi->data, rlm_yubikey_t);
	char const		*passcode = NULL;
	fr_pair_t const		*vp;
	size_t			len;
	int			ret;

	p_result->rcode = RLM_MODULE_NOOP;

	vp = fr_pair_find_by_da_nested(&request->request_pairs, NULL, attr_yubikey_otp);
	if (!vp) {
		RDEBUG2("No Yubikey-OTP attribute found, falling back to User-Password");
		/*
		 *	Can't do yubikey auth if there's no password.
		 */
		vp = fr_pair_find_by_da(&request->request_pairs, NULL, attr_user_password);
		if (!vp) {
			REDEBUG("No User-Password in the request. Can't do Yubikey authentication");
			RETURN_UNLANG_INVALID;
		}
	}

	passcode = vp->vp_strvalue;
	len = vp->vp_length;

	/*
	 *	Verify the passcode is the correct length (in its raw
	 *	modhex encoded form).
	 *
	 *	<public_id (6-16 bytes)> + <aes-block (32 bytes)>
	 */
	if (len != (inst->id_len + YUBIKEY_TOKEN_LEN)) {
		REDEBUG("%s value is not the correct length, expected bytes %u, got bytes %zu",
			vp->da->name, inst->id_len + YUBIKEY_TOKEN_LEN, len);
		RETURN_UNLANG_INVALID;
	}

	ret = otp_string_valid(inst, passcode, (inst->id_len + YUBIKEY_TOKEN_LEN));
	if (ret <= 0) {
		if (RDEBUG_ENABLED3) {
			REMARKER(passcode, -ret, "Passcode (aes-block) value contains non modhex chars");
		} else {
			RERROR("Passcode (aes-block) value contains non modhex chars");
		}
		RETURN_UNLANG_INVALID;
	}

#ifdef HAVE_YUBIKEY
	if (inst->decrypt) {

		rlm_yubikey_decrypt(p_result, mctx, request, passcode);
		if (p_result->rcode != RLM_MODULE_OK) return UNLANG_ACTION_CALCULATE_RESULT;
		/* Fall-Through to doing ykclient auth in addition to local auth */
	}
#endif

#ifdef HAVE_YKCLIENT
	if (inst->validate) return rlm_yubikey_validate(p_result, mctx, request, passcode);
#endif
	return UNLANG_ACTION_CALCULATE_RESULT;
}

/*
 *	The module name should be the only globally exported symbol.
 *	That is, everything else should be 'static'.
 *
 *	If the module needs to temporarily modify it's instantiation
 *	data, the type should be changed to MODULE_TYPE_THREAD_UNSAFE.
 *	The server will then take care of ensuring that the module
 *	is single-threaded.
 */
extern module_rlm_t rlm_yubikey;
module_rlm_t rlm_yubikey = {
	.common = {
		.magic		= MODULE_MAGIC_INIT,
		.name		= "yubikey",
		.inst_size	= sizeof(rlm_yubikey_t),
		.onload		= mod_load,
		.unload		= mod_unload,
		.config		= module_config,
#ifndef HAVE_YUBIKEY
		.bootstrap	= mod_bootstrap,
#endif
		.instantiate	= mod_instantiate,
#ifdef HAVE_YKCLIENT
		.detach		= mod_detach,
#endif
	},
	.method_group = {
		.bindings = (module_method_binding_t[]){
			{ .section = SECTION_NAME("authenticate", CF_IDENT_ANY), .method = mod_authenticate },
			{ .section = SECTION_NAME("recv", "Access-Request"), .method = mod_authorize },
			MODULE_BINDING_TERMINATOR
		}
	}
};

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

/**
 * $Id$
 * @file proto_dhcpv6.c
 * @brief DHCPV6 master protocol handler.
 *
 * @copyright 2020 Network RADIUS SAS (legal@networkradius.com)
 */
#define LOG_PREFIX "proto_dhcpv6"

#include <freeradius-devel/io/listen.h>
#include <freeradius-devel/server/module_rlm.h>
#include <freeradius-devel/util/debug.h>
#include "proto_dhcpv6.h"

extern fr_app_t proto_dhcpv6;
static int type_parse(TALLOC_CTX *ctx, void *out, void *parent, CONF_ITEM *ci, conf_parser_t const *rule);
static int transport_parse(TALLOC_CTX *ctx, void *out, void *parent, CONF_ITEM *ci, conf_parser_t const *rule);

static const conf_parser_t priority_config[] = {
	{ FR_CONF_OFFSET("Solicit", proto_dhcpv6_t, priorities[FR_DHCPV6_SOLICIT]),
	  .func = cf_table_parse_int, .uctx = &(cf_table_parse_ctx_t){ .table = channel_packet_priority, .len = &channel_packet_priority_len }, .dflt = "normal" },
	{ FR_CONF_OFFSET("Request", proto_dhcpv6_t, priorities[FR_DHCPV6_REQUEST]),
	  .func = cf_table_parse_int, .uctx = &(cf_table_parse_ctx_t){ .table = channel_packet_priority, .len = &channel_packet_priority_len }, .dflt = "normal" },
	{ FR_CONF_OFFSET("Renew", proto_dhcpv6_t, priorities[FR_DHCPV6_RENEW]),
	  .func = cf_table_parse_int, .uctx = &(cf_table_parse_ctx_t){ .table = channel_packet_priority, .len = &channel_packet_priority_len }, .dflt = "normal" },
	{ FR_CONF_OFFSET("Rebind", proto_dhcpv6_t, priorities[FR_DHCPV6_REBIND]),
	  .func = cf_table_parse_int, .uctx = &(cf_table_parse_ctx_t){ .table = channel_packet_priority, .len = &channel_packet_priority_len }, .dflt = "normal" },
	{ FR_CONF_OFFSET("Release", proto_dhcpv6_t, priorities[FR_DHCPV6_RELEASE]),
	  .func = cf_table_parse_int, .uctx = &(cf_table_parse_ctx_t){ .table = channel_packet_priority, .len = &channel_packet_priority_len }, .dflt = "normal" },
	{ FR_CONF_OFFSET("Decline", proto_dhcpv6_t, priorities[FR_DHCPV6_DECLINE]),
	  .func = cf_table_parse_int, .uctx = &(cf_table_parse_ctx_t){ .table = channel_packet_priority, .len = &channel_packet_priority_len }, .dflt = "normal" },
	{ FR_CONF_OFFSET("Information-Request", proto_dhcpv6_t, priorities[FR_DHCPV6_INFORMATION_REQUEST]),
	  .func = cf_table_parse_int, .uctx = &(cf_table_parse_ctx_t){ .table = channel_packet_priority, .len = &channel_packet_priority_len }, .dflt = "normal" },
	{ FR_CONF_OFFSET("Relay-Forward", proto_dhcpv6_t, priorities[FR_DHCPV6_RELAY_FORWARD]),
	  .func = cf_table_parse_int, .uctx = &(cf_table_parse_ctx_t){ .table = channel_packet_priority, .len = &channel_packet_priority_len }, .dflt = "normal" },
	CONF_PARSER_TERMINATOR
};

static conf_parser_t const limit_config[] = {
	{ FR_CONF_OFFSET("cleanup_delay", proto_dhcpv6_t, io.cleanup_delay), .dflt = "5.0" } ,
	{ FR_CONF_OFFSET("idle_timeout", proto_dhcpv6_t, io.idle_timeout), .dflt = "30.0" } ,
	{ FR_CONF_OFFSET("dynamic_timeout", proto_dhcpv6_t, io.dynamic_timeout), .dflt = "600.0" } ,
	{ FR_CONF_OFFSET("nak_lifetime", proto_dhcpv6_t, io.nak_lifetime), .dflt = "30.0" } ,

	{ FR_CONF_OFFSET("max_connections", proto_dhcpv6_t, io.max_connections), .dflt = "1024" } ,
	{ FR_CONF_OFFSET("max_clients", proto_dhcpv6_t, io.max_clients), .dflt = "256" } ,
	{ FR_CONF_OFFSET("max_pending_packets", proto_dhcpv6_t, io.max_pending_packets), .dflt = "256" } ,

	/*
	 *	For performance tweaking.  NOT for normal humans.
	 */
	{ FR_CONF_OFFSET("max_packet_size", proto_dhcpv6_t, max_packet_size) } ,
	{ FR_CONF_OFFSET("num_messages", proto_dhcpv6_t, num_messages) } ,
	{ FR_CONF_POINTER("priority", 0, CONF_FLAG_SUBSECTION, NULL), .subcs = (void const *) priority_config },

	CONF_PARSER_TERMINATOR
};

static conf_parser_t const log_config[] = {
	{ FR_CONF_OFFSET("ignored_clients", proto_dhcpv6_t, io.log_ignored_clients), .dflt = "yes" } ,

	CONF_PARSER_TERMINATOR
};

/** How to parse a DHCPV6 listen section
 *
 */
static conf_parser_t const proto_dhcpv6_config[] = {
	{ FR_CONF_OFFSET_FLAGS("type", CONF_FLAG_NOT_EMPTY, proto_dhcpv6_t, allowed_types), .func = type_parse },
	{ FR_CONF_OFFSET_TYPE_FLAGS("transport", FR_TYPE_VOID, 0, proto_dhcpv6_t, io.submodule),
	  .func = transport_parse },

	{ FR_CONF_POINTER("log", 0, CONF_FLAG_SUBSECTION, NULL), .subcs = (void const *) log_config },
	{ FR_CONF_POINTER("limit", 0, CONF_FLAG_SUBSECTION, NULL), .subcs = (void const *) limit_config },

	CONF_PARSER_TERMINATOR
};

static fr_dict_t const *dict_dhcpv6;

extern fr_dict_autoload_t proto_dhcpv6_dict[];
fr_dict_autoload_t proto_dhcpv6_dict[] = {
	{ .out = &dict_dhcpv6, .proto = "dhcpv6" },
	{ NULL }
};

static fr_dict_attr_t const *attr_packet_type;
static fr_dict_attr_t const *attr_client_id;

extern fr_dict_attr_autoload_t proto_dhcpv6_dict_attr[];
fr_dict_attr_autoload_t proto_dhcpv6_dict_attr[] = {
	{ .out = &attr_packet_type, .name = "Packet-Type", .type = FR_TYPE_UINT32, .dict = &dict_dhcpv6},
	{ .out = &attr_client_id, .name = "Client-Id", .type = FR_TYPE_STRUCT, .dict = &dict_dhcpv6},
	{ NULL }
};

/** Translates the packet-type into a submodule name
 *
 * @param[in] ctx	to allocate data in (instance of proto_dhcpv6).
 * @param[out] out	Where to write a module_instance_t containing the module handle and instance.
 * @param[in] parent	Base structure address.
 * @param[in] ci	#CONF_PAIR specifying the name of the type module.
 * @param[in] rule	unused.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
static int type_parse(UNUSED TALLOC_CTX *ctx, void *out, void *parent,
		      CONF_ITEM *ci, UNUSED conf_parser_t const *rule)
{
	proto_dhcpv6_t				*inst = talloc_get_type_abort(parent, proto_dhcpv6_t);
	fr_dict_enum_value_t const		*dv;
	CONF_PAIR				*cp;
	char const				*value;

	cp = cf_item_to_pair(ci);
	value = cf_pair_value(cp);

	dv = fr_dict_enum_by_name(attr_packet_type, value, -1);
	if (!dv || (dv->value->vb_uint32 >= FR_DHCPV6_CODE_MAX)) {
		cf_log_err(ci, "Unknown DHCPv6 packet type '%s'", value);
		return -1;
	}

	inst->allowed[dv->value->vb_uint32] = true;
	*((char const **) out) = value;

	return 0;
}

static int transport_parse(TALLOC_CTX *ctx, void *out, void *parent, CONF_ITEM *ci, conf_parser_t const *rule)
{
	proto_dhcpv6_t		*inst = talloc_get_type_abort(parent, proto_dhcpv6_t);
	module_instance_t	*mi;

	if (unlikely(virtual_server_listen_transport_parse(ctx, out, parent, ci, rule) < 0)) {
		return -1;
	}

	mi = talloc_get_type_abort(*(void **)out, module_instance_t);
	inst->io.app_io = (fr_app_io_t const *)mi->exported;
	inst->io.app_io_instance = mi->data;
	inst->io.app_io_conf = mi->conf;

	return 0;
}

/** Decode the packet
 *
 */
static int mod_decode(UNUSED void const *instance, request_t *request, uint8_t *const data, size_t data_len)
{
	fr_io_track_t const	*track = talloc_get_type_abort_const(request->async->packet_ctx, fr_io_track_t);
	fr_io_address_t const	*address = track->address;
	fr_client_t const		*client;
	fr_packet_t	*packet = request->packet;

	RHEXDUMP3(data, data_len, "proto_dhcpv6 decode packet");

	client = address->radclient;

	/*
	 *	Hacks for now until we have a lower-level decode routine.
	 */
	request->packet->code = data[0];
	request->packet->id = (data[1] << 16) | (data[2] << 8) | data[3];
	request->reply->id = request->packet->id;

	request->packet->data = talloc_memdup(request->packet, data, data_len);
	request->packet->data_len = data_len;

	/*
	 *	Note that we don't set a limit on max_attributes here.
	 *	That MUST be set and checked in the underlying
	 *	transport, via a call to fr_dhcpv6_ok().
	 */
	if (fr_dhcpv6_decode(request->request_ctx, &request->request_pairs, packet->data, packet->data_len) < 0) {
		RPEDEBUG("Failed decoding packet");
		return -1;
	}

	/*
	 *	Set the rest of the fields.
	 */
	request->client = UNCONST(fr_client_t *, client);

	request->packet->socket = address->socket;
	fr_socket_addr_swap(&request->reply->socket, &address->socket);

	REQUEST_VERIFY(request);

	return 0;
}

static ssize_t mod_encode(UNUSED void const *instance, request_t *request, uint8_t *buffer, size_t buffer_len)
{
	fr_io_track_t		*track = talloc_get_type_abort(request->async->packet_ctx, fr_io_track_t);
	fr_io_address_t const	*address = track->address;
	fr_dhcpv6_packet_t	*reply = (fr_dhcpv6_packet_t *) buffer;
	fr_dhcpv6_packet_t	*original = (fr_dhcpv6_packet_t *) request->packet->data;
	ssize_t			data_len;
	fr_client_t const		*client;

	/*
	 *	Process layer NAK, never respond, or "Do not respond".
	 */
	if ((buffer_len == 1) ||
	    (request->reply->code == FR_DHCPV6_DO_NOT_RESPOND) ||
	    (request->reply->code == 0) || (request->reply->code >= FR_DHCPV6_CODE_MAX)) {
		track->do_not_respond = true;
		return 1;
	}

	client = address->radclient;
	fr_assert(client);

	/*
	 *	Dynamic client stuff
	 */
	if (client->dynamic && !client->active) {
		fr_client_t *new_client;

		fr_assert(buffer_len >= sizeof(client));

		/*
		 *	We don't accept the new client, so don't do
		 *	anything.
		 */
		if (request->reply->code != FR_DHCPV6_REPLY) {
			*buffer = true;
			return 1;
		}

		/*
		 *	Allocate the client.  If that fails, send back a NAK.
		 *
		 *	@todo - deal with NUMA zones?  Or just deal with this
		 *	client being in different memory.
		 *
		 *	Maybe we should create a CONF_SECTION from the client,
		 *	and pass *that* back to mod_write(), which can then
		 *	parse it to create the actual client....
		 */
		new_client = client_afrom_request(NULL, request);
		if (!new_client) {
			PERROR("Failed creating new client");
			buffer[0] = true;
			return 1;
		}

		memcpy(buffer, &new_client, sizeof(new_client));
		return sizeof(new_client);
	}

	if (buffer_len < 4) {
		REDEBUG("Output buffer is too small to hold a DHCPv6 packet.");
		return -1;
	}

	memset(buffer, 0, buffer_len);
	memcpy(&reply->transaction_id, &original->transaction_id, sizeof(reply->transaction_id));

	data_len = fr_dhcpv6_encode(&FR_DBUFF_TMP(buffer, buffer_len),
				    request->packet->data, request->packet->data_len,
				    request->reply->code, &request->reply_pairs);
	if (data_len < 0) {
		RPEDEBUG("Failed encoding DHCPv6 reply");
		return -1;
	}

	/*
	 *	ACK the client ID.
	 */
	if (!fr_dhcpv6_option_find(buffer + 4, buffer + data_len, attr_client_id->attr)) {
		uint8_t const *client_id;

		client_id = fr_dhcpv6_option_find(request->packet->data + 4, request->packet->data + request->packet->data_len, attr_client_id->attr);
		if (client_id) {
			size_t len = fr_nbo_to_uint16(client_id + 2);
			if (len <= (buffer_len - (data_len + 4))) {
				memcpy(buffer + data_len, client_id, 4 + len);
				data_len += 4 + len;
			}
		}
	}

	RHEXDUMP3(buffer, data_len, "proto_dhcpv6 encode packet");

	request->reply->data_len = data_len;
	return data_len;
}

static int mod_priority_set(void const *instance, uint8_t const *buffer, UNUSED size_t buflen)
{
	proto_dhcpv6_t const *inst = talloc_get_type_abort_const(instance, proto_dhcpv6_t);

	fr_assert(buffer[0] > 0);
	fr_assert(buffer[0] < FR_DHCPV6_CODE_MAX);

	/*
	 *	Disallowed packet
	 */
	if (!inst->priorities[buffer[0]]) return 0;

	if (!inst->allowed[buffer[0]]) return -1;

	/*
	 *	@todo - if we cared, we could also return -1 for "this
	 *	is a bad packet".  But that's really only for
	 *	mod_inject, as we assume that app_io->read() always
	 *	returns good packets.
	 */

	/*
	 *	Return the configured priority.
	 */
	return inst->priorities[buffer[0]];
}

/** Open listen sockets/connect to external event source
 *
 * @param[in] instance	Ctx data for this application.
 * @param[in] sc	to add our file descriptor to.
 * @param[in] conf	Listen section parsed to give us instance.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
static int mod_open(void *instance, fr_schedule_t *sc, UNUSED CONF_SECTION *conf)
{
	proto_dhcpv6_t 	*inst = talloc_get_type_abort(instance, proto_dhcpv6_t);

	inst->io.app = &proto_dhcpv6;
	inst->io.app_instance = instance;

	return fr_master_io_listen(&inst->io, sc,
				   inst->max_packet_size, inst->num_messages);
}

/** Instantiate the application
 *
 * Instantiate I/O and type submodules.
 *
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
static int mod_instantiate(module_inst_ctx_t const *mctx)
{
	proto_dhcpv6_t		*inst = talloc_get_type_abort(mctx->mi->data, proto_dhcpv6_t);

	/*
	 *	Ensure that the server CONF_SECTION is always set.
	 */
	inst->io.server_cs = cf_item_to_section(cf_parent(mctx->mi->conf));

	fr_assert(dict_dhcpv6 != NULL);
	fr_assert(attr_packet_type != NULL);

	/*
	 *	No IO module, it's an empty listener.
	 */
	if (!inst->io.submodule) return 0;

	/*
	 *	These timers are usually protocol specific.
	 */
	FR_TIME_DELTA_BOUND_CHECK("idle_timeout", inst->io.idle_timeout, >=, fr_time_delta_from_sec(1));
	FR_TIME_DELTA_BOUND_CHECK("idle_timeout", inst->io.idle_timeout, <=, fr_time_delta_from_sec(600));

	FR_TIME_DELTA_BOUND_CHECK("nak_lifetime", inst->io.nak_lifetime, >=, fr_time_delta_from_sec(1));
	FR_TIME_DELTA_BOUND_CHECK("nak_lifetime", inst->io.nak_lifetime, <=, fr_time_delta_from_sec(600));

	FR_TIME_DELTA_BOUND_CHECK("cleanup_delay", inst->io.cleanup_delay, <=, fr_time_delta_from_sec(30));
	FR_TIME_DELTA_BOUND_CHECK("cleanup_delay", inst->io.cleanup_delay, >, fr_time_delta_from_sec(0));

	/*
	 *	Tell the master handler about the main protocol instance.
	 */
	inst->io.app = &proto_dhcpv6;
	inst->io.app_instance = inst;

	/*
	 *	We will need this for dynamic clients and connected sockets.
	 */
	inst->io.mi = mctx->mi;

	/*
	 *	These configuration items are not printed by default,
	 *	because normal people shouldn't be touching them.
	 */
	if (!inst->max_packet_size && inst->io.app_io) inst->max_packet_size = inst->io.app_io->default_message_size;

	if (!inst->num_messages) inst->num_messages = 256;

	FR_INTEGER_BOUND_CHECK("num_messages", inst->num_messages, >=, 32);
	FR_INTEGER_BOUND_CHECK("num_messages", inst->num_messages, <=, 65535);

	FR_INTEGER_BOUND_CHECK("max_packet_size", inst->max_packet_size, >=, 1024);
	FR_INTEGER_BOUND_CHECK("max_packet_size", inst->max_packet_size, <=, 65535);

	/*
	 *	Instantiate the transport module before calling the
	 *	common instantiation function.
	 */
	if (module_instantiate(inst->io.submodule) < 0) return -1;

	/*
	 *	Instantiate the master io submodule
	 */
	return fr_master_app_io.common.instantiate(MODULE_INST_CTX(inst->io.mi));
}

static int mod_load(void)
{
	if (fr_dhcpv6_global_init() < 0) {
		PERROR("Failed initialising protocol library");
		return -1;
	}

	return 0;
}

static void mod_unload(void)
{
	fr_dhcpv6_global_free();
}

fr_app_t proto_dhcpv6 = {
	.common = {
		.magic			= MODULE_MAGIC_INIT,
		.name			= "dhcpv6",
		.config			= proto_dhcpv6_config,
		.inst_size		= sizeof(proto_dhcpv6_t),
		.onload			= mod_load,
		.unload			= mod_unload,

		.instantiate		= mod_instantiate
	},
	.dict			= &dict_dhcpv6,
	.open			= mod_open,
	.decode			= mod_decode,
	.encode			= mod_encode,
	.priority		= mod_priority_set
};

// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2026
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <glib.h>

#include "bluetooth/bluetooth.h"
#include "bluetooth/rfcomm.h"
#include "bluetooth/sdp.h"
#include "bluetooth/sdp_lib.h"
#include "bluetooth/uuid.h"

#include "gdbus/gdbus.h"

#include "emulator/bthost.h"
#include "emulator/hciemu.h"

#include "src/log.h"
#include "src/sdpd.h"
#include "src/shared/tester.h"

#define PROFILE_PATH "/org/bluez/test/profile"
#define AGENT_PATH "/org/bluez/test/agent"
#define PROFILE_UUID SPP_UUID
#define PROFILE_NAME "SPP Remote SDP Discovery"
#define PROFILE_MULTI_RECORD_REMOTE_NAME "Target Serial Port"
#define PROFILE_MULTI_RECORD_LEGACY_NAME "Legacy Serial Port"
#define PROFILE_DYNAMIC_NAME "SPP Dynamic Channel"
#define PROFILE_RECONNECT_NAME "SPP Reconnect Cycle"
#define PROFILE_AUTHENTICATION_NAME "SPP Authentication Required"
#define PROFILE_OUTGOING_AUTHENTICATION_NAME "Serial Port"
#define PROFILE_AUTH_NAME "SPP Authorization Gate"
#define PROFILE_INVALID_RECORD_NAME "SPP Invalid ServiceRecord Version"
#define PROFILE_MALFORMED_RECORD_NAME "SPP Missing Browse Group"
#define REMOTE_RFCOMM_CHANNEL 23
#define LEGACY_RFCOMM_CHANNEL 3
#define LOCAL_AUTHENTICATION_CHANNEL 8
#define LOCAL_AUTH_CHANNEL 7
#define LOCAL_INVALID_VERSION_CHANNEL 9
#define LOCAL_MALFORMED_RECORD_CHANNEL 10
#define TEST_PIN_CODE "0000"
#define BLUEZ_ERROR_INVALID_ARGUMENTS "org.bluez.Error.InvalidArguments"
#define SDP_MTU 672
#define NOT_READY_RETRY_MS 200
#define NOT_READY_MAX_RETRIES 10

#define SPP_TEST_BROWSE_GROUP_ATTR \
	"<attribute id=\"0x0005\">" \
	"<sequence><uuid value=\"0x1002\" /></sequence>" \
	"</attribute>"

#define SPP_TEST_RECORD_TEMPLATE \
	"<?xml version=\"1.0\" encoding=\"UTF-8\" ?>" \
	"<record>" \
	"<attribute id=\"0x0001\">" \
	"<sequence><uuid value=\"0x1101\" /></sequence>" \
	"</attribute>" \
	"%s" \
	"<attribute id=\"0x0004\">" \
	"<sequence>" \
	"<sequence><uuid value=\"0x0100\" /></sequence>" \
	"<sequence><uuid value=\"0x0003\" /><uint8 value=\"0x%02x\" /></sequence>" \
	"</sequence>" \
	"</attribute>" \
	"<attribute id=\"0x0009\">" \
	"<sequence><sequence><uuid value=\"0x1101\" /><uint16 value=\"0x%04x\" /></sequence></sequence>" \
	"</attribute>" \
	"<attribute id=\"0x0100\"><text value=\"%s\" /></attribute>" \
	"</record>"

struct test_data;

struct sdp_connection {
	struct test_data *data;
	uint16_t handle;
	uint16_t cid;
	int server_fd;
	int client_fd;
};

struct test_data {
	DBusConnection *dbus_conn;
	GDBusClient *dbus_client;
	GDBusProxy *adapter_proxy;
	GDBusProxy *device_proxy;
	GDBusProxy *agent_manager_proxy;
	GDBusProxy *manager_proxy;
	struct hciemu *hciemu;
	struct bthost *peer;
	GSList *sdp_connections;
	const char *profile_name;
	const char *profile_role;
	const char *peer_service_name;
	const char *peer_extra_service_name;
	char *service_record;
	const char *expected_register_error;
	bool adapter_powered;
	bool adapter_connectable;
	bool setup_complete;
	bool power_pending;
	bool connectable_pending;
	bool agent_registered;
	bool agent_register_pending;
	bool agent_default_pending;
	bool profile_pending;
	bool profile_registered;
	bool register_error_seen;
	bool authentication_seen;
	bool authorization_seen;
	bool discovery_started;
	bool connect_requested;
	bool test_started;
	bool rfcomm_connected;
	bool test_passed;
	bool tearing_down;
	bool disconnect_requested;
	bool require_authentication;
	bool require_authorization;
	bool use_discovery;
	bool use_connect_profile;
	bool use_incoming_rfcomm;
	bool use_local_sdp_channel_lookup;
	bool incoming_connect_requested;
	bool incoming_rfcomm_requested;
	unsigned int expected_connections;
	unsigned int seen_connections;
	unsigned int discovery_not_ready_retries;
	unsigned int channel_not_ready_retries;
	unsigned int connect_not_ready_retries;
	guint discovery_retry_id;
	guint channel_retry_id;
	guint connect_retry_id;
	guint disconnect_cycle_id;
	uint16_t incoming_handle;
	uint8_t peer_extra_channel;
	uint8_t local_channel;
	int new_connection_fd;
};

static void print_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	tester_print("%s%s", prefix, str);
}

static bool match_proxy_address(GDBusProxy *proxy, const char *expected)
{
	DBusMessageIter iter;
	const char *value;

	if (!g_dbus_proxy_get_property(proxy, "Address", &iter))
		return false;

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return false;

	dbus_message_iter_get_basic(&iter, &value);

	return g_str_equal(value, expected);
}

static bool proxy_get_bool(GDBusProxy *proxy, const char *name, bool *value)
{
	DBusMessageIter iter;
	dbus_bool_t enabled;

	if (!g_dbus_proxy_get_property(proxy, name, &iter))
		return false;

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_BOOLEAN)
		return false;

	dbus_message_iter_get_basic(&iter, &enabled);
	*value = enabled;

	return true;
}

static bool proxy_get_string(GDBusProxy *proxy, const char *name,
						const char **value)
{
	DBusMessageIter iter;

	if (!g_dbus_proxy_get_property(proxy, name, &iter))
		return false;

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return false;

	dbus_message_iter_get_basic(&iter, value);
	return true;
}

static void unref_proxy(GDBusProxy **proxy)
{
	if (!*proxy)
		return;

	g_dbus_proxy_unref(*proxy);
	*proxy = NULL;
}

static void replace_proxy(GDBusProxy **dst, GDBusProxy *src)
{
	if (*dst == src)
		return;

	unref_proxy(dst);
	*dst = g_dbus_proxy_ref(src);
}

static void fail_test(const char *message)
{
	tester_warn("%s", message);
	tester_test_failed();
}

static void fail_setup(const char *message)
{
	tester_warn("%s", message);
	tester_setup_failed();
}

static void maybe_connect_profile(struct test_data *data);
static void maybe_register_agent(struct test_data *data);
static gboolean retry_discovery_cb(gpointer user_data);
static gboolean retry_local_channel_cb(gpointer user_data);
static gboolean retry_connect_profile_cb(gpointer user_data);
static gboolean disconnect_profile_cb(gpointer user_data);
static void maybe_make_adapter_connectable(struct test_data *data);

static bool schedule_not_ready_retry(guint *retry_id, GSourceFunc callback,
						void *user_data)
{
	if (*retry_id)
		return true;

	*retry_id = g_timeout_add(NOT_READY_RETRY_MS, callback, user_data);

	return *retry_id != 0;
}

static bool test_requires_agent(const struct test_data *data)
{
	return data->require_authentication || data->require_authorization;
}

static void maybe_setup_complete(struct test_data *data)
{
	if (data->setup_complete)
		return;

	if (!data->adapter_proxy || !data->manager_proxy)
		return;

	if (test_requires_agent(data) && !data->agent_registered)
		return;

	if (data->use_incoming_rfcomm && !data->adapter_connectable)
		return;

	if (!data->adapter_powered || !data->profile_registered)
		return;

	data->setup_complete = true;
	tester_setup_complete();
}

static struct sdp_connection *find_sdp_connection(struct test_data *data,
							uint16_t handle,
							uint16_t cid)
{
	GSList *entry;

	for (entry = data->sdp_connections; entry; entry = entry->next) {
		struct sdp_connection *conn = entry->data;

		if (conn->handle == handle && conn->cid == cid)
			return conn;
	}

	return NULL;
}

static void free_sdp_connection(struct sdp_connection *conn)
{
	if (!conn)
		return;

	if (conn->server_fd >= 0)
		close(conn->server_fd);

	if (conn->client_fd >= 0)
		close(conn->client_fd);

	g_free(conn);
}

static void remove_sdp_connection(struct sdp_connection *conn)
{
	struct test_data *data;

	if (!conn)
		return;

	data = conn->data;
	data->sdp_connections = g_slist_remove(data->sdp_connections, conn);
	free_sdp_connection(conn);
}

static char *build_spp_service_record(const char *name, uint8_t channel,
						uint16_t version,
						bool include_browse_group)
{
	const char *browse_group = include_browse_group ?
				SPP_TEST_BROWSE_GROUP_ATTR : "";

	return g_strdup_printf(SPP_TEST_RECORD_TEMPLATE, browse_group,
					channel, version, name);
}

static void register_serial_port(const char *name, uint8_t channel)
{
	sdp_list_t *svclass_id, *apseq, *proto[2], *profiles, *root, *aproto;
	uuid_t root_uuid, sp_uuid, l2cap, rfcomm;
	sdp_profile_desc_t profile;
	sdp_data_t *sdp_data, *channel_data;
	sdp_record_t *record = sdp_record_alloc();

	record->handle = sdp_next_handle();

	sdp_record_add(BDADDR_ANY, record);
	sdp_data = sdp_data_alloc(SDP_UINT32, &record->handle);
	sdp_attr_add(record, SDP_ATTR_RECORD_HANDLE, sdp_data);

	sdp_uuid16_create(&root_uuid, PUBLIC_BROWSE_GROUP);
	root = sdp_list_append(NULL, &root_uuid);
	sdp_set_browse_groups(record, root);
	sdp_list_free(root, NULL);

	sdp_uuid16_create(&sp_uuid, SERIAL_PORT_SVCLASS_ID);
	svclass_id = sdp_list_append(NULL, &sp_uuid);
	sdp_set_service_classes(record, svclass_id);
	sdp_list_free(svclass_id, NULL);

	sdp_uuid16_create(&profile.uuid, SERIAL_PORT_PROFILE_ID);
	profile.version = 0x0102;
	profiles = sdp_list_append(NULL, &profile);
	sdp_set_profile_descs(record, profiles);
	sdp_list_free(profiles, NULL);

	sdp_uuid16_create(&l2cap, L2CAP_UUID);
	proto[0] = sdp_list_append(NULL, &l2cap);
	apseq = sdp_list_append(NULL, proto[0]);

	sdp_uuid16_create(&rfcomm, RFCOMM_UUID);
	proto[1] = sdp_list_append(NULL, &rfcomm);
	channel_data = sdp_data_alloc(SDP_UINT8, &channel);
	proto[1] = sdp_list_append(proto[1], channel_data);
	apseq = sdp_list_append(apseq, proto[1]);

	aproto = sdp_list_append(NULL, apseq);
	sdp_set_access_protos(record, aproto);

	sdp_add_lang_attr(record);
	sdp_set_info_attr(record, name ? name : "Serial Port",
				"BlueZ", "SPP test peer");
	sdp_set_service_id(record, sp_uuid);

	sdp_data_free(channel_data);
	sdp_list_free(proto[0], NULL);
	sdp_list_free(proto[1], NULL);
	sdp_list_free(apseq, NULL);
	sdp_list_free(aproto, NULL);
}

static int lookup_serial_port_channel(const bdaddr_t *src,
						const bdaddr_t *dst,
						const char *name,
						uint8_t *channel)
{
	sdp_session_t *sdp;
	sdp_list_t *srch = NULL, *attrs = NULL, *rsp = NULL, *entry;
	uuid_t svclass;
	uint16_t attr_name = SDP_ATTR_SVCNAME_PRIMARY;
	uint16_t attr_proto = SDP_ATTR_PROTO_DESC_LIST;
	int err = -1;

	sdp = sdp_connect(src, dst, SDP_RETRY_IF_BUSY);
	if (!sdp)
		return -1;

	sdp_uuid16_create(&svclass, SERIAL_PORT_SVCLASS_ID);
	srch = sdp_list_append(NULL, &svclass);
	attrs = sdp_list_append(NULL, &attr_name);
	attrs = sdp_list_append(attrs, &attr_proto);

	if (!srch || !attrs)
		goto done;

	if (sdp_service_search_attr_req(sdp, srch,
				SDP_ATTR_REQ_INDIVIDUAL, attrs, &rsp) < 0)
		goto done;

	for (entry = rsp; entry; entry = entry->next) {
		sdp_record_t *record = entry->data;
		sdp_list_t *protos = NULL;
		char service_name[256];
		int port;
		bool have_name = false;

		service_name[0] = '\0';

		if (name) {
			have_name = sdp_get_service_name(record, service_name,
					sizeof(service_name)) == 0;

			if (!have_name || !g_str_equal(service_name, name))
				continue;
		}

		if (sdp_get_access_protos(record, &protos) < 0)
			continue;

		port = sdp_get_proto_port(protos, RFCOMM_UUID);
		sdp_list_free_proto_descs(protos);

		if (port >= 1 && port <= 30) {
			*channel = port;
			err = 0;
			break;
		}
	}

done:
	if (rsp)
		sdp_list_free(rsp, (sdp_free_func_t) sdp_record_free);
	if (attrs)
		sdp_list_free(attrs, NULL);
	if (srch)
		sdp_list_free(srch, NULL);
	sdp_close(sdp);

	return err;
}

static bool resolve_local_profile_channel(struct test_data *data)
{
	bdaddr_t src;
	uint8_t channel;

	str2ba(hciemu_get_address(data->hciemu), &src);

	if (lookup_serial_port_channel(&src, BDADDR_LOCAL,
					data->profile_name, &channel) < 0)
		return false;

	data->local_channel = channel;
	tester_print("Resolved local SPP channel %u via SDP", channel);
	return true;
}

static void rfcomm_connect_cb(uint16_t handle, uint16_t cid, void *user_data,
							bool status)
{
	struct test_data *data = user_data;

	if (!status) {
		fail_test("Peer RFCOMM server rejected connection");
		return;
	}

	data->rfcomm_connected = true;
	tester_print("Peer RFCOMM server accepted channel %u", REMOTE_RFCOMM_CHANNEL);
}

static void rfcomm_wrong_channel_cb(uint16_t handle, uint16_t cid,
						void *user_data,
						bool status)
{
	struct test_data *data = user_data;

	if (!status) {
		fail_test("Legacy peer RFCOMM server rejected connection");
		return;
	}

	data->rfcomm_connected = true;
	fail_test("Daemon selected the legacy remote RFCOMM channel");
}

static void peer_rfcomm_client_cb(uint16_t handle, uint16_t cid,
						void *user_data, bool status)
{
	struct test_data *data = user_data;

	if (!status) {
		fail_test("Peer RFCOMM client failed to connect to local channel");
		return;
	}

	tester_print("Peer RFCOMM client connected to local channel %u",
				data->local_channel);
}

static void peer_acl_connect_cb(uint16_t handle, void *user_data)
{
	struct test_data *data = user_data;

	data->incoming_handle = handle;

	if (!data->use_incoming_rfcomm || !data->test_started ||
				data->incoming_rfcomm_requested)
		return;

	if (!bthost_connect_rfcomm(data->peer, handle, data->local_channel,
				peer_rfcomm_client_cb, data)) {
		fail_test("Failed to start peer RFCOMM connection to local server");
		return;
	}

	data->incoming_rfcomm_requested = true;
	tester_print("Requested peer RFCOMM connection to local channel %u",
				data->local_channel);
}

static void sdp_cid_hook(const void *buf, uint16_t len, void *user_data)
{
	struct sdp_connection *conn = user_data;
	void *request;
	uint8_t rsp[1024];
	ssize_t rsp_len;

	request = malloc(len);
	if (!request) {
		fail_test("Failed to allocate peer SDP request buffer");
		return;
	}

	memcpy(request, buf, len);
	handle_internal_request(conn->server_fd, SDP_MTU, request, len);

	rsp_len = recv(conn->client_fd, rsp, sizeof(rsp), 0);
	if (rsp_len <= 0) {
		fail_test("Peer SDP server failed to produce a response");
		return;
	}

	bthost_send_cid(conn->data->peer, conn->handle, conn->cid, rsp, rsp_len);
	tester_print("Peer SDP server replied with %zd bytes", rsp_len);
}

static void sdp_connect_cb(uint16_t handle, uint16_t cid, void *user_data)
{
	struct test_data *data = user_data;
	struct sdp_connection *conn;
	int sv[2];

	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0) {
		fail_test("Failed to create local socketpair for SDP responder");
		return;
	}

	conn = g_new0(struct sdp_connection, 1);
	conn->data = data;
	conn->handle = handle;
	conn->cid = cid;
	conn->server_fd = sv[0];
	conn->client_fd = sv[1];

	data->sdp_connections = g_slist_append(data->sdp_connections, conn);

	bthost_add_cid_hook(data->peer, handle, cid, sdp_cid_hook, conn);
	tester_print("Accepted peer SDP connection on CID 0x%04x", cid);
}

static void sdp_disconnect_cb(uint16_t handle, uint16_t cid, void *user_data)
{
	struct test_data *data = user_data;
	struct sdp_connection *conn;

	conn = find_sdp_connection(data, handle, cid);
	if (!conn)
		return;

	remove_sdp_connection(conn);
	tester_print("Peer SDP connection on CID 0x%04x closed", cid);
}

static void setup_peer_emulator(struct test_data *data)
{
	data->hciemu = hciemu_new(HCIEMU_TYPE_BREDR);
	if (!data->hciemu) {
		fail_setup("Failed to create hciemu instance");
		return;
	}

	if (tester_use_debug())
		hciemu_set_debug(data->hciemu, print_debug, "hciemu: ", NULL);

	data->peer = hciemu_client_get_host(data->hciemu);
	bthost_set_connect_cb(data->peer, peer_acl_connect_cb, data);
	bthost_set_pin_code(data->peer, (const uint8_t *) TEST_PIN_CODE,
				strlen(TEST_PIN_CODE));
	bthost_add_l2cap_server(data->peer, 0x0003, NULL, NULL, NULL);
	if (data->peer_extra_service_name)
		bthost_add_rfcomm_server(data->peer, data->peer_extra_channel,
					rfcomm_wrong_channel_cb, data);
	bthost_add_rfcomm_server(data->peer, REMOTE_RFCOMM_CHANNEL,
				rfcomm_connect_cb, data);
	bthost_add_l2cap_server(data->peer, 0x0001, sdp_connect_cb,
				sdp_disconnect_cb, data);
	bthost_write_scan_enable(data->peer, 0x03);

	set_fixed_db_timestamp(0x496f0654);
	sdp_svcdb_reset();
	register_public_browse_group();
	register_server_service();
	if (data->peer_extra_service_name)
		register_serial_port(data->peer_extra_service_name,
					data->peer_extra_channel);

	register_serial_port(data->peer_service_name ? data->peer_service_name :
					"Serial Port", REMOTE_RFCOMM_CHANNEL);
}

static void unregister_profile_interface(struct test_data *data)
{
	if (!data->dbus_conn)
		return;

	g_dbus_unregister_interface(data->dbus_conn, PROFILE_PATH,
					"org.bluez.Profile1");
}

static void unregister_agent_interface(struct test_data *data)
{
	if (!data->dbus_conn)
		return;

	g_dbus_unregister_interface(data->dbus_conn, AGENT_PATH,
					"org.bluez.Agent1");
}

static void unregister_profile_setup(DBusMessageIter *iter, void *user_data)
{
	const char *path = PROFILE_PATH;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH, &path);
}

static void unregister_agent_setup(DBusMessageIter *iter, void *user_data)
{
	const char *path = AGENT_PATH;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH, &path);
}

static void finish_teardown(struct test_data *data);

static void unregister_agent_reply(DBusMessage *message, void *user_data)
{
	struct test_data *data = user_data;
	DBusError error;

	dbus_error_init(&error);
	if (dbus_set_error_from_message(&error, message)) {
		tester_warn("UnregisterAgent failed: %s", error.name);
		dbus_error_free(&error);
	} else {
		data->agent_registered = false;
	}

	finish_teardown(data);
}

static void maybe_unregister_agent(struct test_data *data)
{
	if (data->agent_registered && data->agent_manager_proxy) {
		if (g_dbus_proxy_method_call(data->agent_manager_proxy,
					"UnregisterAgent",
					unregister_agent_setup,
					unregister_agent_reply,
					data, NULL))
			return;

		tester_warn("Failed to issue AgentManager1.UnregisterAgent");
	}

	finish_teardown(data);
}

static void finish_teardown(struct test_data *data)
{
	if (data->new_connection_fd >= 0) {
		close(data->new_connection_fd);
		data->new_connection_fd = -1;
	}
	data->test_passed = true;

	data->profile_registered = false;
	data->agent_registered = false;
	unregister_profile_interface(data);
	unregister_agent_interface(data);

	unref_proxy(&data->device_proxy);
	unref_proxy(&data->adapter_proxy);
	unref_proxy(&data->agent_manager_proxy);
	unref_proxy(&data->manager_proxy);

	if (data->dbus_client) {
		g_dbus_client_unref(data->dbus_client);
		data->dbus_client = NULL;
	}

	if (data->dbus_conn) {
		dbus_connection_unref(data->dbus_conn);
		data->dbus_conn = NULL;
	}

	g_slist_free_full(data->sdp_connections,
				(GDestroyNotify) free_sdp_connection);
	data->sdp_connections = NULL;

	sdp_svcdb_reset();

	hciemu_unref(data->hciemu);
	data->hciemu = NULL;

	tester_teardown_complete();
}

static void unregister_profile_reply(DBusMessage *message, void *user_data)
{
	struct test_data *data = user_data;
	DBusError error;

	dbus_error_init(&error);
	if (dbus_set_error_from_message(&error, message)) {
		tester_warn("UnregisterProfile failed: %s", error.name);
		dbus_error_free(&error);
	} else {
		data->profile_registered = false;
	}

	maybe_unregister_agent(data);
}

static DBusMessage *profile_release(DBusConnection *conn,
					DBusMessage *message, void *user_data)
{
	tester_print("Release");
	return g_dbus_create_reply(message, DBUS_TYPE_INVALID);
}

static DBusMessage *profile_cancel(DBusConnection *conn,
					DBusMessage *message, void *user_data)
{
	tester_print("Cancel");
	return g_dbus_create_reply(message, DBUS_TYPE_INVALID);
}

static DBusMessage *agent_release(DBusConnection *conn,
					DBusMessage *message, void *user_data)
{
	tester_print("Agent Release");
	return g_dbus_create_reply(message, DBUS_TYPE_INVALID);
}

static DBusMessage *agent_cancel(DBusConnection *conn,
					DBusMessage *message, void *user_data)
{
	tester_print("Agent Cancel");
	return g_dbus_create_reply(message, DBUS_TYPE_INVALID);
}

static DBusMessage *agent_authorize_service(DBusConnection *conn,
					DBusMessage *message, void *user_data)
{
	struct test_data *data = user_data;
	const char *path;
	const char *uuid;
	DBusError error;

	dbus_error_init(&error);
	if (!dbus_message_get_args(message, &error,
				DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_STRING, &uuid,
				DBUS_TYPE_INVALID)) {
		DBusMessage *reply;

		reply = g_dbus_create_error(message, DBUS_ERROR_INVALID_ARGS,
					"%s", error.message);
		dbus_error_free(&error);
		return reply;
	}

	data->authorization_seen = true;
	tester_print("AuthorizeService(%s, %s)", path, uuid);

	return g_dbus_create_reply(message, DBUS_TYPE_INVALID);
}

static DBusMessage *agent_request_pincode(DBusConnection *conn,
					DBusMessage *message, void *user_data)
{
	struct test_data *data = user_data;
	const char *path;
	const char *pin = "0000";
	DBusError error;

	dbus_error_init(&error);
	if (!dbus_message_get_args(message, &error,
				DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_INVALID)) {
		DBusMessage *reply;

		reply = g_dbus_create_error(message, DBUS_ERROR_INVALID_ARGS,
					"%s", error.message);
		dbus_error_free(&error);
		return reply;
	}

	data->authentication_seen = true;
	tester_print("RequestPinCode(%s)", path);

	return g_dbus_create_reply(message, DBUS_TYPE_STRING, &pin,
					DBUS_TYPE_INVALID);
}

static DBusMessage *agent_display_pincode(DBusConnection *conn,
					DBusMessage *message, void *user_data)
{
	struct test_data *data = user_data;
	const char *path;
	const char *pin;
	DBusError error;

	dbus_error_init(&error);
	if (!dbus_message_get_args(message, &error,
				DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_STRING, &pin,
				DBUS_TYPE_INVALID)) {
		DBusMessage *reply;

		reply = g_dbus_create_error(message, DBUS_ERROR_INVALID_ARGS,
					"%s", error.message);
		dbus_error_free(&error);
		return reply;
	}

	data->authentication_seen = true;
	tester_print("DisplayPinCode(%s, %s)", path, pin);

	return g_dbus_create_reply(message, DBUS_TYPE_INVALID);
}

static DBusMessage *agent_request_passkey(DBusConnection *conn,
					DBusMessage *message, void *user_data)
{
	struct test_data *data = user_data;
	const char *path;
	dbus_uint32_t passkey = 0;
	DBusError error;

	dbus_error_init(&error);
	if (!dbus_message_get_args(message, &error,
				DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_INVALID)) {
		DBusMessage *reply;

		reply = g_dbus_create_error(message, DBUS_ERROR_INVALID_ARGS,
					"%s", error.message);
		dbus_error_free(&error);
		return reply;
	}

	data->authentication_seen = true;
	tester_print("RequestPasskey(%s)", path);

	return g_dbus_create_reply(message, DBUS_TYPE_UINT32, &passkey,
					DBUS_TYPE_INVALID);
}

static DBusMessage *agent_display_passkey(DBusConnection *conn,
					DBusMessage *message, void *user_data)
{
	struct test_data *data = user_data;
	const char *path;
	dbus_uint32_t passkey;
	dbus_uint16_t entered;
	DBusError error;

	dbus_error_init(&error);
	if (!dbus_message_get_args(message, &error,
				DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_UINT32, &passkey,
				DBUS_TYPE_UINT16, &entered,
				DBUS_TYPE_INVALID)) {
		DBusMessage *reply;

		reply = g_dbus_create_error(message, DBUS_ERROR_INVALID_ARGS,
					"%s", error.message);
		dbus_error_free(&error);
		return reply;
	}

	data->authentication_seen = true;
	tester_print("DisplayPasskey(%s, %06u, %u)", path,
				(unsigned int) passkey, (unsigned int) entered);

	return g_dbus_create_reply(message, DBUS_TYPE_INVALID);
}

static DBusMessage *agent_request_confirmation(DBusConnection *conn,
					DBusMessage *message, void *user_data)
{
	struct test_data *data = user_data;
	const char *path;
	dbus_uint32_t passkey;
	DBusError error;

	dbus_error_init(&error);
	if (!dbus_message_get_args(message, &error,
				DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_UINT32, &passkey,
				DBUS_TYPE_INVALID)) {
		DBusMessage *reply;

		reply = g_dbus_create_error(message, DBUS_ERROR_INVALID_ARGS,
					"%s", error.message);
		dbus_error_free(&error);
		return reply;
	}

	data->authentication_seen = true;
	tester_print("RequestConfirmation(%s, %06u)", path,
				(unsigned int) passkey);

	return g_dbus_create_reply(message, DBUS_TYPE_INVALID);
}

static DBusMessage *agent_request_authorization(DBusConnection *conn,
					DBusMessage *message, void *user_data)
{
	struct test_data *data = user_data;
	const char *path;
	DBusError error;

	dbus_error_init(&error);
	if (!dbus_message_get_args(message, &error,
				DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_INVALID)) {
		DBusMessage *reply;

		reply = g_dbus_create_error(message, DBUS_ERROR_INVALID_ARGS,
					"%s", error.message);
		dbus_error_free(&error);
		return reply;
	}

	data->authentication_seen = true;
	tester_print("RequestAuthorization(%s)", path);

	return g_dbus_create_reply(message, DBUS_TYPE_INVALID);
}

static DBusMessage *profile_request_disconnection(DBusConnection *conn,
					DBusMessage *message, void *user_data)
{
	const char *path;
	DBusError error;
	struct test_data *data = user_data;

	dbus_error_init(&error);
	if (!dbus_message_get_args(message, &error,
				DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_INVALID)) {
		DBusMessage *reply;
		reply = g_dbus_create_error(message, DBUS_ERROR_INVALID_ARGS,
						"%s", error.message);
		dbus_error_free(&error);
		return reply;
	}

	tester_print("RequestDisconnection(%s)", path);

	if (data->new_connection_fd >= 0) {
		close(data->new_connection_fd);
		data->new_connection_fd = -1;
	}

	return g_dbus_create_reply(message, DBUS_TYPE_INVALID);
}

static DBusMessage *profile_new_connection(DBusConnection *conn,
					DBusMessage *message, void *user_data)
{
	struct test_data *data = user_data;
	DBusMessageIter iter;
	const char *path;
	int fd = -1;

	if (!dbus_message_iter_init(message, &iter))
		return g_dbus_create_error(message, DBUS_ERROR_INVALID_ARGS,
					"Missing NewConnection arguments");

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH)
		return g_dbus_create_error(message, DBUS_ERROR_INVALID_ARGS,
					"Invalid device path argument");

	dbus_message_iter_get_basic(&iter, &path);
	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_UNIX_FD)
		return g_dbus_create_error(message, DBUS_ERROR_INVALID_ARGS,
					"Invalid file descriptor argument");

	dbus_message_iter_get_basic(&iter, &fd);
	tester_print("NewConnection(%s, %d)", path, fd);

	if (data->new_connection_fd >= 0)
		close(data->new_connection_fd);

	data->new_connection_fd = fd;
	data->seen_connections++;

	if (data->require_authentication && !data->authentication_seen)
		fail_test("NewConnection arrived before authentication completed");

	if (data->require_authorization && !data->authorization_seen)
		fail_test("NewConnection arrived before AuthorizeService");

	if (data->seen_connections < data->expected_connections) {
		tester_print("Completed SPP connection %u/%u",
				data->seen_connections, data->expected_connections);
		if (!data->disconnect_cycle_id)
			data->disconnect_cycle_id = g_idle_add(
						disconnect_profile_cb, data);
		if (!data->disconnect_cycle_id)
			fail_test("Failed to schedule DisconnectProfile cycle");
		return g_dbus_create_reply(message, DBUS_TYPE_INVALID);
	}

	data->test_passed = true;
	tester_test_passed();
	return g_dbus_create_reply(message, DBUS_TYPE_INVALID);
}

static const GDBusMethodTable profile_methods[] = {
	{ GDBUS_METHOD("Release", NULL, NULL, profile_release) },
	{ GDBUS_METHOD("Cancel", NULL, NULL, profile_cancel) },
	{ GDBUS_METHOD("RequestDisconnection",
		GDBUS_ARGS({ "device", "o" }), NULL,
		profile_request_disconnection) },
	{ GDBUS_METHOD("NewConnection",
		GDBUS_ARGS({ "device", "o" }, { "fd", "h" },
			{ "fd_properties", "a{sv}" }),
		NULL, profile_new_connection) },
	{ }
};

static const GDBusMethodTable agent_methods[] = {
	{ GDBUS_METHOD("Release", NULL, NULL, agent_release) },
	{ GDBUS_METHOD("Cancel", NULL, NULL, agent_cancel) },
	{ GDBUS_METHOD("RequestPinCode",
		GDBUS_ARGS({ "device", "o" }),
		GDBUS_ARGS({ "pincode", "s" }), agent_request_pincode) },
	{ GDBUS_METHOD("DisplayPinCode",
		GDBUS_ARGS({ "device", "o" }, { "pincode", "s" }),
		NULL, agent_display_pincode) },
	{ GDBUS_METHOD("RequestPasskey",
		GDBUS_ARGS({ "device", "o" }),
		GDBUS_ARGS({ "passkey", "u" }), agent_request_passkey) },
	{ GDBUS_METHOD("DisplayPasskey",
		GDBUS_ARGS({ "device", "o" }, { "passkey", "u" },
			{ "entered", "q" }),
		NULL, agent_display_passkey) },
	{ GDBUS_METHOD("RequestConfirmation",
		GDBUS_ARGS({ "device", "o" }, { "passkey", "u" }),
		NULL, agent_request_confirmation) },
	{ GDBUS_METHOD("RequestAuthorization",
		GDBUS_ARGS({ "device", "o" }),
		NULL, agent_request_authorization) },
	{ GDBUS_METHOD("AuthorizeService",
		GDBUS_ARGS({ "device", "o" }, { "uuid", "s" }),
		NULL, agent_authorize_service) },
	{ }
};

static void profile_uuid_setup(DBusMessageIter *iter)
{
	const char *uuid = PROFILE_UUID;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &uuid);
}

static void register_agent_setup(DBusMessageIter *iter, void *user_data)
{
	const char *path = AGENT_PATH;
	const char *capability = "NoInputNoOutput";

	dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH, &path);
	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &capability);
}

static void request_default_agent_setup(DBusMessageIter *iter,
						void *user_data)
{
	const char *path = AGENT_PATH;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH, &path);
}

static void connect_profile_setup(DBusMessageIter *iter, void *user_data)
{
	profile_uuid_setup(iter);
}

static void disconnect_profile_setup(DBusMessageIter *iter, void *user_data)
{
	profile_uuid_setup(iter);
}

static void connect_profile_reply(DBusMessage *message, void *user_data)
{
	struct test_data *data = user_data;
	DBusError error;

	dbus_error_init(&error);
	if (dbus_set_error_from_message(&error, message)) {
		if (g_str_equal(error.name, "org.bluez.Error.NotReady") &&
				data->connect_not_ready_retries < NOT_READY_MAX_RETRIES) {
			data->connect_requested = false;
			data->connect_not_ready_retries++;
			tester_debug("ConnectProfile got NotReady, retry %u/%u",
					data->connect_not_ready_retries,
					NOT_READY_MAX_RETRIES);
			dbus_error_free(&error);
			if (!schedule_not_ready_retry(&data->connect_retry_id,
					retry_connect_profile_cb, data))
				fail_test("Failed to schedule ConnectProfile retry");
			return;
		}

		tester_warn("ConnectProfile failed: %s", error.name);
		dbus_error_free(&error);
		tester_test_failed();
		return;
	}

	data->connect_not_ready_retries = 0;
	tester_print("ConnectProfile request accepted");
}

static void request_default_agent_reply(DBusMessage *message, void *user_data)
{
	struct test_data *data = user_data;
	DBusError error;

	data->agent_default_pending = false;

	dbus_error_init(&error);
	if (dbus_set_error_from_message(&error, message)) {
		tester_warn("RequestDefaultAgent failed: %s", error.name);
		dbus_error_free(&error);
		tester_setup_failed();
		return;
	}

	data->agent_registered = true;
	tester_print("Registered default authorization agent");
	maybe_setup_complete(data);
}

static void register_agent_reply(DBusMessage *message, void *user_data)
{
	struct test_data *data = user_data;
	DBusError error;

	data->agent_register_pending = false;

	dbus_error_init(&error);
	if (dbus_set_error_from_message(&error, message)) {
		tester_warn("RegisterAgent failed: %s", error.name);
		dbus_error_free(&error);
		tester_setup_failed();
		return;
	}

	if (!g_dbus_proxy_method_call(data->agent_manager_proxy,
				"RequestDefaultAgent",
				request_default_agent_setup,
				request_default_agent_reply, data, NULL)) {
		fail_setup("Failed to issue AgentManager1.RequestDefaultAgent");
		return;
	}

	data->agent_default_pending = true;
	tester_print("Requested AgentManager1.RequestDefaultAgent");
}

static void disconnect_profile_reply(DBusMessage *message, void *user_data)
{
	struct test_data *data = user_data;
	DBusError error;

	data->disconnect_requested = false;

	dbus_error_init(&error);
	if (dbus_set_error_from_message(&error, message)) {
		tester_warn("DisconnectProfile failed: %s", error.name);
		dbus_error_free(&error);
		tester_test_failed();
		return;
	}

	data->connect_requested = false;
	tester_print("DisconnectProfile request accepted");
	maybe_connect_profile(data);
}

static void maybe_connect_profile(struct test_data *data)
{
	if (!data->use_connect_profile)
		return;

	if (!data->test_started || !data->device_proxy)
		return;

	if (!data->profile_registered || data->connect_requested)
		return;

	if (!g_dbus_proxy_method_call(data->device_proxy, "ConnectProfile",
				connect_profile_setup,
				connect_profile_reply, data, NULL)) {
		fail_test("Failed to issue Device1.ConnectProfile");
		return;
	}

	data->connect_requested = true;
	tester_print("Requested ConnectProfile(%s)", PROFILE_UUID);
}

static gboolean retry_connect_profile_cb(gpointer user_data)
{
	struct test_data *data = user_data;

	data->connect_retry_id = 0;
	maybe_connect_profile(data);

	return FALSE;
}

static void maybe_register_agent(struct test_data *data)
{
	if (!test_requires_agent(data) || !data->agent_manager_proxy)
		return;

	if (data->agent_registered || data->agent_register_pending ||
				data->agent_default_pending)
		return;

	if (!g_dbus_proxy_method_call(data->agent_manager_proxy,
				"RegisterAgent",
				register_agent_setup,
				register_agent_reply, data, NULL)) {
		fail_setup("Failed to issue AgentManager1.RegisterAgent");
		return;
	}

	data->agent_register_pending = true;
	tester_print("Requested AgentManager1.RegisterAgent");
}

static void maybe_disconnect_profile(struct test_data *data)
{
	if (!data->use_connect_profile)
		return;

	if (!data->test_started || !data->device_proxy)
		return;

	if (data->disconnect_requested || data->test_passed)
		return;

	if (data->seen_connections >= data->expected_connections)
		return;

	if (!g_dbus_proxy_method_call(data->device_proxy, "DisconnectProfile",
				disconnect_profile_setup,
				disconnect_profile_reply, data, NULL)) {
		fail_test("Failed to issue Device1.DisconnectProfile");
		return;
	}

	data->disconnect_requested = true;
	tester_print("Requested DisconnectProfile(%s)", PROFILE_UUID);
}

static gboolean disconnect_profile_cb(gpointer user_data)
{
	struct test_data *data = user_data;

	data->disconnect_cycle_id = 0;
	maybe_disconnect_profile(data);

	return FALSE;
}

static void discovery_reply(DBusMessage *message, void *user_data)
{
	struct test_data *data = user_data;
	DBusError error;

	dbus_error_init(&error);
	if (dbus_set_error_from_message(&error, message)) {
		if (g_str_equal(error.name, "org.bluez.Error.NotReady") &&
				data->discovery_not_ready_retries < NOT_READY_MAX_RETRIES) {
			data->discovery_started = false;
			data->discovery_not_ready_retries++;
			tester_debug("StartDiscovery got NotReady, retry %u/%u",
					data->discovery_not_ready_retries,
					NOT_READY_MAX_RETRIES);
			dbus_error_free(&error);
			if (!schedule_not_ready_retry(&data->discovery_retry_id,
					retry_discovery_cb, data))
				fail_test("Failed to schedule StartDiscovery retry");
			return;
		}

		tester_warn("Discovery request failed: %s", error.name);
		dbus_error_free(&error);
		tester_test_failed();
		return;
	}

	data->discovery_not_ready_retries = 0;
	tester_print("Discovery request accepted");
}

static void start_discovery(struct test_data *data)
{
	if (data->discovery_started)
		return;

	if (!g_dbus_proxy_method_call(data->adapter_proxy, "StartDiscovery",
				NULL, discovery_reply, data, NULL)) {
		fail_test("Failed to start discovery");
		return;
	}

	data->discovery_started = true;
	tester_print("Requested adapter discovery");
}

static gboolean retry_discovery_cb(gpointer user_data)
{
	struct test_data *data = user_data;

	data->discovery_retry_id = 0;
	start_discovery(data);

	return FALSE;
}

static void register_profile_setup(DBusMessageIter *iter, void *user_data)
{
	DBusMessageIter dict;
	const char *path = PROFILE_PATH;
	const char *uuid = PROFILE_UUID;
	struct test_data *data = user_data;
	const char *name = data->profile_name;
	const char *role = data->profile_role;
	dbus_bool_t require_authentication = data->require_authentication;
	dbus_bool_t require_authorization = data->require_authorization;
	uint16_t channel = data->local_channel;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH, &path);
	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &uuid);

	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
	g_dbus_dict_append_entry(&dict, "Name", DBUS_TYPE_STRING, &name);
	g_dbus_dict_append_entry(&dict, "Role", DBUS_TYPE_STRING, &role);
	g_dbus_dict_append_entry(&dict, "RequireAuthentication",
					DBUS_TYPE_BOOLEAN,
					&require_authentication);
	g_dbus_dict_append_entry(&dict, "RequireAuthorization",
					DBUS_TYPE_BOOLEAN,
					&require_authorization);
	if (channel)
		g_dbus_dict_append_entry(&dict, "Channel", DBUS_TYPE_UINT16,
					&channel);
	if (data->service_record)
		g_dbus_dict_append_entry(&dict, "ServiceRecord",
					DBUS_TYPE_STRING,
					&data->service_record);
	dbus_message_iter_close_container(iter, &dict);
}

static void register_profile_reply(DBusMessage *message, void *user_data)
{
	struct test_data *data = user_data;
	DBusError error;

	data->profile_pending = false;

	dbus_error_init(&error);
	if (dbus_set_error_from_message(&error, message)) {
		if (data->expected_register_error) {
			if (!g_str_equal(error.name,
						data->expected_register_error)) {
				tester_warn("RegisterProfile failed with unexpected error: %s",
								error.name);
				dbus_error_free(&error);
				tester_setup_failed();
				return;
			}

			data->register_error_seen = true;
			tester_print("RegisterProfile failed as expected: %s",
								error.name);
			dbus_error_free(&error);
			data->setup_complete = true;
			tester_setup_complete();
			return;
		}

		tester_warn("RegisterProfile failed: %s", error.name);
		dbus_error_free(&error);
		tester_setup_failed();
		return;
	}

	if (data->expected_register_error) {
		data->profile_registered = true;
		fail_setup("RegisterProfile succeeded unexpectedly");
		return;
	}

	data->profile_registered = true;
	tester_print("Registered external SPP profile %s", data->profile_role);
	maybe_setup_complete(data);
	maybe_connect_profile(data);
}

static void maybe_register_profile(struct test_data *data)
{
	if (!data->manager_proxy || data->profile_registered || data->profile_pending)
		return;

	if (!g_dbus_proxy_method_call(data->manager_proxy, "RegisterProfile",
				register_profile_setup,
				register_profile_reply, data, NULL)) {
		fail_setup("Failed to issue ProfileManager1.RegisterProfile");
		return;
	}

	data->profile_pending = true;
	tester_print("Requested ProfileManager1.RegisterProfile");
}

static void power_reply(const DBusError *error, void *user_data)
{
	struct test_data *data = user_data;

	data->power_pending = false;

	if (error != NULL && dbus_error_is_set((DBusError *) error)) {
		if (g_str_equal(error->name, "org.bluez.Error.Busy")) {
			tester_debug("Adapter power request already in progress");
			return;
		}

		tester_warn("Failed to power adapter: %s", error->name);
		tester_setup_failed();
		return;
	}

	tester_print("Requested adapter power on");
}

static void maybe_power_adapter(struct test_data *data)
{
	dbus_bool_t powered = TRUE;

	if (!data->adapter_proxy || data->adapter_powered || data->power_pending)
		return;

	if (!g_dbus_proxy_set_property_basic(data->adapter_proxy, "Powered",
					DBUS_TYPE_BOOLEAN, &powered,
					power_reply, data, NULL)) {
		fail_setup("Failed to set Adapter1.Powered");
		return;
	}

	data->power_pending = true;
}

static void connectable_reply(const DBusError *error, void *user_data)
{
	struct test_data *data = user_data;

	data->connectable_pending = false;

	if (error != NULL && dbus_error_is_set((DBusError *) error)) {
		if (g_str_equal(error->name, "org.bluez.Error.Busy")) {
			tester_debug("Adapter connectable request already in progress");
			return;
		}

		tester_warn("Failed to set adapter connectable: %s", error->name);
		tester_setup_failed();
		return;
	}

	tester_print("Requested adapter connectable on");
}

static void maybe_make_adapter_connectable(struct test_data *data)
{
	dbus_bool_t connectable = TRUE;

	if (!data->use_incoming_rfcomm)
		return;

	if (!data->adapter_proxy || !data->adapter_powered ||
				data->adapter_connectable || data->connectable_pending)
		return;

	if (!g_dbus_proxy_set_property_basic(data->adapter_proxy, "Connectable",
					DBUS_TYPE_BOOLEAN, &connectable,
					connectable_reply, data, NULL)) {
		fail_setup("Failed to set Adapter1.Connectable");
		return;
	}

	data->connectable_pending = true;
}

static void maybe_start_incoming_rfcomm(struct test_data *data)
{
	if (!data->use_incoming_rfcomm || data->incoming_connect_requested)
		return;

	if (data->use_local_sdp_channel_lookup && data->local_channel == 0) {
		if (!resolve_local_profile_channel(data)) {
			if (data->channel_not_ready_retries >= NOT_READY_MAX_RETRIES) {
				fail_test("Failed to resolve local SPP channel via SDP");
				return;
			}

			data->channel_not_ready_retries++;
			tester_debug("Local SDP channel lookup not ready, retry %u/%u",
					data->channel_not_ready_retries,
					NOT_READY_MAX_RETRIES);
			if (!schedule_not_ready_retry(&data->channel_retry_id,
					retry_local_channel_cb, data))
				fail_test("Failed to schedule local SDP channel retry");
			return;
		}

		data->channel_not_ready_retries = 0;
	}

	if (data->local_channel == 0) {
		fail_test("No local RFCOMM channel available for incoming test");
		return;
	}

	bthost_hci_connect(data->peer,
			hciemu_get_central_bdaddr(data->hciemu),
			BDADDR_BREDR);
	data->incoming_connect_requested = true;
	tester_print("Requested peer BR/EDR connection to local adapter");
}

static gboolean retry_local_channel_cb(gpointer user_data)
{
	struct test_data *data = user_data;

	data->channel_retry_id = 0;
	maybe_start_incoming_rfcomm(data);

	return FALSE;
}

static void connect_handler(DBusConnection *connection, void *user_data)
{
	tester_print("Connected to daemon");
}

static void disconnect_handler(DBusConnection *connection, void *user_data)
{
	struct test_data *data = user_data;

	if (data->tearing_down)
		return;

	if (data->test_passed)
		return;

	if (data->setup_complete)
		fail_test("bluetoothd disconnected during test execution");
	else
		fail_setup("bluetoothd disconnected during setup");
}

static void property_changed(GDBusProxy *proxy, const char *name,
					DBusMessageIter *iter, void *user_data)
{
	struct test_data *data = user_data;
	const char *interface = g_dbus_proxy_get_interface(proxy);

	if (g_str_equal(interface, "org.bluez.Adapter1") &&
			g_str_equal(name, "Powered")) {
		dbus_bool_t powered;

		dbus_message_iter_get_basic(iter, &powered);
		data->adapter_powered = powered;
		if (powered) {
			maybe_make_adapter_connectable(data);
			maybe_register_agent(data);
			maybe_register_profile(data);
			maybe_setup_complete(data);
		}
		return;
	}

	if (g_str_equal(interface, "org.bluez.Adapter1") &&
			g_str_equal(name, "Connectable")) {
		dbus_bool_t connectable;

		dbus_message_iter_get_basic(iter, &connectable);
		data->adapter_connectable = connectable;
		if (connectable)
			maybe_setup_complete(data);
		return;
	}

	if (g_str_equal(interface, "org.bluez.Device1") &&
			g_str_equal(name, "Connected")) {
		dbus_bool_t connected;

		dbus_message_iter_get_basic(iter, &connected);
		tester_print("Remote device Connected=%s",
				connected ? "true" : "false");
	}
}

static void proxy_added(GDBusProxy *proxy, void *user_data)
{
	struct test_data *data = user_data;
	const char *interface = g_dbus_proxy_get_interface(proxy);
	const char *path = g_dbus_proxy_get_path(proxy);
	const char *addr = NULL;
	char remote_addr[18];

	proxy_get_string(proxy, "Address", &addr);

	if (tester_use_debug())
		tester_debug("Proxy added: %s %s%s%s", interface, path,
				addr ? " address=" : "",
				addr ? addr : "");

	if (g_str_equal(interface, "org.bluez.ProfileManager1")) {
		replace_proxy(&data->manager_proxy, proxy);
		if (tester_use_debug())
			tester_debug("Matched ProfileManager1 proxy");
		maybe_register_profile(data);
		maybe_setup_complete(data);
		return;
	}

	if (g_str_equal(interface, "org.bluez.AgentManager1")) {
		replace_proxy(&data->agent_manager_proxy, proxy);
		maybe_register_agent(data);
		maybe_setup_complete(data);
		return;
	}

	if (g_str_equal(interface, "org.bluez.Adapter1")) {
		if (!match_proxy_address(proxy, hciemu_get_address(data->hciemu))) {
			if (tester_use_debug())
				tester_debug("Ignoring adapter %s, expected %s",
					addr ? addr : "<missing>",
					hciemu_get_address(data->hciemu));
			return;
		}

		replace_proxy(&data->adapter_proxy, proxy);
		if (tester_use_debug())
			tester_debug("Matched emulated adapter %s",
					hciemu_get_address(data->hciemu));
		proxy_get_bool(proxy, "Powered", &data->adapter_powered);
		proxy_get_bool(proxy, "Connectable", &data->adapter_connectable);
		if (!data->adapter_powered)
			maybe_power_adapter(data);
		else {
			maybe_make_adapter_connectable(data);
			maybe_register_agent(data);
			maybe_register_profile(data);
			maybe_setup_complete(data);
		}
		return;
	}

	if (!g_str_equal(interface, "org.bluez.Device1"))
		return;

	ba2str((const bdaddr_t *) hciemu_get_client_bdaddr(data->hciemu),
			remote_addr);
	if (!match_proxy_address(proxy, remote_addr)) {
		if (tester_use_debug())
			tester_debug("Ignoring device %s, expected %s",
				addr ? addr : "<missing>", remote_addr);
		return;
	}

	replace_proxy(&data->device_proxy, proxy);
	tester_print("Discovered emulated remote device %s", remote_addr);
	maybe_connect_profile(data);
}

static void proxy_removed(GDBusProxy *proxy, void *user_data)
{
	struct test_data *data = user_data;

	if (proxy == data->adapter_proxy)
		unref_proxy(&data->adapter_proxy);

	if (proxy == data->device_proxy)
		unref_proxy(&data->device_proxy);

	if (proxy == data->agent_manager_proxy)
		unref_proxy(&data->agent_manager_proxy);

	if (proxy == data->manager_proxy)
		unref_proxy(&data->manager_proxy);
}

static void test_setup(const void *test_data)
{
	struct test_data *data = tester_get_data();

	data->new_connection_fd = -1;
	data->seen_connections = 0;
	data->test_passed = false;
	data->disconnect_requested = false;
	data->disconnect_cycle_id = 0;
	data->authentication_seen = false;
	data->authorization_seen = false;
	data->adapter_connectable = false;
	data->channel_not_ready_retries = 0;
	data->channel_retry_id = 0;
	data->connectable_pending = false;
	data->incoming_handle = 0;
	data->incoming_connect_requested = false;
	data->incoming_rfcomm_requested = false;
	data->agent_registered = false;
	data->agent_register_pending = false;
	data->agent_default_pending = false;
	data->register_error_seen = false;

	__btd_log_init("*", 0);

	data->dbus_conn = g_dbus_setup_private(DBUS_BUS_SYSTEM, NULL, NULL);
	if (!data->dbus_conn) {
		fail_setup("Failed to connect to the system bus");
		return;
	}

	if (!g_dbus_register_interface(data->dbus_conn, PROFILE_PATH,
					"org.bluez.Profile1",
					profile_methods, NULL, NULL,
					data, NULL)) {
		fail_setup("Failed to export org.bluez.Profile1");
		return;
	}

	if (test_requires_agent(data) &&
			!g_dbus_register_interface(data->dbus_conn, AGENT_PATH,
					"org.bluez.Agent1",
					agent_methods, NULL, NULL,
					data, NULL)) {
		fail_setup("Failed to export org.bluez.Agent1");
		return;
	}

	setup_peer_emulator(data);
	if (!data->hciemu)
		return;

	data->dbus_client = g_dbus_client_new(data->dbus_conn, "org.bluez",
						"/org/bluez");
	if (!data->dbus_client) {
		fail_setup("Failed to create D-Bus object manager client");
		return;
	}

	g_dbus_client_set_connect_watch(data->dbus_client, connect_handler, data);
	g_dbus_client_set_disconnect_watch(data->dbus_client,
					disconnect_handler, data);
	g_dbus_client_set_proxy_handlers(data->dbus_client, proxy_added,
					proxy_removed, property_changed, data);
}

static void test_run(const void *test_data)
{
	struct test_data *data = tester_get_data();

	data->test_started = true;
	if (data->expected_register_error) {
		if (!data->register_error_seen) {
			fail_test("Expected RegisterProfile failure was not observed");
			return;
		}

		data->test_passed = true;
		tester_test_passed();
		return;
	}

	if (data->use_discovery)
		start_discovery(data);
	maybe_connect_profile(data);
	maybe_start_incoming_rfcomm(data);
}

static void test_teardown(const void *test_data)
{
	struct test_data *data = tester_get_data();

	data->tearing_down = true;

	if (data->discovery_retry_id) {
		g_source_remove(data->discovery_retry_id);
		data->discovery_retry_id = 0;
	}

	if (data->connect_retry_id) {
		g_source_remove(data->connect_retry_id);
		data->connect_retry_id = 0;
	}

	if (data->channel_retry_id) {
		g_source_remove(data->channel_retry_id);
		data->channel_retry_id = 0;
	}

	if (data->disconnect_cycle_id) {
		g_source_remove(data->disconnect_cycle_id);
		data->disconnect_cycle_id = 0;
	}

	if (data->profile_registered && data->manager_proxy) {
		if (g_dbus_proxy_method_call(data->manager_proxy,
					"UnregisterProfile",
					unregister_profile_setup,
					unregister_profile_reply,
					data, NULL))
			return;

		tester_warn("Failed to issue ProfileManager1.UnregisterProfile");
	}

	maybe_unregister_agent(data);
}

static void destroy_data(void *user_data)
{
	struct test_data *data = user_data;

	g_free(data->service_record);
	g_free(data);
}

int main(int argc, char *argv[])
{
	struct test_data *data;
	struct test_data *multi_record_data;
	struct test_data *dynamic_data;
	struct test_data *reconnect_data;
	struct test_data *authentication_data;
	struct test_data *outgoing_authentication_data;
	struct test_data *auth_data;
	struct test_data *invalid_version_data;
	struct test_data *malformed_record_data;

	tester_init(&argc, &argv);

	data = g_new0(struct test_data, 1);
	data->new_connection_fd = -1;
	data->profile_name = PROFILE_NAME;
	data->profile_role = "client";
	data->use_discovery = true;
	data->use_connect_profile = true;
	data->expected_connections = 1;

	tester_add_full("ProfileManager SPP Remote SDP Discovery", NULL,
				NULL, test_setup, test_run, test_teardown,
				NULL, 15, data, destroy_data);

	multi_record_data = g_new0(struct test_data, 1);
	multi_record_data->new_connection_fd = -1;
	multi_record_data->profile_name = PROFILE_MULTI_RECORD_REMOTE_NAME;
	multi_record_data->profile_role = "client";
	multi_record_data->peer_service_name = PROFILE_MULTI_RECORD_REMOTE_NAME;
	multi_record_data->peer_extra_service_name =
				PROFILE_MULTI_RECORD_LEGACY_NAME;
	multi_record_data->peer_extra_channel = LEGACY_RFCOMM_CHANNEL;
	multi_record_data->use_discovery = true;
	multi_record_data->use_connect_profile = true;
	multi_record_data->expected_connections = 1;

	tester_add_full("ProfileManager SPP Remote SDP Multi Record", NULL,
				NULL, test_setup, test_run, test_teardown,
				NULL, 15, multi_record_data, destroy_data);

	dynamic_data = g_new0(struct test_data, 1);
	dynamic_data->new_connection_fd = -1;
	dynamic_data->profile_name = PROFILE_DYNAMIC_NAME;
	dynamic_data->profile_role = "server";
	dynamic_data->use_incoming_rfcomm = true;
	dynamic_data->use_local_sdp_channel_lookup = true;
	dynamic_data->expected_connections = 1;

	tester_add_full("ProfileManager SPP Dynamic Channel", NULL,
				NULL, test_setup, test_run, test_teardown,
				NULL, 20, dynamic_data, destroy_data);

	reconnect_data = g_new0(struct test_data, 1);
	reconnect_data->new_connection_fd = -1;
	reconnect_data->profile_name = PROFILE_RECONNECT_NAME;
	reconnect_data->profile_role = "client";
	reconnect_data->use_discovery = true;
	reconnect_data->use_connect_profile = true;
	reconnect_data->expected_connections = 2;

	tester_add_full("ProfileManager SPP Reconnect Cycle", NULL,
				NULL, test_setup, test_run, test_teardown,
				NULL, 20, reconnect_data, destroy_data);

	authentication_data = g_new0(struct test_data, 1);
	authentication_data->new_connection_fd = -1;
	authentication_data->profile_name = PROFILE_AUTHENTICATION_NAME;
	authentication_data->profile_role = "server";
	authentication_data->require_authentication = true;
	authentication_data->use_incoming_rfcomm = true;
	authentication_data->local_channel = LOCAL_AUTHENTICATION_CHANNEL;
	authentication_data->expected_connections = 1;

	tester_add_full("ProfileManager SPP Authentication Required", NULL,
				NULL, test_setup, test_run, test_teardown,
				NULL, 20, authentication_data, destroy_data);

	outgoing_authentication_data = g_new0(struct test_data, 1);
	outgoing_authentication_data->new_connection_fd = -1;
	outgoing_authentication_data->profile_name =
				PROFILE_OUTGOING_AUTHENTICATION_NAME;
	outgoing_authentication_data->profile_role = "client";
	outgoing_authentication_data->require_authentication = true;
	outgoing_authentication_data->use_discovery = true;
	outgoing_authentication_data->use_connect_profile = true;
	outgoing_authentication_data->expected_connections = 1;

	tester_add_full("ProfileManager SPP Outgoing Authentication Required",
				NULL, NULL, test_setup, test_run, test_teardown,
				NULL, 20, outgoing_authentication_data,
				destroy_data);

	auth_data = g_new0(struct test_data, 1);
	auth_data->new_connection_fd = -1;
	auth_data->profile_name = PROFILE_AUTH_NAME;
	auth_data->profile_role = "server";
	auth_data->require_authorization = true;
	auth_data->use_incoming_rfcomm = true;
	auth_data->local_channel = LOCAL_AUTH_CHANNEL;
	auth_data->expected_connections = 1;

	tester_add_full("ProfileManager SPP Authorization Gate", NULL,
				NULL, test_setup, test_run, test_teardown,
				NULL, 20, auth_data, destroy_data);

	invalid_version_data = g_new0(struct test_data, 1);
	invalid_version_data->new_connection_fd = -1;
	invalid_version_data->profile_name = PROFILE_INVALID_RECORD_NAME;
	invalid_version_data->profile_role = "server";
	invalid_version_data->local_channel = LOCAL_INVALID_VERSION_CHANNEL;
	invalid_version_data->service_record = build_spp_service_record(
				"Serial Port", LOCAL_INVALID_VERSION_CHANNEL,
				0x0100, true);
	invalid_version_data->expected_register_error =
				BLUEZ_ERROR_INVALID_ARGUMENTS;

	tester_add_full("ProfileManager SPP Invalid ServiceRecord Version",
				NULL, NULL, test_setup, test_run, test_teardown,
				NULL, 15, invalid_version_data, destroy_data);

	malformed_record_data = g_new0(struct test_data, 1);
	malformed_record_data->new_connection_fd = -1;
	malformed_record_data->profile_name = PROFILE_MALFORMED_RECORD_NAME;
	malformed_record_data->profile_role = "server";
	malformed_record_data->local_channel = LOCAL_MALFORMED_RECORD_CHANNEL;
	malformed_record_data->service_record = build_spp_service_record(
				"Serial Port", LOCAL_MALFORMED_RECORD_CHANNEL,
				0x0102, false);
	malformed_record_data->expected_register_error =
				BLUEZ_ERROR_INVALID_ARGUMENTS;

	tester_add_full("ProfileManager SPP Missing Browse Group", NULL,
				NULL, test_setup, test_run, test_teardown,
				NULL, 15, malformed_record_data, destroy_data);

	return tester_run();
}
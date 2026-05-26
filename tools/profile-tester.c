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
#define PROFILE_UUID SPP_UUID
#define PROFILE_NAME "SPP Remote SDP Discovery"
#define REMOTE_RFCOMM_CHANNEL 23
#define SDP_MTU 672

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
	GDBusProxy *manager_proxy;
	struct hciemu *hciemu;
	struct bthost *peer;
	GSList *sdp_connections;
	bool adapter_powered;
	bool setup_complete;
	bool power_pending;
	bool profile_pending;
	bool profile_registered;
	bool discovery_started;
	bool connect_requested;
	bool test_started;
	bool rfcomm_connected;
	bool tearing_down;
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

static void maybe_setup_complete(struct test_data *data)
{
	if (data->setup_complete)
		return;

	if (!data->adapter_proxy || !data->manager_proxy)
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

static void register_serial_port(uint8_t channel)
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
	sdp_set_info_attr(record, "Serial Port", "BlueZ", "SPP test peer");
	sdp_set_service_id(record, sp_uuid);

	sdp_data_free(channel_data);
	sdp_list_free(proto[0], NULL);
	sdp_list_free(proto[1], NULL);
	sdp_list_free(apseq, NULL);
	sdp_list_free(aproto, NULL);
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

static void sdp_cid_hook(const void *buf, uint16_t len, void *user_data)
{
	struct sdp_connection *conn = user_data;
	uint8_t rsp[1024];
	ssize_t rsp_len;

	handle_internal_request(conn->server_fd, SDP_MTU, (void *) buf, len);

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
	bthost_add_rfcomm_server(data->peer, REMOTE_RFCOMM_CHANNEL,
				rfcomm_connect_cb, data);
	bthost_add_l2cap_server(data->peer, 0x0001, sdp_connect_cb,
				sdp_disconnect_cb, data);
	bthost_write_scan_enable(data->peer, 0x03);

	set_fixed_db_timestamp(0x496f0654);
	sdp_svcdb_reset();
	register_public_browse_group();
	register_server_service();
	register_serial_port(REMOTE_RFCOMM_CHANNEL);
}

static void unregister_profile_interface(struct test_data *data)
{
	if (!data->dbus_conn)
		return;

	g_dbus_unregister_interface(data->dbus_conn, PROFILE_PATH,
					"org.bluez.Profile1");
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

static void connect_profile_setup(DBusMessageIter *iter, void *user_data)
{
	const char *uuid = PROFILE_UUID;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &uuid);
}

static void connect_profile_reply(DBusMessage *message, void *user_data)
{
	DBusError error;

	dbus_error_init(&error);
	if (dbus_set_error_from_message(&error, message)) {
		tester_warn("ConnectProfile failed: %s", error.name);
		dbus_error_free(&error);
		tester_test_failed();
		return;
	}

	tester_print("ConnectProfile request accepted");
}

static void maybe_connect_profile(struct test_data *data)
{
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

static void discovery_reply(DBusMessage *message, void *user_data)
{
	DBusError error;

	dbus_error_init(&error);
	if (dbus_set_error_from_message(&error, message)) {
		tester_warn("Discovery request failed: %s", error.name);
		dbus_error_free(&error);
		tester_test_failed();
		return;
	}

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

static void register_profile_setup(DBusMessageIter *iter, void *user_data)
{
	DBusMessageIter dict;
	const char *path = PROFILE_PATH;
	const char *uuid = PROFILE_UUID;
	const char *name = PROFILE_NAME;
	const char *role = "client";
	dbus_bool_t require_authentication = FALSE;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH, &path);
	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &uuid);

	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
	g_dbus_dict_append_entry(&dict, "Name", DBUS_TYPE_STRING, &name);
	g_dbus_dict_append_entry(&dict, "Role", DBUS_TYPE_STRING, &role);
	g_dbus_dict_append_entry(&dict, "RequireAuthentication",
					DBUS_TYPE_BOOLEAN,
					&require_authentication);
	dbus_message_iter_close_container(iter, &dict);
}

static void register_profile_reply(DBusMessage *message, void *user_data)
{
	struct test_data *data = user_data;
	DBusError error;

	data->profile_pending = false;

	dbus_error_init(&error);
	if (dbus_set_error_from_message(&error, message)) {
		tester_warn("RegisterProfile failed: %s", error.name);
		dbus_error_free(&error);
		tester_setup_failed();
		return;
	}

	data->profile_registered = true;
	tester_print("Registered external SPP profile client");
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

	if (error != NULL) {
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

static void connect_handler(DBusConnection *connection, void *user_data)
{
	tester_print("Connected to daemon");
}

static void disconnect_handler(DBusConnection *connection, void *user_data)
{
	struct test_data *data = user_data;

	if (data->tearing_down)
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
			maybe_register_profile(data);
			maybe_setup_complete(data);
		}
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
	char remote_addr[18];

	if (g_str_equal(interface, "org.bluez.ProfileManager1")) {
		replace_proxy(&data->manager_proxy, proxy);
		maybe_register_profile(data);
		maybe_setup_complete(data);
		return;
	}

	if (g_str_equal(interface, "org.bluez.Adapter1")) {
		if (!match_proxy_address(proxy, hciemu_get_address(data->hciemu)))
			return;

		replace_proxy(&data->adapter_proxy, proxy);
		proxy_get_bool(proxy, "Powered", &data->adapter_powered);
		if (!data->adapter_powered)
			maybe_power_adapter(data);
		else {
			maybe_register_profile(data);
			maybe_setup_complete(data);
		}
		return;
	}

	if (!g_str_equal(interface, "org.bluez.Device1"))
		return;

	ba2str((const bdaddr_t *) hciemu_get_client_bdaddr(data->hciemu),
			remote_addr);
	if (!match_proxy_address(proxy, remote_addr))
		return;

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

	if (proxy == data->manager_proxy)
		unref_proxy(&data->manager_proxy);
}

static void test_setup(const void *test_data)
{
	struct test_data *data = tester_get_data();

	data->new_connection_fd = -1;

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
	start_discovery(data);
	maybe_connect_profile(data);
}

static void test_teardown(const void *test_data)
{
	struct test_data *data = tester_get_data();

	data->tearing_down = true;

	if (data->new_connection_fd >= 0) {
		close(data->new_connection_fd);
		data->new_connection_fd = -1;
	}

	unregister_profile_interface(data);

	unref_proxy(&data->device_proxy);
	unref_proxy(&data->adapter_proxy);
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
}

static void destroy_data(void *user_data)
{
	struct test_data *data = user_data;

	g_free(data);
}

int main(int argc, char *argv[])
{
	struct test_data *data;

	tester_init(&argc, &argv);

	data = g_new0(struct test_data, 1);
	data->new_connection_fd = -1;

	tester_add_full("ProfileManager SPP Remote SDP Discovery", NULL,
				NULL, test_setup, test_run, test_teardown,
				NULL, 15, data, destroy_data);

	return tester_run();
}
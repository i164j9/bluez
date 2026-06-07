// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *
 */

#define _GNU_SOURCE

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdint.h>
#include <stdlib.h>

#include <glib.h>

#include "src/shared/tester.h"

#define main rfcomm_main
#include "tools/rfcomm.c"
#undef main

static sdp_record_t *create_serial_port_record(const char *name, uint8_t channel)
{
	sdp_list_t *apseq = NULL, *aproto = NULL, *proto[2] = { NULL, NULL };
	uuid_t l2cap, rfcomm;
	sdp_data_t *channel_data;
	sdp_record_t *record;

	record = sdp_record_alloc();
	g_assert_nonnull(record);

	sdp_uuid16_create(&l2cap, L2CAP_UUID);
	proto[0] = sdp_list_append(NULL, &l2cap);
	apseq = sdp_list_append(NULL, proto[0]);

	sdp_uuid16_create(&rfcomm, RFCOMM_UUID);
	proto[1] = sdp_list_append(NULL, &rfcomm);
	channel_data = sdp_data_alloc(SDP_UINT8, &channel);
	g_assert_nonnull(channel_data);
	proto[1] = sdp_list_append(proto[1], channel_data);
	apseq = sdp_list_append(apseq, proto[1]);

	aproto = sdp_list_append(NULL, apseq);
	sdp_set_access_protos(record, aproto);
	sdp_set_info_attr(record, name, "BlueZ", "RFCOMM test record");

	sdp_data_free(channel_data);
	sdp_list_free(proto[0], NULL);
	sdp_list_free(proto[1], NULL);
	sdp_list_free(apseq, NULL);
	sdp_list_free(aproto, NULL);

	return record;
}

static void free_record_list(sdp_list_t *records)
{
	sdp_list_free(records, (sdp_free_func_t) sdp_record_free);
}

static void test_select_first_serial_port_channel(const void *data)
{
	sdp_list_t *records = NULL;
	unsigned int matches = 0;
	uint8_t channel = 0;
	int err;

	records = sdp_list_append(records,
				create_serial_port_record("Legacy Serial Port", 3));
	records = sdp_list_append(records,
				create_serial_port_record("Target Serial Port", 7));

	err = select_serial_port_channel_from_records(records, NULL,
							&channel, &matches);
	g_assert_cmpint(err, ==, 0);
	g_assert_cmpuint(matches, ==, 2);
	g_assert_cmpuint(channel, ==, 3);

	free_record_list(records);
	tester_test_passed();
}

static void test_select_serial_port_channel_by_service_name(const void *data)
{
	sdp_list_t *records = NULL;
	unsigned int matches = 0;
	uint8_t channel = 0;
	int err;

	records = sdp_list_append(records,
				create_serial_port_record("Legacy Serial Port", 3));
	records = sdp_list_append(records,
				create_serial_port_record("Target Serial Port", 7));

	err = select_serial_port_channel_from_records(records,
							"Target Serial Port",
							&channel, &matches);
	g_assert_cmpint(err, ==, 0);
	g_assert_cmpuint(matches, ==, 1);
	g_assert_cmpuint(channel, ==, 7);

	free_record_list(records);
	tester_test_passed();
}

static void test_select_serial_port_channel_missing_service_name(const void *data)
{
	sdp_list_t *records = NULL;
	unsigned int matches = 0;
	uint8_t channel = 0;
	int err;

	records = sdp_list_append(records,
				create_serial_port_record("Legacy Serial Port", 3));
	records = sdp_list_append(records,
				create_serial_port_record("Target Serial Port", 7));

	err = select_serial_port_channel_from_records(records,
							"Missing Serial Port",
							&channel, &matches);
	g_assert_cmpint(err, ==, -1);
	g_assert_cmpuint(matches, ==, 0);
	g_assert_cmpuint(channel, ==, 0);

	free_record_list(records);
	tester_test_passed();
}

int main(int argc, char *argv[])
{
	tester_init(&argc, &argv);

	tester_add("/rfcomm/select/first-record-without-filter", NULL, NULL,
			test_select_first_serial_port_channel, NULL);
	tester_add("/rfcomm/select/service-name-match", NULL, NULL,
			test_select_serial_port_channel_by_service_name, NULL);
	tester_add("/rfcomm/select/service-name-missing", NULL, NULL,
			test_select_serial_port_channel_missing_service_name, NULL);

	return tester_run();
}
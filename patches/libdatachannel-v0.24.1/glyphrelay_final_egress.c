/**
 * Copyright (c) 2026 GlyphRelay contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "juice/juice.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static void wait_millis(unsigned int millis) { Sleep(millis); }
#else
#include <unistd.h>
static void wait_millis(unsigned int millis) { usleep(millis * 1000); }
#endif

typedef struct peer_context {
	juice_agent_t **other;
	atomic_int state;
	atomic_int received;
	atomic_int control_hooks;
	atomic_int media_hooks;
	atomic_int invalid_hooks;
} peer_context_t;

static int final_udp_send(const char *data, size_t size, juice_egress_class_t egress_class,
	                      juice_datagram_path_t path, juice_datagram_protocol_t protocol,
	                      juice_ip_family_t ip_family, juice_cb_native_udp_send_t native_send,
	                      void *native_send_ptr, void *user_ptr) {
	peer_context_t *context = user_ptr;
	if (!data || path != JUICE_DATAGRAM_PATH_DIRECT_UDP ||
	    ip_family != JUICE_IP_FAMILY_IPV4) {
		atomic_fetch_add(&context->invalid_hooks, 1);
	}
	if (egress_class == JUICE_EGRESS_CLASS_MEDIA) {
		atomic_fetch_add(&context->media_hooks, 1);
		if (protocol != JUICE_DATAGRAM_PROTOCOL_SRTP)
			atomic_fetch_add(&context->invalid_hooks, 1);
	} else {
		atomic_fetch_add(&context->control_hooks, 1);
		if (protocol != JUICE_DATAGRAM_PROTOCOL_STUN &&
		    protocol != JUICE_DATAGRAM_PROTOCOL_UNKNOWN_CONTROL)
			atomic_fetch_add(&context->invalid_hooks, 1);
		if (size == 0 && protocol != JUICE_DATAGRAM_PROTOCOL_UNKNOWN_CONTROL)
			atomic_fetch_add(&context->invalid_hooks, 1);
	}
	return native_send(native_send_ptr);
}

static void state_changed(juice_agent_t *agent, juice_state_t state, void *user_ptr) {
	(void)agent;
	peer_context_t *context = user_ptr;
	atomic_store(&context->state, (int)state);
}

static void candidate(juice_agent_t *agent, const char *sdp, void *user_ptr) {
	(void)agent;
	peer_context_t *context = user_ptr;
	if (*context->other)
		juice_add_remote_candidate(*context->other, sdp);
}

static void gathering_done(juice_agent_t *agent, void *user_ptr) {
	(void)agent;
	peer_context_t *context = user_ptr;
	if (*context->other)
		juice_set_remote_gathering_done(*context->other);
}

static void received(juice_agent_t *agent, const char *data, size_t size, void *user_ptr) {
	(void)agent;
	peer_context_t *context = user_ptr;
	static const char expected[] = "glyphrelay-final-egress";
	if (size == sizeof(expected) - 1 && memcmp(data, expected, size) == 0)
		atomic_store(&context->received, 1);
}

static bool is_ready(const peer_context_t *context) {
	int state = atomic_load(&context->state);
	return state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED;
}

static int wait_for_connection(peer_context_t *first, peer_context_t *second) {
	for (int attempt = 0; attempt < 1000; ++attempt) {
		if (is_ready(first) && is_ready(second))
			return 0;
		wait_millis(10);
	}
	return -1;
}

static void initialize_config(juice_config_t *config, peer_context_t *context) {
	config->concurrency_mode = JUICE_CONCURRENCY_MODE_THREAD;
	config->bind_address = "127.0.0.1";
	config->cb_state_changed = state_changed;
	config->cb_candidate = candidate;
	config->cb_gathering_done = gathering_done;
	config->cb_recv = received;
	config->cb_final_udp_send = final_udp_send;
	config->user_ptr = context;
}

static int begin_pair(juice_agent_t *first, juice_agent_t *second) {
	char first_sdp[JUICE_MAX_SDP_STRING_LEN];
	char second_sdp[JUICE_MAX_SDP_STRING_LEN];
	return juice_get_local_description(first, first_sdp, sizeof(first_sdp)) == JUICE_ERR_SUCCESS &&
	               juice_get_local_description(second, second_sdp, sizeof(second_sdp)) ==
	                   JUICE_ERR_SUCCESS &&
	               juice_set_remote_description(first, second_sdp) == JUICE_ERR_SUCCESS &&
	               juice_set_remote_description(second, first_sdp) == JUICE_ERR_SUCCESS &&
	               juice_gather_candidates(first) == JUICE_ERR_SUCCESS &&
	               juice_gather_candidates(second) == JUICE_ERR_SUCCESS
	           ? 0
	           : -1;
}

static int relay_only_blocks_direct_pair(void) {
	juice_agent_t *first = NULL;
	juice_agent_t *second = NULL;
	peer_context_t first_context = {.other = &second};
	peer_context_t second_context = {.other = &first};
	juice_config_t first_config = {0};
	juice_config_t second_config = {0};
	initialize_config(&first_config, &first_context);
	initialize_config(&second_config, &second_context);
	first_config.relay_only = true;
	first = juice_create(&first_config);
	second = juice_create(&second_config);
	if (!first || !second || begin_pair(first, second) != 0)
		goto failure;

	for (int attempt = 0; attempt < 100; ++attempt) {
		if (is_ready(&first_context))
			goto failure;
		wait_millis(10);
	}
	juice_destroy(first);
	juice_destroy(second);
	return 0;

failure:
	if (first)
		juice_destroy(first);
	if (second)
		juice_destroy(second);
	return -1;
}

int main(void) {
	juice_set_log_level(JUICE_LOG_LEVEL_WARN);
	juice_config_t rejected_mux = {0};
	rejected_mux.concurrency_mode = JUICE_CONCURRENCY_MODE_MUX;
	rejected_mux.cb_final_udp_send = final_udp_send;
	if (juice_create(&rejected_mux) != NULL) {
		fprintf(stderr, "final UDP hook did not reject socket mux\n");
		return 1;
	}
	if (relay_only_blocks_direct_pair() != 0) {
		fprintf(stderr, "relay-only policy admitted a direct candidate pair\n");
		return 1;
	}

	juice_agent_t *first = NULL;
	juice_agent_t *second = NULL;
	peer_context_t first_context = {.other = &second};
	peer_context_t second_context = {.other = &first};
	juice_config_t first_config = {0};
	juice_config_t second_config = {0};
	initialize_config(&first_config, &first_context);
	initialize_config(&second_config, &second_context);
	first = juice_create(&first_config);
	second = juice_create(&second_config);
	if (!first || !second || begin_pair(first, second) != 0 ||
	    wait_for_connection(&first_context, &second_context) != 0)
		goto failure;

	static const char payload[] = "glyphrelay-final-egress";
	if (juice_send_diffserv_classified(first, payload, sizeof(payload) - 1, 0,
	                                   JUICE_EGRESS_CLASS_MEDIA,
	                                   JUICE_DATAGRAM_PROTOCOL_SRTP) != JUICE_ERR_SUCCESS)
		goto failure;
	for (int attempt = 0; attempt < 500 && !atomic_load(&second_context.received); ++attempt)
		wait_millis(10);

	if (!atomic_load(&second_context.received) || atomic_load(&first_context.media_hooks) != 1 ||
	    atomic_load(&first_context.control_hooks) == 0 ||
	    atomic_load(&second_context.control_hooks) == 0 ||
	    atomic_load(&first_context.invalid_hooks) != 0 ||
	    atomic_load(&second_context.invalid_hooks) != 0)
		goto failure;

	juice_destroy(first);
	juice_destroy(second);
	return 0;

failure:
	if (first)
		juice_destroy(first);
	if (second)
		juice_destroy(second);
	fprintf(stderr, "final UDP hook loopback contract failed\n");
	return 1;
}

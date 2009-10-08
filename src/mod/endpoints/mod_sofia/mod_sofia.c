/* 
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2009, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * 
 * Anthony Minessale II <anthm@freeswitch.org>
 * Ken Rice <krice at cometsig.com>
 * Paul D. Tinsley <pdt at jackhammer.org>
 * Bret McDanel <trixter AT 0xdecafbad.com>
 *
 *
 * mod_sofia.c -- SOFIA SIP Endpoint
 *
 */

/* Best viewed in a 160 x 60 VT100 Terminal or so the line below at least fits across your screen*/
/*************************************************************************************************************************************************************/
#include "mod_sofia.h"
#include "sofia-sip/sip_extra.h"
SWITCH_MODULE_LOAD_FUNCTION(mod_sofia_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_sofia_shutdown);
SWITCH_MODULE_DEFINITION(mod_sofia, mod_sofia_load, mod_sofia_shutdown, NULL);

struct mod_sofia_globals mod_sofia_globals;
switch_endpoint_interface_t *sofia_endpoint_interface;
static switch_frame_t silence_frame = { 0 };
static char silence_data[13] = "";

#define STRLEN 15

static switch_status_t sofia_on_init(switch_core_session_t *session);

static switch_status_t sofia_on_exchange_media(switch_core_session_t *session);
static switch_status_t sofia_on_soft_execute(switch_core_session_t *session);
static switch_call_cause_t sofia_outgoing_channel(switch_core_session_t *session, switch_event_t *var_event,
												  switch_caller_profile_t *outbound_profile, switch_core_session_t **new_session,
												  switch_memory_pool_t **pool, switch_originate_flag_t flags);
static switch_status_t sofia_read_frame(switch_core_session_t *session, switch_frame_t **frame, switch_io_flag_t flags, int stream_id);
static switch_status_t sofia_write_frame(switch_core_session_t *session, switch_frame_t *frame, switch_io_flag_t flags, int stream_id);
static switch_status_t sofia_read_video_frame(switch_core_session_t *session, switch_frame_t **frame, switch_io_flag_t flags, int stream_id);
static switch_status_t sofia_write_video_frame(switch_core_session_t *session, switch_frame_t *frame, switch_io_flag_t flags, int stream_id);
static switch_status_t sofia_kill_channel(switch_core_session_t *session, int sig);

/* BODY OF THE MODULE */
/*************************************************************************************************************************************************************/

/* 
   State methods they get called when the state changes to the specific state 
   returning SWITCH_STATUS_SUCCESS tells the core to execute the standard state method next
   so if you fully implement the state you can return SWITCH_STATUS_FALSE to skip it.
*/
static switch_status_t sofia_on_init(switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	private_object_t *tech_pvt = (private_object_t *) switch_core_session_get_private(session);
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	switch_assert(tech_pvt != NULL);

	tech_pvt->read_frame.buflen = SWITCH_RTP_MAX_BUF_LEN;
	switch_mutex_lock(tech_pvt->sofia_mutex);

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s SOFIA INIT\n", switch_channel_get_name(channel));
	if (switch_channel_test_flag(channel, CF_PROXY_MODE) || switch_channel_test_flag(channel, CF_PROXY_MEDIA)) {
		sofia_glue_tech_absorb_sdp(tech_pvt);
	}

	if (sofia_test_flag(tech_pvt, TFLAG_OUTBOUND)) {
		const char *var;

		if ((var = switch_channel_get_variable(channel, SOFIA_SECURE_MEDIA_VARIABLE)) && !switch_strlen_zero(var)) {
			if (switch_true(var) || !strcasecmp(var, SWITCH_RTP_CRYPTO_KEY_32)) {
				sofia_set_flag_locked(tech_pvt, TFLAG_SECURE);
				sofia_glue_build_crypto(tech_pvt, 1, AES_CM_128_HMAC_SHA1_32, SWITCH_RTP_CRYPTO_SEND);
			} else if (!strcasecmp(var, SWITCH_RTP_CRYPTO_KEY_80)) {
				sofia_set_flag_locked(tech_pvt, TFLAG_SECURE);
				sofia_glue_build_crypto(tech_pvt, 1, AES_CM_128_HMAC_SHA1_80, SWITCH_RTP_CRYPTO_SEND);
			}
		}


		if (sofia_glue_do_invite(session) != SWITCH_STATUS_SUCCESS) {
			switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
			assert(switch_channel_get_state(channel) != CS_INIT);
			status = SWITCH_STATUS_FALSE;
			goto end;
		}
	}

	/* Move channel's state machine to ROUTING */
	switch_channel_set_state(channel, CS_ROUTING);
	assert(switch_channel_get_state(channel) != CS_INIT);

 end:

	switch_mutex_unlock(tech_pvt->sofia_mutex);

	return status;
}

static switch_status_t sofia_on_routing(switch_core_session_t *session)
{
	private_object_t *tech_pvt = (private_object_t *) switch_core_session_get_private(session);
	switch_assert(tech_pvt != NULL);

	if (!sofia_test_flag(tech_pvt, TFLAG_HOLD_LOCK)) {
		sofia_clear_flag_locked(tech_pvt, TFLAG_SIP_HOLD);
	}

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s SOFIA ROUTING\n", switch_channel_get_name(switch_core_session_get_channel(session)));

	return SWITCH_STATUS_SUCCESS;
}


static switch_status_t sofia_on_reset(switch_core_session_t *session)
{
	private_object_t *tech_pvt = (private_object_t *) switch_core_session_get_private(session);
	switch_assert(tech_pvt != NULL);

	if (!sofia_test_flag(tech_pvt, TFLAG_HOLD_LOCK)) {
		sofia_clear_flag_locked(tech_pvt, TFLAG_SIP_HOLD);
	}

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s SOFIA RESET\n", switch_channel_get_name(switch_core_session_get_channel(session)));

	return SWITCH_STATUS_SUCCESS;
}


static switch_status_t sofia_on_hibernate(switch_core_session_t *session)
{
	private_object_t *tech_pvt = switch_core_session_get_private(session);
	switch_assert(tech_pvt != NULL);

	if (!sofia_test_flag(tech_pvt, TFLAG_HOLD_LOCK)) {
		sofia_clear_flag_locked(tech_pvt, TFLAG_SIP_HOLD);
	}

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s SOFIA HIBERNATE\n", switch_channel_get_name(switch_core_session_get_channel(session)));

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t sofia_on_execute(switch_core_session_t *session)
{
	private_object_t *tech_pvt = (private_object_t *) switch_core_session_get_private(session);
	switch_assert(tech_pvt != NULL);

	if (!sofia_test_flag(tech_pvt, TFLAG_HOLD_LOCK)) {
		sofia_clear_flag_locked(tech_pvt, TFLAG_SIP_HOLD);
	}
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s SOFIA EXECUTE\n", switch_channel_get_name(switch_core_session_get_channel(session)));

	return SWITCH_STATUS_SUCCESS;
}

char *generate_pai_str(switch_core_session_t *session) 
{
	private_object_t *tech_pvt = (private_object_t *) switch_core_session_get_private(session);
	const char *callee_name = NULL, *callee_number = NULL;
	char *pai = NULL;

	if ((callee_name = switch_channel_get_variable(tech_pvt->channel, "sip_callee_id_name"))) {
		if (!(callee_number = switch_channel_get_variable(tech_pvt->channel, "sip_callee_id_number"))) {
			callee_number = tech_pvt->caller_profile->destination_number;
		}

		if (!strchr(callee_number, '@')) {
			char *tmp = switch_core_session_sprintf(session, "sip:%s@cluecon.com", callee_number);
			callee_number = tmp;
		}

		pai = switch_core_session_sprintf(tech_pvt->session, "P-Asserted-Identity: \"%s\" <%s>", callee_name, callee_number);
	}
	return pai;
}

/* map QSIG cause codes to SIP from RFC4497 section 8.4.1 */
static int hangup_cause_to_sip(switch_call_cause_t cause)
{
	switch (cause) {
	case SWITCH_CAUSE_UNALLOCATED_NUMBER:
	case SWITCH_CAUSE_NO_ROUTE_TRANSIT_NET:
	case SWITCH_CAUSE_NO_ROUTE_DESTINATION:
		return 404;
	case SWITCH_CAUSE_USER_BUSY:
		return 486;
	case SWITCH_CAUSE_NO_USER_RESPONSE:
		return 408;
	case SWITCH_CAUSE_NO_ANSWER:
	case SWITCH_CAUSE_SUBSCRIBER_ABSENT:
		return 480;
	case SWITCH_CAUSE_CALL_REJECTED:
		return 603;
	case SWITCH_CAUSE_NUMBER_CHANGED:
	case SWITCH_CAUSE_REDIRECTION_TO_NEW_DESTINATION:
		return 410;
	case SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER:
		return 502;
	case SWITCH_CAUSE_INVALID_NUMBER_FORMAT:
		return 484;
	case SWITCH_CAUSE_FACILITY_REJECTED:
		return 501;
	case SWITCH_CAUSE_NORMAL_UNSPECIFIED:
		return 480;
	case SWITCH_CAUSE_REQUESTED_CHAN_UNAVAIL:
	case SWITCH_CAUSE_NORMAL_CIRCUIT_CONGESTION:
	case SWITCH_CAUSE_NETWORK_OUT_OF_ORDER:
	case SWITCH_CAUSE_NORMAL_TEMPORARY_FAILURE:
	case SWITCH_CAUSE_SWITCH_CONGESTION:
		return 503;
	case SWITCH_CAUSE_OUTGOING_CALL_BARRED:
	case SWITCH_CAUSE_INCOMING_CALL_BARRED:
	case SWITCH_CAUSE_BEARERCAPABILITY_NOTAUTH:
		return 403;
	case SWITCH_CAUSE_BEARERCAPABILITY_NOTAVAIL:
		return 503;
	case SWITCH_CAUSE_BEARERCAPABILITY_NOTIMPL:
	case SWITCH_CAUSE_INCOMPATIBLE_DESTINATION:
		return 488;
	case SWITCH_CAUSE_FACILITY_NOT_IMPLEMENTED:
	case SWITCH_CAUSE_SERVICE_NOT_IMPLEMENTED:
		return 501;
	case SWITCH_CAUSE_RECOVERY_ON_TIMER_EXPIRE:
		return 504;
	case SWITCH_CAUSE_ORIGINATOR_CANCEL:
		return 487;
	case SWITCH_CAUSE_EXCHANGE_ROUTING_ERROR:
		return 483;
	default:
		return 480;
	}
}

switch_status_t sofia_on_destroy(switch_core_session_t *session)
{
	private_object_t *tech_pvt = (private_object_t *) switch_core_session_get_private(session);
	switch_channel_t *channel = switch_core_session_get_channel(session);

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s SOFIA DESTROY\n", switch_channel_get_name(channel));

	if (tech_pvt) {
		if (switch_core_codec_ready(&tech_pvt->read_codec)) {
			switch_core_codec_destroy(&tech_pvt->read_codec);
		}
		
		if (switch_core_codec_ready(&tech_pvt->write_codec)) {
			switch_core_codec_destroy(&tech_pvt->write_codec);
		}

		switch_core_session_unset_read_codec(session);
		switch_core_session_unset_write_codec(session);

		switch_mutex_lock(tech_pvt->profile->flag_mutex);
		tech_pvt->profile->inuse--;
		switch_mutex_unlock(tech_pvt->profile->flag_mutex);

		sofia_glue_deactivate_rtp(tech_pvt);
	}

	return SWITCH_STATUS_SUCCESS;

}

switch_status_t sofia_on_hangup(switch_core_session_t *session)
{
	switch_core_session_t *a_session;
	private_object_t *tech_pvt = (private_object_t *) switch_core_session_get_private(session);
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_call_cause_t cause = switch_channel_get_cause(channel);
	int sip_cause = hangup_cause_to_sip(cause);
	const char *ps_cause = NULL, *use_my_cause;

	switch_mutex_lock(tech_pvt->sofia_mutex);

	if (!switch_channel_test_flag(channel, CF_ANSWERED)) {
		if (switch_channel_direction(channel) == SWITCH_CALL_DIRECTION_OUTBOUND) {
			tech_pvt->profile->ob_failed_calls++;
		} else {
			tech_pvt->profile->ib_failed_calls++;
		}
	}

	if (!((use_my_cause = switch_channel_get_variable(channel, "sip_ignore_remote_cause")) && switch_true(use_my_cause))) {
		ps_cause = switch_channel_get_variable(channel, SWITCH_PROTO_SPECIFIC_HANGUP_CAUSE_VARIABLE);
	}

	if (!switch_strlen_zero(ps_cause) && (!strncasecmp(ps_cause, "sip:", 4) || !strncasecmp(ps_cause, "sips:", 5))) {
		int new_cause = atoi(sofia_glue_strip_proto(ps_cause));
		if (new_cause) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s Overriding SIP cause %d with %d from the other leg\n",
							  switch_channel_get_name(channel), sip_cause, new_cause);
			sip_cause = new_cause;
		}
	}

	if (sofia_test_flag(tech_pvt, TFLAG_SIP_HOLD) && cause != SWITCH_CAUSE_ATTENDED_TRANSFER) {
		const char *buuid;
		switch_core_session_t *bsession;
		switch_channel_t *bchannel;
		const char *lost_ext;

		if (tech_pvt->max_missed_packets) {
			switch_rtp_set_max_missed_packets(tech_pvt->rtp_session, tech_pvt->max_missed_packets);
		}
		switch_channel_presence(tech_pvt->channel, "unknown", "unhold", NULL);

		if ((buuid = switch_channel_get_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE))) {
			if ((bsession = switch_core_session_locate(buuid))) {
				bchannel = switch_core_session_get_channel(bsession);
				if (switch_channel_test_flag(bchannel, CF_BROADCAST)) {
					if ((lost_ext = switch_channel_get_variable(bchannel, "left_hanging_extension"))) {
						switch_ivr_session_transfer(bsession, lost_ext, NULL, NULL);
					}
					switch_channel_stop_broadcast(bchannel);
				}
				switch_core_session_rwunlock(bsession);
			}
		}
		sofia_clear_flag_locked(tech_pvt, TFLAG_SIP_HOLD);
	}

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Channel %s hanging up, cause: %s\n",
					  switch_channel_get_name(channel), switch_channel_cause2str(cause));

	if (tech_pvt->hash_key) {
		switch_core_hash_delete(tech_pvt->profile->chat_hash, tech_pvt->hash_key);
	}

	if (session && tech_pvt->profile->pres_type) {
		char *sql = switch_mprintf("delete from sip_dialogs where call_id='%q'", tech_pvt->call_id);
		switch_assert(sql);
		sofia_glue_execute_sql(tech_pvt->profile, &sql, SWITCH_TRUE);
	}

	if (tech_pvt->kick && (a_session = switch_core_session_locate(tech_pvt->kick))) {
		switch_channel_t *a_channel = switch_core_session_get_channel(a_session);
		switch_channel_hangup(a_channel, switch_channel_get_cause(channel));
		switch_core_session_rwunlock(a_session);
	}

	if (tech_pvt->nh && !sofia_test_flag(tech_pvt, TFLAG_BYE)) {
		char reason[128] = "";
		char *bye_headers = sofia_glue_get_extra_headers(channel, SOFIA_SIP_BYE_HEADER_PREFIX);
		
		if (cause > 0 && cause < 128) {
			switch_snprintf(reason, sizeof(reason), "Q.850;cause=%d;text=\"%s\"", cause, switch_channel_cause2str(cause));
		} else if (cause == SWITCH_CAUSE_PICKED_OFF || cause == SWITCH_CAUSE_LOSE_RACE) {
			switch_snprintf(reason, sizeof(reason), "SIP;cause=200;text=\"Call completed elsewhere\"");
		} else {
			switch_snprintf(reason, sizeof(reason), "%s;cause=%d;text=\"%s\"",
					tech_pvt->profile->username, 
					cause,
					switch_channel_cause2str(cause));
		}

		if (switch_channel_test_flag(channel, CF_ANSWERED)) {
			if (!tech_pvt->got_bye) {
				switch_channel_set_variable(channel, "sip_hangup_disposition", "send_bye");
			}
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Sending BYE to %s\n", switch_channel_get_name(channel));
			if (!sofia_test_flag(tech_pvt, TFLAG_BYE)) {
				nua_bye(tech_pvt->nh, 
						SIPTAG_REASON_STR(reason),
						TAG_IF(!switch_strlen_zero(tech_pvt->user_via), SIPTAG_VIA_STR(tech_pvt->user_via)),
						TAG_IF(!switch_strlen_zero(bye_headers), SIPTAG_HEADER_STR(bye_headers)),
						TAG_END());
			}
		} else {
			if (switch_channel_test_flag(channel, CF_OUTBOUND)) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Sending CANCEL to %s\n", switch_channel_get_name(channel));
				if (!tech_pvt->got_bye) {
					switch_channel_set_variable(channel, "sip_hangup_disposition", "send_cancel");
				}
				if (!sofia_test_flag(tech_pvt, TFLAG_BYE)) {
					nua_cancel(tech_pvt->nh, 
							   SIPTAG_REASON_STR(reason), 
							   TAG_IF(!switch_strlen_zero(bye_headers), SIPTAG_HEADER_STR(bye_headers)),
							   TAG_END());
				}
			} else {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Responding to INVITE with: %d\n", sip_cause);
				if (!tech_pvt->got_bye) {
					switch_channel_set_variable(channel, "sip_hangup_disposition", "send_refuse");
				}
				if (!sofia_test_flag(tech_pvt, TFLAG_BYE)) {
					nua_respond(tech_pvt->nh, sip_cause, sip_status_phrase(sip_cause), 
								SIPTAG_REASON_STR(reason), 
								TAG_IF(!switch_strlen_zero(bye_headers), SIPTAG_HEADER_STR(bye_headers)),
								TAG_END());
				}
			}
		}

		sofia_set_flag_locked(tech_pvt, TFLAG_BYE);
		switch_safe_free(bye_headers);
	}

	sofia_clear_flag(tech_pvt, TFLAG_IO);

	if (tech_pvt->sofia_private) {
		*tech_pvt->sofia_private->uuid = '\0';
	}
	

	sofia_glue_set_rtp_stats(tech_pvt);

	switch_mutex_unlock(tech_pvt->sofia_mutex);

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t sofia_on_exchange_media(switch_core_session_t *session)
{
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "SOFIA LOOPBACK\n");
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t sofia_on_soft_execute(switch_core_session_t *session)
{
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "SOFIA TRANSMIT\n");
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t sofia_answer_channel(switch_core_session_t *session)
{
	private_object_t *tech_pvt = (private_object_t *) switch_core_session_get_private(session);
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_status_t status;
	uint32_t session_timeout = tech_pvt->profile->session_timeout;
	const char *val;
	const char *b_sdp = NULL;
	int is_proxy = 0;
	char *sticky = NULL;

	if (sofia_test_flag(tech_pvt, TFLAG_ANS) || switch_channel_test_flag(channel, CF_OUTBOUND)) {
		return SWITCH_STATUS_SUCCESS;
	}


	b_sdp = switch_channel_get_variable(channel, SWITCH_B_SDP_VARIABLE);
	is_proxy = (switch_channel_test_flag(channel, CF_PROXY_MODE) || switch_channel_test_flag(channel, CF_PROXY_MEDIA));

	if (b_sdp && is_proxy) {
		sofia_glue_tech_set_local_sdp(tech_pvt, b_sdp, SWITCH_TRUE);

		if (switch_channel_test_flag(channel, CF_PROXY_MEDIA)) {
			sofia_glue_tech_patch_sdp(tech_pvt);
			if (sofia_glue_activate_rtp(tech_pvt, 0) != SWITCH_STATUS_SUCCESS) {
				return SWITCH_STATUS_FALSE;
			}
		}
	} else {
		/* This if statement check and handles the 3pcc proxy mode */
		if (sofia_test_pflag(tech_pvt->profile, PFLAG_3PCC_PROXY) && sofia_test_flag(tech_pvt, TFLAG_3PCC)) {
			/* Send the 200 OK */
			if (!sofia_test_flag(tech_pvt, TFLAG_BYE)) {
				char *extra_headers = sofia_glue_get_extra_headers(channel, SOFIA_SIP_PROGRESS_HEADER_PREFIX);
				nua_respond(tech_pvt->nh, SIP_200_OK,
							SIPTAG_CONTACT_STR(tech_pvt->profile->url),
							SOATAG_USER_SDP_STR(tech_pvt->local_sdp_str),
							SOATAG_REUSE_REJECTED(1),
							SOATAG_ORDERED_USER(1), SOATAG_AUDIO_AUX("cn telephone-event"), NUTAG_INCLUDE_EXTRA_SDP(1), 
							TAG_IF(!switch_strlen_zero(extra_headers), SIPTAG_HEADER_STR(extra_headers)),
							SIPTAG_HEADER_STR("X-Actually-Support: "SOFIA_ACTUALLY_SUPPORT),
							TAG_END());

				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "3PCC-PROXY, Sent a 200 OK, waiting for ACK\n");
				switch_safe_free(extra_headers);
			}

			/* Unlock the session signal to allow the ack to make it in */
			// Maybe we should timeout?
			switch_mutex_unlock(tech_pvt->sofia_mutex);
			
			while(switch_channel_ready(channel) && !sofia_test_flag(tech_pvt, TFLAG_3PCC_HAS_ACK)) {
				switch_cond_next();
			}
			
			/*  Regain lock on sofia */
			switch_mutex_lock(tech_pvt->sofia_mutex);
			
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "3PCC-PROXY, Done waiting for ACK\n");
		}

		if ((is_proxy && !b_sdp) || sofia_test_flag(tech_pvt, TFLAG_LATE_NEGOTIATION) || !tech_pvt->iananame) {
			sofia_clear_flag_locked(tech_pvt, TFLAG_LATE_NEGOTIATION);

			if (is_proxy) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Disabling proxy mode due to call answer with no bridge\n");
				switch_channel_clear_flag(channel, CF_PROXY_MEDIA);
				switch_channel_clear_flag(channel, CF_PROXY_MODE);
			}

			if (!switch_channel_test_flag(tech_pvt->channel, CF_OUTBOUND)) {
				const char *r_sdp = switch_channel_get_variable(channel, SWITCH_R_SDP_VARIABLE);
				tech_pvt->num_codecs = 0;
				sofia_glue_tech_prepare_codecs(tech_pvt);

				if (sofia_glue_tech_media(tech_pvt, r_sdp) != SWITCH_STATUS_SUCCESS) {
					switch_channel_set_variable(channel, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "CODEC NEGOTIATION ERROR");
					//switch_mutex_lock(tech_pvt->sofia_mutex);
					//nua_respond(tech_pvt->nh, SIP_488_NOT_ACCEPTABLE, TAG_END());
					//switch_mutex_unlock(tech_pvt->sofia_mutex);
					return SWITCH_STATUS_FALSE;
				}
			}
		}

		if ((status = sofia_glue_tech_choose_port(tech_pvt, 0)) != SWITCH_STATUS_SUCCESS) {
			switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
			return status;
		}
		
		sofia_glue_set_local_sdp(tech_pvt, NULL, 0, NULL, 0);
		if (sofia_glue_activate_rtp(tech_pvt, 0) != SWITCH_STATUS_SUCCESS) {
			switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
		}

		if (tech_pvt->nh) {
			if (tech_pvt->local_sdp_str) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Local SDP %s:\n%s\n", switch_channel_get_name(channel), tech_pvt->local_sdp_str);
			}
		}
	}

	if ((val = switch_channel_get_variable(channel, SOFIA_SESSION_TIMEOUT))) {
		int v_session_timeout = atoi(val);
		if (v_session_timeout >= 0) {
			session_timeout = v_session_timeout;
		}
	}

	if (sofia_test_flag(tech_pvt, TFLAG_NAT) ||
		(val = switch_channel_get_variable(channel, "sip-force-contact")) ||
		((val = switch_channel_get_variable(channel, "sip_sticky_contact")) && switch_true(val))) {
		sticky = tech_pvt->record_route;
		session_timeout = SOFIA_NAT_SESSION_TIMEOUT;
		switch_channel_set_variable(channel, "sip_nat_detected", "true");
	}

	if (!sofia_test_flag(tech_pvt, TFLAG_BYE)) {
		char *extra_headers = sofia_glue_get_extra_headers(channel, SOFIA_SIP_PROGRESS_HEADER_PREFIX);
		nua_respond(tech_pvt->nh, SIP_200_OK,
					NUTAG_AUTOANSWER(0),
					TAG_IF(sticky, NUTAG_PROXY(tech_pvt->record_route)),
					SIPTAG_HEADER_STR(generate_pai_str(session)),
					NUTAG_SESSION_TIMER(session_timeout),
					SIPTAG_CONTACT_STR(tech_pvt->reply_contact),
					SIPTAG_CALL_INFO_STR(switch_channel_get_variable(tech_pvt->channel, SOFIA_SIP_HEADER_PREFIX "call_info")),
					SOATAG_USER_SDP_STR(tech_pvt->local_sdp_str),
					SOATAG_REUSE_REJECTED(1), SOATAG_ORDERED_USER(1), SOATAG_AUDIO_AUX("cn telephone-event"), NUTAG_INCLUDE_EXTRA_SDP(1), 
					TAG_IF(!switch_strlen_zero(extra_headers), SIPTAG_HEADER_STR(extra_headers)),
					SIPTAG_HEADER_STR("X-Actually-Support: "SOFIA_ACTUALLY_SUPPORT),
					TAG_END());
		switch_safe_free(extra_headers);
		sofia_set_flag_locked(tech_pvt, TFLAG_ANS);
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t sofia_read_video_frame(switch_core_session_t *session, switch_frame_t **frame, switch_io_flag_t flags, int stream_id)
{
	private_object_t *tech_pvt = (private_object_t *) switch_core_session_get_private(session);
	switch_channel_t *channel = switch_core_session_get_channel(session);
	int payload = 0;

	switch_assert(tech_pvt != NULL);

	if (sofia_test_flag(tech_pvt, TFLAG_HUP)) {
		return SWITCH_STATUS_FALSE;
	}

	while (!(tech_pvt->video_read_codec.implementation && switch_rtp_ready(tech_pvt->video_rtp_session))) {
		if (switch_channel_ready(channel)) {
			switch_yield(10000);
		} else {
			return SWITCH_STATUS_GENERR;
		}
	}

	tech_pvt->video_read_frame.datalen = 0;

	if (sofia_test_flag(tech_pvt, TFLAG_IO)) {
		switch_status_t status;

		if (!sofia_test_flag(tech_pvt, TFLAG_RTP)) {
			return SWITCH_STATUS_GENERR;
		}

		switch_assert(tech_pvt->rtp_session != NULL);
		tech_pvt->video_read_frame.datalen = 0;

		while (sofia_test_flag(tech_pvt, TFLAG_IO) && tech_pvt->video_read_frame.datalen == 0) {
			tech_pvt->video_read_frame.flags = SFF_NONE;

			status = switch_rtp_zerocopy_read_frame(tech_pvt->video_rtp_session, &tech_pvt->video_read_frame, flags);
			if (status != SWITCH_STATUS_SUCCESS && status != SWITCH_STATUS_BREAK) {
				if (status == SWITCH_STATUS_TIMEOUT) {
					if (sofia_test_flag(tech_pvt, TFLAG_SIP_HOLD)) {
						sofia_glue_toggle_hold(tech_pvt, 0);
						sofia_clear_flag_locked(tech_pvt, TFLAG_SIP_HOLD);
					}
					switch_channel_hangup(tech_pvt->channel, SWITCH_CAUSE_MEDIA_TIMEOUT);
				}
				return status;
			}

			payload = tech_pvt->video_read_frame.payload;

			if (tech_pvt->video_read_frame.datalen > 0) {
				break;
			}
		}
	}

	if (tech_pvt->video_read_frame.datalen == 0) {
		*frame = NULL;
		return SWITCH_STATUS_GENERR;
	}

	*frame = &tech_pvt->video_read_frame;

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t sofia_write_video_frame(switch_core_session_t *session, switch_frame_t *frame, switch_io_flag_t flags, int stream_id)
{
	private_object_t *tech_pvt = (private_object_t *) switch_core_session_get_private(session);
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	switch_assert(tech_pvt != NULL);

	while (!(tech_pvt->video_read_codec.implementation && switch_rtp_ready(tech_pvt->video_rtp_session))) {
		if (switch_channel_ready(channel)) {
			switch_yield(10000);
		} else {
			return SWITCH_STATUS_GENERR;
		}
	}

	if (sofia_test_flag(tech_pvt, TFLAG_HUP)) {
		return SWITCH_STATUS_FALSE;
	}

	if (!sofia_test_flag(tech_pvt, TFLAG_RTP)) {
		return SWITCH_STATUS_GENERR;
	}

	if (!sofia_test_flag(tech_pvt, TFLAG_IO)) {
		return SWITCH_STATUS_SUCCESS;
	}

	if (!switch_test_flag(frame, SFF_CNG)) {
		switch_rtp_write_frame(tech_pvt->video_rtp_session, frame);
	}

	return status;
}

static switch_status_t sofia_read_frame(switch_core_session_t *session, switch_frame_t **frame, switch_io_flag_t flags, int stream_id)
{
	private_object_t *tech_pvt = switch_core_session_get_private(session);
	switch_channel_t *channel = switch_core_session_get_channel(session);
	int payload = 0;
	uint32_t sanity = 1000;

	switch_assert(tech_pvt != NULL);

	if (!sofia_test_pflag(tech_pvt->profile, PFLAG_RUNNING)) {
		switch_channel_hangup(tech_pvt->channel, SWITCH_CAUSE_NORMAL_CLEARING);
		return SWITCH_STATUS_FALSE;
	}

	if (sofia_test_flag(tech_pvt, TFLAG_HUP)) {
		return SWITCH_STATUS_FALSE;
	}

	while (!(tech_pvt->read_codec.implementation && switch_rtp_ready(tech_pvt->rtp_session) && !switch_channel_test_flag(channel, CF_REQ_MEDIA))) {
		if (--sanity && switch_channel_ready(channel)) {
			switch_yield(10000);
		} else {
			switch_channel_hangup(tech_pvt->channel, SWITCH_CAUSE_RECOVERY_ON_TIMER_EXPIRE);
			return SWITCH_STATUS_GENERR;
		}
	}

	tech_pvt->read_frame.datalen = 0;
	sofia_set_flag_locked(tech_pvt, TFLAG_READING);

	if (sofia_test_flag(tech_pvt, TFLAG_HUP) || sofia_test_flag(tech_pvt, TFLAG_BYE) || !tech_pvt->read_codec.implementation || 
		!switch_core_codec_ready(&tech_pvt->read_codec)) {
		return SWITCH_STATUS_FALSE;
	}

	if (sofia_test_flag(tech_pvt, TFLAG_IO)) {
		switch_status_t status;

		if (!sofia_test_flag(tech_pvt, TFLAG_RTP)) {
			return SWITCH_STATUS_GENERR;
		}

		switch_assert(tech_pvt->rtp_session != NULL);
		tech_pvt->read_frame.datalen = 0;
		
		while (sofia_test_flag(tech_pvt, TFLAG_IO) && tech_pvt->read_frame.datalen == 0) {
			tech_pvt->read_frame.flags = SFF_NONE;

			status = switch_rtp_zerocopy_read_frame(tech_pvt->rtp_session, &tech_pvt->read_frame, flags);

			if (status != SWITCH_STATUS_SUCCESS && status != SWITCH_STATUS_BREAK) {
				if (status == SWITCH_STATUS_TIMEOUT) {
					
					if (sofia_test_flag(tech_pvt, TFLAG_SIP_HOLD)) {
						sofia_glue_toggle_hold(tech_pvt, 0);
						sofia_clear_flag_locked(tech_pvt, TFLAG_SIP_HOLD);
					}
					
					switch_channel_hangup(tech_pvt->channel, SWITCH_CAUSE_MEDIA_TIMEOUT);
				}
				return status;
			}

			/* Fast PASS! */
			if (switch_test_flag((&tech_pvt->read_frame), SFF_PROXY_PACKET)) {
				sofia_clear_flag_locked(tech_pvt, TFLAG_READING);
				*frame = &tech_pvt->read_frame;
				return SWITCH_STATUS_SUCCESS;
			}

			payload = tech_pvt->read_frame.payload;

			if (switch_rtp_has_dtmf(tech_pvt->rtp_session)) {
				switch_dtmf_t dtmf = { 0 };
				switch_rtp_dequeue_dtmf(tech_pvt->rtp_session, &dtmf);
				switch_channel_queue_dtmf(channel, &dtmf);
			}

			if (tech_pvt->read_frame.datalen > 0) {
				size_t bytes = 0;
				int frames = 1;
				
				if (!switch_test_flag((&tech_pvt->read_frame), SFF_CNG)) {
					if (!tech_pvt->read_codec.implementation || !switch_core_codec_ready(&tech_pvt->read_codec)) {
						*frame = NULL;
						return SWITCH_STATUS_GENERR;
					}
					
					if ((tech_pvt->read_frame.datalen % 10) == 0 &&
						sofia_test_pflag(tech_pvt->profile, PFLAG_AUTOFIX_TIMING) && tech_pvt->check_frames++ < MAX_CODEC_CHECK_FRAMES) {
						if (!tech_pvt->read_impl.encoded_bytes_per_packet) {
							tech_pvt->check_frames = MAX_CODEC_CHECK_FRAMES;
							goto skip;
						}
						
						if (tech_pvt->last_ts && tech_pvt->read_frame.datalen != tech_pvt->read_impl.encoded_bytes_per_packet) {
							switch_size_t codec_ms = (int)(tech_pvt->read_frame.timestamp - 
														   tech_pvt->last_ts) / (tech_pvt->read_impl.samples_per_second / 1000);
							if ((codec_ms % 10) != 0) {
								tech_pvt->check_frames = MAX_CODEC_CHECK_FRAMES;
								goto skip;
							}
							
							if (tech_pvt->last_codec_ms && tech_pvt->last_codec_ms == codec_ms) {
								tech_pvt->mismatch_count++;
							}

							tech_pvt->last_codec_ms = codec_ms;

							if (tech_pvt->mismatch_count > MAX_MISMATCH_FRAMES) {
								if (switch_rtp_ready(tech_pvt->rtp_session) && codec_ms != tech_pvt->codec_ms) {
									const char *val;
									int rtp_timeout_sec = 0;
									int rtp_hold_timeout_sec = 0;
								
									if (codec_ms > 120) { /* yeah right */
										switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, 
														  "Your phone is trying to send timestamps that suggest an increment of %dms per packet\n"
														  "That seems hard to believe so I am going to go on ahead and um ignore that, mmkay?", (int)codec_ms);
										tech_pvt->check_frames = MAX_CODEC_CHECK_FRAMES;
										goto skip;
									}

									tech_pvt->read_frame.datalen = 0;
									switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, 
													  "We were told to use ptime %d but what they meant to say was %d\n"
													  "This issue has so far been identified to happen on the following broken platforms/devices:\n" 
													  "Linksys/Sipura aka Cisco\n"
													  "ShoreTel\n"
													  "Sonus/L3\n"
													  "We will try to fix it but some of the devices on this list are so broken who knows what will happen..\n"
													  , 
													  (int)tech_pvt->codec_ms, (int)codec_ms);
									tech_pvt->codec_ms = codec_ms;
									switch_core_session_lock_codec_write(session);
									switch_core_session_lock_codec_read(session);

									switch_core_codec_destroy(&tech_pvt->read_codec);									
									switch_core_codec_destroy(&tech_pvt->write_codec);
									
									if (sofia_glue_tech_set_codec(tech_pvt, 2) != SWITCH_STATUS_SUCCESS) {
										*frame = NULL;
										switch_core_session_unlock_codec_write(session);
										switch_core_session_unlock_codec_read(session);
										return SWITCH_STATUS_GENERR;
									}
									
									if ((val = switch_channel_get_variable(tech_pvt->channel, "rtp_timeout_sec"))) {
										int v = atoi(val);
										if (v >= 0) {
											rtp_timeout_sec = v;
										}
									}
									
									if ((val = switch_channel_get_variable(tech_pvt->channel, "rtp_hold_timeout_sec"))) {
										int v = atoi(val);
										if (v >= 0) {
											rtp_hold_timeout_sec = v;
										}
									}
									
									if (rtp_timeout_sec) {
										tech_pvt->max_missed_packets = (tech_pvt->read_impl.samples_per_second * rtp_timeout_sec) /
											tech_pvt->read_impl.samples_per_packet;
										
										switch_rtp_set_max_missed_packets(tech_pvt->rtp_session, tech_pvt->max_missed_packets);
										if (!rtp_hold_timeout_sec) {
											rtp_hold_timeout_sec = rtp_timeout_sec * 10;
										}
									}
									
									if (rtp_hold_timeout_sec) {
										tech_pvt->max_missed_hold_packets = (tech_pvt->read_impl.samples_per_second * rtp_hold_timeout_sec) /
											tech_pvt->read_impl.samples_per_packet;
									}
									
									if (switch_rtp_change_interval(tech_pvt->rtp_session, 
																   tech_pvt->codec_ms * 1000,
																   tech_pvt->read_impl.samples_per_packet
																   ) != SWITCH_STATUS_SUCCESS) {
										switch_channel_hangup(tech_pvt->channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
										
									}

									tech_pvt->check_frames = MAX_CODEC_CHECK_FRAMES;
									/* inform them of the codec they are actually sending */
									sofia_glue_do_invite(session);
									switch_core_session_unlock_codec_write(session);
									switch_core_session_unlock_codec_read(session);

								}
							
							}
							
						} else {
							tech_pvt->mismatch_count = 0;
						}
						tech_pvt->last_ts = tech_pvt->read_frame.timestamp;
					} else {
						tech_pvt->mismatch_count = 0;
						tech_pvt->last_ts = 0;
					}
				skip:
					
					if ((bytes = tech_pvt->read_impl.encoded_bytes_per_packet)) {
						frames = (tech_pvt->read_frame.datalen / bytes);
					}
					tech_pvt->read_frame.samples = (int) (frames * tech_pvt->read_impl.samples_per_packet);

					if (tech_pvt->read_frame.datalen == 0) {
						continue;
					}
				}
				break;
			}
		}
	}

	sofia_clear_flag_locked(tech_pvt, TFLAG_READING);

	if (tech_pvt->read_frame.datalen == 0) {
		*frame = NULL;
		return SWITCH_STATUS_GENERR;
	}

	*frame = &tech_pvt->read_frame;

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t sofia_write_frame(switch_core_session_t *session, switch_frame_t *frame, switch_io_flag_t flags, int stream_id)
{
	private_object_t *tech_pvt = switch_core_session_get_private(session);
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	int bytes = 0, samples = 0, frames = 0;

	switch_assert(tech_pvt != NULL);

	while (!(tech_pvt->read_codec.implementation && switch_rtp_ready(tech_pvt->rtp_session) && !switch_channel_test_flag(channel, CF_REQ_MEDIA))) {
		if (switch_channel_ready(channel)) {
			switch_yield(10000);
		} else {
			return SWITCH_STATUS_GENERR;
		}
	}

	if (!tech_pvt->read_codec.implementation || !switch_core_codec_ready(&tech_pvt->read_codec)) {
		return SWITCH_STATUS_GENERR;
	}

	if (sofia_test_flag(tech_pvt, TFLAG_HUP)) {
		return SWITCH_STATUS_FALSE;
	}

	if (!sofia_test_flag(tech_pvt, TFLAG_RTP)) {
		return SWITCH_STATUS_GENERR;
	}

	if (!sofia_test_flag(tech_pvt, TFLAG_IO)) {
		return SWITCH_STATUS_SUCCESS;
	}

	if (sofia_test_flag(tech_pvt, TFLAG_BYE) || !tech_pvt->read_codec.implementation || !switch_core_codec_ready(&tech_pvt->read_codec)) {
		return SWITCH_STATUS_FALSE;
	}

	sofia_set_flag_locked(tech_pvt, TFLAG_WRITING);

	if (!switch_test_flag(frame, SFF_CNG) && !switch_test_flag(frame, SFF_PROXY_PACKET)) {
		if (tech_pvt->read_impl.encoded_bytes_per_packet) {
			bytes = tech_pvt->read_impl.encoded_bytes_per_packet;
			frames = ((int) frame->datalen / bytes);
		} else
			frames = 1;

		samples = frames * tech_pvt->read_impl.samples_per_packet;
	}

	tech_pvt->timestamp_send += samples;
	switch_rtp_write_frame(tech_pvt->rtp_session, frame);

	sofia_clear_flag_locked(tech_pvt, TFLAG_WRITING);
	return status;
}

static switch_status_t sofia_kill_channel(switch_core_session_t *session, int sig)
{
	private_object_t *tech_pvt = switch_core_session_get_private(session);

	if (!tech_pvt) {
		return SWITCH_STATUS_FALSE;
	}

	switch (sig) {
	case SWITCH_SIG_BREAK:
		if (switch_rtp_ready(tech_pvt->rtp_session)) {
			switch_rtp_break(tech_pvt->rtp_session);
		}
		if (switch_rtp_ready(tech_pvt->video_rtp_session)) {
			switch_rtp_break(tech_pvt->video_rtp_session);
		}
		break;
	case SWITCH_SIG_KILL:
	default:
		sofia_clear_flag_locked(tech_pvt, TFLAG_IO);
		sofia_set_flag_locked(tech_pvt, TFLAG_HUP);

		if (switch_rtp_ready(tech_pvt->rtp_session)) {
			switch_rtp_kill_socket(tech_pvt->rtp_session);
		}
		if (switch_rtp_ready(tech_pvt->video_rtp_session)) {
			switch_rtp_kill_socket(tech_pvt->video_rtp_session);
		}
		break;
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t sofia_send_dtmf(switch_core_session_t *session, const switch_dtmf_t *dtmf)
{
	private_object_t *tech_pvt;
	char message[128] = "";
	sofia_dtmf_t dtmf_type;

	tech_pvt = (private_object_t *) switch_core_session_get_private(session);
	switch_assert(tech_pvt != NULL);

	dtmf_type = tech_pvt->dtmf_type;

	/* We only can send INFO when we have no media */
	if (!tech_pvt->rtp_session || !switch_channel_media_ready(tech_pvt->channel) || switch_channel_test_flag(tech_pvt->channel, CF_PROXY_MODE)) {
		dtmf_type = DTMF_INFO;
	}

	switch (dtmf_type) {
	case DTMF_2833:
		{
			return switch_rtp_queue_rfc2833(tech_pvt->rtp_session, dtmf);
		}
	case DTMF_INFO:
		{
			snprintf(message, sizeof(message), "Signal=%c\r\nDuration=%d\r\n", dtmf->digit, dtmf->duration / 8);
			switch_mutex_lock(tech_pvt->sofia_mutex);
			nua_info(tech_pvt->nh, SIPTAG_CONTENT_TYPE_STR("application/dtmf-relay"), SIPTAG_PAYLOAD_STR(message), TAG_END());
			switch_mutex_unlock(tech_pvt->sofia_mutex);
		}
		break;
	case DTMF_NONE:
		break;
	default:
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Unhandled DTMF type!\n");
		break;
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t sofia_receive_message(switch_core_session_t *session, switch_core_session_message_t *msg)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	private_object_t *tech_pvt = switch_core_session_get_private(session);
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	char* extra_headers;

	if (switch_channel_down(channel) || !tech_pvt || sofia_test_flag(tech_pvt, TFLAG_BYE)) {
		status = SWITCH_STATUS_FALSE;
		goto end;
	}

	/* ones that do not need to lock sofia mutex */
	switch (msg->message_id) {
	case SWITCH_MESSAGE_INDICATE_TRANSCODING_NECESSARY:
		if (tech_pvt->rtp_session && switch_rtp_test_flag(tech_pvt->rtp_session, SWITCH_RTP_FLAG_PASS_RFC2833)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Pass 2833 mode may not work on a transcoded call.\n");
		}
		goto end;

	case SWITCH_MESSAGE_INDICATE_BRIDGE:
		if (switch_rtp_ready(tech_pvt->rtp_session)) {
			const char *val;
			int ok = 0;

			if (sofia_test_flag(tech_pvt, TFLAG_PASS_RFC2833) && switch_channel_test_flag_partner(channel, CF_FS_RTP)) {
				switch_rtp_set_flag(tech_pvt->rtp_session, SWITCH_RTP_FLAG_PASS_RFC2833);
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s activate passthru 2833 mode.\n", switch_channel_get_name(channel));
			}

			if ((val = switch_channel_get_variable(channel, "rtp_autoflush_during_bridge"))) {
				ok = switch_true(val);
			} else {
				ok = sofia_test_pflag(tech_pvt->profile, PFLAG_RTP_AUTOFLUSH_DURING_BRIDGE);
			}
			
			if (ok) {
				rtp_flush_read_buffer(tech_pvt->rtp_session, SWITCH_RTP_FLUSH_STICK);
			} else {
				rtp_flush_read_buffer(tech_pvt->rtp_session, SWITCH_RTP_FLUSH_ONCE);
			}
		}
		goto end;
	case SWITCH_MESSAGE_INDICATE_UNBRIDGE:
		if (switch_rtp_ready(tech_pvt->rtp_session)) {
			const char *val;
			int ok = 0;

			if (switch_rtp_test_flag(tech_pvt->rtp_session, SWITCH_RTP_FLAG_PASS_RFC2833)) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s deactivate passthru 2833 mode.\n", switch_channel_get_name(channel));
				switch_rtp_clear_flag(tech_pvt->rtp_session, SWITCH_RTP_FLAG_PASS_RFC2833);
			}

			if ((val = switch_channel_get_variable(channel, "rtp_autoflush_during_bridge"))) {
				ok = switch_true(val);
			} else {
				ok = sofia_test_pflag(tech_pvt->profile, PFLAG_RTP_AUTOFLUSH_DURING_BRIDGE);
			}
			
			if (ok) {
				rtp_flush_read_buffer(tech_pvt->rtp_session, SWITCH_RTP_FLUSH_UNSTICK);
			} else {
				rtp_flush_read_buffer(tech_pvt->rtp_session, SWITCH_RTP_FLUSH_ONCE);
			}

		}
		goto end;
	case SWITCH_MESSAGE_INDICATE_AUDIO_SYNC:
		if (switch_rtp_ready(tech_pvt->rtp_session)) {
			rtp_flush_read_buffer(tech_pvt->rtp_session, SWITCH_RTP_FLUSH_ONCE);
		}
		goto end;

	case SWITCH_MESSAGE_INDICATE_ANSWER:
	case SWITCH_MESSAGE_INDICATE_PROGRESS:
		{
			const char *var;
			if ((var = switch_channel_get_variable(channel, SOFIA_SECURE_MEDIA_VARIABLE)) && switch_true(var)) {
				sofia_set_flag_locked(tech_pvt, TFLAG_SECURE);
			}
		}
		break;
	default:
		break;
	}

	/* ones that do need to lock sofia mutex */
	switch_mutex_lock(tech_pvt->sofia_mutex);

	if (switch_channel_down(channel) || !tech_pvt || sofia_test_flag(tech_pvt, TFLAG_BYE)) {
		status = SWITCH_STATUS_FALSE;
		goto end_lock;
	}

	switch (msg->message_id) {
	case SWITCH_MESSAGE_INDICATE_VIDEO_REFRESH_REQ:
		{
			const char *pl =
				"<?xml version=\"1.0\" encoding=\"utf-8\" ?>\r\n"
				" <media_control>\r\n"
				"  <vc_primitive>\r\n"
				"   <to_encoder>\r\n"
				"    <picture_fast_update>\r\n" 
				"    </picture_fast_update>\r\n" 
				"   </to_encoder>\r\n" 
				"  </vc_primitive>\r\n" 
				" </media_control>\r\n";

			nua_info(tech_pvt->nh, SIPTAG_CONTENT_TYPE_STR("application/media_control+xml"), SIPTAG_PAYLOAD_STR(pl), TAG_END());

		}
		break;
	case SWITCH_MESSAGE_INDICATE_BROADCAST:
		{
			const char *ip = NULL, *port = NULL;
			ip = switch_channel_get_variable(channel, SWITCH_REMOTE_MEDIA_IP_VARIABLE);
			port = switch_channel_get_variable(channel, SWITCH_REMOTE_MEDIA_PORT_VARIABLE);
			if (ip && port) {
				sofia_glue_set_local_sdp(tech_pvt, ip, atoi(port), msg->string_arg, 1);
			}

			if (!sofia_test_flag(tech_pvt, TFLAG_BYE)) {
				nua_respond(tech_pvt->nh, SIP_200_OK,
							SIPTAG_CONTACT_STR(tech_pvt->reply_contact),
							SOATAG_USER_SDP_STR(tech_pvt->local_sdp_str),
							SOATAG_REUSE_REJECTED(1), SOATAG_ORDERED_USER(1), SOATAG_AUDIO_AUX("cn telephone-event"), NUTAG_INCLUDE_EXTRA_SDP(1), TAG_END());
				switch_channel_mark_answered(channel);
			}
		}
		break;
	case SWITCH_MESSAGE_INDICATE_NOMEDIA:
		{
			const char *uuid;
			switch_core_session_t *other_session;
			switch_channel_t *other_channel;
			const char *ip = NULL, *port = NULL;

			switch_channel_set_flag(channel, CF_PROXY_MODE);
			sofia_glue_tech_set_local_sdp(tech_pvt, NULL, SWITCH_FALSE);

			if ((uuid = switch_channel_get_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE))
				&& (other_session = switch_core_session_locate(uuid))) {
				other_channel = switch_core_session_get_channel(other_session);
				ip = switch_channel_get_variable(other_channel, SWITCH_REMOTE_MEDIA_IP_VARIABLE);
				port = switch_channel_get_variable(other_channel, SWITCH_REMOTE_MEDIA_PORT_VARIABLE);
				switch_core_session_rwunlock(other_session);
				if (ip && port) {
					sofia_glue_set_local_sdp(tech_pvt, ip, atoi(port), NULL, 1);
				}
			}


			if (!tech_pvt->local_sdp_str) {
				sofia_glue_tech_absorb_sdp(tech_pvt);
			}
			sofia_glue_do_invite(session);
		}
		break;

	case SWITCH_MESSAGE_INDICATE_MEDIA_REDIRECT:
		{
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s Sending media re-direct:\n%s\n", 
							  switch_channel_get_name(channel), msg->string_arg);
			sofia_glue_tech_set_local_sdp(tech_pvt, msg->string_arg, SWITCH_TRUE);

			sofia_set_flag_locked(tech_pvt, TFLAG_SENT_UPDATE);
			sofia_glue_do_invite(session);
		}
		break;


	case SWITCH_MESSAGE_INDICATE_REQUEST_IMAGE_MEDIA:
		{
			switch_t38_options_t *t38_options = (switch_t38_options_t *) msg->pointer_arg;

			sofia_glue_set_image_sdp(tech_pvt, t38_options);

			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s Sending request for image media. %s\n", 
							  switch_channel_get_name(channel), tech_pvt->local_sdp_str);
							  
			
			sofia_set_flag_locked(tech_pvt, TFLAG_SENT_UPDATE);
			sofia_glue_do_invite(session);
		}
		break;

	case SWITCH_MESSAGE_INDICATE_MEDIA:
		{
			uint32_t send_invite = 1;

			switch_channel_clear_flag(channel, CF_PROXY_MODE);
			sofia_glue_tech_set_local_sdp(tech_pvt, NULL, SWITCH_FALSE);

			if (!switch_channel_media_ready(channel)) {
				if (!switch_channel_test_flag(tech_pvt->channel, CF_OUTBOUND)) {
					const char *r_sdp = switch_channel_get_variable(channel, SWITCH_R_SDP_VARIABLE);

					tech_pvt->num_codecs = 0;
					sofia_glue_tech_prepare_codecs(tech_pvt);
					if (sofia_glue_tech_media(tech_pvt, r_sdp) != SWITCH_STATUS_SUCCESS) {
						switch_channel_set_variable(channel, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "CODEC NEGOTIATION ERROR");
						status = SWITCH_STATUS_FALSE;
						goto end_lock;
					}
					send_invite = 0;
				}
			}

			if (!switch_rtp_ready(tech_pvt->rtp_session)) {
				sofia_glue_tech_prepare_codecs(tech_pvt);
				if ((status = sofia_glue_tech_choose_port(tech_pvt, 0)) != SWITCH_STATUS_SUCCESS) {
					switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
					goto end_lock;
				}
			}
			sofia_glue_set_local_sdp(tech_pvt, NULL, 0, NULL, 1);

			if (send_invite) {
				switch_channel_set_flag(channel, CF_REQ_MEDIA);
				sofia_glue_do_invite(session);
			}
		}
		break;

	case SWITCH_MESSAGE_INDICATE_DISPLAY:
		{
			const char *name = msg->string_array_arg[0], *number = msg->string_array_arg[1];
			char *arg = NULL;
			char *argv[2] = { 0 };
			int argc;
			
			if (switch_strlen_zero(name) && !switch_strlen_zero(msg->string_arg)) {
				arg = strdup(msg->string_arg);
				switch_assert(arg);

				argc = switch_separate_string(arg, '|', argv, (sizeof(argv) / sizeof(argv[0])));
				name = argv[0];
				number = argv[1];

			}


			if (!switch_strlen_zero(name)) {
				char message[256] = "";
				const char *ua = switch_channel_get_variable(tech_pvt->channel, "sip_user_agent");
				switch_event_t *event;

				if (switch_strlen_zero(number)) {
					number = tech_pvt->caller_profile->destination_number;
				}

				if (ua && switch_stristr("snom", ua)) {
					snprintf(message, sizeof(message), "From:\r\nTo: \"%s\" %s\r\n", name, number);
					nua_info(tech_pvt->nh, SIPTAG_CONTENT_TYPE_STR("message/sipfrag"),
							 TAG_IF(!switch_strlen_zero(tech_pvt->user_via), SIPTAG_VIA_STR(tech_pvt->user_via)),
							 SIPTAG_PAYLOAD_STR(message), TAG_END());
				} else if ((ua && (switch_stristr("polycom", ua) || switch_stristr("FreeSWITCH", ua))) || 
						   switch_stristr("UPDATE", tech_pvt->x_actually_support_remote)) {
					snprintf(message, sizeof(message), "P-Asserted-Identity: \"%s\" <%s>", name, number);
					nua_update(tech_pvt->nh,
							   TAG_IF(!switch_strlen_zero_buf(message), SIPTAG_HEADER_STR(message)),
							   TAG_IF(!switch_strlen_zero(tech_pvt->user_via), SIPTAG_VIA_STR(tech_pvt->user_via)),
							   TAG_END());
				}


				
				if (switch_event_create(&event, SWITCH_EVENT_CALL_UPDATE) == SWITCH_STATUS_SUCCESS) {
					const char *uuid = switch_channel_get_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE);
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Direction", "SEND");
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Callee-Name", name);
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Callee-Number", number);
					if (uuid) {
						switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Bridged-To", uuid);
					}
					switch_channel_event_set_data(channel, event);
					switch_event_fire(&event);
				}



			}

			switch_safe_free(arg);
		}
		break;

	case SWITCH_MESSAGE_INDICATE_HOLD:
		{
			sofia_set_flag_locked(tech_pvt, TFLAG_SIP_HOLD);
			sofia_glue_do_invite(session);
			if (!switch_strlen_zero(msg->string_arg)) {
				char message[256] = "";
				const char *ua = switch_channel_get_variable(tech_pvt->channel, "sip_user_agent");

				if (ua && switch_stristr("snom", ua)) {
					snprintf(message, sizeof(message), "From:\r\nTo: \"%s\" %s\r\n", msg->string_arg, tech_pvt->caller_profile->destination_number);
					nua_info(tech_pvt->nh, SIPTAG_CONTENT_TYPE_STR("message/sipfrag"),
							 TAG_IF(!switch_strlen_zero(tech_pvt->user_via), SIPTAG_VIA_STR(tech_pvt->user_via)),
							 SIPTAG_PAYLOAD_STR(message), TAG_END());
				} else if (ua && switch_stristr("polycom", ua)) {
					snprintf(message, sizeof(message), "P-Asserted-Identity: \"%s\" <%s>", msg->string_arg, tech_pvt->caller_profile->destination_number);
					nua_update(tech_pvt->nh,
							   TAG_IF(!switch_strlen_zero_buf(message), SIPTAG_HEADER_STR(message)),
							   TAG_IF(!switch_strlen_zero(tech_pvt->user_via), SIPTAG_VIA_STR(tech_pvt->user_via)),
							   TAG_END());
				}
			}
		}
		break;

	case SWITCH_MESSAGE_INDICATE_UNHOLD:
		{
			sofia_clear_flag_locked(tech_pvt, TFLAG_SIP_HOLD);
			sofia_glue_do_invite(session);
		}
		break;
	case SWITCH_MESSAGE_INDICATE_REDIRECT:
		if (!switch_strlen_zero(msg->string_arg)) {
			if (!switch_channel_test_flag(channel, CF_ANSWERED) && !sofia_test_flag(tech_pvt, TFLAG_BYE)) {
				char *dest = (char *) msg->string_arg;

				if (!strchr(msg->string_arg, '<') && !strchr(msg->string_arg, '>')) {
					dest = switch_core_session_sprintf(session, "\"unknown\" <%s>", msg->string_arg);
				}
				
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Redirecting to %s\n", dest);

				nua_respond(tech_pvt->nh, SIP_302_MOVED_TEMPORARILY, SIPTAG_CONTACT_STR(dest), TAG_END());
				sofia_set_flag_locked(tech_pvt, TFLAG_BYE);
			} else {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Too late for redirecting to %s, already answered\n", msg->string_arg);
			}
		}
		break;
	case SWITCH_MESSAGE_INDICATE_DEFLECT:
		{
			char ref_to[128] = "";
			const char *var;

			if (!strstr(msg->string_arg, "sip:")) {
				const char *format = strchr(tech_pvt->profile->sipip, ':') ? "sip:%s@[%s]" : "sip:%s@%s";
				switch_snprintf(ref_to, sizeof(ref_to), format, msg->string_arg, tech_pvt->profile->sipip);
			} else {
				switch_set_string(ref_to, msg->string_arg);
			}
			nua_refer(tech_pvt->nh, SIPTAG_REFER_TO_STR(ref_to), SIPTAG_REFERRED_BY_STR(tech_pvt->contact_url), TAG_END());
			switch_mutex_unlock(tech_pvt->sofia_mutex);
			sofia_wait_for_reply(tech_pvt, 9999, 300);
			switch_mutex_lock(tech_pvt->sofia_mutex);
			if ((var = switch_channel_get_variable(tech_pvt->channel, "sip_refer_reply"))) {
				msg->string_reply = switch_core_session_strdup(session, var); 
			} else {
				msg->string_reply = "no reply";
			}
		}
		break;

	case SWITCH_MESSAGE_INDICATE_RESPOND:
		if (msg->numeric_arg || msg->string_arg) {
			int code = msg->numeric_arg;
			const char *reason = NULL;
			
			if (code) {
				reason = msg->string_arg;
			} else {
				if (!switch_strlen_zero(msg->string_arg)) {
					if ((code = atoi(msg->string_arg))) {
						if ((reason = strchr(msg->string_arg, ' '))) {
							reason++;
						}
					}
				}
			}

			if (!code) {
				code = 488;
			}

			if (!switch_channel_test_flag(channel, CF_ANSWERED) && code >= 300) {
				if (sofia_test_flag(tech_pvt, TFLAG_BYE)) {
					goto end_lock;
				}
			}
			
			if (switch_strlen_zero(reason) && code != 407 && code != 302) {
				reason = sip_status_phrase(code);
				if (switch_strlen_zero(reason)) {
					reason = "Because";
				}
			}

			extra_headers = sofia_glue_get_extra_headers(channel, SOFIA_SIP_RESPONSE_HEADER_PREFIX);

			if (code == 407 && !msg->numeric_arg) {
				const char *to_uri = switch_channel_get_variable(channel, "sip_to_uri");
				const char *to_host = reason;

				if (switch_strlen_zero(to_host)) {
					to_host = switch_channel_get_variable(channel, "sip_to_host");
				}
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Challenging call %s\n", to_uri);
				sofia_reg_auth_challenge(NULL, tech_pvt->profile, tech_pvt->nh, REG_INVITE, to_host, 0);
				switch_channel_hangup(channel, SWITCH_CAUSE_USER_CHALLENGE);
			} else if (code == 484 && msg->numeric_arg) {
				const char *to = switch_channel_get_variable(channel, "sip_to_uri");
				const char *max_forwards = switch_channel_get_variable(channel, SWITCH_MAX_FORWARDS_VARIABLE);

				char *to_uri = NULL;

				if (to) {
					char *p;
					to_uri = switch_core_session_sprintf(session, "sip:%s", to);
					if ((p = strstr(to_uri, ":5060"))) {
						*p = '\0';
					}
				}

				if (!switch_channel_test_flag(channel, CF_ANSWERED) && !sofia_test_flag(tech_pvt, TFLAG_BYE)) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Overlap Dial with %d %s\n", code, reason);
					nua_respond(tech_pvt->nh, code, su_strdup(nua_handle_home(tech_pvt->nh), reason), TAG_IF(to_uri, SIPTAG_CONTACT_STR(to_uri)),
								SIPTAG_SUPPORTED_STR(NULL), SIPTAG_ACCEPT_STR(NULL),
								TAG_IF(!switch_strlen_zero(extra_headers), SIPTAG_HEADER_STR(extra_headers)),
								TAG_IF(!switch_strlen_zero(max_forwards), SIPTAG_MAX_FORWARDS_STR(max_forwards)), TAG_END());
					
					sofia_set_flag_locked(tech_pvt, TFLAG_BYE);
				}
			} else if (code == 302 && !switch_strlen_zero(msg->string_arg)) {
				char *p;

				if ((p = strchr(msg->string_arg, ' '))) {
					*p = '\0';
					msg->string_arg = p;
				}
				
				msg->message_id = SWITCH_MESSAGE_INDICATE_REDIRECT;
				switch_core_session_receive_message(session, msg);
				goto end_lock;
			} else {
				if (!sofia_test_flag(tech_pvt, TFLAG_BYE)) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Responding with %d [%s]\n", code, reason);
					if (!switch_strlen_zero(((char *) msg->pointer_arg))) {
						sofia_glue_tech_set_local_sdp(tech_pvt, (char *) msg->pointer_arg, SWITCH_TRUE);
						
						if (switch_channel_test_flag(channel, CF_PROXY_MEDIA)) {
							sofia_glue_tech_patch_sdp(tech_pvt);
							sofia_glue_tech_proxy_remote_addr(tech_pvt);
						}
						nua_respond(tech_pvt->nh, code, su_strdup(nua_handle_home(tech_pvt->nh), reason), SIPTAG_CONTACT_STR(tech_pvt->reply_contact),
									SOATAG_USER_SDP_STR(tech_pvt->local_sdp_str),
									SOATAG_REUSE_REJECTED(1),
									SOATAG_ORDERED_USER(1), SOATAG_AUDIO_AUX("cn telephone-event"), NUTAG_INCLUDE_EXTRA_SDP(1), 
									TAG_IF(!switch_strlen_zero(extra_headers), SIPTAG_HEADER_STR(extra_headers)),
									TAG_END());
					} else {
						nua_respond(tech_pvt->nh, code, su_strdup(nua_handle_home(tech_pvt->nh), reason), SIPTAG_CONTACT_STR(tech_pvt->reply_contact), 
									TAG_IF(!switch_strlen_zero(extra_headers), SIPTAG_HEADER_STR(extra_headers)),
									TAG_END());
					}
				}
			}

		}
		break;
	case SWITCH_MESSAGE_INDICATE_RINGING:
		if (!switch_channel_test_flag(channel, CF_RING_READY) && !sofia_test_flag(tech_pvt, TFLAG_BYE) &&
			!switch_channel_test_flag(channel, CF_EARLY_MEDIA) && !switch_channel_test_flag(channel, CF_ANSWERED)) {
			char *extra_header = sofia_glue_get_extra_headers(channel, SOFIA_SIP_PROGRESS_HEADER_PREFIX);
			nua_respond(tech_pvt->nh, SIP_180_RINGING,
						SIPTAG_CONTACT_STR(tech_pvt->reply_contact),
						SIPTAG_HEADER_STR(generate_pai_str(session)), 
						TAG_IF(!switch_strlen_zero(extra_header), SIPTAG_HEADER_STR(extra_header)),
						SIPTAG_HEADER_STR("X-Actually-Support: "SOFIA_ACTUALLY_SUPPORT),
						TAG_END());
			switch_safe_free(extra_header);
			switch_channel_mark_ring_ready(channel);
		}
		break;
	case SWITCH_MESSAGE_INDICATE_ANSWER:
		status = sofia_answer_channel(session);
		break;
	case SWITCH_MESSAGE_INDICATE_PROGRESS:
		{
			char *sticky = NULL;
			const char *val = NULL;

			if (!sofia_test_flag(tech_pvt, TFLAG_ANS) && !sofia_test_flag(tech_pvt, TFLAG_EARLY_MEDIA)) {

				sofia_set_flag_locked(tech_pvt, TFLAG_EARLY_MEDIA);
				switch_log_printf(SWITCH_CHANNEL_ID_LOG, msg->_file, msg->_func, msg->_line, NULL, SWITCH_LOG_INFO, "Sending early media\n");

				/* Transmit 183 Progress with SDP */
				if (switch_channel_test_flag(channel, CF_PROXY_MODE) || switch_channel_test_flag(channel, CF_PROXY_MEDIA)) {
					const char *sdp = NULL;
					if ((sdp = switch_channel_get_variable(channel, SWITCH_B_SDP_VARIABLE))) {
						sofia_glue_tech_set_local_sdp(tech_pvt, sdp, SWITCH_TRUE);
					}
					if (switch_channel_test_flag(channel, CF_PROXY_MEDIA)) {
						
						sofia_glue_tech_patch_sdp(tech_pvt);
						
						if (sofia_glue_activate_rtp(tech_pvt, 0) != SWITCH_STATUS_SUCCESS) {
							status = SWITCH_STATUS_FALSE;
							goto end_lock;
						}
					}
				} else {
					if (sofia_test_flag(tech_pvt, TFLAG_LATE_NEGOTIATION) || !tech_pvt->iananame) {
						sofia_clear_flag_locked(tech_pvt, TFLAG_LATE_NEGOTIATION);
						if (!switch_channel_test_flag(tech_pvt->channel, CF_OUTBOUND)) {
							const char *r_sdp = switch_channel_get_variable(channel, SWITCH_R_SDP_VARIABLE);

							tech_pvt->num_codecs = 0;
							sofia_glue_tech_prepare_codecs(tech_pvt);
							if (sofia_glue_tech_media(tech_pvt, r_sdp) != SWITCH_STATUS_SUCCESS) {
								switch_channel_set_variable(channel, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "CODEC NEGOTIATION ERROR");
								//nua_respond(tech_pvt->nh, SIP_488_NOT_ACCEPTABLE, TAG_END());
								status = SWITCH_STATUS_FALSE;
								goto end_lock;
							}
						}
					}

					if ((status = sofia_glue_tech_choose_port(tech_pvt, 0)) != SWITCH_STATUS_SUCCESS) {
						switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
						goto end_lock;
					}
					sofia_glue_set_local_sdp(tech_pvt, NULL, 0, NULL, 0);
					if (sofia_glue_activate_rtp(tech_pvt, 0) != SWITCH_STATUS_SUCCESS) {
						switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
					}
					if (tech_pvt->local_sdp_str) {
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Ring SDP:\n%s\n", tech_pvt->local_sdp_str);
					}
				}
				switch_channel_mark_pre_answered(channel);


				if (sofia_test_flag(tech_pvt, TFLAG_NAT) ||
					(val = switch_channel_get_variable(channel, "sip-force-contact")) ||
					((val = switch_channel_get_variable(channel, "sip_sticky_contact")) && switch_true(val))) {
					sticky = tech_pvt->record_route;
					switch_channel_set_variable(channel, "sip_nat_detected", "true");
				}

				if (!sofia_test_flag(tech_pvt, TFLAG_BYE)) {
					char *extra_header = sofia_glue_get_extra_headers(channel, SOFIA_SIP_PROGRESS_HEADER_PREFIX);
					nua_respond(tech_pvt->nh,
								SIP_183_SESSION_PROGRESS,
								NUTAG_AUTOANSWER(0),
								TAG_IF(sticky, NUTAG_PROXY(tech_pvt->record_route)),
								SIPTAG_HEADER_STR(generate_pai_str(session)),
								SIPTAG_CONTACT_STR(tech_pvt->reply_contact),
								SOATAG_REUSE_REJECTED(1),
								SOATAG_ORDERED_USER(1),
								SOATAG_ADDRESS(tech_pvt->adv_sdp_audio_ip),
								SOATAG_USER_SDP_STR(tech_pvt->local_sdp_str), SOATAG_AUDIO_AUX("cn telephone-event"), 
								TAG_IF(!switch_strlen_zero(extra_header), SIPTAG_HEADER_STR(extra_header)),
								SIPTAG_HEADER_STR("X-Actually-Support: "SOFIA_ACTUALLY_SUPPORT),
								TAG_END());
					switch_safe_free(extra_header);
				}
			}
		}
		break;
	default:
		break;
	}

 end_lock:

	if (msg->message_id == SWITCH_MESSAGE_INDICATE_ANSWER || msg->message_id == SWITCH_MESSAGE_INDICATE_PROGRESS) {
		sofia_send_callee_id(session, NULL, NULL);
	}

	switch_mutex_unlock(tech_pvt->sofia_mutex);

  end:

	if (switch_channel_down(channel) || !tech_pvt || sofia_test_flag(tech_pvt, TFLAG_BYE)) {
		status = SWITCH_STATUS_FALSE;
	}

	return status;

}

static switch_status_t sofia_receive_event(switch_core_session_t *session, switch_event_t *event)
{
	struct private_object *tech_pvt = switch_core_session_get_private(session);
	char *body;
	nua_handle_t *msg_nh;

	switch_assert(tech_pvt != NULL);

	if (!(body = switch_event_get_body(event))) {
		body = "";
	}

	if (tech_pvt->hash_key) {
		switch_mutex_lock(tech_pvt->sofia_mutex);
		msg_nh = nua_handle(tech_pvt->profile->nua, NULL,
							SIPTAG_FROM_STR(tech_pvt->chat_from),
							NUTAG_URL(tech_pvt->chat_to), SIPTAG_TO_STR(tech_pvt->chat_to), SIPTAG_CONTACT_STR(tech_pvt->profile->url), TAG_END());
		nua_handle_bind(msg_nh, &mod_sofia_globals.destroy_private);
		nua_message(msg_nh, SIPTAG_CONTENT_TYPE_STR("text/html"), SIPTAG_PAYLOAD_STR(body), TAG_END());
		switch_mutex_unlock(tech_pvt->sofia_mutex);
	}

	return SWITCH_STATUS_SUCCESS;
}

typedef switch_status_t (*sofia_command_t) (char **argv, int argc, switch_stream_handle_t *stream);

static const char *sofia_state_names[] = { 
	"UNREGED",
	"TRYING",
	"REGISTER",
	"REGED",
	"UNREGISTER",
	"FAILED",
	"FAIL_WAIT",
	"EXPIRED",
	"NOREG",
	NULL
};

const char * sofia_state_string(int state) 
{
	return sofia_state_names[state];
}

struct cb_helper {
	sofia_profile_t *profile;
	switch_stream_handle_t *stream;
};



static int show_reg_callback(void *pArg, int argc, char **argv, char **columnNames)
{
	struct cb_helper *cb = (struct cb_helper *) pArg;
	char exp_buf[128] = "";
	switch_time_exp_t tm;

	if (argv[6]) {
		switch_time_t etime = atoi(argv[6]);
		switch_size_t retsize;

		switch_time_exp_lt(&tm, switch_time_from_sec(etime));
		switch_strftime_nocheck(exp_buf, &retsize, sizeof(exp_buf), "%Y-%m-%d %T", &tm);
	}

	cb->stream->write_function(cb->stream,
							   "Call-ID:    \t%s\n"
							   "User:       \t%s@%s\n"
							   "Contact:    \t%s\n"
							   "Agent:      \t%s\n"
							   "Status:     \t%s(%s) EXP(%s)\n"
							   "Host:       \t%s\n"
							   "IP:         \t%s\n"
							   "Port:       \t%s\n"
							   "Auth-User:  \t%s\n"
							   "Auth-Realm: \t%s\n"
							   "MWI-Account:\t%s@%s\n\n",
							   switch_str_nil(argv[0]), switch_str_nil(argv[1]), switch_str_nil(argv[2]), switch_str_nil(argv[3]),
							   switch_str_nil(argv[7]), switch_str_nil(argv[4]), switch_str_nil(argv[5]), exp_buf, switch_str_nil(argv[11]),
							   switch_str_nil(argv[12]), switch_str_nil(argv[13]), switch_str_nil(argv[14]), switch_str_nil(argv[15]),
							   switch_str_nil(argv[16]), switch_str_nil(argv[17]));
	return 0;
}

static int show_reg_callback_xml(void *pArg, int argc, char **argv, char **columnNames)
{
	struct cb_helper *cb = (struct cb_helper *) pArg;
	char exp_buf[128] = "";
	switch_time_exp_t tm;
	const int buflen = 2048;
	char xmlbuf[2048];
  
	if (argv[6]) {
		switch_time_t etime = atoi(argv[6]);
		switch_size_t retsize;

		switch_time_exp_lt(&tm, switch_time_from_sec(etime));
		switch_strftime_nocheck(exp_buf, &retsize, sizeof(exp_buf), "%Y-%m-%d %T", &tm);
	}

	cb->stream->write_function(cb->stream,
							   "    <registration>\n"
							   "        <call-id>%s</call-id>\n"
							   "        <user>%s@%s</user>\n"
							   "        <contact>%s</contact>\n"
							   "        <agent>%s</agent>\n"
							   "        <status>%s(%s) exp(%s)</status>\n"
							   "        <host>%s</host>\n"
							   "        <network-ip>%s</network-ip>\n"
							   "        <network-port>%s</network-port>\n"
							   "        <sip-auth-user>%s</sip-auth-user>\n"
							   "        <sip-auth-realm>%s</sip-auth-realm>\n"
							   "        <mwi-account>%s@%s</mwi-account>\n"
							   "    </registration>\n", 
							   switch_str_nil(argv[0]), switch_str_nil(argv[1]), switch_str_nil(argv[2]), 
							   switch_amp_encode(switch_str_nil(argv[3]),xmlbuf,buflen),
							   switch_str_nil(argv[7]), switch_str_nil(argv[4]), switch_str_nil(argv[5]), exp_buf, switch_str_nil(argv[11]),
							   switch_str_nil(argv[12]), switch_str_nil(argv[13]), switch_str_nil(argv[14]), switch_str_nil(argv[15]),
							   switch_str_nil(argv[16]), switch_str_nil(argv[17]));
	return 0;
}

static const char *status_names[] = { "DOWN", "UP", NULL };

static switch_status_t cmd_status(char **argv, int argc, switch_stream_handle_t *stream)
{
	sofia_profile_t *profile = NULL;
	sofia_gateway_t *gp;
	switch_hash_index_t *hi;
	void *val;
	const void *vvar;
	int c = 0;
	int ac = 0;
	const char *line = "=================================================================================================";

	if (argc > 0) {
		if (argc == 1) {
			stream->write_function(stream, "Invalid Syntax!\n");
			return SWITCH_STATUS_SUCCESS;
		}
		if (!strcasecmp(argv[0], "gateway")) {
			if ((gp = sofia_reg_find_gateway(argv[1]))) {
				switch_assert(gp->state < REG_STATE_LAST);

				stream->write_function(stream, "%s\n", line);
				stream->write_function(stream, "Name    \t%s\n", switch_str_nil(gp->name));
				stream->write_function(stream, "Profile \t%s\n", gp->profile->name);
				stream->write_function(stream, "Scheme  \t%s\n", switch_str_nil(gp->register_scheme));
				stream->write_function(stream, "Realm   \t%s\n", switch_str_nil(gp->register_realm));
				stream->write_function(stream, "Username\t%s\n", switch_str_nil(gp->register_username));
				stream->write_function(stream, "Password\t%s\n", switch_strlen_zero(gp->register_password) ? "no" : "yes");
				stream->write_function(stream, "From    \t%s\n", switch_str_nil(gp->register_from));
				stream->write_function(stream, "Contact \t%s\n", switch_str_nil(gp->register_contact));
				stream->write_function(stream, "Exten   \t%s\n", switch_str_nil(gp->extension));
				stream->write_function(stream, "To      \t%s\n", switch_str_nil(gp->register_to));
				stream->write_function(stream, "Proxy   \t%s\n", switch_str_nil(gp->register_proxy));
				stream->write_function(stream, "Context \t%s\n", switch_str_nil(gp->register_context));
				stream->write_function(stream, "Expires \t%s\n", switch_str_nil(gp->expires_str));
				stream->write_function(stream, "Freq    \t%d\n", gp->freq);
				stream->write_function(stream, "Ping    \t%d\n", gp->ping);
				stream->write_function(stream, "PingFreq\t%d\n", gp->ping_freq);
				stream->write_function(stream, "State   \t%s\n", sofia_state_names[gp->state]);
				stream->write_function(stream, "Status  \t%s%s\n", status_names[gp->status], gp->pinging ? " (ping)" : "");
				stream->write_function(stream, "CallsIN \t%d\n", gp->ib_calls);
				stream->write_function(stream, "CallsOUT\t%d\n", gp->ob_calls);
				stream->write_function(stream, "%s\n", line);
				sofia_reg_release_gateway(gp);
			} else {
				stream->write_function(stream, "Invalid Gateway!\n");
			}
		} else if (!strcasecmp(argv[0], "profile")) {
			struct cb_helper cb;
			char *sql = NULL;

			if ((argv[1]) && (profile = sofia_glue_find_profile(argv[1]))) {
				if (!argv[2] || (strcasecmp(argv[2], "reg") && strcasecmp(argv[2], "user"))) {
					stream->write_function(stream, "%s\n", line);
					stream->write_function(stream, "Name             \t%s\n", switch_str_nil(argv[1]));
					stream->write_function(stream, "Domain Name      \t%s\n", profile->domain_name ? profile->domain_name : "N/A");
					if (strcasecmp(argv[1], profile->name)) {
					stream->write_function(stream, "Alias Of         \t%s\n", switch_str_nil(profile->name));
					}
					stream->write_function(stream, "DBName           \t%s\n", switch_str_nil(profile->dbname));
					stream->write_function(stream, "Pres Hosts       \t%s\n", switch_str_nil(profile->presence_hosts));
					stream->write_function(stream, "Dialplan         \t%s\n", switch_str_nil(profile->dialplan));
					stream->write_function(stream, "Context          \t%s\n", switch_str_nil(profile->context));
					stream->write_function(stream, "Challenge Realm  \t%s\n", 
										   switch_strlen_zero(profile->challenge_realm) ? "auto_to" : profile->challenge_realm);
					stream->write_function(stream, "RTP-IP           \t%s\n", switch_str_nil(profile->rtpip));
					if (profile->extrtpip) {
					stream->write_function(stream, "Ext-RTP-IP       \t%s\n", profile->extrtpip);
					}

					stream->write_function(stream, "SIP-IP           \t%s\n", switch_str_nil(profile->sipip));
					if (profile->extsipip) {
					stream->write_function(stream, "Ext-SIP-IP       \t%s\n", profile->extsipip);
					}
					stream->write_function(stream, "URL              \t%s\n", switch_str_nil(profile->url));
					stream->write_function(stream, "BIND-URL         \t%s\n", switch_str_nil(profile->bindurl));
					if (sofia_test_pflag(profile, PFLAG_TLS)) {
					stream->write_function(stream, "TLS-URL          \t%s\n", switch_str_nil(profile->tls_url));
					stream->write_function(stream, "TLS-BIND-URL     \t%s\n", switch_str_nil(profile->tls_bindurl));
					}
					stream->write_function(stream, "HOLD-MUSIC       \t%s\n", switch_strlen_zero(profile->hold_music) ? "N/A" : profile->hold_music);
					stream->write_function(stream, "OUTBOUND-PROXY   \t%s\n", switch_strlen_zero(profile->outbound_proxy) ? "N/A" : profile->outbound_proxy);
					stream->write_function(stream, "CODECS           \t%s\n", switch_str_nil(profile->codec_string));
					stream->write_function(stream, "TEL-EVENT        \t%d\n", profile->te);
					if (profile->dtmf_type == DTMF_2833) {
					stream->write_function(stream, "DTMF-MODE        \trfc2833\n");
					} else if (profile->dtmf_type == DTMF_INFO) {
					stream->write_function(stream, "DTMF-MODE        \tinfo\n");
					} else {
					stream->write_function(stream, "DTMF-MODE        \tnone\n");
					}
					stream->write_function(stream, "CNG              \t%d\n", profile->cng_pt);
					stream->write_function(stream, "SESSION-TO       \t%d\n", profile->session_timeout);
					stream->write_function(stream, "MAX-DIALOG       \t%d\n", profile->max_proceeding);
					stream->write_function(stream, "NOMEDIA          \t%s\n", sofia_test_flag(profile, TFLAG_INB_NOMEDIA) ? "true" : "false");
					stream->write_function(stream, "LATE-NEG         \t%s\n", sofia_test_flag(profile, TFLAG_LATE_NEGOTIATION) ? "true" : "false");
					stream->write_function(stream, "PROXY-MEDIA      \t%s\n", sofia_test_flag(profile, TFLAG_PROXY_MEDIA) ? "true" : "false");
					stream->write_function(stream, "AGGRESSIVENAT    \t%s\n", sofia_test_pflag(profile, PFLAG_AGGRESSIVE_NAT_DETECTION) ? "true" : "false");
					stream->write_function(stream, "STUN-ENABLED     \t%s\n", sofia_test_pflag(profile, PFLAG_STUN_ENABLED) ? "true" : "false");
					stream->write_function(stream, "STUN-AUTO-DISABLE\t%s\n", sofia_test_pflag(profile, PFLAG_STUN_AUTO_DISABLE) ? "true" : "false");
					stream->write_function(stream, "CALLS-IN         \t%d\n", profile->ib_calls);
					stream->write_function(stream, "FAILED-CALLS-IN  \t%d\n", profile->ib_failed_calls);
					stream->write_function(stream, "CALLS-OUT        \t%d\n", profile->ob_calls);
					stream->write_function(stream, "FAILED-CALLS-OUT \t%d\n", profile->ob_failed_calls);
				}
				stream->write_function(stream, "\nRegistrations:\n%s\n", line);

				cb.profile = profile;
				cb.stream = stream;
				
				if (!sql && argv[2] && !strcasecmp(argv[2], "pres") && argv[3]) {
					sql = switch_mprintf("select call_id,sip_user,sip_host,contact,status,"
							"rpid,expires,user_agent,server_user,server_host,profile_name,hostname,"
							"network_ip,network_port,sip_username,sip_realm,mwi_user,mwi_host"
							" from sip_registrations where profile_name='%q' and presence_hosts like '%%%q%%'", 
							profile->name, argv[3]);
				}
				if (!sql && argv[2] && !strcasecmp(argv[2], "reg") && argv[3]) {
					sql = switch_mprintf("select call_id,sip_user,sip_host,contact,status,"
							"rpid,expires,user_agent,server_user,server_host,profile_name,hostname,"
							"network_ip,network_port,sip_username,sip_realm,mwi_user,mwi_host"
							" from sip_registrations where profile_name='%q' and contact like '%%%q%%'", 
							profile->name, argv[3]);
				}
				if (!sql && argv[2] && !strcasecmp(argv[2], "user") && argv[3]) {
					char *dup = strdup(argv[3]);
					char *host = NULL, *user = NULL;
					char *sqlextra = NULL;

					switch_assert(dup);

					if ((host = strchr(dup, '@'))) {
						*host++ = '\0';
						user = dup;
					} else {
						host = dup;
					}

					if (switch_strlen_zero(user) ) {
						sqlextra = switch_mprintf("(sip_host='%q')", host);
					} else if (switch_strlen_zero(host)) {
						sqlextra = switch_mprintf("(sip_user='%q')", user);
					} else {
						sqlextra = switch_mprintf("(sip_user='%q' and sip_host='%q')", user, host);
					}

					sql = switch_mprintf("select call_id,sip_user,sip_host,contact,status,"
							"rpid,expires,user_agent,server_user,server_host,profile_name,hostname,"
							"network_ip,network_port,sip_username,sip_realm,mwi_user,mwi_host"
							" from sip_registrations where profile_name='%q' and %s",
							profile->name, sqlextra);
					switch_safe_free(dup);
					switch_safe_free(sqlextra);
				}

				if (!sql) {
					sql = switch_mprintf("select call_id,sip_user,sip_host,contact,status,"
										 "rpid,expires,user_agent,server_user,server_host,profile_name,hostname,"
										 "network_ip,network_port,sip_username,sip_realm,mwi_user,mwi_host"
										 " from sip_registrations where profile_name='%q'", 
										 profile->name);
				}

				sofia_glue_execute_sql_callback(profile, SWITCH_FALSE, profile->ireg_mutex, sql, show_reg_callback, &cb);
				switch_safe_free(sql);

				stream->write_function(stream, "%s\n", line);

				sofia_glue_release_profile(profile);
			} else {
				stream->write_function(stream, "Invalid Profile!\n");
			}
		} else {
			stream->write_function(stream, "Invalid Syntax!\n");
		}

		return SWITCH_STATUS_SUCCESS;
	}

	stream->write_function(stream, "%25s\t%s\t  %32s\t%s\n", "Name", "   Type", "Data", "State");
	stream->write_function(stream, "%s\n", line);
	switch_mutex_lock(mod_sofia_globals.hash_mutex);
	for (hi = switch_hash_first(NULL, mod_sofia_globals.profile_hash); hi; hi = switch_hash_next(hi)) {
		switch_hash_this(hi, &vvar, NULL, &val);
		profile = (sofia_profile_t *) val;
		if (sofia_test_pflag(profile, PFLAG_RUNNING)) {

			if (strcmp(vvar, profile->name)) {
				ac++;
				stream->write_function(stream, "%25s\t%s\t  %32s\t%s\n", vvar, "  alias", profile->name, "ALIASED");
			} else {
				stream->write_function(stream, "%25s\t%s\t  %32s\t%s (%u)\n", profile->name, "profile", profile->url,
									   sofia_test_pflag(profile, PFLAG_RUNNING) ? "RUNNING" : "DOWN", profile->inuse);

				if (sofia_test_pflag(profile, PFLAG_TLS)) {
					stream->write_function(stream, "%25s\t%s\t  %32s\t%s (%u) (TLS)\n", profile->name, "profile", profile->tls_url,
										   sofia_test_pflag(profile, PFLAG_RUNNING) ? "RUNNING" : "DOWN", profile->inuse);
				}

				c++;

				for (gp = profile->gateways; gp; gp = gp->next) {
					switch_assert(gp->state < REG_STATE_LAST);
					stream->write_function(stream, "%25s\t%s\t  %32s\t%s", gp->name, "gateway", gp->register_to, sofia_state_names[gp->state]);
					if (gp->state == REG_STATE_FAILED || gp->state == REG_STATE_TRYING) {
						time_t now = switch_epoch_time_now(NULL);
						if (gp->retry > now) {
							stream->write_function(stream, " (retry: %ds)", gp->retry - now);
						} else {
							stream->write_function(stream, " (retry: NEVER)");
						}
					}
					stream->write_function(stream, "\n");
				}
			}
		}
	}
	switch_mutex_unlock(mod_sofia_globals.hash_mutex);
	stream->write_function(stream, "%s\n", line);
	stream->write_function(stream, "%d profile%s %d alias%s\n", c, c == 1 ? "" : "s", ac, ac == 1 ? "" : "es");
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t cmd_xml_status(char **argv, int argc, switch_stream_handle_t *stream)
{
	sofia_profile_t *profile = NULL;
	sofia_gateway_t *gp;
	switch_hash_index_t *hi;
	void *val;
	const void *vvar;
	const int buflen = 2096;
	char xmlbuf[2096];
	int c = 0;
	int ac = 0;
  const char *header = "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>";
	
  if (argc > 0) {
		if (argc == 1) {
			stream->write_function(stream, "Invalid Syntax!\n");
			return SWITCH_STATUS_SUCCESS;
		}
		if (!strcasecmp(argv[0], "gateway")) {
			if ((gp = sofia_reg_find_gateway(argv[1]))) {
				switch_assert(gp->state < REG_STATE_LAST);
				stream->write_function(stream, "%s\n", header);
				stream->write_function(stream, "  <gateway>\n");
				stream->write_function(stream, "    <name>%s</name>\n", switch_str_nil(gp->name));
				stream->write_function(stream, "    <profile>%s</profile>\n", gp->profile->name);
				stream->write_function(stream, "    <scheme>%s</scheme>\n", switch_str_nil(gp->register_scheme));
				stream->write_function(stream, "    <realm>%s</realm>\n", switch_str_nil(gp->register_realm));
				stream->write_function(stream, "    <username>%s</username>\n", switch_str_nil(gp->register_username));
				stream->write_function(stream, "    <password>%s</password>\n", switch_strlen_zero(gp->register_password) ? "no" : "yes");
				stream->write_function(stream, "    <from>%s</from>\n", switch_amp_encode(switch_str_nil(gp->register_from),xmlbuf,buflen));
				stream->write_function(stream, "    <contact>%s</contact>\n", switch_amp_encode(switch_str_nil(gp->register_contact),xmlbuf,buflen));
				stream->write_function(stream, "    <exten>%s</exten>\n", switch_amp_encode(switch_str_nil(gp->extension),xmlbuf,buflen));
				stream->write_function(stream, "    <to>%s</to>\n", switch_str_nil(gp->register_to));
				stream->write_function(stream, "    <proxy>%s</proxy>\n", switch_str_nil(gp->register_proxy));
				stream->write_function(stream, "    <context>%s</context>\n", switch_str_nil(gp->register_context));
				stream->write_function(stream, "    <expires>%s</expires>\n", switch_str_nil(gp->expires_str));
				stream->write_function(stream, "    <freq>%d</freq>\n", gp->freq);
				stream->write_function(stream, "    <ping>%d</ping>\n", gp->ping);
				stream->write_function(stream, "    <pingfreq>%d</pingfreq>\n", gp->ping_freq);
				stream->write_function(stream, "    <state>%s</state>\n", sofia_state_names[gp->state]);
				stream->write_function(stream, "    <status>%s%s</status>\n", status_names[gp->status], gp->pinging ? " (ping)" : "");
				stream->write_function(stream, "    <calls-in>%d</calls-in>\n", gp->ib_calls);
				stream->write_function(stream, "    <calls-out>%d</calls-out>\n", gp->ob_calls);

				stream->write_function(stream, "  </gateway>\n");
				sofia_reg_release_gateway(gp);
			} else {
				stream->write_function(stream, "Invalid Gateway!\n");
			}
		} else if (!strcasecmp(argv[0], "profile")) {
			struct cb_helper cb;
			char *sql = NULL;

			if ((argv[1]) && (profile = sofia_glue_find_profile(argv[1]))) {
				stream->write_function(stream, "%s\n", header);
				stream->write_function(stream, "<profile>\n");
				if (!argv[2] || (strcasecmp(argv[2], "reg") && strcasecmp(argv[2], "user"))) {
					stream->write_function(stream, "  <profile-info>\n");
					stream->write_function(stream, "    <name>%s</name>\n", switch_str_nil(argv[1]));
					stream->write_function(stream, "    <domain-name>%s</domain-name>\n", profile->domain_name ? profile->domain_name : "N/A");
					if (strcasecmp(argv[1], profile->name)) {
						stream->write_function(stream, "    <alias-of>%s</alias-of>\n", switch_str_nil(profile->name));
					}
					stream->write_function(stream, "    <db-name>%s</db-name>\n", switch_str_nil(profile->dbname));
					stream->write_function(stream, "    <pres-hosts>%s</pres-hosts>\n", switch_str_nil(profile->presence_hosts));
					stream->write_function(stream, "    <dialplan>%s</dialplan>\n", switch_str_nil(profile->dialplan));
					stream->write_function(stream, "    <context>%s</context>\n", switch_str_nil(profile->context));
					stream->write_function(stream, "    <challenge-realm>%s</challenge-realm>\n", 
										   switch_strlen_zero(profile->challenge_realm) ? "auto_to" : profile->challenge_realm);
					stream->write_function(stream, "    <rtp-ip>%s</rtp-ip>\n", switch_str_nil(profile->rtpip));
					stream->write_function(stream, "    <ext-rtp-ip>%s</ext-rtp-ip>\n", profile->extrtpip);
					stream->write_function(stream, "    <sip-ip>%s</sip-ip>\n", switch_str_nil(profile->sipip));
					stream->write_function(stream, "    <ext-sip-ip>%s</ext-sip-ip>\n", profile->extsipip);
					stream->write_function(stream, "    <url>%s</url>\n", switch_str_nil(profile->url));
					stream->write_function(stream, "    <bind-url>%s</bind-url>\n", switch_str_nil(profile->bindurl));
					stream->write_function(stream, "    <tls-url>%s</tls-url>\n", switch_str_nil(profile->tls_url));
					stream->write_function(stream, "    <tls-bind-url>%s</tls-bind-url>\n", switch_str_nil(profile->tls_bindurl));
					stream->write_function(stream, "    <hold-music>%s</hold-music>\n", switch_strlen_zero(profile->hold_music) ? "N/A" : profile->hold_music);
					stream->write_function(stream, "    <outbound-proxy>%s</outbound-proxy>\n", switch_strlen_zero(profile->outbound_proxy) ? "N/A" : profile->outbound_proxy);
					stream->write_function(stream, "    <codecs>%s</codecs>\n", switch_str_nil(profile->codec_string));
					stream->write_function(stream, "    <tel-event>%d</tel-event>\n", profile->te);
					stream->write_function(stream, "    <dtmf-mode>rfc2833</dtmf-mode>\n");
					stream->write_function(stream, "    <dtmf-mode>info</dtmf-mode>\n");
					stream->write_function(stream, "    <dtmf-mode>none</dtmf-mode>\n");
					stream->write_function(stream, "    <cng>%d</cng>\n", profile->cng_pt);
					stream->write_function(stream, "    <session-to>%d</session-to>\n", profile->session_timeout);
					stream->write_function(stream, "    <max-dialog>%d</max-dialog>\n", profile->max_proceeding);
					stream->write_function(stream, "    <nomedia>%s</nomedia>\n", sofia_test_flag(profile, TFLAG_INB_NOMEDIA) ? "true" : "false");
					stream->write_function(stream, "    <late-neg>%s</late-neg>\n", sofia_test_flag(profile, TFLAG_LATE_NEGOTIATION) ? "true" : "false");
					stream->write_function(stream, "    <proxy-media>%s</proxy-media>\n", sofia_test_flag(profile, TFLAG_PROXY_MEDIA) ? "true" : "false");
					stream->write_function(stream, "    <aggressive-nat>%s</aggressive-nat>\n", 
										   sofia_test_pflag(profile, PFLAG_AGGRESSIVE_NAT_DETECTION) ? "true" : "false");
					stream->write_function(stream, "    <stun-enabled>%s</stun-enabled>\n", sofia_test_pflag(profile, PFLAG_STUN_ENABLED) ? "true" : "false");
					stream->write_function(stream, "    <stun-auto-disable>%s</stun-auto-disable>\n", 
										   sofia_test_pflag(profile, PFLAG_STUN_AUTO_DISABLE) ? "true" : "false");
					stream->write_function(stream, "    <calls-in>%d</calls-in>\n", profile->ib_calls);
					stream->write_function(stream, "    <calls-out>%d</calls-out>\n", profile->ob_calls);
					stream->write_function(stream, "    <failed-calls-in>%d</failed-calls-in>\n", profile->ib_failed_calls);
					stream->write_function(stream, "    <failed-calls-out>%d</failed-calls-out>\n", profile->ob_failed_calls);
					stream->write_function(stream, "  </profile-info>\n");
				}
				stream->write_function(stream, "  <registrations>\n");

				cb.profile = profile;
				cb.stream = stream;

				if (!sql && argv[2] && !strcasecmp(argv[2], "pres") && argv[3]) {

					sql = switch_mprintf("select call_id,sip_user,sip_host,contact,status,"
							"rpid,expires,user_agent,server_user,server_host,profile_name,hostname,"
							"network_ip,network_port,sip_username,sip_realm,mwi_user,mwi_host"
							" from sip_registrations where profile_name='%q' and presence_hosts like '%%%q%%'", 
							profile->name, argv[3]);
				}
				if (!sql && argv[2] && !strcasecmp(argv[2], "reg") && argv[3]) {

					sql = switch_mprintf("select call_id,sip_user,sip_host,contact,status,"
							"rpid,expires,user_agent,server_user,server_host,profile_name,hostname,"
							"network_ip,network_port,sip_username,sip_realm,mwi_user,mwi_host"
							" from sip_registrations where profile_name='%q' and contact like '%%%q%%'", 
							profile->name, argv[3]);
				}
				if (!sql && argv[2] && !strcasecmp(argv[2], "user") && argv[3]) {
					char *dup = strdup(argv[3]);
					char *host = NULL, *user = NULL;
					char *sqlextra = NULL;

					switch_assert(dup);

					if ((host = strchr(dup, '@'))) {
						*host++ = '\0';
						user = dup;
					} else {
						host = dup;
					}

					if (switch_strlen_zero(user)) {
						sqlextra = switch_mprintf("(sip_host='%q')", host);
					} else if (switch_strlen_zero(host)) {
						sqlextra = switch_mprintf("(sip_user='%q')", user);
					} else {
						sqlextra = switch_mprintf("(sip_user='%q' and sip_host='%q')", user, host);
					}

					sql = switch_mprintf("select call_id,sip_user,sip_host,contact,status,"
							"rpid,expires,user_agent,server_user,server_host,profile_name,hostname,"
							"network_ip,network_port,sip_username,sip_realm,mwi_user,mwi_host"
							" from sip_registrations where profile_name='%q' and %s",
							profile->name, sqlextra);
					switch_safe_free(dup);
					switch_safe_free(sqlextra);
				}

				if (!sql) {
					sql = switch_mprintf("select call_id,sip_user,sip_host,contact,status,"
										 "rpid,expires,user_agent,server_user,server_host,profile_name,hostname,"
										 "network_ip,network_port,sip_username,sip_realm,mwi_user,mwi_host"
										 " from sip_registrations where profile_name='%q'", 
										 profile->name);
				}

				sofia_glue_execute_sql_callback(profile, SWITCH_FALSE, profile->ireg_mutex, sql, show_reg_callback_xml, &cb);
				switch_safe_free(sql);

				stream->write_function(stream, "  </registrations>\n");
				stream->write_function(stream, "</profile>\n");

				sofia_glue_release_profile(profile);
			} else {
				stream->write_function(stream, "Invalid Profile!\n");
			}
		} else {
			stream->write_function(stream, "Invalid Syntax!\n");
		}

		return SWITCH_STATUS_SUCCESS;
	}

  stream->write_function(stream, "%s\n", header);
  stream->write_function(stream, "<profiles>\n");
	switch_mutex_lock(mod_sofia_globals.hash_mutex);
	for (hi = switch_hash_first(NULL, mod_sofia_globals.profile_hash); hi; hi = switch_hash_next(hi)) {
		switch_hash_this(hi, &vvar, NULL, &val);
		profile = (sofia_profile_t *) val;
		if (sofia_test_pflag(profile, PFLAG_RUNNING)) {

			if (strcmp(vvar, profile->name)) {
				ac++;
				stream->write_function(stream, "<alias>\n<name>%s</name>\n<type>%s</type>\n<data>%s</data>\n<state>%s</state>\n</alias>\n", vvar, "alias", profile->name, "ALIASED");
			} else {
				stream->write_function(stream, "<profile>\n<name>%s</name>\n<type>%s</type>\n<data>%s</data>\n<state>%s (%u)</state>\n</profile>\n", profile->name, "profile", profile->url,
									   sofia_test_pflag(profile, PFLAG_RUNNING) ? "RUNNING" : "DOWN", profile->inuse);

				if (sofia_test_pflag(profile, PFLAG_TLS)) {
					stream->write_function(stream, "<profile>\n<name>%s</name>\n<type>%s</type>\n<data>%s</data>\n<state>%s (%u) (TLS)</state>\n</profile>\n", profile->name, "profile", profile->tls_url,
										   sofia_test_pflag(profile, PFLAG_RUNNING) ? "RUNNING" : "DOWN", profile->inuse);
				}

				c++;

				for (gp = profile->gateways; gp; gp = gp->next) {
					switch_assert(gp->state < REG_STATE_LAST);
					stream->write_function(stream, "<gateway>\n<name>%s</name>\n<type>%s</type>\n<data>%s</data>\n<state>%s</state>\n</gateway>\n", gp->name, "gateway", gp->register_to, sofia_state_names[gp->state]);
					if (gp->state == REG_STATE_FAILED || gp->state == REG_STATE_TRYING) {
						time_t now = switch_epoch_time_now(NULL);
						if (gp->retry > now) {
							stream->write_function(stream, " (retry: %ds)", gp->retry - now);
						} else {
							stream->write_function(stream, " (retry: NEVER)");
						}
					}
					stream->write_function(stream, "\n");
				}
			}
		}
	}
	switch_mutex_unlock(mod_sofia_globals.hash_mutex);
	stream->write_function(stream, "</profiles>\n");
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t cmd_profile(char **argv, int argc, switch_stream_handle_t *stream)
{
	sofia_profile_t *profile = NULL;
	char *profile_name = argv[0];
	const char *err;
	switch_xml_t xml_root;

	if (argc < 2) {
		stream->write_function(stream, "Invalid Args!\n");
		return SWITCH_STATUS_SUCCESS;
	}

	if (!strcasecmp(argv[1], "start")) {
		if (argc > 2 && !strcasecmp(argv[2], "reloadxml")) {
			if ((xml_root = switch_xml_open_root(1, &err))) {
				switch_xml_free(xml_root);
			}
			stream->write_function(stream, "Reload XML [%s]\n", err);
		}
		if (config_sofia(1, argv[0]) == SWITCH_STATUS_SUCCESS) {
			stream->write_function(stream, "%s started successfully\n", argv[0]);
		} else {
			stream->write_function(stream, "Failure starting %s\n", argv[0]);
		}
		return SWITCH_STATUS_SUCCESS;
	}

	if (argv[1] && !strcasecmp(argv[0], "restart") && !strcasecmp(argv[1], "all")) {
		sofia_glue_restart_all_profiles();
		return SWITCH_STATUS_SUCCESS;
	}
	
	if (switch_strlen_zero(profile_name) || !(profile = sofia_glue_find_profile(profile_name))) {
		stream->write_function(stream, "Invalid Profile [%s]", switch_str_nil(profile_name));
		return SWITCH_STATUS_SUCCESS;
	}

	if (!strcasecmp(argv[1], "killgw")) {
		sofia_gateway_t *gateway_ptr;
		if (argc < 3) {
			stream->write_function(stream, "-ERR missing gw name\n");
			goto done;
		}

		if ((gateway_ptr = sofia_reg_find_gateway(argv[2]))) { 			
			sofia_glue_del_gateway(gateway_ptr);
			sofia_reg_release_gateway(gateway_ptr);
			stream->write_function(stream, "+OK gateway marked for deletion.\n");
		} else {
			stream->write_function(stream, "-ERR no such gateway.\n");
		}

		goto done;
	}

	if (!strcasecmp(argv[1], "stun-auto-disable")) {
		if (argv[2]) {
			int is_true = switch_true(argv[2]);
			if (is_true) {
				sofia_set_pflag(profile, PFLAG_STUN_AUTO_DISABLE);
			} else {
				sofia_clear_pflag(profile, PFLAG_STUN_AUTO_DISABLE);
			}
		}

		stream->write_function(stream, "+OK stun-auto-disable=%s", sofia_test_pflag(profile, PFLAG_STUN_AUTO_DISABLE) ? "true" : "false");
		
		goto done;
	}

	if (!strcasecmp(argv[1], "stun-enabled")) {
		if (argv[2]) {
			int is_true = switch_true(argv[2]);
			if (is_true) {
				sofia_set_pflag(profile, PFLAG_STUN_ENABLED);
			} else {
				sofia_clear_pflag(profile, PFLAG_STUN_ENABLED);
			}
		}

		stream->write_function(stream, "+OK stun-enabled=%s", sofia_test_pflag(profile, PFLAG_STUN_ENABLED) ? "true" : "false");
		
		goto done;
	}


	if (!strcasecmp(argv[1], "rescan")) {

		if (argc > 2 && !strcasecmp(argv[2], "reloadxml")) {
			if ((xml_root = switch_xml_open_root(1, &err))) {
				switch_xml_free(xml_root);
			}
			stream->write_function(stream, "Reload XML [%s]\n", err);
		}
			
		if (reconfig_sofia(profile) == SWITCH_STATUS_SUCCESS) {
			stream->write_function(stream, "+OK scan complete\n");
		} else {
			stream->write_function(stream, "-ERR cannot find config for profile %s\n", profile->name);
		}
		goto done;
	}

	if (!strcasecmp(argv[1], "flush_inbound_reg")) {
		int reboot = 0;

		if (argc > 2) {
			if (!strcasecmp(argv[2], "reboot")) {
				reboot = 1;
				argc = 2;
			}
		}

		if (argc > 2) {
			if (argc > 3 && !strcasecmp(argv[3], "reboot")) {
				reboot = 1;
			}

			sofia_reg_expire_call_id(profile, argv[2], reboot);
			stream->write_function(stream, "+OK %s all registrations matching specified call_id\n", reboot ? "rebooting" : "flushing");
		} else {
			sofia_reg_check_expire(profile, 0, reboot);
			stream->write_function(stream, "+OK %s all registrations\n", reboot ? "rebooting" : "flushing");
		}

		goto done;
	}

	if (!strcasecmp(argv[1], "register")) {
		char *gname = argv[2];
		sofia_gateway_t *gateway_ptr;

		if (switch_strlen_zero(gname)) {
			stream->write_function(stream, "No gateway name provided!\n");
			goto done;
		}

		if (!strcasecmp(gname, "all")) {
			for (gateway_ptr = profile->gateways; gateway_ptr; gateway_ptr = gateway_ptr->next) {
				gateway_ptr->retry = 0;
				gateway_ptr->state = REG_STATE_UNREGED;
			}
			stream->write_function(stream, "+OK\n");
		} else if ((gateway_ptr = sofia_reg_find_gateway(gname))) {
			gateway_ptr->retry = 0;
			gateway_ptr->state = REG_STATE_UNREGED;
			stream->write_function(stream, "+OK\n");
			sofia_reg_release_gateway(gateway_ptr);
		} else {
			stream->write_function(stream, "Invalid gateway!\n");
		}

		goto done;
	}

	if (!strcasecmp(argv[1], "unregister")) {
		char *gname = argv[2];
		sofia_gateway_t *gateway_ptr;

		if (switch_strlen_zero(gname)) {
			stream->write_function(stream, "No gateway name provided!\n");
			goto done;
		}

		if (!strcasecmp(gname, "all")) {
			for (gateway_ptr = profile->gateways; gateway_ptr; gateway_ptr = gateway_ptr->next) {
				gateway_ptr->retry = 0;
				gateway_ptr->state = REG_STATE_UNREGISTER;
			}
			stream->write_function(stream, "+OK\n");
		} else if ((gateway_ptr = sofia_reg_find_gateway(gname))) {
			gateway_ptr->retry = 0;
			gateway_ptr->state = REG_STATE_UNREGISTER;
			stream->write_function(stream, "+OK\n");
			sofia_reg_release_gateway(gateway_ptr);
		} else {
			stream->write_function(stream, "Invalid gateway!\n");
		}
		goto done;
	}

	if (!strcasecmp(argv[1], "stop") || !strcasecmp(argv[1], "restart")) {
		int rsec = 10;
		int diff = (int) (switch_epoch_time_now(NULL) - profile->started);
		int remain = rsec - diff;
		if (diff < rsec) {
			stream->write_function(stream, "Profile %s must be up for at least %d seconds to stop/restart.\nPlease wait %d second%s\n",
								   profile->name, rsec, remain, remain == 1 ? "" : "s");
		} else {

			if (argc > 2 && !strcasecmp(argv[2], "reloadxml")) {
				if ((xml_root = switch_xml_open_root(1, &err))) {
					switch_xml_free(xml_root);
				}
				stream->write_function(stream, "Reload XML [%s]\n", err);
			}

			if (!strcasecmp(argv[1], "stop")) {
				sofia_clear_pflag_locked(profile, PFLAG_RUNNING);
				stream->write_function(stream, "stopping: %s", profile->name);
			} else {
				sofia_set_pflag_locked(profile, PFLAG_RESPAWN);
				sofia_clear_pflag_locked(profile, PFLAG_RUNNING);
				stream->write_function(stream, "restarting: %s", profile->name);
			}
		}
		goto done;
	}
	
	if (!strcasecmp(argv[1], "siptrace")) {
		if (argc > 2) {
			int value = switch_true(argv[2]);
			nua_set_params(profile->nua, TPTAG_LOG(value), TAG_END());
			stream->write_function(stream, "%s sip debugging on %s", value ? "Enabled" : "Disabled", profile->name);
		} else {
			stream->write_function(stream, "Usage: sofia profile <name> siptrace <on/off>\n");
		}
		goto done;
	}

	stream->write_function(stream, "-ERR Unknown command!\n");

  done:
	if (profile) {
		sofia_glue_release_profile(profile);
	}

	return SWITCH_STATUS_SUCCESS;
}

static int contact_callback(void *pArg, int argc, char **argv, char **columnNames)
{
	struct cb_helper *cb = (struct cb_helper *) pArg;
	char *contact;

	if (!switch_strlen_zero(argv[0]) && (contact = sofia_glue_get_url_from_contact(argv[0], 1)) ) {
		cb->stream->write_function(cb->stream, "%ssofia/%s/sip:%s,", argv[2], argv[1], sofia_glue_strip_proto(contact));
		free(contact);
	}

	return 0;
}

SWITCH_STANDARD_API(sofia_contact_function)
{
	char *data;
	char *user = NULL;
	char *domain = NULL;
	char *concat = NULL;
	char *profile_name = NULL;
	char *p;
	sofia_profile_t *profile = NULL;
	const char *exclude_contact = NULL;
	char *reply = "error/facility_not_subscribed";
	
	if (!cmd) {
		stream->write_function(stream, "%s", "");
		return SWITCH_STATUS_SUCCESS;
	}

	if (session) {
		switch_channel_t *channel = switch_core_session_get_channel(session);
		exclude_contact = switch_channel_get_variable(channel, "sip_exclude_contact");
	}


	data = strdup(cmd);
	switch_assert(data);

	if ((p = strchr(data, '/'))) {
		profile_name = data;
		*p++ = '\0';
		user = p;
	} else {
		user = data;
	}

	if ((domain = strchr(user, '@'))) {
		*domain++ = '\0';
		if ( (concat = strchr( domain, '/')) ) {
		    *concat++ = '\0';
		}
	}
	else {
		if ( (concat = strchr( user, '/')) ) {
		    *concat++ = '\0';
		}
	}

	if (!profile_name && domain) {
		profile_name = domain;
	}

	if (user && profile_name) {
		char *sql;

		if (!(profile = sofia_glue_find_profile(profile_name))) {
			profile_name = domain;
			domain = NULL;
		}

		if (!profile && profile_name) {
			profile = sofia_glue_find_profile(profile_name);
		}

		if (profile) {
			struct cb_helper cb;
			switch_stream_handle_t mystream = { 0 };

			if (!domain || !strchr(domain, '.')) {
				domain = profile->name;
			}

			SWITCH_STANDARD_STREAM(mystream);
			switch_assert(mystream.data);
			cb.profile = profile;
			cb.stream = &mystream;

			if (exclude_contact) {
				sql = switch_mprintf("select contact, profile_name, '%q' "
									 "from sip_registrations where sip_user='%q' and (sip_host='%q' or presence_hosts like '%%%q%%') "
									 "and contact not like '%%%s%%'",
									 ( concat != NULL ) ? concat : "", user, domain, domain, exclude_contact);
			} else {
				sql = switch_mprintf("select contact, profile_name, '%q' "
									 "from sip_registrations where sip_user='%q' and (sip_host='%q' or presence_hosts like '%%%q%%')", 
									 ( concat != NULL ) ? concat : "", user, domain, domain);
			}

			switch_assert(sql);
			sofia_glue_execute_sql_callback(profile, SWITCH_FALSE, profile->ireg_mutex, sql, contact_callback, &cb);
			switch_safe_free(sql);
			reply = (char *) mystream.data;
			if (!switch_strlen_zero(reply) && end_of(reply) == ',') {
				end_of(reply) = '\0';
			}

			if (switch_strlen_zero(reply)) {
				reply = "error/user_not_registered";
			}

			stream->write_function(stream, "%s", reply);
			reply = NULL;
			
			switch_safe_free(mystream.data);
		}
	}

	if (reply) {
		stream->write_function(stream, "%s", reply);
	}

	switch_safe_free(data);

	if (profile) {
		sofia_glue_release_profile(profile);
	}

	return SWITCH_STATUS_SUCCESS;
}

/* <gateway_name> [ivar|ovar|var] <name> */
SWITCH_STANDARD_API(sofia_gateway_data_function)
{
	char *argv[4];
	char *mydata;
	int argc;
	sofia_gateway_t *gateway;
	char *gwname, *param, *varname;
	const char *val = NULL;
	
	if (switch_strlen_zero(cmd)) {
		stream->write_function(stream, "-ERR Parameter missing\n");
		return SWITCH_STATUS_SUCCESS;
	}
	if (!(mydata = strdup(cmd))) {
		return SWITCH_STATUS_FALSE;
	}
	
	if (!(argc = switch_separate_string(mydata, ' ', argv, (sizeof(argv) / sizeof(argv[0])))) || !argv[0]) {
		goto end;
	}
	
	gwname = argv[0];
	param = argv[1];
	varname = argv[2];
	
	if (switch_strlen_zero(gwname) || switch_strlen_zero(param) || switch_strlen_zero(varname)) {
		goto end;
	}
	
	if (!(gateway = sofia_reg_find_gateway(gwname))) {
		goto end;
	}
	
	if (!strcasecmp(param, "ivar") && gateway->ib_vars && (val = switch_event_get_header(gateway->ib_vars, varname))) {
		stream->write_function(stream, "%s", val);
	} else if (!strcasecmp(param, "ovar") && gateway->ob_vars && (val = switch_event_get_header(gateway->ob_vars, varname))) {
		stream->write_function(stream, "%s", val);		
	} else if (!strcasecmp(param, "var")) {
		if (gateway->ib_vars && (val = switch_event_get_header(gateway->ib_vars, varname))) {
			stream->write_function(stream, "%s", val);
		} else if (gateway->ob_vars && (val = switch_event_get_header(gateway->ob_vars, varname))) {
			stream->write_function(stream, "%s", val);
		}
	}
	
	sofia_reg_release_gateway(gateway);
	
end:
	switch_safe_free(mydata);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_API(sofia_function)
{
	char *argv[1024] = { 0 };
	int argc = 0;
	char *mycmd = NULL;
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	sofia_command_t func = NULL;
	int lead = 1;
	const char *usage_string = "USAGE:\n"
		"--------------------------------------------------------------------------------\n"
		"sofia help\n"
		"sofia profile <profile_name> [[start|stop|restart|rescan] [reloadxml]|flush_inbound_reg [<call_id>] [reboot]|[register|unregister] [<gateway name>|all]|killgw <gateway name>|[stun-auto-disable|stun-enabled] [true|false]]|siptrace [on|off]\n"
		"sofia status profile <name> [ reg <contact str> ] | [ pres <pres str> ] | [ user <user@domain> ]\n"
		"sofia status gateway <name>\n"
		"sofia loglevel <all|default|tport|iptsec|nea|nta|nth_client|nth_server|nua|soa|sresolv|stun> [0-9]\n"
		"--------------------------------------------------------------------------------\n";

	if (session) {
		return SWITCH_STATUS_FALSE;
	}

	if (switch_strlen_zero(cmd)) {
		stream->write_function(stream, "%s", usage_string);
		goto done;
	}

	if (!(mycmd = strdup(cmd))) {
		status = SWITCH_STATUS_MEMERR;
		goto done;
	}

	if (!(argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])))) || !argv[0]) {
		stream->write_function(stream, "%s", usage_string);
		goto done;
	}

	if (!strcasecmp(argv[0], "profile")) {
		func = cmd_profile;
	} else if (!strcasecmp(argv[0], "status")) {
		func = cmd_status;
	} else if (!strcasecmp(argv[0], "xmlstatus")) {
		func = cmd_xml_status;
	} else if (!strcasecmp(argv[0], "tracelevel")) {
		if (argv[1]) {
			mod_sofia_globals.tracelevel = switch_log_str2level(argv[1]);
		}
		stream->write_function(stream, "+OK tracelevel is %s", switch_log_level2str(mod_sofia_globals.tracelevel));
        goto done;
	} else if (!strcasecmp(argv[0], "loglevel")) {
		if (argc > 2 && argv[2] && switch_is_number(argv[2])) {
			int level = atoi(argv[2]);
			if (sofia_set_loglevel(argv[1], level) == SWITCH_STATUS_SUCCESS) {
				stream->write_function(stream, "Sofia log level for component [%s] has been set to [%d]", argv[1], level);
			} else {
				stream->write_function(stream, "%s", usage_string);
			}
		} else if (argc > 1 && argv[1]) {
			int level = sofia_get_loglevel(argv[1]);
			if (level >= 0) {
				stream->write_function(stream, "Sofia-sip loglevel for [%s] is [%d]", argv[1], level);
			} else {
				stream->write_function(stream, "%s", usage_string);
			}
		} else {
			stream->write_function(stream, "%s", usage_string);
		}
		goto done;
	} else if (!strcasecmp(argv[0], "help")) {
		stream->write_function(stream, "%s", usage_string);
		goto done;
	}

	if (func) {
		status = func(&argv[lead], argc - lead, stream);
	} else {
		stream->write_function(stream, "Unknown Command [%s]\n", argv[0]);
	}

  done:
	switch_safe_free(mycmd);
	return status;
}

switch_io_routines_t sofia_io_routines = {
	/*.outgoing_channel */ sofia_outgoing_channel,
	/*.read_frame */ sofia_read_frame,
	/*.write_frame */ sofia_write_frame,
	/*.kill_channel */ sofia_kill_channel,
	/*.send_dtmf */ sofia_send_dtmf,
	/*.receive_message */ sofia_receive_message,
	/*.receive_event */ sofia_receive_event,
	/*.state_change */ NULL,
	/*.read_video_frame */ sofia_read_video_frame,
	/*.write_video_frame */ sofia_write_video_frame
};

switch_state_handler_table_t sofia_event_handlers = {
	/*.on_init */ sofia_on_init,
	/*.on_routing */ sofia_on_routing,
	/*.on_execute */ sofia_on_execute,
	/*.on_hangup */ sofia_on_hangup,
	/*.on_exchange_media */ sofia_on_exchange_media,
	/*.on_soft_execute */ sofia_on_soft_execute,
	/*.on_consume_media */ NULL,
	/*.on_hibernate */ sofia_on_hibernate,
	/*.on_reset */ sofia_on_reset,
	/*.on_park*/ NULL,
	/*.on_reporting*/ NULL,
	/*.on_destroy*/ sofia_on_destroy
};

static switch_status_t sofia_manage(char *relative_oid, switch_management_action_t action, char *data, switch_size_t datalen)
{
	return SWITCH_STATUS_SUCCESS;
}

static switch_call_cause_t sofia_outgoing_channel(switch_core_session_t *session, switch_event_t *var_event,
												  switch_caller_profile_t *outbound_profile, switch_core_session_t **new_session,
												  switch_memory_pool_t **pool, switch_originate_flag_t flags)
{
	switch_call_cause_t cause = SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
	switch_core_session_t *nsession = NULL;
	char *data, *profile_name, *dest, *dest_num = NULL;
	sofia_profile_t *profile = NULL;
	switch_caller_profile_t *caller_profile = NULL;
	private_object_t *tech_pvt = NULL;
	switch_channel_t *nchannel;
	char *host = NULL, *dest_to = NULL, *p;
	const char *hval = NULL;

	*new_session = NULL;

	if (!(nsession = switch_core_session_request(sofia_endpoint_interface, SWITCH_CALL_DIRECTION_OUTBOUND, pool))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Error Creating Session\n");
		goto error;
	}

	if (!(tech_pvt = (struct private_object *) switch_core_session_alloc(nsession, sizeof(*tech_pvt)))) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT, "Error Creating Session\n");
		goto error;
	}
	switch_mutex_init(&tech_pvt->flag_mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(nsession));
	switch_mutex_init(&tech_pvt->sofia_mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(nsession));

	data = switch_core_session_strdup(nsession, outbound_profile->destination_number);
	if ((dest_to = strchr(data, '^'))) {
		*dest_to++ = '\0';
	}
	profile_name = data;

	nchannel = switch_core_session_get_channel(nsession);

	if ((hval = switch_event_get_header(var_event, "sip_invite_to_uri"))) {
		dest_to = switch_core_session_strdup(nsession, hval);
	}

	if (!strncasecmp(profile_name, "gateway", 7)) {
		char *gw, *params;
		sofia_gateway_t *gateway_ptr = NULL;

		if (!(gw = strchr(profile_name, '/'))) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Invalid URL\n");
			cause = SWITCH_CAUSE_INVALID_NUMBER_FORMAT;
			goto error;
		}

		*gw++ = '\0';

		if (!(dest = strchr(gw, '/'))) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Invalid URL\n");
			cause = SWITCH_CAUSE_INVALID_NUMBER_FORMAT;
			goto error;
		}

		*dest++ = '\0';

		if (!(gateway_ptr = sofia_reg_find_gateway(gw))) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Invalid Gateway\n");
			cause = SWITCH_CAUSE_INVALID_NUMBER_FORMAT;
			goto error;
		}

		if (gateway_ptr->status != SOFIA_GATEWAY_UP) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Gateway is down!\n");
			cause = SWITCH_CAUSE_NETWORK_OUT_OF_ORDER;
			sofia_reg_release_gateway(gateway_ptr);
			gateway_ptr = NULL;
			goto error;
		}

		tech_pvt->transport = gateway_ptr->register_transport;


		/*
		 * Handle params, strip them off the destination and add them to the
		 * invite contact.
		 *
		 * TODO:
		 *  - Add parameters back to destination url?
		 */
		if ((params = strchr(dest, ';'))) {
			char *tp_param;

			*params++ = '\0';

			if ((tp_param = (char *) switch_stristr("port=", params))) {
				tp_param += 5;
				tech_pvt->transport = sofia_glue_str2transport(tp_param);
				if (tech_pvt->transport == SOFIA_TRANSPORT_UNKNOWN) {
					cause = SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
					goto error;
				}
			}
		}

		if (tech_pvt->transport != gateway_ptr->register_transport) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
							  "You are trying to use a different transport type for this gateway (overriding the register-transport), this is unsupported!\n");
			cause = SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
			goto error;
		}

		profile = gateway_ptr->profile;
		tech_pvt->gateway_name = switch_core_session_strdup(nsession, gateway_ptr->name);
		switch_channel_set_variable(nchannel, "sip_gateway_name", gateway_ptr->name);

		if (!sofia_test_flag(gateway_ptr, REG_FLAG_CALLERID)) {
			tech_pvt->gateway_from_str = switch_core_session_strdup(nsession, gateway_ptr->register_from);
		}

		if (!strchr(dest, '@')) {
			tech_pvt->dest = switch_core_session_sprintf(nsession, "sip:%s@%s", dest, sofia_glue_strip_proto(gateway_ptr->register_proxy));
		} else {
			tech_pvt->dest = switch_core_session_sprintf(nsession, "sip:%s", dest);
		}

		if ((host = switch_core_session_strdup(nsession, tech_pvt->dest))) {
			char *p = strchr(host, '@');
			if (p) {
				host = p+1;
			} else {
				host = NULL;
				dest_to = NULL;
			}
		}

		if (params) {
			tech_pvt->invite_contact = switch_core_session_sprintf(nsession, "%s;%s", gateway_ptr->register_contact, params);
		} else {
			tech_pvt->invite_contact = switch_core_session_strdup(nsession, gateway_ptr->register_contact);
		}
		
		gateway_ptr->ob_calls++;
		
		if (!switch_strlen_zero(gateway_ptr->from_domain) && !switch_channel_get_variable(nchannel, "sip_invite_domain")) {
			switch_channel_set_variable(nchannel, "sip_invite_domain", gateway_ptr->from_domain);
		}

		if (!switch_strlen_zero(gateway_ptr->outbound_sticky_proxy) && !switch_channel_get_variable(nchannel, "sip_route_uri")) {
			switch_channel_set_variable(nchannel, "sip_route_uri", gateway_ptr->outbound_sticky_proxy);
		}
		
		if (gateway_ptr->ob_vars) {
			switch_event_header_t *hp;
			for(hp = gateway_ptr->ob_vars->headers; hp; hp = hp->next) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s setting variable [%s]=[%s]\n",
								  switch_channel_get_name(nchannel), hp->name, hp->value);
				switch_channel_set_variable(nchannel, hp->name, hp->value);
			}
		}

	} else {
		if (!(dest = strchr(profile_name, '/'))) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Invalid URL\n");
			cause = SWITCH_CAUSE_INVALID_NUMBER_FORMAT;
			goto error;
		}
		*dest++ = '\0';

		if (!(profile = sofia_glue_find_profile(profile_name))) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Invalid Profile\n");
			cause = SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
			goto error;
		}

		if (profile->domain_name && profile->domain_name != profile->name) {
			profile_name = profile->domain_name;
		}

		if (!strncasecmp(dest, "sip:", 4) || !strncasecmp(dest, "sips:", 5)) {
			tech_pvt->dest = switch_core_session_strdup(nsession, dest);
		} else if ((host = strchr(dest, '%'))) {
			char buf[1024];
			*host = '@';
			tech_pvt->e_dest = switch_core_session_strdup(nsession, dest);
			*host++ = '\0';
			if (sofia_reg_find_reg_url(profile, dest, host, buf, sizeof(buf))) {
				tech_pvt->dest = switch_core_session_strdup(nsession, buf);
				tech_pvt->local_url = switch_core_session_sprintf(nsession, "%s@%s", dest, host);
			} else {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Cannot locate registered user %s@%s\n", dest, host);
				cause = SWITCH_CAUSE_USER_NOT_REGISTERED;
				goto error;
			}
		} else if (!(host = strchr(dest, '@'))) {
			char buf[1024];
			tech_pvt->e_dest = switch_core_session_strdup(nsession, dest);
			if (sofia_reg_find_reg_url(profile, dest, profile_name, buf, sizeof(buf))) {
				tech_pvt->dest = switch_core_session_strdup(nsession, buf);
				tech_pvt->local_url = switch_core_session_sprintf(nsession, "%s@%s", dest, profile_name);
				host = profile_name;
			} else {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Cannot locate registered user %s@%s\n", dest, profile_name);
				cause = SWITCH_CAUSE_USER_NOT_REGISTERED;
				goto error;
			}
		} else {
			tech_pvt->dest = switch_core_session_alloc(nsession, strlen(dest) + 5);
			switch_snprintf(tech_pvt->dest, strlen(dest) + 5, "sip:%s", dest);
		}
	}

	sofia_glue_get_user_host(switch_core_session_strdup(nsession, tech_pvt->dest), NULL, &tech_pvt->remote_ip);

	if (dest_to) {
		if (strchr(dest_to, '@')) {
			tech_pvt->dest_to = switch_core_session_sprintf(nsession, "sip:%s", dest_to);
		} else {
			tech_pvt->dest_to = switch_core_session_sprintf(nsession, "sip:%s@%s", dest_to, host);
		}
	}


	if (!tech_pvt->dest_to) {
		tech_pvt->dest_to = tech_pvt->dest;
	}

	sofia_glue_attach_private(nsession, profile, tech_pvt, dest);

	if (tech_pvt->local_url) {
		switch_channel_set_variable(nchannel, "sip_local_url", tech_pvt->local_url);
		if (profile->pres_type) {
			switch_channel_set_variable(nchannel, "presence_id", tech_pvt->local_url);
		}
	}
	switch_channel_set_variable(nchannel, "sip_destination_url", tech_pvt->dest);

	dest_num = switch_core_session_strdup(nsession, dest);
	if ((p = strchr(dest_num, ':'))) {
		dest_num = p + 1;
		if ((p = strchr(dest_num, '@'))) {
			*p = '\0';
		}
	}

	caller_profile = switch_caller_profile_clone(nsession, outbound_profile);
	caller_profile->destination_number = switch_core_strdup(caller_profile->pool, dest_num);
	switch_channel_set_caller_profile(nchannel, caller_profile);
	switch_channel_set_flag(nchannel, CF_OUTBOUND);
	sofia_set_flag_locked(tech_pvt, TFLAG_OUTBOUND);
	sofia_clear_flag_locked(tech_pvt, TFLAG_LATE_NEGOTIATION);
	if (switch_channel_get_state(nchannel) == CS_NEW) {
		switch_channel_set_state(nchannel, CS_INIT);
	}
	tech_pvt->caller_profile = caller_profile;
	*new_session = nsession;
	cause = SWITCH_CAUSE_SUCCESS;

	if ((hval = switch_event_get_header(var_event, "sip_auto_answer")) && switch_true(hval)) {
		switch_channel_set_variable_printf(nchannel, "sip_h_Call-Info", "<sip:%s>;answer-after=0", profile->sipip);
		switch_channel_set_variable(nchannel, "sip_invite_params", "intercom=true");
	}

	if (session) {
		switch_channel_t *o_channel = switch_core_session_get_channel(session);
		const char *vval = NULL;

		if ((vval = switch_channel_get_variable(o_channel, "sip_auto_answer")) && switch_true(vval)) {
			switch_channel_set_variable_printf(nchannel, "sip_h_Call-Info", "<sip:%s>;answer-after=0", profile->sipip);
			switch_channel_set_variable(nchannel, "sip_invite_params", "intercom=true");
		}
		
		switch_ivr_transfer_variable(session, nsession, SOFIA_REPLACES_HEADER);
		switch_ivr_transfer_variable(session, nsession, "sip_auto_answer");
		switch_ivr_transfer_variable(session, nsession, SOFIA_SIP_HEADER_PREFIX_T);
		switch_ivr_transfer_variable(session, nsession, "sip_video_fmtp");
		switch_ivr_transfer_variable(session, nsession, "sip-force-contact");
		switch_ivr_transfer_variable(session, nsession, "sip_sticky_contact");
		switch_ivr_transfer_variable(session, nsession, "sip_cid_type");

		if (switch_core_session_compare(session, nsession)) {
			/* It's another sofia channel! so lets cache what they use as a pt for telephone event so 
			   we can keep it the same
			 */
			private_object_t *ctech_pvt;
			ctech_pvt = switch_core_session_get_private(session);
			switch_assert(ctech_pvt != NULL);
			tech_pvt->bte = ctech_pvt->te;
			tech_pvt->bcng_pt = ctech_pvt->cng_pt;
		}

		if (switch_channel_test_flag(o_channel, CF_PROXY_MEDIA)) {
			const char *r_sdp = switch_channel_get_variable(o_channel, SWITCH_R_SDP_VARIABLE);

			if (switch_stristr("m=video", r_sdp)) {
				sofia_glue_tech_choose_video_port(tech_pvt, 1);
				tech_pvt->video_rm_encoding = "PROXY-VID";
				tech_pvt->video_rm_rate = 90000;
				tech_pvt->video_codec_ms = 0;
				switch_channel_set_flag(tech_pvt->channel, CF_VIDEO);
				sofia_set_flag(tech_pvt, TFLAG_VIDEO);
			}
		}
	}

	goto done;

  error:
	if (nsession) {
		switch_core_session_destroy(&nsession);
	}
	*pool = NULL;

  done:

	if (profile) {
		if (cause == SWITCH_CAUSE_SUCCESS) {
			profile->ob_calls++;
		} else {
			profile->ob_failed_calls++;
		}
		sofia_glue_release_profile(profile);
	}
	return cause;
}


static int notify_callback(void *pArg, int argc, char **argv, char **columnNames)
{

	nua_handle_t *nh;
	sofia_profile_t *ext_profile = NULL, *profile = (sofia_profile_t *) pArg;
	char *user = argv[0];
	char *host = argv[1];
	char *contact_in = argv[2];
	char *profile_name = argv[3];
	char *ct = argv[4];
	char *es = argv[5];
	char *body = argv[6];
	char *id = NULL;
	char *p , *contact;

	
	if (profile_name && strcasecmp(profile_name, profile->name)) {
        if ((ext_profile = sofia_glue_find_profile(profile_name))) {
            profile = ext_profile;
        }
    }

	id = switch_mprintf("sip:%s@%s", user, host);
	switch_assert(id);
	contact = sofia_glue_get_url_from_contact(contact_in, 1);
				
	if ((p = strstr(contact, ";fs_"))) {
		*p = '\0';
	}

	nh = nua_handle(profile->nua, 
					NULL, 
					NUTAG_URL(contact), 
					SIPTAG_FROM_STR(id), 
					SIPTAG_TO_STR(id), 
					SIPTAG_CONTACT_STR(profile->url), 
					TAG_END());
				
	nua_handle_bind(nh, &mod_sofia_globals.destroy_private);

	nua_notify(nh,
			   NUTAG_NEWSUB(1),
			   SIPTAG_EVENT_STR(es), 
			   SIPTAG_CONTENT_TYPE_STR(ct), 
			   TAG_IF(!switch_strlen_zero(body), SIPTAG_PAYLOAD_STR(body)),
			   TAG_END());

				
	free(id);
	free(contact);

	if (ext_profile) {
		sofia_glue_release_profile(ext_profile);
	}	

	return 0;
}

static void general_event_handler(switch_event_t *event)
{
	switch (event->event_id) {
	case SWITCH_EVENT_NOTIFY:
		{
			const char *profile_name = switch_event_get_header(event, "profile");
			const char *ct = switch_event_get_header(event, "content-type");
			const char *es = switch_event_get_header(event, "event-string");
			const char *user = switch_event_get_header(event, "user");
			const char *host = switch_event_get_header(event, "host");
			const char *call_id = switch_event_get_header(event, "call-id");
			const char *uuid = switch_event_get_header(event, "uuid");
			const char *body = switch_event_get_body(event);
			const char *to_uri = switch_event_get_header(event, "to-uri");
			const char *from_uri = switch_event_get_header(event, "from-uri");
			sofia_profile_t *profile;


			if (to_uri || from_uri) {
				
				if (!to_uri) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Missing To-URI header\n");
					return;
				}
				
				if (!from_uri) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Missing From-URI header\n");
					return;
				}
				
				if (!es) {
					es = "message-summary";
				}

				if (!ct) {
					ct = "application/simple-message-summary";
				}

				if (!profile_name) {
					profile_name = "default";
				}
				
				if (!(profile = sofia_glue_find_profile(profile_name))) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Can't find profile %s\n", profile_name);
					return;
				}

				
				if (to_uri && from_uri && ct && es && profile_name && (profile = sofia_glue_find_profile(profile_name))) {
					nua_handle_t *nh = nua_handle(profile->nua, 
												  NULL, 
												  NUTAG_URL(to_uri), 
												  SIPTAG_FROM_STR(from_uri), 
												  SIPTAG_TO_STR(to_uri), 
												  SIPTAG_CONTACT_STR(profile->url), 
												  TAG_END());
				
					nua_handle_bind(nh, &mod_sofia_globals.destroy_private);
				
					nua_notify(nh,
							   NUTAG_NEWSUB(1),
							   NUTAG_WITH_THIS(profile->nua),
							   SIPTAG_EVENT_STR(es), 
							   TAG_IF(ct, SIPTAG_CONTENT_TYPE_STR(ct)),
							   TAG_IF(!switch_strlen_zero(body), SIPTAG_PAYLOAD_STR(body)),
							   TAG_END());
				
					sofia_glue_release_profile(profile);
				}

				return;
			}

			if (uuid && ct && es) {
				switch_core_session_t *session;
				private_object_t *tech_pvt;

				if ((session = switch_core_session_locate(uuid))) {
					if ((tech_pvt = switch_core_session_get_private(session))) {
						nua_notify(tech_pvt->nh,
								   NUTAG_NEWSUB(1),
								   SIPTAG_EVENT_STR(es), 
								   SIPTAG_CONTENT_TYPE_STR(ct), 
								   TAG_IF(!switch_strlen_zero(body), SIPTAG_PAYLOAD_STR(body)),
								   TAG_END());
					}
					switch_core_session_rwunlock(session);
				}
			} else if (profile_name && ct && es && user && host && (profile = sofia_glue_find_profile(profile_name))) {
				char *sql;

				if (call_id) {
					sql = switch_mprintf("select sip_user,sip_host,contact,profile_name,'%q','%q','%q' "
										 "from sip_registrations where call_id='%q'", ct, es, switch_str_nil(body), call_id
										 );
				} else {
					if (!strcasecmp(es, "message-summary")) {
						sql = switch_mprintf("select sip_user,sip_host,contact,profile_name,'%q','%q','%q' "
								"from sip_registrations where mwi_user='%s' and mwi_host='%q'",
								ct, es, switch_str_nil(body), switch_str_nil(user), switch_str_nil(host)
								);
					} else {
						sql = switch_mprintf("select sip_user,sip_host,contact,profile_name,'%q','%q','%q' "
								"from sip_registrations where sip_user='%s' and sip_host='%q'",
								ct, es, switch_str_nil(body), switch_str_nil(user), switch_str_nil(host)
								);

					}
				}
				

				switch_mutex_lock(profile->ireg_mutex);
				sofia_glue_execute_sql_callback(profile, SWITCH_TRUE, NULL, sql, notify_callback, profile);
				switch_mutex_unlock(profile->ireg_mutex);
				sofia_glue_release_profile(profile);

				free(sql);
			}
			
		}
		break;
	case SWITCH_EVENT_SEND_MESSAGE:
		{
			const char *profile_name = switch_event_get_header(event, "profile");
			const char *ct = switch_event_get_header(event, "content-type");
			const char *user = switch_event_get_header(event, "user");
			const char *host = switch_event_get_header(event, "host");
			const char *body = switch_event_get_body(event);
			sofia_profile_t *profile;
			nua_handle_t *nh;

			if (profile_name && ct && user && host) {
				char *id = NULL;
				char *contact, *p;
				char buf[512] = "";

				if (!(profile = sofia_glue_find_profile(profile_name))) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Can't find profile %s\n", profile_name);
					return;
				}


				if (!sofia_reg_find_reg_url(profile, user, host, buf, sizeof(buf))) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Can't find user %s@%s\n", user, host);
					return;
				}

				id = switch_mprintf("sip:%s@%s", user, host);

				switch_assert(id);
				contact = sofia_glue_get_url_from_contact(buf, 0);
				
				if ((p = strstr(contact, ";fs_"))) {
					*p = '\0';
				}
				
				nh = nua_handle(profile->nua, 
								NULL, 
								NUTAG_URL(contact), 
								SIPTAG_FROM_STR(id), 
								SIPTAG_TO_STR(id), 
								SIPTAG_CONTACT_STR(profile->url), 
								TAG_END());
				
				nua_message(nh,
							NUTAG_NEWSUB(1),
 							SIPTAG_CONTENT_TYPE_STR(ct), 
							TAG_IF(!switch_strlen_zero(body), SIPTAG_PAYLOAD_STR(body)),
							TAG_END());

				
				free(id);
				sofia_glue_release_profile(profile);
			}

		}
		break;
	case SWITCH_EVENT_SEND_INFO:
		{
			const char *profile_name = switch_event_get_header(event, "profile");
			const char *ct = switch_event_get_header(event, "content-type");
			const char *to_uri = switch_event_get_header(event, "to-uri");
			const char *local_user_full = switch_event_get_header(event, "local-user");
			const char *from_uri = switch_event_get_header(event, "from-uri");
			const char *call_info = switch_event_get_header(event, "call-info");
			const char *alert_info = switch_event_get_header(event, "alert-info");
			const char *call_id = switch_event_get_header(event, "call-id");
			const char *body = switch_event_get_body(event);
			sofia_profile_t *profile = NULL;
			nua_handle_t *nh;
			char *local_dup = NULL;
			char *local_user, *local_host;
			char buf[1024] = "";
			char *p;

			if (!profile_name) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Missing Profile Name\n");
                goto done;
            }

            if (!call_id && !to_uri && !local_user_full) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Missing To-URI header\n");
                goto done;
            }

            if (!call_id && !from_uri) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Missing From-URI header\n");
                goto done;
            }


            if (!(profile = sofia_glue_find_profile(profile_name))) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Can't find profile %s\n", profile_name);
                goto done;
            }

			if (call_id) {
				nh = nua_handle_by_call_id(profile->nua, call_id);

				if (!nh) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid Call-ID %s\n", call_id);
					goto done;
				}
			}

			else {
				if (local_user_full) {
					local_dup = strdup(local_user_full);
					local_user = local_dup;
					if ((local_host = strchr(local_user, '@'))) {
						*local_host++ = '\0';
					}

					if (!local_user || !local_host || !sofia_reg_find_reg_url(profile, local_user, local_host, buf, sizeof(buf))) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Can't find local user\n");
						goto done;
					}

					to_uri = sofia_glue_get_url_from_contact(buf, 0);
					
					if ((p = strstr(to_uri, ";fs_"))) {
						*p = '\0';
					}

				}


				nh = nua_handle(profile->nua, 
								NULL, 
								NUTAG_URL(to_uri), 
								SIPTAG_FROM_STR(from_uri), 
								SIPTAG_TO_STR(to_uri), 
								SIPTAG_CONTACT_STR(profile->url), 
								TAG_END());
				
				nua_handle_bind(nh, &mod_sofia_globals.destroy_private);
			}

			nua_info(nh,
					 NUTAG_WITH_THIS(profile->nua),
					 TAG_IF(ct, SIPTAG_CONTENT_TYPE_STR(ct)),
					 TAG_IF(alert_info, SIPTAG_ALERT_INFO_STR(alert_info)),
					 TAG_IF(call_info, SIPTAG_CALL_INFO_STR(call_info)),
					 TAG_IF(!switch_strlen_zero(body), SIPTAG_PAYLOAD_STR(body)),
					 TAG_END());

			if (call_id && nh) {
				nua_handle_unref(nh);
			}

			if (profile) {
				sofia_glue_release_profile(profile);
			}
			
		done:

			switch_safe_free(local_dup);
			
		}
		break;
	case SWITCH_EVENT_TRAP:
		{
			const char *cond = switch_event_get_header(event, "condition");

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "EVENT_TRAP: IP change detected\n");

			if (cond && !strcmp(cond, "network-address-change") && mod_sofia_globals.auto_restart) {
				const char *old_ip4 = switch_event_get_header_nil(event, "network-address-previous-v4");
				const char *new_ip4 = switch_event_get_header_nil(event, "network-address-change-v4");
				const char *old_ip6 = switch_event_get_header_nil(event, "network-address-previous-v6");
				const char *new_ip6 = switch_event_get_header_nil(event, "network-address-change-v6");
				switch_hash_index_t *hi;
				const void *var;
				void *val;
				sofia_profile_t *profile;

				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "IP change detected [%s]->[%s] [%s]->[%s]\n", old_ip4, new_ip4, old_ip6, new_ip6);

				switch_mutex_lock(mod_sofia_globals.hash_mutex);
				if (mod_sofia_globals.profile_hash) {
					for (hi = switch_hash_first(NULL, mod_sofia_globals.profile_hash); hi; hi = switch_hash_next(hi)) {
						int rb = 0;
						switch_hash_this(hi, &var, NULL, &val);
						if ((profile = (sofia_profile_t *) val) && profile->auto_restart) {
							if (!strcmp(profile->sipip, old_ip4)) {
								profile->sipip = switch_core_strdup(profile->pool, new_ip4);
								rb++;
							}
							if (!strcmp(profile->rtpip, old_ip4)) {
								profile->rtpip = switch_core_strdup(profile->pool, new_ip4);
								rb++;
							}
							if (!strcmp(profile->sipip, old_ip6)) {
								profile->sipip = switch_core_strdup(profile->pool, new_ip6);
								rb++;
							}
							if (!strcmp(profile->rtpip, old_ip6)) {
								profile->rtpip = switch_core_strdup(profile->pool, new_ip6);
								rb++;
							}

							if (rb) {
								sofia_set_pflag_locked(profile, PFLAG_RESPAWN);
								sofia_clear_pflag_locked(profile, PFLAG_RUNNING);
							}
						}
					}
				}
				switch_mutex_unlock(mod_sofia_globals.hash_mutex);
				sofia_glue_restart_all_profiles();
			}
			
		}
		break;
	default:
		break;
	}
}

SWITCH_MODULE_LOAD_FUNCTION(mod_sofia_load)
{
	switch_chat_interface_t *chat_interface;
	switch_api_interface_t *api_interface;
	switch_management_interface_t *management_interface;
	struct in_addr in;

	silence_frame.data = silence_data;
	silence_frame.datalen = sizeof(silence_data);
	silence_frame.buflen = sizeof(silence_data);
	silence_frame.flags = SFF_CNG;

	memset(&mod_sofia_globals, 0, sizeof(mod_sofia_globals));
	mod_sofia_globals.destroy_private.destroy_nh = 1;
	mod_sofia_globals.destroy_private.is_static = 1;
	mod_sofia_globals.keep_private.is_static = 1;
	mod_sofia_globals.pool = pool;
	switch_mutex_init(&mod_sofia_globals.mutex, SWITCH_MUTEX_NESTED, mod_sofia_globals.pool);

	switch_find_local_ip(mod_sofia_globals.guess_ip, sizeof(mod_sofia_globals.guess_ip), &mod_sofia_globals.guess_mask, AF_INET);
	in.s_addr = mod_sofia_globals.guess_mask;
	switch_set_string(mod_sofia_globals.guess_mask_str, inet_ntoa(in));
	gethostname(mod_sofia_globals.hostname, sizeof(mod_sofia_globals.hostname));


	switch_core_hash_init(&mod_sofia_globals.profile_hash, mod_sofia_globals.pool);
	switch_core_hash_init(&mod_sofia_globals.gateway_hash, mod_sofia_globals.pool);
	switch_mutex_init(&mod_sofia_globals.hash_mutex, SWITCH_MUTEX_NESTED, mod_sofia_globals.pool);

	switch_mutex_lock(mod_sofia_globals.mutex);
	mod_sofia_globals.running = 1;
	switch_mutex_unlock(mod_sofia_globals.mutex);

	mod_sofia_globals.auto_nat = (switch_core_get_variable("nat_type") ? 1 : 0);

	switch_queue_create(&mod_sofia_globals.presence_queue, SOFIA_QUEUE_SIZE, mod_sofia_globals.pool);
	switch_queue_create(&mod_sofia_globals.mwi_queue, SOFIA_QUEUE_SIZE, mod_sofia_globals.pool);

	if (config_sofia(0, NULL) != SWITCH_STATUS_SUCCESS) {
		mod_sofia_globals.running = 0;
		return SWITCH_STATUS_GENERR;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Waiting for profiles to start\n");
	switch_yield(1500000);

	if (switch_event_bind_removable(modname, SWITCH_EVENT_CUSTOM, MULTICAST_EVENT, event_handler, NULL,
									&mod_sofia_globals.custom_node) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't bind!\n");
		return SWITCH_STATUS_TERM;
	}

	if (switch_event_bind_removable(modname, SWITCH_EVENT_PRESENCE_IN, SWITCH_EVENT_SUBCLASS_ANY, sofia_presence_event_handler, NULL,
									&mod_sofia_globals.in_node) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't bind!\n");
		return SWITCH_STATUS_GENERR;
	}

	if (switch_event_bind_removable(modname, SWITCH_EVENT_PRESENCE_OUT, SWITCH_EVENT_SUBCLASS_ANY, sofia_presence_event_handler, NULL,
									&mod_sofia_globals.out_node) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't bind!\n");
		return SWITCH_STATUS_GENERR;
	}

	if (switch_event_bind_removable(modname, SWITCH_EVENT_PRESENCE_PROBE, SWITCH_EVENT_SUBCLASS_ANY, sofia_presence_event_handler, NULL,
									&mod_sofia_globals.probe_node) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't bind!\n");
		return SWITCH_STATUS_GENERR;
	}

	if (switch_event_bind_removable(modname, SWITCH_EVENT_ROSTER, SWITCH_EVENT_SUBCLASS_ANY, sofia_presence_event_handler, NULL,
									&mod_sofia_globals.roster_node) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't bind!\n");
		return SWITCH_STATUS_GENERR;
	}

	if (switch_event_bind_removable(modname, SWITCH_EVENT_MESSAGE_WAITING, SWITCH_EVENT_SUBCLASS_ANY, sofia_presence_mwi_event_handler, NULL,
									&mod_sofia_globals.mwi_node) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't bind!\n");
		return SWITCH_STATUS_GENERR;
	}

	if (switch_event_bind(modname, SWITCH_EVENT_TRAP, SWITCH_EVENT_SUBCLASS_ANY, general_event_handler, NULL) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't bind!\n");
		return SWITCH_STATUS_GENERR;
	}

	if (switch_event_bind(modname, SWITCH_EVENT_NOTIFY, SWITCH_EVENT_SUBCLASS_ANY, general_event_handler, NULL) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't bind!\n");
		return SWITCH_STATUS_GENERR;
	}

	if (switch_event_bind(modname, SWITCH_EVENT_SEND_MESSAGE, SWITCH_EVENT_SUBCLASS_ANY, general_event_handler, NULL) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't bind!\n");
		return SWITCH_STATUS_GENERR;
	}

	if (switch_event_bind(modname, SWITCH_EVENT_SEND_INFO, SWITCH_EVENT_SUBCLASS_ANY, general_event_handler, NULL) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't bind!\n");
		return SWITCH_STATUS_GENERR;
	}

	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);
	sofia_endpoint_interface = switch_loadable_module_create_interface(*module_interface, SWITCH_ENDPOINT_INTERFACE);
	sofia_endpoint_interface->interface_name = "sofia";
	sofia_endpoint_interface->io_routines = &sofia_io_routines;
	sofia_endpoint_interface->state_handler = &sofia_event_handlers;

	management_interface = switch_loadable_module_create_interface(*module_interface, SWITCH_MANAGEMENT_INTERFACE);
	management_interface->relative_oid = "1";
	management_interface->management_function = sofia_manage;

	SWITCH_ADD_API(api_interface, "sofia", "Sofia Controls", sofia_function, "<cmd> <args>");
	SWITCH_ADD_API(api_interface, "sofia_gateway_data", "Get data from a sofia gateway", sofia_gateway_data_function, "<gateway_name> [ivar|ovar|var] <name>");
	switch_console_set_complete("add sofia help");
	switch_console_set_complete("add sofia status");
	switch_console_set_complete("add sofia loglevel");
	switch_console_set_complete("add sofia profile");
	switch_console_set_complete("add sofia profile restart all");

	SWITCH_ADD_API(api_interface, "sofia_contact", "Sofia Contacts", sofia_contact_function, "[profile/]<user>@<domain>");
	SWITCH_ADD_CHAT(chat_interface, SOFIA_CHAT_PROTO, sofia_presence_chat_send);

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_sofia_shutdown)
{
	int sanity = 0;

	switch_mutex_lock(mod_sofia_globals.mutex);
	if (mod_sofia_globals.running == 1) {
		mod_sofia_globals.running = 0;
	}
	switch_mutex_unlock(mod_sofia_globals.mutex);

	switch_event_unbind(&mod_sofia_globals.in_node);
	switch_event_unbind(&mod_sofia_globals.probe_node);
	switch_event_unbind(&mod_sofia_globals.out_node);
	switch_event_unbind(&mod_sofia_globals.roster_node);
	switch_event_unbind(&mod_sofia_globals.custom_node);
	switch_event_unbind(&mod_sofia_globals.mwi_node);
	switch_event_unbind_callback(general_event_handler);
	
	while (mod_sofia_globals.threads) {
		switch_cond_next();
		if (++sanity >= 60000) {
			break;
		}
	}

	//switch_yield(1000000);
	su_deinit();

	switch_mutex_lock(mod_sofia_globals.hash_mutex);
	switch_core_hash_destroy(&mod_sofia_globals.profile_hash);
	switch_core_hash_destroy(&mod_sofia_globals.gateway_hash);
	switch_mutex_unlock(mod_sofia_globals.hash_mutex);

	return SWITCH_STATUS_SUCCESS;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4:
 */

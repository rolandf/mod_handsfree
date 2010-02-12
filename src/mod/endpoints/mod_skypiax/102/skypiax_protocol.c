#include "skypiax.h"

#ifdef ASTERISK
#define skypiax_sleep usleep
#define skypiax_strncpy strncpy
#define tech_pvt p
extern int skypiax_debug;
extern char *skypiax_console_active;
#else /* FREESWITCH */
#define skypiax_sleep switch_sleep
#define skypiax_strncpy switch_copy_string
extern switch_memory_pool_t *skypiax_module_pool;
extern switch_endpoint_interface_t *skypiax_endpoint_interface;
#endif /* ASTERISK */
int samplerate_skypiax = SAMPLERATE_SKYPIAX;

extern int running;

/*************************************/
/* suspicious globals FIXME */
#ifdef WIN32
DWORD win32_dwThreadId;
#else
XErrorHandler old_handler = 0;
int xerror = 0;
#endif /* WIN32 */
/*************************************/
#ifndef WIN32
int skypiax_socket_create_and_bind(private_t * tech_pvt, int *which_port)
#else
int skypiax_socket_create_and_bind(private_t * tech_pvt, unsigned short *which_port)
#endif							//WIN32
{
	int s = -1;
	struct sockaddr_in my_addr;
#ifndef WIN32
	int start_port = 6001;
#else
	unsigned short start_port = 6001;
#endif //WIN32
	int sockbufsize = 0;
	unsigned int size = sizeof(int);


	memset(&my_addr, 0, sizeof(my_addr));
	my_addr.sin_family = AF_INET;
	my_addr.sin_addr.s_addr = htonl(0x7f000001);	/* use the localhost */

	if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		ERRORA("socket Error\n", SKYPIAX_P_LOG);
		return -1;
	}

	if (*which_port != 0)
		start_port = *which_port;

	my_addr.sin_port = htons(start_port);
/* NONBLOCKING ? */
	//fcntl(s, F_SETFL, O_NONBLOCK);

	*which_port = start_port;
	while (bind(s, (struct sockaddr *) &my_addr, sizeof(struct sockaddr)) < 0) {
		DEBUGA_SKYPE("*which_port=%d, tech_pvt->tcp_cli_port=%d, tech_pvt->tcp_srv_port=%d\n", SKYPIAX_P_LOG, *which_port, tech_pvt->tcp_cli_port,
					 tech_pvt->tcp_srv_port);
		DEBUGA_SKYPE("bind errno=%d, error: %s\n", SKYPIAX_P_LOG, errno, strerror(errno));
		start_port++;
		my_addr.sin_port = htons(start_port);
		*which_port = start_port;
		DEBUGA_SKYPE("*which_port=%d, tech_pvt->tcp_cli_port=%d, tech_pvt->tcp_srv_port=%d\n", SKYPIAX_P_LOG, *which_port, tech_pvt->tcp_cli_port,
					 tech_pvt->tcp_srv_port);

		if (start_port > 65000) {
			ERRORA("NO MORE PORTS! *which_port=%d, tech_pvt->tcp_cli_port=%d, tech_pvt->tcp_srv_port=%d\n", SKYPIAX_P_LOG, *which_port,
				   tech_pvt->tcp_cli_port, tech_pvt->tcp_srv_port);
			return -1;
		}
	}

	DEBUGA_SKYPE("SUCCESS! *which_port=%d, tech_pvt->tcp_cli_port=%d, tech_pvt->tcp_srv_port=%d\n", SKYPIAX_P_LOG, *which_port, tech_pvt->tcp_cli_port,
				 tech_pvt->tcp_srv_port);

	sockbufsize = 0;
	size = sizeof(int);
	getsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *) &sockbufsize, &size);
	DEBUGA_SKYPE("1 SO_RCVBUF is %d, size is %d\n", SKYPIAX_P_LOG, sockbufsize, size);
	sockbufsize = 0;
	size = sizeof(int);
	getsockopt(s, SOL_SOCKET, SO_SNDBUF, (char *) &sockbufsize, &size);
	DEBUGA_SKYPE("1 SO_SNDBUF is %d, size is %d\n", SKYPIAX_P_LOG, sockbufsize, size);



/* for virtual machines, eg: Linux domU-12-31-39-02-68-28 2.6.18-xenU-ec2-v1.0 #2 SMP Tue Feb 19 10:51:53 EST 2008 i686 athlon i386 GNU/Linux
 * use:
 * sockbufsize=SAMPLES_PER_FRAME * 8;
 */
#ifdef WIN32
	sockbufsize = SAMPLES_PER_FRAME * 8 * 3;
#else
	sockbufsize = SAMPLES_PER_FRAME * 8;
#endif //WIN32
	size = sizeof(int);
	setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *) &sockbufsize, size);

	sockbufsize = 0;
	size = sizeof(int);
	getsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *) &sockbufsize, &size);
	DEBUGA_SKYPE("2 SO_RCVBUF is %d, size is %d\n", SKYPIAX_P_LOG, sockbufsize, size);

/* for virtual machines, eg: Linux domU-12-31-39-02-68-28 2.6.18-xenU-ec2-v1.0 #2 SMP Tue Feb 19 10:51:53 EST 2008 i686 athlon i386 GNU/Linux
 * use:
 * sockbufsize=SAMPLES_PER_FRAME * 8;
 */
#ifdef WIN32
	sockbufsize = SAMPLES_PER_FRAME * 8 * 3;
#else
	sockbufsize = SAMPLES_PER_FRAME * 8;
#endif //WIN32
	size = sizeof(int);
	setsockopt(s, SOL_SOCKET, SO_SNDBUF, (char *) &sockbufsize, size);


	sockbufsize = 0;
	size = sizeof(int);
	getsockopt(s, SOL_SOCKET, SO_SNDBUF, (char *) &sockbufsize, &size);
	DEBUGA_SKYPE("2 SO_SNDBUF is %d, size is %d\n", SKYPIAX_P_LOG, sockbufsize, size);



	return s;
}

int skypiax_signaling_read(private_t * tech_pvt)
{
	char read_from_pipe[4096];
	char message[4096];
	char message_2[4096];
	char *buf, obj[512] = "", id[512] = "", prop[512] = "", value[512] = "", *where;
	char **stringp = NULL;
	int a;
	unsigned int howmany;
	unsigned int i;

	memset(read_from_pipe, 0, 4096);
	memset(message, 0, 4096);
	memset(message_2, 0, 4096);

	howmany = skypiax_pipe_read(tech_pvt->SkypiaxHandles.fdesc[0], (short *) read_from_pipe, sizeof(read_from_pipe));

	a = 0;
	for (i = 0; i < howmany; i++) {
		message[a] = read_from_pipe[i];
		a++;

		if (read_from_pipe[i] == '\0') {

			//if (!strstr(message, "DURATION")) {
			DEBUGA_SKYPE("READING: |||%s||| \n", SKYPIAX_P_LOG, message);
			//}

			if (!strcasecmp(message, "ERROR 68")) {
				DEBUGA_SKYPE
					("If I don't connect immediately, please give the Skype client authorization to be connected by Skypiax (and to not ask you again)\n",
					 SKYPIAX_P_LOG);
				skypiax_sleep(1000000);
				skypiax_signaling_write(tech_pvt, "PROTOCOL 7");
				skypiax_sleep(10000);
				return 0;
			}
			if (!strncasecmp(message, "ERROR 92 CALL", 12)) {
				ERRORA("Skype got ERROR: |||%s|||, the (skypeout) number we called was not recognized as valid\n", SKYPIAX_P_LOG, message);
				tech_pvt->skype_callflow = CALLFLOW_STATUS_FINISHED;
				DEBUGA_SKYPE("skype_call now is DOWN\n", SKYPIAX_P_LOG);
				tech_pvt->skype_call_id[0] = '\0';

				if (tech_pvt->interface_state != SKYPIAX_STATE_HANGUP_REQUESTED) {
					tech_pvt->interface_state = SKYPIAX_STATE_DOWN;
					return CALLFLOW_INCOMING_HANGUP;
				} else {
					tech_pvt->interface_state = SKYPIAX_STATE_DOWN;
				}
			}

			if (!strncasecmp(message, "ERROR", 4)) {
				if (!strncasecmp(message, "ERROR 96 CALL", 12)) {
					DEBUGA_SKYPE
						("Skype got ERROR: |||%s|||, we are trying to use this interface to make or receive a call, but another call is half-active on this interface. Let's the previous one to continue.\n",
						 SKYPIAX_P_LOG, message);
				} else if (!strncasecmp(message, "ERROR 99 CALL", 12)) {
					ERRORA("Skype got ERROR: |||%s|||, another call is active on this interface\n\n\n", SKYPIAX_P_LOG, message);
					tech_pvt->interface_state = SKYPIAX_STATE_ERROR_DOUBLE_CALL;
				} else if (!strncasecmp(message, "ERROR 592 ALTER CALL", 19)) {
					ERRORA("Skype got ERROR about TRANSFERRING, no problem: |||%s|||\n", SKYPIAX_P_LOG, message);
				} else if (!strncasecmp(message, "ERROR 559 CALL", 13)) {
					DEBUGA_SKYPE("Skype got ERROR about a failed action (probably TRYING to HANGUP A CALL), no problem: |||%s|||\n", SKYPIAX_P_LOG,
								 message);
				} else {
					ERRORA("Skype got ERROR: |||%s|||\n", SKYPIAX_P_LOG, message);
					tech_pvt->skype_callflow = CALLFLOW_STATUS_FINISHED;
					ERRORA("skype_call now is DOWN\n", SKYPIAX_P_LOG);
					tech_pvt->skype_call_id[0] = '\0';

					if (tech_pvt->interface_state != SKYPIAX_STATE_HANGUP_REQUESTED) {
						tech_pvt->interface_state = SKYPIAX_STATE_DOWN;
						return CALLFLOW_INCOMING_HANGUP;
					} else {
						tech_pvt->interface_state = SKYPIAX_STATE_DOWN;
					}
				}
			}




			skypiax_strncpy(message_2, message, sizeof(message) - 1);
			buf = message;
			stringp = &buf;
			where = strsep(stringp, " ");
			if (!where) {
				WARNINGA("Skype MSG without spaces: %s\n", SKYPIAX_P_LOG, message);
			}








			if (!strcasecmp(message, "CURRENTUSERHANDLE")) {
				skypiax_strncpy(obj, where, sizeof(obj) - 1);
				where = strsep(stringp, " ");
				skypiax_strncpy(id, where, sizeof(id) - 1);
				if (!strcasecmp(id, tech_pvt->skype_user)) {
					tech_pvt->SkypiaxHandles.currentuserhandle = 1;
					DEBUGA_SKYPE
						("Skype MSG: message: %s, currentuserhandle: %s, cuh: %s, skype_user: %s!\n",
						 SKYPIAX_P_LOG, message, obj, id, tech_pvt->skype_user);
				}
			}
			if (!strcasecmp(message, "USER")) {
				skypiax_strncpy(obj, where, sizeof(obj) - 1);
				where = strsep(stringp, " ");
				skypiax_strncpy(id, where, sizeof(id) - 1);
				where = strsep(stringp, " ");
				skypiax_strncpy(prop, where, sizeof(prop) - 1);
				if (!strcasecmp(prop, "RECEIVEDAUTHREQUEST")) {
					char msg_to_skype[256];
					DEBUGA_SKYPE("Skype MSG: message: %s, obj: %s, id: %s, prop: %s!\n", SKYPIAX_P_LOG, message, obj, id, prop);
					//TODO: allow authorization based on config param
					sprintf(msg_to_skype, "SET USER %s ISAUTHORIZED TRUE", id);
					skypiax_signaling_write(tech_pvt, msg_to_skype);
				}
			}
			if (!strcasecmp(message, "MESSAGE")) {
				skypiax_strncpy(obj, where, sizeof(obj) - 1);
				where = strsep(stringp, " ");
				skypiax_strncpy(id, where, sizeof(id) - 1);
				where = strsep(stringp, " ");
				skypiax_strncpy(prop, where, sizeof(prop) - 1);
				if (!strcasecmp(prop, "STATUS")) {
					where = strsep(stringp, " ");
					skypiax_strncpy(value, where, sizeof(value) - 1);
					if (!strcasecmp(value, "RECEIVED")) {
						char msg_to_skype[256];
						DEBUGA_SKYPE("Skype MSG: message: %s, obj: %s, id: %s, prop: %s value: %s!\n", SKYPIAX_P_LOG, message, obj, id, prop, value);
						//TODO: authomatically flag messages as read based on config param
						sprintf(msg_to_skype, "SET MESSAGE %s SEEN", id);
						skypiax_signaling_write(tech_pvt, msg_to_skype);
					}
				} else if (!strcasecmp(prop, "BODY")) {
					char msg_to_skype[256];
					DEBUGA_SKYPE("Skype MSG: message: %s, obj: %s, id: %s, prop: %s!\n", SKYPIAX_P_LOG, message, obj, id, prop);
					//TODO: authomatically flag messages as read based on config param
					sprintf(msg_to_skype, "SET MESSAGE %s SEEN", id);
					skypiax_signaling_write(tech_pvt, msg_to_skype);
				}
			}
			if (!strcasecmp(message, "CHAT")) {
				char msg_to_skype[256];
				int i;
				int found;

				skypiax_strncpy(obj, where, sizeof(obj) - 1);
				where = strsep(stringp, " ");
				skypiax_strncpy(id, where, sizeof(id) - 1);
				where = strsep(stringp, " ");
				skypiax_strncpy(prop, where, sizeof(prop) - 1);
				skypiax_strncpy(value, *stringp, sizeof(value) - 1);

				if (!strcasecmp(prop, "STATUS") && !strcasecmp(value, "DIALOG")) {
					DEBUGA_SKYPE("CHAT %s is DIALOG\n", SKYPIAX_P_LOG, id);
					sprintf(msg_to_skype, "GET CHAT %s DIALOG_PARTNER", id);
					skypiax_signaling_write(tech_pvt, msg_to_skype);
				}

				if (!strcasecmp(prop, "DIALOG_PARTNER")) {
					DEBUGA_SKYPE("CHAT %s has DIALOG_PARTNER %s\n", SKYPIAX_P_LOG, id, value);
					found = 0;
					for (i = 0; i < MAX_CHATS; i++) {
						if (strlen(tech_pvt->chats[i].chatname) == 0 || !strcmp(tech_pvt->chats[i].chatname, id)) {
							strncpy(tech_pvt->chats[i].chatname, id, sizeof(tech_pvt->chats[i].chatname));
							strncpy(tech_pvt->chats[i].dialog_partner, value, sizeof(tech_pvt->chats[i].dialog_partner));
							found = 1;
							break;
						}
					}
					if (!found) {
						DEBUGA_SKYPE("why we do not have a chats slot free? we have more than %d chats in parallel?\n", SKYPIAX_P_LOG, MAX_CHATS);
					}

					DEBUGA_SKYPE("CHAT %s is in position %d in the chats array, chatname=%s, dialog_partner=%s\n", SKYPIAX_P_LOG, id, i,
								 tech_pvt->chats[i].chatname, tech_pvt->chats[i].dialog_partner);
				}

			}


			if (!strcasecmp(message, "CHATMESSAGE")) {
				char msg_to_skype[256];
				int i;
				int found;

				skypiax_strncpy(obj, where, sizeof(obj) - 1);
				where = strsep(stringp, " ");
				skypiax_strncpy(id, where, sizeof(id) - 1);
				where = strsep(stringp, " ");
				skypiax_strncpy(prop, where, sizeof(prop) - 1);
				skypiax_strncpy(value, *stringp, sizeof(value) - 1);

				if (!strcasecmp(prop, "STATUS") && !strcasecmp(value, "RECEIVED")) {
					DEBUGA_SKYPE("RECEIVED CHATMESSAGE %s, let's see which type it is\n", SKYPIAX_P_LOG, id);
					sprintf(msg_to_skype, "GET CHATMESSAGE %s TYPE", id);
					skypiax_signaling_write(tech_pvt, msg_to_skype);
				}

				if (!strcasecmp(prop, "TYPE") && !strcasecmp(value, "SAID")) {
					DEBUGA_SKYPE("CHATMESSAGE %s is of type SAID, let's get the other infos\n", SKYPIAX_P_LOG, id);
					found = 0;
					for (i = 0; i < MAX_CHATMESSAGES; i++) {
						if (strlen(tech_pvt->chatmessages[i].id) == 0) {
							strncpy(tech_pvt->chatmessages[i].id, id, sizeof(tech_pvt->chatmessages[i].id));
							strncpy(tech_pvt->chatmessages[i].type, value, sizeof(tech_pvt->chatmessages[i].type));
							found = 1;
							break;
						}
					}
					if (!found) {
						DEBUGA_SKYPE("why we do not have a chatmessages slot free? we have more than %d chatmessages in parallel?\n", SKYPIAX_P_LOG,
									 MAX_CHATMESSAGES);
					} else {
						DEBUGA_SKYPE("CHATMESSAGE %s is in position %d in the chatmessages array, type=%s, id=%s\n", SKYPIAX_P_LOG, id, i,
									 tech_pvt->chatmessages[i].type, tech_pvt->chatmessages[i].id);
						sprintf(msg_to_skype, "GET CHATMESSAGE %s CHATNAME", id);
						skypiax_signaling_write(tech_pvt, msg_to_skype);
						skypiax_sleep(1000);
						sprintf(msg_to_skype, "GET CHATMESSAGE %s FROM_HANDLE", id);
						skypiax_signaling_write(tech_pvt, msg_to_skype);
						skypiax_sleep(1000);
						sprintf(msg_to_skype, "GET CHATMESSAGE %s FROM_DISPNAME", id);
						skypiax_signaling_write(tech_pvt, msg_to_skype);
						skypiax_sleep(1000);
						sprintf(msg_to_skype, "GET CHATMESSAGE %s BODY", id);
						skypiax_signaling_write(tech_pvt, msg_to_skype);
					}
				}

				if (!strcasecmp(prop, "CHATNAME")) {
					DEBUGA_SKYPE("CHATMESSAGE %s belongs to the CHAT %s\n", SKYPIAX_P_LOG, id, value);
					found = 0;
					for (i = 0; i < MAX_CHATMESSAGES; i++) {
						if (!strcmp(tech_pvt->chatmessages[i].id, id)) {
							strncpy(tech_pvt->chatmessages[i].chatname, value, sizeof(tech_pvt->chatmessages[i].chatname));
							found = 1;
							break;
						}
					}
					if (!found) {
						DEBUGA_SKYPE("why chatmessage %s was not found in the chatmessages array??\n", SKYPIAX_P_LOG, id);
					}
				}
				if (!strcasecmp(prop, "FROM_HANDLE")) {
					DEBUGA_SKYPE("CHATMESSAGE %s was sent by FROM_HANDLE %s\n", SKYPIAX_P_LOG, id, value);
					found = 0;
					for (i = 0; i < MAX_CHATMESSAGES; i++) {
						if (!strcmp(tech_pvt->chatmessages[i].id, id)) {
							strncpy(tech_pvt->chatmessages[i].from_handle, value, sizeof(tech_pvt->chatmessages[i].from_handle));
							found = 1;
							break;
						}
					}
					if (!found) {
						DEBUGA_SKYPE("why chatmessage %s was not found in the chatmessages array??\n", SKYPIAX_P_LOG, id);
					}

				}
				if (!strcasecmp(prop, "FROM_DISPNAME")) {
					DEBUGA_SKYPE("CHATMESSAGE %s was sent by FROM_DISPNAME %s\n", SKYPIAX_P_LOG, id, value);
					found = 0;
					for (i = 0; i < MAX_CHATMESSAGES; i++) {
						if (!strcmp(tech_pvt->chatmessages[i].id, id)) {
							strncpy(tech_pvt->chatmessages[i].from_dispname, value, sizeof(tech_pvt->chatmessages[i].from_dispname));
							found = 1;
							break;
						}
					}
					if (!found) {
						DEBUGA_SKYPE("why chatmessage %s was not found in the chatmessages array??\n", SKYPIAX_P_LOG, id);
					}

				}
				if (!strcasecmp(prop, "BODY")) {
					DEBUGA_SKYPE("CHATMESSAGE %s has BODY %s\n", SKYPIAX_P_LOG, id, value);
					found = 0;
					for (i = 0; i < MAX_CHATMESSAGES; i++) {
						if (!strcmp(tech_pvt->chatmessages[i].id, id)) {
							strncpy(tech_pvt->chatmessages[i].body, value, sizeof(tech_pvt->chatmessages[i].body));
							found = 1;
							break;
						}
					}
					if (!found) {
						DEBUGA_SKYPE("why chatmessage %s was not found in the chatmessages array??\n", SKYPIAX_P_LOG, id);
					} else {
						DEBUGA_SKYPE
							("CHATMESSAGE %s is in position %d in the chatmessages array, type=%s, id=%s, chatname=%s, from_handle=%s, from_dispname=%s, body=%s\n",
							 SKYPIAX_P_LOG, id, i, tech_pvt->chatmessages[i].type, tech_pvt->chatmessages[i].id, tech_pvt->chatmessages[i].chatname,
							 tech_pvt->chatmessages[i].from_handle, tech_pvt->chatmessages[i].from_dispname, tech_pvt->chatmessages[i].body);
						if (strcmp(tech_pvt->chatmessages[i].from_handle, tech_pvt->skype_user)) {	//if the message was not sent by myself
							incoming_chatmessage(tech_pvt, i);
						}
					}

				}

			}


			if (!strcasecmp(message, "CALL")) {
				skypiax_strncpy(obj, where, sizeof(obj) - 1);
				where = strsep(stringp, " ");
				skypiax_strncpy(id, where, sizeof(id) - 1);
				where = strsep(stringp, " ");
				skypiax_strncpy(prop, where, sizeof(prop) - 1);
				where = strsep(stringp, " ");
				skypiax_strncpy(value, where, sizeof(value) - 1);
				where = strsep(stringp, " ");

				//DEBUGA_SKYPE
				//("Skype MSG: message: %s, obj: %s, id: %s, prop: %s, value: %s,where: %s!\n",
				//SKYPIAX_P_LOG, message, obj, id, prop, value, where ? where : "NULL");

				if (!strcasecmp(prop, "PARTNER_HANDLE")) {
					if (tech_pvt->interface_state != SKYPIAX_STATE_SELECTED && (!strlen(tech_pvt->skype_call_id) || !strlen(tech_pvt->session_uuid_str))) {
						/* we are NOT inside an active call */
						DEBUGA_SKYPE("Call %s TRY ANSWER\n", SKYPIAX_P_LOG, id);
						skypiax_answer(tech_pvt, id, value);
					} else {
						/* we are inside an active call */
						if (!strcasecmp(tech_pvt->skype_call_id, id)) {
							/* this is the call in which we are calling out */
							DEBUGA_SKYPE("Call %s DO NOTHING\n", SKYPIAX_P_LOG, id);
						} else {
							skypiax_sleep(400000);	//0.4 seconds
							DEBUGA_SKYPE("Call %s TRY TRANSFER\n", SKYPIAX_P_LOG, id);
							skypiax_transfer(tech_pvt, id, value);
						}
					}
				}
				if (!strcasecmp(prop, "PARTNER_DISPNAME")) {
					snprintf(tech_pvt->callid_name, sizeof(tech_pvt->callid_name) - 1, "%s%s%s", value, where ? " " : "", where ? where : "");
					//DEBUGA_SKYPE
					//("the skype_call %s caller PARTNER_DISPNAME (tech_pvt->callid_name) is: %s\n",
					//SKYPIAX_P_LOG, id, tech_pvt->callid_name);
				}
				if (!strcasecmp(prop, "CONF_ID") && !strcasecmp(value, "0")) {
					//DEBUGA_SKYPE("the skype_call %s is NOT a conference call\n", SKYPIAX_P_LOG, id);
					//if (tech_pvt->interface_state == SKYPIAX_STATE_DOWN)
					//tech_pvt->interface_state = SKYPIAX_STATE_PRERING;
				}
				if (!strcasecmp(prop, "CONF_ID") && strcasecmp(value, "0")) {
					DEBUGA_SKYPE("the skype_call %s is a conference call\n", SKYPIAX_P_LOG, id);
					//if (tech_pvt->interface_state == SKYPIAX_STATE_DOWN)
					//tech_pvt->interface_state = SKYPIAX_STATE_PRERING;
				}
				if (!strcasecmp(prop, "DTMF")) {
					DEBUGA_SKYPE("Call %s received a DTMF: %s\n", SKYPIAX_P_LOG, id, value);
					dtmf_received(tech_pvt, value);
				}
				if (!strcasecmp(prop, "FAILUREREASON")) {
					DEBUGA_SKYPE("Skype FAILED on skype_call %s. Let's wait for the FAILED message.\n", SKYPIAX_P_LOG, id);
				}
				if (!strcasecmp(prop, "DURATION") && (!strcasecmp(value, "1"))) {
					if (strcasecmp(id, tech_pvt->skype_call_id)) {
						skypiax_strncpy(tech_pvt->skype_call_id, id, sizeof(tech_pvt->skype_call_id) - 1);
						DEBUGA_SKYPE("We called a Skype contact and he answered us on skype_call: %s.\n", SKYPIAX_P_LOG, id);
					}
				}

				if (!strcasecmp(prop, "DURATION") && (tech_pvt->interface_state == SKYPIAX_STATE_ERROR_DOUBLE_CALL)) {
					char msg_to_skype[1024];
					skypiax_strncpy(tech_pvt->skype_call_id, id, sizeof(tech_pvt->skype_call_id) - 1);
					ERRORA("We are in a double call situation, trying to get out hanging up call id: %s.\n", SKYPIAX_P_LOG, id);
					sprintf(msg_to_skype, "ALTER CALL %s HANGUP", id);
					skypiax_signaling_write(tech_pvt, msg_to_skype);
					skypiax_sleep(10000);
					//return CALLFLOW_INCOMING_HANGUP;
				}

				if (!strcasecmp(prop, "STATUS")) {

					if (!strcasecmp(value, "RINGING")) {
						char msg_to_skype[1024];
						if ((tech_pvt->interface_state != SKYPIAX_STATE_SELECTED && tech_pvt->interface_state != SKYPIAX_STATE_DIALING)
							&& (!strlen(tech_pvt->skype_call_id) || !strlen(tech_pvt->session_uuid_str))) {
							/* we are NOT inside an active call */

							DEBUGA_SKYPE("NO ACTIVE calls in this moment, skype_call %s is RINGING, to ask PARTNER_HANDLE\n", SKYPIAX_P_LOG, id);
							sprintf(msg_to_skype, "GET CALL %s PARTNER_HANDLE", id);
							skypiax_signaling_write(tech_pvt, msg_to_skype);
							skypiax_sleep(10000);
						} else {
							/* we are inside an active call */
							if (!strcasecmp(tech_pvt->skype_call_id, id)) {
								/* this is the call in which we are calling out */
								tech_pvt->skype_callflow = CALLFLOW_STATUS_RINGING;
								tech_pvt->interface_state = SKYPIAX_STATE_RINGING;
								skypiax_strncpy(tech_pvt->skype_call_id, id, sizeof(tech_pvt->skype_call_id) - 1);
								DEBUGA_SKYPE("Our remote party in skype_call %s is RINGING\n", SKYPIAX_P_LOG, id);
								remote_party_is_ringing(tech_pvt);
							} else {
								DEBUGA_SKYPE
									("We are in another call, but skype_call %s is RINGING on us, let's ask PARTNER_HANDLE, so maybe we'll TRANSFER\n",
									 SKYPIAX_P_LOG, id);
								sprintf(msg_to_skype, "GET CALL %s PARTNER_HANDLE", id);
								skypiax_signaling_write(tech_pvt, msg_to_skype);
								skypiax_sleep(10000);
							}
						}
					} else if (!strcasecmp(value, "EARLYMEDIA")) {
						char msg_to_skype[1024];
						tech_pvt->skype_callflow = CALLFLOW_STATUS_EARLYMEDIA;
						tech_pvt->interface_state = SKYPIAX_STATE_DIALING;
						DEBUGA_SKYPE("Our remote party in skype_call %s is EARLYMEDIA\n", SKYPIAX_P_LOG, id);
						if (tech_pvt->tcp_cli_thread == NULL) {
							DEBUGA_SKYPE("START start_audio_threads\n", SKYPIAX_P_LOG);
							if (start_audio_threads(tech_pvt)) {
								ERRORA("start_audio_threads FAILED\n", SKYPIAX_P_LOG);
								return CALLFLOW_INCOMING_HANGUP;
							}
						}
						skypiax_sleep(1000);
						sprintf(msg_to_skype, "ALTER CALL %s SET_INPUT PORT=\"%d\"", id, tech_pvt->tcp_cli_port);
						skypiax_signaling_write(tech_pvt, msg_to_skype);
						sprintf(msg_to_skype, "#output ALTER CALL %s SET_OUTPUT PORT=\"%d\"", id, tech_pvt->tcp_srv_port);
						skypiax_signaling_write(tech_pvt, msg_to_skype);

						remote_party_is_early_media(tech_pvt);
					} else if (!strcasecmp(value, "MISSED")) {
						DEBUGA_SKYPE("We missed skype_call %s\n", SKYPIAX_P_LOG, id);
					} else if (!strcasecmp(value, "FINISHED")) {
						//DEBUGA_SKYPE("skype_call %s now is DOWN\n", SKYPIAX_P_LOG, id);
						if (!strcasecmp(tech_pvt->skype_call_id, id)) {
							//tech_pvt->skype_callflow = CALLFLOW_STATUS_FINISHED;
							DEBUGA_SKYPE("skype_call %s is MY call, now I'm going DOWN\n", SKYPIAX_P_LOG, id);
							//tech_pvt->skype_call_id[0] = '\0';
							if (tech_pvt->interface_state != SKYPIAX_STATE_HANGUP_REQUESTED) {
								//tech_pvt->interface_state = SKYPIAX_STATE_DOWN;
								return CALLFLOW_INCOMING_HANGUP;
							} else {
								tech_pvt->interface_state = SKYPIAX_STATE_DOWN;
							}
						} else {
							DEBUGA_SKYPE("skype_call %s is NOT MY call, ignoring\n", SKYPIAX_P_LOG, id);
						}

					} else if (!strcasecmp(value, "CANCELLED")) {
						tech_pvt->skype_callflow = CALLFLOW_STATUS_CANCELLED;
						DEBUGA_SKYPE("we tried to call Skype on skype_call %s and Skype has now CANCELLED\n", SKYPIAX_P_LOG, id);
						tech_pvt->skype_call_id[0] = '\0';
						if (tech_pvt->interface_state != SKYPIAX_STATE_HANGUP_REQUESTED) {
							tech_pvt->interface_state = SKYPIAX_STATE_DOWN;
							return CALLFLOW_INCOMING_HANGUP;
						} else {
							tech_pvt->interface_state = SKYPIAX_STATE_DOWN;
						}
					} else if (!strcasecmp(value, "FAILED")) {
						tech_pvt->skype_callflow = CALLFLOW_STATUS_FAILED;
						DEBUGA_SKYPE("we tried to call Skype on skype_call %s and Skype has now FAILED\n", SKYPIAX_P_LOG, id);
						tech_pvt->skype_call_id[0] = '\0';
						skypiax_strncpy(tech_pvt->skype_call_id, id, sizeof(tech_pvt->skype_call_id) - 1);
						tech_pvt->interface_state = SKYPIAX_STATE_DOWN;
						return CALLFLOW_INCOMING_HANGUP;
					} else if (!strcasecmp(value, "REFUSED")) {
						if (!strcasecmp(id, tech_pvt->skype_call_id)) {
							/* this is the id of the call we are in, probably we generated it */
							tech_pvt->skype_callflow = CALLFLOW_STATUS_REFUSED;
							DEBUGA_SKYPE("we tried to call Skype on skype_call %s and Skype has now REFUSED\n", SKYPIAX_P_LOG, id);
							skypiax_strncpy(tech_pvt->skype_call_id, id, sizeof(tech_pvt->skype_call_id) - 1);
							tech_pvt->interface_state = SKYPIAX_STATE_DOWN;
							tech_pvt->skype_call_id[0] = '\0';
							return CALLFLOW_INCOMING_HANGUP;
						} else {
							/* we're here because were us that refused an incoming call */
							DEBUGA_SKYPE("we REFUSED skype_call %s\n", SKYPIAX_P_LOG, id);
						}
					} else if (!strcasecmp(value, "TRANSFERRING")) {
						DEBUGA_SKYPE("skype_call %s is transferring\n", SKYPIAX_P_LOG, id);
					} else if (!strcasecmp(value, "TRANSFERRED")) {
						DEBUGA_SKYPE("skype_call %s has been transferred\n", SKYPIAX_P_LOG, id);
					} else if (!strcasecmp(value, "ROUTING")) {
						tech_pvt->skype_callflow = CALLFLOW_STATUS_ROUTING;
						tech_pvt->interface_state = SKYPIAX_STATE_DIALING;
						skypiax_strncpy(tech_pvt->skype_call_id, id, sizeof(tech_pvt->skype_call_id) - 1);
						DEBUGA_SKYPE("skype_call: %s is now ROUTING\n", SKYPIAX_P_LOG, id);
					} else if (!strcasecmp(value, "UNPLACED")) {
						tech_pvt->skype_callflow = CALLFLOW_STATUS_UNPLACED;
						tech_pvt->interface_state = SKYPIAX_STATE_DIALING;
						skypiax_strncpy(tech_pvt->skype_call_id, id, sizeof(tech_pvt->skype_call_id) - 1);
						DEBUGA_SKYPE("skype_call: %s is now UNPLACED\n", SKYPIAX_P_LOG, id);
					} else if (!strcasecmp(value, "INPROGRESS")) {
						char msg_to_skype[1024];

						if (!strlen(tech_pvt->session_uuid_str)) {
							DEBUGA_SKYPE("no tech_pvt->session_uuid_str\n", SKYPIAX_P_LOG);
						}
						if (tech_pvt->skype_callflow != CALLFLOW_STATUS_REMOTEHOLD) {
							if (!strlen(tech_pvt->session_uuid_str) || !strlen(tech_pvt->skype_call_id)
								|| !strcasecmp(tech_pvt->skype_call_id, id)) {
								skypiax_strncpy(tech_pvt->skype_call_id, id, sizeof(tech_pvt->skype_call_id) - 1);
								DEBUGA_SKYPE("skype_call: %s is now active\n", SKYPIAX_P_LOG, id);

								if (tech_pvt->skype_callflow != CALLFLOW_STATUS_EARLYMEDIA) {
									tech_pvt->skype_callflow = CALLFLOW_STATUS_INPROGRESS;
									tech_pvt->interface_state = SKYPIAX_STATE_UP;

									if (tech_pvt->tcp_cli_thread == NULL) {
										DEBUGA_SKYPE("START start_audio_threads\n", SKYPIAX_P_LOG);
										if (start_audio_threads(tech_pvt)) {
											ERRORA("start_audio_threads FAILED\n", SKYPIAX_P_LOG);
											return CALLFLOW_INCOMING_HANGUP;
										}
									}
									skypiax_sleep(1000);	//FIXME
									sprintf(msg_to_skype, "ALTER CALL %s SET_INPUT PORT=\"%d\"", id, tech_pvt->tcp_cli_port);
									skypiax_signaling_write(tech_pvt, msg_to_skype);
									skypiax_sleep(1000);	//FIXME
									sprintf(msg_to_skype, "#output ALTER CALL %s SET_OUTPUT PORT=\"%d\"", id, tech_pvt->tcp_srv_port);
									skypiax_signaling_write(tech_pvt, msg_to_skype);
								}
								tech_pvt->skype_callflow = CALLFLOW_STATUS_INPROGRESS;
								if (!strlen(tech_pvt->session_uuid_str)) {
									DEBUGA_SKYPE("New Inbound Channel!\n\n\n\n", SKYPIAX_P_LOG);
									new_inbound_channel(tech_pvt);
								} else {
									tech_pvt->interface_state = SKYPIAX_STATE_UP;
									DEBUGA_SKYPE("Outbound Channel Answered! session_uuid_str=%s\n", SKYPIAX_P_LOG, tech_pvt->session_uuid_str);
									outbound_channel_answered(tech_pvt);
								}
							} else {
								DEBUGA_SKYPE("I'm on %s, skype_call %s is NOT MY call, ignoring\n", SKYPIAX_P_LOG, tech_pvt->skype_call_id, id);
							}
						} else {
							tech_pvt->skype_callflow = CALLFLOW_STATUS_INPROGRESS;
							DEBUGA_SKYPE("Back from REMOTEHOLD!\n", SKYPIAX_P_LOG);
						}

					} else if (!strcasecmp(value, "REMOTEHOLD")) {
						tech_pvt->skype_callflow = CALLFLOW_STATUS_REMOTEHOLD;
						DEBUGA_SKYPE("skype_call: %s is now REMOTEHOLD\n", SKYPIAX_P_LOG, id);

					} else if (!strcasecmp(value, "BUSY")) {
						tech_pvt->skype_callflow = CALLFLOW_STATUS_FAILED;
						DEBUGA_SKYPE
							("we tried to call Skype on skype_call %s and remote party (destination) was BUSY. Our outbound call has failed\n",
							 SKYPIAX_P_LOG, id);
						skypiax_strncpy(tech_pvt->skype_call_id, id, sizeof(tech_pvt->skype_call_id) - 1);
						tech_pvt->interface_state = SKYPIAX_STATE_DOWN;
						tech_pvt->skype_call_id[0] = '\0';
						skypiax_sleep(1000);
						return CALLFLOW_INCOMING_HANGUP;
					} else if (!strcasecmp(value, "WAITING_REDIAL_COMMAND")) {
						tech_pvt->skype_callflow = CALLFLOW_STATUS_FAILED;
						DEBUGA_SKYPE
							("we tried to call Skype on skype_call %s and remote party (destination) has rejected us (WAITING_REDIAL_COMMAND). Our outbound call has failed\n",
							 SKYPIAX_P_LOG, id);
						skypiax_strncpy(tech_pvt->skype_call_id, id, sizeof(tech_pvt->skype_call_id) - 1);
						tech_pvt->interface_state = SKYPIAX_STATE_DOWN;
						tech_pvt->skype_call_id[0] = '\0';
						skypiax_sleep(1000);
						return CALLFLOW_INCOMING_HANGUP;
					} else {
						WARNINGA("skype_call: %s, STATUS: %s is not recognized\n", SKYPIAX_P_LOG, id, value);
					}
				}				//STATUS
			}					//CALL
			/* the "numbered" messages that follows are used by the directory application, not yet ported */
			if (!strcasecmp(message, "#333")) {
				/* DEBUGA_SKYPE("Skype MSG: message_2: %s, message2[11]: %s\n", SKYPIAX_P_LOG,
				 * message_2, &message_2[11]); */
				memset(tech_pvt->skype_friends, 0, 4096);
				skypiax_strncpy(tech_pvt->skype_friends, &message_2[11], 4095);
			}
			if (!strcasecmp(message, "#222")) {
				/* DEBUGA_SKYPE("Skype MSG: message_2: %s, message2[10]: %s\n", SKYPIAX_P_LOG,
				 * message_2, &message_2[10]); */
				memset(tech_pvt->skype_fullname, 0, 512);
				skypiax_strncpy(tech_pvt->skype_fullname, &message_2[10], 511);
			}
			if (!strcasecmp(message, "#765")) {
				/* DEBUGA_SKYPE("Skype MSG: message_2: %s, message2[10]: %s\n", SKYPIAX_P_LOG,
				 * message_2, &message_2[10]); */
				memset(tech_pvt->skype_displayname, 0, 512);
				skypiax_strncpy(tech_pvt->skype_displayname, &message_2[10], 511);
			}
			a = 0;
		}						//message end
	}							//read_from_pipe
	return 0;
}

void *skypiax_do_tcp_srv_thread_func(void *obj)
{
	private_t *tech_pvt = obj;
	int s;
	unsigned int len;
	//unsigned int i;
	//unsigned int a;
#if defined(WIN32) && !defined(__CYGWIN__)
	int sin_size;
#else /* WIN32 */
	unsigned int sin_size;
#endif /* WIN32 */
	unsigned int fd;
	//short srv_in[SAMPLES_PER_FRAME];
	//short srv_out[SAMPLES_PER_FRAME / 2];
	//struct sockaddr_in my_addr;
	struct sockaddr_in remote_addr;
	//int exit = 0;
	unsigned int kill_cli_size;
	short kill_cli_buff[SAMPLES_PER_FRAME];
	//short totalbuf[SAMPLES_PER_FRAME];
	int sockbufsize = 0;
	unsigned int size = sizeof(int);

	s = skypiax_socket_create_and_bind(tech_pvt, &tech_pvt->tcp_srv_port);
	if (s < 0) {
		ERRORA("skypiax_socket_create_and_bind error!\n", SKYPIAX_P_LOG);
		return NULL;
	}
	DEBUGA_SKYPE("started tcp_srv_thread thread.\n", SKYPIAX_P_LOG);

	listen(s, 6);

	sin_size = sizeof(remote_addr);

  /****************************/
	while (tech_pvt->interface_state != SKYPIAX_STATE_DOWN
		   && (tech_pvt->skype_callflow == CALLFLOW_STATUS_INPROGRESS
			   || tech_pvt->skype_callflow == CALLFLOW_STATUS_EARLYMEDIA
			   || tech_pvt->skype_callflow == CALLFLOW_STATUS_REMOTEHOLD || tech_pvt->skype_callflow == SKYPIAX_STATE_UP)) {

		unsigned int fdselectgio;
		int rtgio;
		fd_set fsgio;
		struct timeval togio;

		if (!(running && tech_pvt->running))
			break;
		FD_ZERO(&fsgio);
		togio.tv_usec = 20000;	//20msec
		togio.tv_sec = 0;
		fdselectgio = s;
		FD_SET(fdselectgio, &fsgio);

		rtgio = select(fdselectgio + 1, &fsgio, NULL, NULL, &togio);

		if (rtgio) {

  /****************************/

			while (s > 0 && (fd = accept(s, (struct sockaddr *) &remote_addr, &sin_size)) > 0) {
				DEBUGA_SKYPE("ACCEPTED here I send you %d\n", SKYPIAX_P_LOG, tech_pvt->tcp_srv_port);

				sockbufsize = 0;
				size = sizeof(int);
				getsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *) &sockbufsize, &size);
				DEBUGA_SKYPE("3 SO_RCVBUF is %d, size is %d\n", SKYPIAX_P_LOG, sockbufsize, size);
				sockbufsize = 0;
				size = sizeof(int);
				getsockopt(s, SOL_SOCKET, SO_SNDBUF, (char *) &sockbufsize, &size);
				DEBUGA_SKYPE("3 SO_SNDBUF is %d, size is %d\n", SKYPIAX_P_LOG, sockbufsize, size);


				if (!(running && tech_pvt->running))
					break;
				while (tech_pvt->interface_state != SKYPIAX_STATE_DOWN
					   && (tech_pvt->skype_callflow == CALLFLOW_STATUS_INPROGRESS
						   || tech_pvt->skype_callflow == CALLFLOW_STATUS_EARLYMEDIA
						   || tech_pvt->skype_callflow == CALLFLOW_STATUS_REMOTEHOLD || tech_pvt->skype_callflow == SKYPIAX_STATE_UP)) {

					tech_pvt->readfd = fd;
						//WARNINGA("read HERE\n", SKYPIAX_P_LOG);
						skypiax_sleep(100000);


#ifdef NOLOOP

					unsigned int fdselect;
					int rt;
					fd_set fs;
					struct timeval to;

					if (!(running && tech_pvt->running))
						break;
					//exit = 1;

					fdselect = fd;
					FD_ZERO(&fs);
					FD_SET(fdselect, &fs);
					//to.tv_usec = 2000000;     //2000 msec
					to.tv_usec = 60000;	//60 msec
					to.tv_sec = 0;

					rt = select(fdselect + 1, &fs, NULL, NULL, &to);
					if (rt > 0) {

						if (tech_pvt->skype_callflow != CALLFLOW_STATUS_REMOTEHOLD) {
							len = recv(fd, (char *) srv_in, 320, 0);	//seems that Skype only sends 320 bytes at time
						} else {
							len = 0;
						}

						if (len == 320) {
							unsigned int howmany;

							if (samplerate_skypiax == 8000) {
								/* we're downsampling from 16khz to 8khz, srv_out will contain each other sample from srv_in */
								a = 0;
								for (i = 0; i < len / sizeof(short); i++) {
									srv_out[a] = srv_in[i];
									i++;
									a++;
								}
							} else if (samplerate_skypiax == 16000) {
								/* we're NOT downsampling, srv_out will contain ALL samples from srv_in */
								for (i = 0; i < len / sizeof(short); i++) {
									srv_out[i] = srv_in[i];
								}
							} else {
								ERRORA("SAMPLERATE_SKYPIAX can only be 8000 or 16000\n", SKYPIAX_P_LOG);
							}
							/* if not yet done, let's store the half incoming frame */
							if (!tech_pvt->audiobuf_is_loaded) {
								for (i = 0; i < SAMPLES_PER_FRAME / 2; i++) {
									tech_pvt->audiobuf[i] = srv_out[i];
								}
								tech_pvt->audiobuf_is_loaded = 1;
							} else {
								/* we got a stored half frame, build a complete frame in totalbuf using the stored half frame and the current half frame */
								for (i = 0; i < SAMPLES_PER_FRAME / 2; i++) {
									totalbuf[i] = tech_pvt->audiobuf[i];
								}
								for (a = 0; a < SAMPLES_PER_FRAME / 2; a++) {
									totalbuf[i] = srv_out[a];
									i++;
								}
								/* send the complete frame through the pipe to our code waiting for incoming audio */
								//howmany = skypiax_pipe_write(tech_pvt->audiopipe_srv[1], totalbuf, SAMPLES_PER_FRAME * sizeof(short));
								//FIXME while(tech_pvt->flag_audio_srv == 1){
								//FIXME switch_sleep(100); //1 millisec
								//NOTICA("read now is 1\n", SKYPIAX_P_LOG);
								//FIXME }
								//WARNINGA("read is now 0\n", SKYPIAX_P_LOG);


								howmany = SAMPLES_PER_FRAME * sizeof(short);
								//while (tech_pvt->flag_audio_srv == 1) {
									//switch_sleep(1000);	//10 millisec
						//WARNINGA("read now is 1\n", SKYPIAX_P_LOG);
								//}
								//if (tech_pvt->flag_audio_srv == 0) {
	//switch_mutex_lock(tech_pvt->flag_audio_srv_mutex);
									memcpy(tech_pvt->audiobuf_srv, totalbuf, SAMPLES_PER_FRAME * sizeof(short));
									tech_pvt->flag_audio_srv = 1;
	//switch_mutex_unlock(tech_pvt->flag_audio_srv_mutex);
								//}
								//NOTICA("read \n", SKYPIAX_P_LOG);
								if (howmany != SAMPLES_PER_FRAME * sizeof(short)) {
									ERRORA("howmany is %d, but was expected to be %d\n", SKYPIAX_P_LOG,
										   howmany, (int) (SAMPLES_PER_FRAME * sizeof(short)));
								}
								/* done with the stored half frame */
								tech_pvt->audiobuf_is_loaded = 0;
							}

						} else if (len == 0) {
							skypiax_sleep(1000);
						} else {
							DEBUGA_SKYPE("len=%d, expected 320\n", SKYPIAX_P_LOG, len);
						}

					} else {
						if (rt)
							ERRORA("SRV rt=%d\n", SKYPIAX_P_LOG, rt);
						skypiax_sleep(10000);
					}

#endif// NOLOOP
				}

				/* let's send some frame in the pipes, so both tcp_cli and tcp_srv will have an occasion to die */
				kill_cli_size = SAMPLES_PER_FRAME * sizeof(short);
				len = skypiax_pipe_write(tech_pvt->audiopipe_srv[1], kill_cli_buff, kill_cli_size);
				kill_cli_size = SAMPLES_PER_FRAME * sizeof(short);
				len = skypiax_pipe_write(tech_pvt->audiopipe_cli[1], kill_cli_buff, kill_cli_size);
				//tech_pvt->interface_state = SKYPIAX_STATE_DOWN;
				kill_cli_size = SAMPLES_PER_FRAME * sizeof(short);
				len = skypiax_pipe_write(tech_pvt->audiopipe_srv[1], kill_cli_buff, kill_cli_size);
				kill_cli_size = SAMPLES_PER_FRAME * sizeof(short);
				len = skypiax_pipe_write(tech_pvt->audiopipe_cli[1], kill_cli_buff, kill_cli_size);

				tech_pvt->flag_audio_cli = 1;	//let's send some frame in the pipes, so both tcp_cli and tcp_srv will have an occasion to die
				skypiax_sleep(2000);
				tech_pvt->flag_audio_srv = 1;	//let's send some frame in the pipes, so both tcp_cli and tcp_srv will have an occasion to die
				skypiax_sleep(2000);
				tech_pvt->interface_state = SKYPIAX_STATE_DOWN;
				tech_pvt->flag_audio_cli = 1;	//let's send some frame in the pipes, so both tcp_cli and tcp_srv will have an occasion to die
				skypiax_sleep(2000);
				tech_pvt->flag_audio_srv = 1;	//let's send some frame in the pipes, so both tcp_cli and tcp_srv will have an occasion to die
				skypiax_sleep(2000);
				DEBUGA_SKYPE("Skype incoming audio GONE\n", SKYPIAX_P_LOG);
				skypiax_close_socket(fd);
				break;
			}
		}
	}

	DEBUGA_SKYPE("incoming audio server (I am it) EXITING\n", SKYPIAX_P_LOG);
	skypiax_close_socket(s);
	s = -1;
	return NULL;
}

void *skypiax_do_tcp_cli_thread_func(void *obj)
{
	private_t *tech_pvt = obj;
	int s;
	//struct sockaddr_in my_addr;
	struct sockaddr_in remote_addr;
#ifdef NOVARS
	unsigned int got;
	unsigned int len;
	unsigned int i;
	unsigned int a;
	short cli_out[SAMPLES_PER_FRAME * 2];
	short cli_in[SAMPLES_PER_FRAME];
#endif// NOVARS
	unsigned int fd;
#ifdef WIN32
	int sin_size;
#else
	unsigned int sin_size;
#endif /* WIN32 */
	int sockbufsize = 0;
	unsigned int size = sizeof(int);

	s = skypiax_socket_create_and_bind(tech_pvt, &tech_pvt->tcp_cli_port);
	if (s < 0) {
		ERRORA("skypiax_socket_create_and_bind error!\n", SKYPIAX_P_LOG);
		return NULL;
	}



	DEBUGA_SKYPE("started tcp_cli_thread thread.\n", SKYPIAX_P_LOG);

	listen(s, 6);

	sin_size = sizeof(remote_addr);

  /****************************/
	while (tech_pvt->interface_state != SKYPIAX_STATE_DOWN
		   && (tech_pvt->skype_callflow == CALLFLOW_STATUS_INPROGRESS
			   || tech_pvt->skype_callflow == CALLFLOW_STATUS_EARLYMEDIA
			   || tech_pvt->skype_callflow == CALLFLOW_STATUS_REMOTEHOLD || tech_pvt->skype_callflow == SKYPIAX_STATE_UP)) {

		unsigned int fdselectgio;
		int rtgio;
		fd_set fsgio;
		struct timeval togio;

		if (!(running && tech_pvt->running))
			break;
		FD_ZERO(&fsgio);
		togio.tv_usec = 20000;	//20msec
		togio.tv_sec = 0;
		fdselectgio = s;
		FD_SET(fdselectgio, &fsgio);

		rtgio = select(fdselectgio + 1, &fsgio, NULL, NULL, &togio);

		if (rtgio) {

  /****************************/

			while (s > 0 && (fd = accept(s, (struct sockaddr *) &remote_addr, &sin_size)) > 0) {
				DEBUGA_SKYPE("ACCEPTED here you send me %d\n", SKYPIAX_P_LOG, tech_pvt->tcp_cli_port);

				sockbufsize = 0;
				size = sizeof(int);
				getsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *) &sockbufsize, &size);
				DEBUGA_SKYPE("4 SO_RCVBUF is %d, size is %d\n", SKYPIAX_P_LOG, sockbufsize, size);
				sockbufsize = 0;
				size = sizeof(int);
				getsockopt(s, SOL_SOCKET, SO_SNDBUF, (char *) &sockbufsize, &size);
				DEBUGA_SKYPE("4 SO_SNDBUF is %d, size is %d\n", SKYPIAX_P_LOG, sockbufsize, size);



#ifndef WIN32
				fcntl(tech_pvt->audiopipe_cli[0], F_SETFL, O_NONBLOCK);
				fcntl(tech_pvt->audiopipe_cli[1], F_SETFL, O_NONBLOCK);
#endif //WIN32

				if (!(running && tech_pvt->running))
					break;




				while (tech_pvt->interface_state != SKYPIAX_STATE_DOWN
					   && (tech_pvt->skype_callflow == CALLFLOW_STATUS_INPROGRESS
						   || tech_pvt->skype_callflow == CALLFLOW_STATUS_EARLYMEDIA
						   || tech_pvt->skype_callflow == CALLFLOW_STATUS_REMOTEHOLD || tech_pvt->skype_callflow == SKYPIAX_STATE_UP)) {
					tech_pvt->writefd = fd;
						//NOTICA("write HERE\n", SKYPIAX_P_LOG);
						skypiax_sleep(100000);
#if 0
#ifdef NOVARS
					unsigned int fdselect;
					int rt;
					fd_set fs;
					struct timeval to;

					if (!(running && tech_pvt->running))
						break;
					FD_ZERO(&fs);
					to.tv_usec = 120000;	//120msec
					to.tv_sec = 0;
#if defined(WIN32) && !defined(__CYGWIN__)
/* on win32 we cannot select from the apr "pipe", so we select on socket writability */
					fdselect = fd;
					FD_SET(fdselect, &fs);

					//rt = select(fdselect + 1, NULL, &fs, NULL, &to);
#else
/* on *unix and cygwin we select from the real pipe */
					//XXX fdselect = tech_pvt->audiopipe_cli[0];
					//XXX FD_SET(fdselect, &fs);

					//rt = select(fdselect + 1, &fs, NULL, NULL, &to);
#endif
					fdselect = fd;
					FD_SET(fdselect, &fs);

#endif// NOVARS
#if 0
					rt = select(fdselect + 1, NULL, &fs, NULL, NULL);
					while (tech_pvt->flag_audio_cli == 0) {
#ifdef WIN32
						skypiax_sleep(100);	//0.1 millisec
#else
						skypiax_sleep(1000);	//10 millisec
#endif //WIN32
						NOTICA("write now is 0\n", SKYPIAX_P_LOG);
					}
					//ERRORA("write is now 1\n", SKYPIAX_P_LOG);

					rt = 1;
#endif //0
					//rt = select(fdselect + 1, NULL, &fs, NULL, NULL);

#ifdef NOLOOP
					if (rt > 0) {
						int counter;
#if 0
					while (tech_pvt->flag_audio_cli == 0) {
#ifdef WIN32
						skypiax_sleep(100);	//0.1 millisec
#else
						skypiax_sleep(10000);	//10 millisec
#endif //WIN32
						WARNINGA("write now is 0\n", SKYPIAX_P_LOG);
					}
#endif //0
	
						/* until we drained the pipe to empty */
						for (counter = 0; counter < 1; counter++) {
							/* read from the pipe the audio frame we are supposed to send out */
							//got = skypiax_pipe_read(tech_pvt->audiopipe_cli[0], cli_in, SAMPLES_PER_FRAME * sizeof(short));


							got = SAMPLES_PER_FRAME * sizeof(short);
	switch_mutex_lock(tech_pvt->flag_audio_cli_mutex);
							memcpy(cli_in, tech_pvt->audiobuf_cli, SAMPLES_PER_FRAME * sizeof(short));




							if (got == -1)
								break;

							if (got != SAMPLES_PER_FRAME * sizeof(short)) {
								WARNINGA("got is %d, but was expected to be %d\n", SKYPIAX_P_LOG, got, (int) (SAMPLES_PER_FRAME * sizeof(short)));
							}

							if (got == SAMPLES_PER_FRAME * sizeof(short)) {
								if (samplerate_skypiax == 8000) {

									/* we're upsampling from 8khz to 16khz, cli_out will contain two times each sample from cli_in */
									a = 0;
									for (i = 0; i < got / sizeof(short); i++) {
										cli_out[a] = cli_in[i];
										a++;
										cli_out[a] = cli_in[i];
										a++;
									}
									got = got * 2;
								} else if (samplerate_skypiax == 16000) {
									/* we're NOT upsampling, cli_out will contain just ALL samples from cli_in */
									for (i = 0; i < got / sizeof(short); i++) {
										cli_out[i] = cli_in[i];
									}
								} else {
									ERRORA("SAMPLERATE_SKYPIAX can only be 8000 or 16000\n", SKYPIAX_P_LOG);
								}

								/* send the 16khz frame to the Skype client waiting for incoming audio to be sent to the remote party */
								if (tech_pvt->skype_callflow != CALLFLOW_STATUS_REMOTEHOLD) {
									len = send(fd, (char *) cli_out, got, 0);
							tech_pvt->flag_audio_cli = 0;
									//skypiax_sleep(5000);  //5 msec

									if (len == -1) {
										break;
									} else if (len != got) {
										ERRORA("len=%d\n", SKYPIAX_P_LOG, len);
										skypiax_sleep(1000);
										break;
									}
								}

	switch_mutex_unlock(tech_pvt->flag_audio_cli_mutex);
							} else {

								WARNINGA("got is %d, but was expected to be %d\n", SKYPIAX_P_LOG, got, (int) (SAMPLES_PER_FRAME * sizeof(short)));
							}
						}
					} else {
						if (rt)
							ERRORA("CLI rt=%d\n", SKYPIAX_P_LOG, rt);
						memset(cli_out, 0, sizeof(cli_out));
						if (tech_pvt->skype_callflow != CALLFLOW_STATUS_REMOTEHOLD) {
							len = send(fd, (char *) cli_out, sizeof(cli_out), 0);
							len = send(fd, (char *) cli_out, sizeof(cli_out) / 2, 0);
							//WARNINGA("sent %d of zeros to keep the Skype client socket busy\n", SKYPIAX_P_LOG, sizeof(cli_out) + sizeof(cli_out)/2);
						} else {
							/*
							   XXX do nothing 
							 */
							//WARNINGA("we don't send it\n", SKYPIAX_P_LOG);
						}
						skypiax_sleep(1000);
					}

#endif// NOLOOP
#endif// 0
				}
				DEBUGA_SKYPE("Skype outbound audio GONE\n", SKYPIAX_P_LOG);
				skypiax_close_socket(fd);
				break;
			}
		}
	}

	DEBUGA_SKYPE("outbound audio server (I am it) EXITING\n", SKYPIAX_P_LOG);
	skypiax_close_socket(s);
	s = -1;
	return NULL;
}

int skypiax_audio_read(private_t * tech_pvt)
{
	unsigned int samples;

#if 0
	while (tech_pvt->flag_audio_srv == 0) {
#ifdef WIN32
		skypiax_sleep(100);		//0.1 millisec
#else
		skypiax_sleep(1000);	//10 millisec
#endif //WIN32

		WARNINGA("read now is 0\n", SKYPIAX_P_LOG);
	}
#endif //0
	//ERRORA("read is now 1\n", SKYPIAX_P_LOG);
	//samples = skypiax_pipe_read(tech_pvt->audiopipe_srv[0], tech_pvt->read_frame.data, SAMPLES_PER_FRAME * sizeof(short));
	samples = SAMPLES_PER_FRAME * sizeof(short);
	//switch_mutex_lock(tech_pvt->flag_audio_srv_mutex);
	memcpy(tech_pvt->read_frame.data, tech_pvt->audiobuf_srv, SAMPLES_PER_FRAME * sizeof(short));
	tech_pvt->flag_audio_srv = 0;
	//switch_mutex_unlock(tech_pvt->flag_audio_srv_mutex);

	if (samples != SAMPLES_PER_FRAME * sizeof(short)) {
		if (samples)
			WARNINGA("read samples=%u expected=%u\n", SKYPIAX_P_LOG, samples, (int) (SAMPLES_PER_FRAME * sizeof(short)));
		return 0;
	} else {
		/* A real frame */
		tech_pvt->read_frame.datalen = samples;
	}
	return 1;
}

int skypiax_senddigit(private_t * tech_pvt, char digit)
{
	char msg_to_skype[1024];

	DEBUGA_SKYPE("DIGIT received: %c\n", SKYPIAX_P_LOG, digit);
	sprintf(msg_to_skype, "SET CALL %s DTMF %c", tech_pvt->skype_call_id, digit);
	skypiax_signaling_write(tech_pvt, msg_to_skype);

	return 0;
}

int skypiax_call(private_t * tech_pvt, char *rdest, int timeout)
{
	char msg_to_skype[1024];

	//skypiax_sleep(5000);
	DEBUGA_SKYPE("Calling Skype, rdest is: %s\n", SKYPIAX_P_LOG, rdest);
	//skypiax_signaling_write(tech_pvt, "SET AGC OFF");
	//skypiax_sleep(10000);
	//skypiax_signaling_write(tech_pvt, "SET AEC OFF");
	//skypiax_sleep(10000);

	sprintf(msg_to_skype, "CALL %s", rdest);
	if (skypiax_signaling_write(tech_pvt, msg_to_skype) < 0) {
		ERRORA("failed to communicate with Skype client, now exit\n", SKYPIAX_P_LOG);
		return -1;
	}
	return 0;
}

/***************************/
/* PLATFORM SPECIFIC */
/***************************/
#if defined(WIN32) && !defined(__CYGWIN__)
int skypiax_pipe_read(switch_file_t * pipe, short *buf, int howmany)
{
	switch_size_t quantity;

	quantity = howmany;

	switch_file_read(pipe, buf, &quantity);

	howmany = quantity;

	return howmany;
}

int skypiax_pipe_write(switch_file_t * pipe, short *buf, int howmany)
{
	switch_size_t quantity;

	quantity = howmany;

	switch_file_write(pipe, buf, &quantity);

	howmany = quantity;

	return howmany;
}

int skypiax_close_socket(unsigned int fd)
{
	int res;

	res = closesocket(fd);

	return res;
}

int skypiax_audio_init(private_t * tech_pvt)
{
	switch_status_t rv;
	rv = switch_file_pipe_create(&tech_pvt->audiopipe_srv[0], &tech_pvt->audiopipe_srv[1], skypiax_module_pool);
	rv = switch_file_pipe_create(&tech_pvt->audiopipe_cli[0], &tech_pvt->audiopipe_cli[1], skypiax_module_pool);
	return 0;
}
#else /* WIN32 */
int skypiax_pipe_read(int pipe, short *buf, int howmany)
{
	howmany = read(pipe, buf, howmany);
	return howmany;
}

int skypiax_pipe_write(int pipe, short *buf, int howmany)
{
	if (buf) {
		howmany = write(pipe, buf, howmany);
		return howmany;
	} else {
		return 0;
	}
}

int skypiax_close_socket(unsigned int fd)
{
	int res;

	res = close(fd);

	return res;
}

int skypiax_audio_init(private_t * tech_pvt)
{
	if (pipe(tech_pvt->audiopipe_srv)) {
		fcntl(tech_pvt->audiopipe_srv[0], F_SETFL, O_NONBLOCK);
		fcntl(tech_pvt->audiopipe_srv[1], F_SETFL, O_NONBLOCK);
	}
	if (pipe(tech_pvt->audiopipe_cli)) {
		fcntl(tech_pvt->audiopipe_cli[0], F_SETFL, O_NONBLOCK);
		fcntl(tech_pvt->audiopipe_cli[1], F_SETFL, O_NONBLOCK);
	}

/* this pipe is the audio fd for asterisk to poll on during a call. FS do not use it */
	tech_pvt->skypiax_sound_capt_fd = tech_pvt->audiopipe_srv[0];

	return 0;
}
#endif /* WIN32 */

#ifdef WIN32

enum {
	SKYPECONTROLAPI_ATTACH_SUCCESS = 0,	/*  Client is successfully 
										   attached and API window handle can be found
										   in wParam parameter */
	SKYPECONTROLAPI_ATTACH_PENDING_AUTHORIZATION = 1,	/*  Skype has acknowledged
														   connection request and is waiting
														   for confirmation from the user. */
	/*  The client is not yet attached 
	 * and should wait for SKYPECONTROLAPI_ATTACH_SUCCESS message */
	SKYPECONTROLAPI_ATTACH_REFUSED = 2,	/*  User has explicitly
										   denied access to client */
	SKYPECONTROLAPI_ATTACH_NOT_AVAILABLE = 3,	/*  API is not available
												   at the moment.
												   For example, this happens when no user
												   is currently logged in. */
	/*  Client should wait for 
	 * SKYPECONTROLAPI_ATTACH_API_AVAILABLE 
	 * broadcast before making any further */
	/*  connection attempts. */
	SKYPECONTROLAPI_ATTACH_API_AVAILABLE = 0x8001
};

/* Visual C do not have strsep? */
char
    *strsep(char **stringp, const char *delim)
{
	char *res;

	if (!stringp || !*stringp || !**stringp)
		return (char *) 0;

	res = *stringp;
	while (**stringp && !strchr(delim, **stringp))
		++(*stringp);

	if (**stringp) {
		**stringp = '\0';
		++(*stringp);
	}

	return res;
}

int skypiax_signaling_write(private_t * tech_pvt, char *msg_to_skype)
{
	static char acInputRow[1024];
	COPYDATASTRUCT oCopyData;

	DEBUGA_SKYPE("SENDING: |||%s||||\n", SKYPIAX_P_LOG, msg_to_skype);

	sprintf(acInputRow, "%s", msg_to_skype);
	DEBUGA_SKYPE("acInputRow: |||%s||||\n", SKYPIAX_P_LOG, acInputRow);
	/*  send command to skype */
	oCopyData.dwData = 0;
	oCopyData.lpData = acInputRow;
	oCopyData.cbData = strlen(acInputRow) + 1;
	if (oCopyData.cbData != 1) {
		if (SendMessage
			(tech_pvt->SkypiaxHandles.win32_hGlobal_SkypeAPIWindowHandle, WM_COPYDATA,
			 (WPARAM) tech_pvt->SkypiaxHandles.win32_hInit_MainWindowHandle, (LPARAM) & oCopyData) == FALSE) {
			ERRORA("Sending message failed - probably Skype crashed.\n\nPlease shutdown Skypiax, then launch Skypiax and try again.\n", SKYPIAX_P_LOG);
			return -1;
		}
	}

	return 0;

}

LRESULT APIENTRY skypiax_present(HWND hWindow, UINT uiMessage, WPARAM uiParam, LPARAM ulParam)
{
	LRESULT lReturnCode;
	int fIssueDefProc;
	private_t *tech_pvt = NULL;

	lReturnCode = 0;
	fIssueDefProc = 0;
	tech_pvt = (private_t *) GetWindowLong(hWindow, GWL_USERDATA);
	if (!running)
		return lReturnCode;
	switch (uiMessage) {
	case WM_CREATE:
		tech_pvt = (private_t *) ((LPCREATESTRUCT) ulParam)->lpCreateParams;
		SetWindowLong(hWindow, GWL_USERDATA, (LONG) tech_pvt);
		DEBUGA_SKYPE("got CREATE\n", SKYPIAX_P_LOG);
		break;
	case WM_DESTROY:
		DEBUGA_SKYPE("got DESTROY\n", SKYPIAX_P_LOG);
		tech_pvt->SkypiaxHandles.win32_hInit_MainWindowHandle = NULL;
		PostQuitMessage(0);
		break;
	case WM_COPYDATA:
		if (tech_pvt->SkypiaxHandles.win32_hGlobal_SkypeAPIWindowHandle == (HWND) uiParam) {
			unsigned int howmany;
			char msg_from_skype[2048];

			PCOPYDATASTRUCT poCopyData = (PCOPYDATASTRUCT) ulParam;

			memset(msg_from_skype, '\0', sizeof(msg_from_skype));
			skypiax_strncpy(msg_from_skype, (const char *) poCopyData->lpData, sizeof(msg_from_skype) - 2);

			howmany = strlen(msg_from_skype) + 1;
			howmany = skypiax_pipe_write(tech_pvt->SkypiaxHandles.fdesc[1], (short *) msg_from_skype, howmany);
			//DEBUGA_SKYPE("From Skype API: %s\n", SKYPIAX_P_LOG, msg_from_skype);
			lReturnCode = 1;
		}
		break;
	default:
		if (tech_pvt && tech_pvt->SkypiaxHandles.win32_uiGlobal_MsgID_SkypeControlAPIAttach) {
			if (uiMessage == tech_pvt->SkypiaxHandles.win32_uiGlobal_MsgID_SkypeControlAPIAttach) {
				switch (ulParam) {
				case SKYPECONTROLAPI_ATTACH_SUCCESS:
					if (!tech_pvt->SkypiaxHandles.currentuserhandle) {
						//DEBUGA_SKYPE("\n\n\tConnected to Skype API!\n", SKYPIAX_P_LOG);
						tech_pvt->SkypiaxHandles.api_connected = 1;
						tech_pvt->SkypiaxHandles.win32_hGlobal_SkypeAPIWindowHandle = (HWND) uiParam;
						tech_pvt->SkypiaxHandles.win32_hGlobal_SkypeAPIWindowHandle = tech_pvt->SkypiaxHandles.win32_hGlobal_SkypeAPIWindowHandle;
					}
					break;
				case SKYPECONTROLAPI_ATTACH_PENDING_AUTHORIZATION:
					//DEBUGA_SKYPE ("\n\n\tIf I do not (almost) immediately connect to Skype API,\n\tplease give the Skype client authorization to be connected \n\tby Asterisk and to not ask you again.\n\n", SKYPIAX_P_LOG);
					skypiax_sleep(5000);
#if 0
					if (!tech_pvt->SkypiaxHandles.currentuserhandle) {
						SendMessage(HWND_BROADCAST,
									tech_pvt->SkypiaxHandles.
									win32_uiGlobal_MsgID_SkypeControlAPIDiscover, (WPARAM) tech_pvt->SkypiaxHandles.win32_hInit_MainWindowHandle, 0);
					}
#endif
					break;
				case SKYPECONTROLAPI_ATTACH_REFUSED:
					ERRORA("Skype client refused to be connected by Skypiax!\n", SKYPIAX_P_LOG);
					break;
				case SKYPECONTROLAPI_ATTACH_NOT_AVAILABLE:
					ERRORA("Skype API not (yet?) available\n", SKYPIAX_P_LOG);
					break;
				case SKYPECONTROLAPI_ATTACH_API_AVAILABLE:
					DEBUGA_SKYPE("Skype API available\n", SKYPIAX_P_LOG);
					skypiax_sleep(5000);
#if 0
					if (!tech_pvt->SkypiaxHandles.currentuserhandle) {
						SendMessage(HWND_BROADCAST,
									tech_pvt->SkypiaxHandles.
									win32_uiGlobal_MsgID_SkypeControlAPIDiscover, (WPARAM) tech_pvt->SkypiaxHandles.win32_hInit_MainWindowHandle, 0);
					}
#endif
					break;
				default:
					WARNINGA("GOT AN UNKNOWN SKYPE WINDOWS MSG\n", SKYPIAX_P_LOG);
				}
				lReturnCode = 1;
				break;
			}
		}
		fIssueDefProc = 1;
		break;
	}
	if (fIssueDefProc)
		lReturnCode = DefWindowProc(hWindow, uiMessage, uiParam, ulParam);
	return (lReturnCode);
}

int win32_Initialize_CreateWindowClass(private_t * tech_pvt)
{
	unsigned char *paucUUIDString;
	RPC_STATUS lUUIDResult;
	int fReturnStatus;
	UUID oUUID;

	fReturnStatus = 0;
	lUUIDResult = UuidCreate(&oUUID);
	tech_pvt->SkypiaxHandles.win32_hInit_ProcessHandle = (HINSTANCE) OpenProcess(PROCESS_DUP_HANDLE, FALSE, GetCurrentProcessId());
	if (tech_pvt->SkypiaxHandles.win32_hInit_ProcessHandle != NULL && (lUUIDResult == RPC_S_OK || lUUIDResult == RPC_S_UUID_LOCAL_ONLY)) {
		if (UuidToString(&oUUID, &paucUUIDString) == RPC_S_OK) {
			WNDCLASS oWindowClass;

			strcpy(tech_pvt->SkypiaxHandles.win32_acInit_WindowClassName, "Skype-API-Skypiax-");
			strcat(tech_pvt->SkypiaxHandles.win32_acInit_WindowClassName, (char *) paucUUIDString);

			oWindowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
			oWindowClass.lpfnWndProc = (WNDPROC) & skypiax_present;
			oWindowClass.cbClsExtra = 0;
			oWindowClass.cbWndExtra = 0;
			oWindowClass.hInstance = tech_pvt->SkypiaxHandles.win32_hInit_ProcessHandle;
			oWindowClass.hIcon = NULL;
			oWindowClass.hCursor = NULL;
			oWindowClass.hbrBackground = NULL;
			oWindowClass.lpszMenuName = NULL;
			oWindowClass.lpszClassName = tech_pvt->SkypiaxHandles.win32_acInit_WindowClassName;

			if (RegisterClass(&oWindowClass) != 0)
				fReturnStatus = 1;

			RpcStringFree(&paucUUIDString);
		}
	}
	if (fReturnStatus == 0)
		CloseHandle(tech_pvt->SkypiaxHandles.win32_hInit_ProcessHandle);
	tech_pvt->SkypiaxHandles.win32_hInit_ProcessHandle = NULL;
	return (fReturnStatus);
}

void win32_DeInitialize_DestroyWindowClass(private_t * tech_pvt)
{
	UnregisterClass(tech_pvt->SkypiaxHandles.win32_acInit_WindowClassName, tech_pvt->SkypiaxHandles.win32_hInit_ProcessHandle);
	CloseHandle(tech_pvt->SkypiaxHandles.win32_hInit_ProcessHandle);
	tech_pvt->SkypiaxHandles.win32_hInit_ProcessHandle = NULL;
}

int win32_Initialize_CreateMainWindow(private_t * tech_pvt)
{
	tech_pvt->SkypiaxHandles.win32_hInit_MainWindowHandle =
		CreateWindowEx(WS_EX_APPWINDOW | WS_EX_WINDOWEDGE,
					   tech_pvt->SkypiaxHandles.win32_acInit_WindowClassName, "",
					   WS_BORDER | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
					   128, 128, NULL, 0, tech_pvt->SkypiaxHandles.win32_hInit_ProcessHandle, tech_pvt);
	return (tech_pvt->SkypiaxHandles.win32_hInit_MainWindowHandle != NULL ? 1 : 0);
}

void win32_DeInitialize_DestroyMainWindow(private_t * tech_pvt)
{
	if (tech_pvt->SkypiaxHandles.win32_hInit_MainWindowHandle != NULL)
		DestroyWindow(tech_pvt->SkypiaxHandles.win32_hInit_MainWindowHandle), tech_pvt->SkypiaxHandles.win32_hInit_MainWindowHandle = NULL;
}

void *skypiax_do_skypeapi_thread_func(void *obj)
{
	private_t *tech_pvt = obj;
#if defined(WIN32) && !defined(__CYGWIN__)
	switch_status_t rv;

	switch_file_pipe_create(&tech_pvt->SkypiaxHandles.fdesc[0], &tech_pvt->SkypiaxHandles.fdesc[1], skypiax_module_pool);
	rv = switch_file_pipe_create(&tech_pvt->SkypiaxHandles.fdesc[0], &tech_pvt->SkypiaxHandles.fdesc[1], skypiax_module_pool);
#else /* WIN32 */
	if (pipe(tech_pvt->SkypiaxHandles.fdesc)) {
		fcntl(tech_pvt->SkypiaxHandles.fdesc[0], F_SETFL, O_NONBLOCK);
		fcntl(tech_pvt->SkypiaxHandles.fdesc[1], F_SETFL, O_NONBLOCK);
	}
#endif /* WIN32 */

	tech_pvt->SkypiaxHandles.win32_uiGlobal_MsgID_SkypeControlAPIAttach = RegisterWindowMessage("SkypeControlAPIAttach");
	tech_pvt->SkypiaxHandles.win32_uiGlobal_MsgID_SkypeControlAPIDiscover = RegisterWindowMessage("SkypeControlAPIDiscover");

	skypiax_sleep(200000);		//0,2 sec

	if (tech_pvt->SkypiaxHandles.win32_uiGlobal_MsgID_SkypeControlAPIAttach != 0
		&& tech_pvt->SkypiaxHandles.win32_uiGlobal_MsgID_SkypeControlAPIDiscover != 0) {
		if (win32_Initialize_CreateWindowClass(tech_pvt)) {
			if (win32_Initialize_CreateMainWindow(tech_pvt)) {
				if (SendMessage
					(HWND_BROADCAST,
					 tech_pvt->SkypiaxHandles.win32_uiGlobal_MsgID_SkypeControlAPIDiscover,
					 (WPARAM) tech_pvt->SkypiaxHandles.win32_hInit_MainWindowHandle, 0) != 0) {
					tech_pvt->SkypiaxHandles.win32_hInit_MainWindowHandle = tech_pvt->SkypiaxHandles.win32_hInit_MainWindowHandle;
					while (running && tech_pvt->running) {
						MSG oMessage;
						if (!(running && tech_pvt->running))
							break;
						while (GetMessage(&oMessage, 0, 0, 0)) {
							TranslateMessage(&oMessage);
							DispatchMessage(&oMessage);
						}
					}
				}
				win32_DeInitialize_DestroyMainWindow(tech_pvt);
			}
			win32_DeInitialize_DestroyWindowClass(tech_pvt);
		}
	}

	return NULL;
}

#else /* NOT WIN32 */
int X11_errors_handler(Display * dpy, XErrorEvent * err)
{
	private_t *tech_pvt = NULL;
	(void) dpy;

	xerror = err->error_code;
	ERRORA("Received error code %d from X Server\n\n", SKYPIAX_P_LOG, xerror);	///FIXME why crash the entire skypiax? just crash the interface, instead
	//running = 0;
	return 0;					/*  ignore the error */
}

static void X11_errors_trap(void)
{
	xerror = 0;
	old_handler = XSetErrorHandler(X11_errors_handler);
}

static int X11_errors_untrap(void)
{
	XSetErrorHandler(old_handler);
	return (xerror != BadValue) && (xerror != BadWindow);
}

int skypiax_send_message(private_t * tech_pvt, const char *message_P)
{
	struct SkypiaxHandles *SkypiaxHandles = &tech_pvt->SkypiaxHandles;
	Window w_P = SkypiaxHandles->skype_win;
	Display *disp = SkypiaxHandles->disp;
	Window handle_P = SkypiaxHandles->win;
	int ok;
	//private_t *tech_pvt = NULL;


	Atom atom1 = XInternAtom(disp, "SKYPECONTROLAPI_MESSAGE_BEGIN", False);
	Atom atom2 = XInternAtom(disp, "SKYPECONTROLAPI_MESSAGE", False);
	unsigned int pos = 0;
	unsigned int len = strlen(message_P);
	XEvent e;

	memset(&e, 0, sizeof(e));
	e.xclient.type = ClientMessage;
	e.xclient.message_type = atom1;	/*  leading message */
	e.xclient.display = disp;
	e.xclient.window = handle_P;
	e.xclient.format = 8;

	X11_errors_trap();
	//XLockDisplay(disp);
	do {
		unsigned int i;
		for (i = 0; i < 20 && i + pos <= len; ++i)
			e.xclient.data.b[i] = message_P[i + pos];
		XSendEvent(disp, w_P, False, 0, &e);

		e.xclient.message_type = atom2;	/*  following messages */
		pos += i;
	} while (pos <= len);

	XSync(disp, False);
	ok = X11_errors_untrap();

	if (!ok) {
		ERRORA("Sending message failed with status %d\n", SKYPIAX_P_LOG, xerror);
		tech_pvt->running = 0;
		return 0;
	}
	//XUnlockDisplay(disp);

	return 1;
}

int skypiax_signaling_write(private_t * tech_pvt, char *msg_to_skype)
{

	DEBUGA_SKYPE("SENDING: |||%s||||\n", SKYPIAX_P_LOG, msg_to_skype);


	if (!skypiax_send_message(tech_pvt, msg_to_skype)) {
		ERRORA
			("Sending message failed - probably Skype crashed.\n\nPlease shutdown Skypiax, then restart Skype, then launch Skypiax and try again.\n",
			 SKYPIAX_P_LOG);
		return -1;
	}

	return 0;

}

int skypiax_present(struct SkypiaxHandles *SkypiaxHandles)
{
	Atom skype_inst = XInternAtom(SkypiaxHandles->disp, "_SKYPE_INSTANCE", True);

	Atom type_ret;
	int format_ret;
	unsigned long nitems_ret;
	unsigned long bytes_after_ret;
	unsigned char *prop;
	int status;
	private_t *tech_pvt = NULL;

	X11_errors_trap();
	//XLockDisplay(disp);
	status =
		XGetWindowProperty(SkypiaxHandles->disp, DefaultRootWindow(SkypiaxHandles->disp),
						   skype_inst, 0, 1, False, XA_WINDOW, &type_ret, &format_ret, &nitems_ret, &bytes_after_ret, &prop);
	//XUnlockDisplay(disp);
	X11_errors_untrap();

	/*  sanity check */
	if (status != Success || format_ret != 32 || nitems_ret != 1) {
		SkypiaxHandles->skype_win = (Window) - 1;
		DEBUGA_SKYPE("Skype instance not found\n", SKYPIAX_P_LOG);
		running = 0;
		SkypiaxHandles->api_connected = 0;
		return 0;
	}

	SkypiaxHandles->skype_win = *(const unsigned long *) prop & 0xffffffff;
	DEBUGA_SKYPE("Skype instance found with id #%d\n", SKYPIAX_P_LOG, (unsigned int) SkypiaxHandles->skype_win);
	SkypiaxHandles->api_connected = 1;
	return 1;
}

void skypiax_clean_disp(void *data)
{

	int *dispptr;
	int disp;
	private_t *tech_pvt = NULL;

	dispptr = data;
	disp = *dispptr;

	if (disp) {
		DEBUGA_SKYPE("to be destroyed disp %d\n", SKYPIAX_P_LOG, disp);
		close(disp);
		DEBUGA_SKYPE("destroyed disp\n", SKYPIAX_P_LOG);
	} else {
		DEBUGA_SKYPE("NOT destroyed disp\n", SKYPIAX_P_LOG);
	}
	DEBUGA_SKYPE("OUT destroyed disp\n", SKYPIAX_P_LOG);
	skypiax_sleep(1000);
}

void *skypiax_do_skypeapi_thread_func(void *obj)
{

	private_t *tech_pvt = obj;
	struct SkypiaxHandles *SkypiaxHandles;
	char buf[512];
	Display *disp = NULL;
	Window root = -1;
	Window win = -1;
	int xfd;

	if (!strlen(tech_pvt->X11_display))
		strcpy(tech_pvt->X11_display, getenv("DISPLAY"));

	if (!tech_pvt->tcp_srv_port)
		tech_pvt->tcp_srv_port = 10160;

	if (!tech_pvt->tcp_cli_port)
		tech_pvt->tcp_cli_port = 10161;

	if (pipe(tech_pvt->SkypiaxHandles.fdesc)) {
		fcntl(tech_pvt->SkypiaxHandles.fdesc[0], F_SETFL, O_NONBLOCK);
		fcntl(tech_pvt->SkypiaxHandles.fdesc[1], F_SETFL, O_NONBLOCK);
	}
	SkypiaxHandles = &tech_pvt->SkypiaxHandles;
	disp = XOpenDisplay(tech_pvt->X11_display);
	if (!disp) {
		ERRORA("Cannot open X Display '%s', exiting skype thread\n", SKYPIAX_P_LOG, tech_pvt->X11_display);
		running = 0;
		return NULL;
	} else {
		DEBUGA_SKYPE("X Display '%s' opened\n", SKYPIAX_P_LOG, tech_pvt->X11_display);
	}

	xfd = XConnectionNumber(disp);
	fcntl(xfd, F_SETFD, FD_CLOEXEC);

	SkypiaxHandles->disp = disp;

	if (skypiax_present(SkypiaxHandles)) {
		root = DefaultRootWindow(disp);
		win = XCreateSimpleWindow(disp, root, 0, 0, 1, 1, 0, BlackPixel(disp, DefaultScreen(disp)), BlackPixel(disp, DefaultScreen(disp)));

		SkypiaxHandles->win = win;

		snprintf(buf, 512, "NAME skypiax");

		if (!skypiax_send_message(tech_pvt, buf)) {
			ERRORA("Sending message failed - probably Skype crashed. Please run/restart Skype manually and launch Skypiax again\n", SKYPIAX_P_LOG);
			running = 0;
			//if(disp)
			//XCloseDisplay(disp);
			return NULL;
		}

		snprintf(buf, 512, "PROTOCOL 7");
		if (!skypiax_send_message(tech_pvt, buf)) {
			ERRORA("Sending message failed - probably Skype crashed. Please run/restart Skype manually and launch Skypiax again\n", SKYPIAX_P_LOG);
			running = 0;
			//if(disp)
			//XCloseDisplay(disp);
			return NULL;
		}

		{
			/* perform an events loop */
			XEvent an_event;
			char buf[21];		/*  can't be longer */
			char buffer[17000];
			char continuebuffer[17000];
			char *b;
			int i;
			int continue_is_broken = 0;
			int there_were_continues = 0;
			Atom atom_begin = XInternAtom(disp, "SKYPECONTROLAPI_MESSAGE_BEGIN", False);
			Atom atom_continue = XInternAtom(disp, "SKYPECONTROLAPI_MESSAGE", False);

			memset(buffer, '\0', 17000);
			memset(continuebuffer, '\0', 17000);
			b = buffer;

			while (running && tech_pvt->running) {
				XNextEvent(disp, &an_event);
				if (!(running && tech_pvt->running))
					break;
				switch (an_event.type) {
				case ClientMessage:

					if (an_event.xclient.format != 8) {
						skypiax_sleep(1000);	//0.1 msec
						break;
					}

					for (i = 0; i < 20 && an_event.xclient.data.b[i] != '\0'; ++i)
						buf[i] = an_event.xclient.data.b[i];

					buf[i] = '\0';

					//DEBUGA_SKYPE ("BUF=|||%s|||\n", SKYPIAX_P_LOG, buf);

					if (an_event.xclient.message_type == atom_begin) {
						//DEBUGA_SKYPE ("BEGIN BUF=|||%s|||\n", SKYPIAX_P_LOG, buf);

						if (strlen(buffer)) {
							unsigned int howmany;
							howmany = strlen(b) + 1;
							howmany = write(SkypiaxHandles->fdesc[1], b, howmany);
							WARNINGA
								("A begin atom while the previous message is not closed???? value of previous message (between vertical bars) is=|||%s|||, will be lost\n",
								 SKYPIAX_P_LOG, buffer);
							memset(buffer, '\0', 17000);
						}
						if (continue_is_broken) {
							continue_is_broken = 0;
							there_were_continues = 1;
						}
					}
					if (an_event.xclient.message_type == atom_continue) {
						//DEBUGA_SKYPE ("CONTINUE BUF=|||%s|||\n", SKYPIAX_P_LOG, buf);

						if (!strlen(buffer)) {
							DEBUGA_SKYPE
								("Got a 'continue' XAtom without a previous 'begin'. It's value (between vertical bars) is=|||%s|||, let's store it and hope next 'begin' will be the good one\n",
								 SKYPIAX_P_LOG, buf);
							strcat(continuebuffer, buf);
							continue_is_broken = 1;
							if (!strncmp(buf, "ognised identity", 15)) {
								WARNINGA
									("Got a 'continue' XAtom without a previous 'begin'. It's value (between vertical bars) is=|||%s|||. Let's introduce a 1 second delay.\n",
									 SKYPIAX_P_LOG, buf);
								skypiax_sleep(1000000);	//1 sec
							}
							skypiax_sleep(1000);	//0.1 msec
							break;
						}
					}
					if (continue_is_broken) {
						XFlush(disp);
						skypiax_sleep(1000);	//0.1 msec
						continue;
					}
					//DEBUGA_SKYPE ("i=%d, buffer=|||%s|||\n", SKYPIAX_P_LOG, i, buffer);
					strcat(buffer, buf);
					//DEBUGA_SKYPE ("i=%d, buffer=|||%s|||\n", SKYPIAX_P_LOG, i, buffer);
					strcat(buffer, continuebuffer);
					//DEBUGA_SKYPE ("i=%d, buffer=|||%s|||\n", SKYPIAX_P_LOG, i, buffer);
					memset(continuebuffer, '\0', 17000);

					if (i < 20 || there_were_continues) {	/* last fragment */
						unsigned int howmany;

						howmany = strlen(b) + 1;

						howmany = write(SkypiaxHandles->fdesc[1], b, howmany);
						//DEBUGA_SKYPE ("RECEIVED=|||%s|||\n", SKYPIAX_P_LOG, buffer);
						memset(buffer, '\0', 17000);
						XFlush(disp);
						there_were_continues = 0;
					}

					skypiax_sleep(1000);	//0.1 msec
					break;
				default:
					skypiax_sleep(1000);	//0.1 msec
					break;
				}
			}
		}
	} else {
		ERRORA("Skype is not running, maybe crashed. Please run/restart Skype and relaunch Skypiax\n", SKYPIAX_P_LOG);
		running = 0;
		//if(disp)
		//XCloseDisplay(disp);
		return NULL;
	}
	//running = 0;
	//if(disp)
	//XCloseDisplay(disp);
	return NULL;

}
#endif // WIN32

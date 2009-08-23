/*
 * soliloque-server, an open source implementation of the TeamSpeak protocol.
 * Copyright (C) 2009 Hugo Camboulive <hugo.camboulive AT gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "control_packet.h"
#include "log.h"
#include "packet_tools.h"
#include "acknowledge_packet.h"
#include "server_stat.h"
#include "database.h"

#include <errno.h>
#include <string.h>

/**
 * Notify players that a channel has been deleted
 *
 * @param s the server
 * @param del_id the id of the deleted channel
 */
static void s_notify_channel_deleted(struct server *s, uint32_t del_id)
{
	char *data, *ptr;
	struct player *tmp_pl;
	int data_size = 30;
	size_t iter;

	data = (char *)calloc(data_size, sizeof(char));
	if (data == NULL) {
		logger(LOG_WARN, "s_notify_channel_deleted, packet allocation failed : %s.", strerror(errno));
		return;
	}
	ptr = data;

	*(uint16_t *)ptr = PKT_TYPE_CTL;	ptr += 2;	/* */
	*(uint16_t *)ptr = CTL_CHANDELETE;	ptr += 2;	/* */
	/* private ID */			ptr += 4;	/* filled later */
	/* public ID */				ptr += 4;	/* filled later */
	/* packet counter */			ptr += 4;	/* filled later */
	/* packet version */			ptr += 4;	/* not done yet */
	/* empty checksum */			ptr += 4;	/* filled later */
	*(uint32_t *)ptr = del_id;		ptr += 2;	/* ID of deleted channel */
	*(uint32_t *)ptr = 1;			ptr += 4;	/* ????? the previous 
								   ptr += 2 is not an error*/

	ar_each(struct player *, tmp_pl, iter, s->players)
			*(uint32_t *)(data + 4) = tmp_pl->private_id;
			*(uint32_t *)(data + 8) = tmp_pl->public_id;
			*(uint32_t *)(data + 12) = tmp_pl->f0_s_counter;
			packet_add_crc_d(data, data_size);
			send_to(s, data, data_size, 0, tmp_pl);
			tmp_pl->f0_s_counter++;
	ar_end_each;
	free(data);
}

/**
 * Notify a player that his request to delete a channel failed (channel not empty)
 *
 * @param s the server
 * @param pl the player who wants to delete the channel
 * @param pkt_cnt the counter of the packet we failed to execute
 */
static void s_resp_cannot_delete_channel(struct player *pl, uint32_t pkt_cnt)
{
	char *data, *ptr;
	int data_size = 30;
	struct server *s = pl->in_chan->in_server;

	data = (char *)calloc(data_size, sizeof(char));
	if (data == NULL) {
		logger(LOG_WARN, "s_resp_cannot_delete_channel, packet allocation failed : %s.", strerror(errno));
		return;
	}
	ptr = data;

	*(uint16_t *)ptr = PKT_TYPE_CTL;	ptr += 2;	/* */
	*(uint16_t *)ptr = CTL_CHANDELETE_ERROR;ptr += 2;	/* */
	*(uint32_t *)ptr = pl->private_id;	ptr += 4;	/* private ID */
	*(uint32_t *)ptr = pl->public_id;	ptr += 4;	/* public ID */
	*(uint32_t *)ptr = pl->f0_s_counter;	ptr += 4;	/* packet counter */
	/* packet version */			ptr += 4;	/* not done yet */
	/* empty checksum */			ptr += 4;	/* filled later */
	*(uint16_t *)ptr = 0x00d1;		ptr += 2;	/* ID of player who switched */
	*(uint32_t *)ptr = pkt_cnt;		ptr += 4;	/* ??? */
	packet_add_crc_d(data, data_size);

	send_to(s, data, data_size, 0, pl);
	pl->f0_s_counter++;
	free(data);
}

/**
 * Handles a request by a client to delete a channel.
 * This request will fail if there still are people in the channel.
 *
 * @param data the request packet
 * @param len the length of data
 * @param cli_addr the address of the sender
 * @param cli_len the length of cli_adr
 */
void *c_req_delete_channel(char *data, unsigned int len, struct player *pl)
{
	struct channel *del;
	uint32_t pkt_cnt, del_id;
	struct server *s = pl->in_chan->in_server;

	memcpy(&pkt_cnt, data + 12, 4);

	memcpy(&del_id, data + 24, 4);
	del = get_channel_by_id(s, del_id);

	send_acknowledge(pl);
	if (player_has_privilege(pl, SP_CHA_DELETE, del)) {
		if (del == NULL || del->players->used_slots > 0) {
			s_resp_cannot_delete_channel(pl, pkt_cnt);
		} else {
			/* if the channel is registered, remove it from the db */
			logger(LOG_INFO, "Flags : %i", ch_getflags(del));
			if ((ch_getflags(del) & CHANNEL_FLAG_UNREGISTERED) == 0)
				db_unregister_channel(s->conf, del);
			s_notify_channel_deleted(s, del_id);
			destroy_channel_by_id(s, del->id);
		}
	}
	return NULL;
}

/**
 * Notify all players on a server that a new channel has been created
 *
 * @param ch the new channel
 * @param creator the player who created the channel
 */
static void s_notify_channel_created(struct channel *ch, struct player *creator)
{
	char *data, *ptr;
	int data_size;
	struct player *tmp_pl;
	struct server *s = ch->in_server;
	size_t iter;

	data_size = 24 + 4;
	data_size += channel_to_data_size(ch);

	data = (char *)calloc(data_size, sizeof(char));
	if (data == NULL) {
		logger(LOG_WARN, "s_notify_channel_created, packet allocation failed : %s.", strerror(errno));
		return;
	}
	ptr = data;


	*(uint16_t *)ptr = PKT_TYPE_CTL;	ptr += 2;	/* */
	*(uint16_t *)ptr = CTL_CREATE_CH;	ptr += 2;	/* */
	/* private ID */			ptr += 4;/* filled later */
	/* public ID */				ptr += 4;/* filled later */
	/* packet counter */			ptr += 4;/* filled later */
	/* packet version */			ptr += 4;/* not done yet */
	/* empty checksum */			ptr += 4;/* filled later */
	*(uint32_t *)ptr = creator->public_id;	ptr += 4;/* id of creator */
	channel_to_data(ch, ptr);

	ar_each(struct player *, tmp_pl, iter, s->players)
			*(uint32_t *)(data + 4) = tmp_pl->private_id;
			*(uint32_t *)(data + 8) = tmp_pl->public_id;
			*(uint32_t *)(data + 12) = tmp_pl->f0_s_counter;
			packet_add_crc_d(data, data_size);
			send_to(s, data, data_size, 0, tmp_pl);
			tmp_pl->f0_s_counter++;
	ar_end_each;
	free(data);
}

/**
 * Handle a player request to create a new channel
 *
 * @param data the request packet
 * @param len the length of data
 * @param pl the player requesting the creation
 */
void *c_req_create_channel(char *data, unsigned int len, struct player *pl)
{
	struct channel *ch;
	size_t bytes_read;
	char *ptr;
	struct server *s;
	struct channel *parent;
	int priv_nok = 0;
	int flags;

	s = pl->in_chan->in_server;
	send_acknowledge(pl);

	ptr = data + 24;
	bytes_read = channel_from_data(ptr, len - (ptr - data), &ch);
	ptr += bytes_read;
	strncpy(ch->password, ptr, MIN(29, len - (ptr - data) - 1));

	flags = ch_getflags(ch);
	/* Check the privileges */
	/* TODO : when we support subchannels, context will have to
	 * be changed to the parent channel (NULL if we create the
	 * channel at the root */
	if (flags & CHANNEL_FLAG_UNREGISTERED) {
		if (!player_has_privilege(pl, SP_CHA_CREATE_UNREGISTERED, NULL))
			priv_nok++;
	} else {
		if (!player_has_privilege(pl, SP_CHA_CREATE_REGISTERED, NULL))
			priv_nok++;
	}
	if (flags & CHANNEL_FLAG_DEFAULT
			&& !player_has_privilege(pl, SP_CHA_CREATE_DEFAULT, NULL))
		priv_nok++;
	if (flags & CHANNEL_FLAG_MODERATED
			&& !player_has_privilege(pl, SP_CHA_CREATE_MODERATED, NULL))
		priv_nok++;
	if (flags & CHANNEL_FLAG_SUBCHANNELS
			&& !player_has_privilege(pl, SP_CHA_CREATE_SUBCHANNELED, NULL))
		priv_nok++;

	if (priv_nok == 0) {
		add_channel(s, ch);
		if (ch->parent_id != 0) {
			parent = get_channel_by_id(s, ch->parent_id);
			channel_add_subchannel(parent, ch);
			/* if the parent is registered, register this one */
			if (!(ch_getflags(parent) & CHANNEL_FLAG_UNREGISTERED)) {
				db_register_channel(s->conf, ch);
			}
		}
		logger(LOG_INFO, "New channel created");
		print_channel(ch);
		if (! (ch_getflags(ch) & CHANNEL_FLAG_UNREGISTERED))
			db_register_channel(s->conf, ch);
		print_channel(ch);
		s_notify_channel_created(ch, pl);
	}
	return NULL;
}

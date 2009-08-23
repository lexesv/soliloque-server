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
#include "database.h"
#include "packet_tools.h"
#include "acknowledge_packet.h"
#include "server_stat.h"

#include <errno.h>
#include <string.h>
#include <strings.h>

/**
 * Send a "player switched channel" notification to all players.
 *
 * @param s the server
 * @param pl the player who switched
 * @param from the channel the player was in
 * @param to the channel he is moving to
 */
static void s_notify_switch_channel(struct player *pl, struct channel *from, struct channel *to)
{
	char *data, *ptr;
	struct player *tmp_pl;
	int data_size = 38;
	struct server *s = pl->in_chan->in_server;
	struct player_channel_privilege *new_priv;
	size_t iter;

	data = (char *)calloc(data_size, sizeof(char));
	if (data == NULL) {
		logger(LOG_WARN, "s_notify_switch_channel, packet allocation failed : %s.", strerror(errno));
		return;
	}
	new_priv = get_player_channel_privilege(pl, to);
	ptr = data;

	*(uint16_t *)ptr = PKT_TYPE_CTL;	ptr += 2;	/* */
	*(uint16_t *)ptr = CTL_SWITCHCHAN;	ptr += 2;	/* */
	/* private ID */			ptr += 4;	/* filled later */
	/* public ID */				ptr += 4;	/* filled later */
	/* packet counter */			ptr += 4;	/* filled later */
	/* packet version */			ptr += 4;	/* not done yet */
	/* empty checksum */			ptr += 4;	/* filled later */
	*(uint32_t *)ptr = pl->public_id;	ptr += 4;	/* ID of player who switched */
	*(uint32_t *)ptr = from->id;		ptr += 4;	/* ID of previous channel */
	*(uint32_t *)ptr = to->id;		ptr += 4;	/* channel the player switched to */
	*(uint16_t *)ptr = new_priv->flags;	ptr += 2;

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
 * Handle a request from a client to switch to another channel.
 *
 * @param data the request packet
 * @param len the length of data
 * @param cli_addr the sender
 * @param cli_len the size of cli_addr
 */
void *c_req_switch_channel(char *data, unsigned int len, struct player *pl)
{
	struct channel *to, *from;
	uint32_t to_id;
	char pass[30];
	struct server *s = pl->in_chan->in_server;

	memcpy(&to_id, data + 24, 4);
	to = get_channel_by_id(s, to_id);
	bzero(pass, 30);
	strncpy(pass, data + 29, MIN(29, data[28]));

	if (to != NULL) {
		send_acknowledge(pl);		/* ACK */
		/* the player can join if one of these :
		 * - there is no password on the channel
		 * - he has the privilege to join without giving a password
		 * - he gives the correct password */
		if (!(ch_getflags(to) & CHANNEL_FLAG_PASSWORD)
				|| player_has_privilege(pl, SP_CHA_JOIN_WO_PASS, to)
				|| strcmp(pass, to->password) == 0) {
			logger(LOG_INFO, "Player switching to channel %s.", to->name);
			from = pl->in_chan;
			if (move_player(pl, to)) {
				s_notify_switch_channel(pl, from, to);
				logger(LOG_INFO, "Player moved, notify sent.");
				/* TODO change privileges */
			}
		}
	}
	return NULL;
}

/**
 * Notify all players that a player's channel privilege
 * has been granted/revoked.
 *
 * @param pl the player who granted/revoked the privilege
 * @param tgt the player whose privileges are going to change
 * @param right the offset of the right (1 << right == CHANNEL_PRIV_XXX)
 * @param on_off switch this right on or off
 */
static void s_notify_player_ch_priv_changed(struct player *pl, struct player *tgt, char right, char on_off)
{
	char *data, *ptr;
	struct player *tmp_pl;
	int data_size = 34;
	struct server *s = pl->in_chan->in_server;
	size_t iter;

	data = (char *)calloc(data_size, sizeof(char));
	if (data == NULL) {
		logger(LOG_WARN, "s_notify_player_ch_priv_changed, packet allocation failed : %s.", strerror(errno));
		return;
	}
	ptr = data;

	*(uint16_t *)ptr = PKT_TYPE_CTL;	ptr += 2;	/* */
	*(uint16_t *)ptr = CTL_CHANGE_PL_CHPRIV;ptr += 2;	/* */
	/* private ID */			ptr += 4;/* filled later */
	/* public ID */				ptr += 4;/* filled later */
	/* packet counter */			ptr += 4;/* filled later */
	/* packet version */			ptr += 4;/* not done yet */
	/* empty checksum */			ptr += 4;/* filled later */
	*(uint32_t *)ptr = tgt->public_id;	ptr += 4;/* ID of player whose channel priv changed */
	*(uint8_t *)ptr = on_off;		ptr += 1;/* switch the priv ON/OFF */
	*(uint8_t *)ptr = right;		ptr += 1;/* offset of the privilege (1<<right) */
	*(uint32_t *)ptr = pl->public_id;	ptr += 4;/* ID of the player who changed the priv */

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
 * Handle a request to change a player's channel privileges
 *
 * @param data the request packet
 * @param len the length of data
 * @param pl the player asking for it
 */
void *c_req_change_player_ch_priv(char *data, unsigned int len, struct player *pl)
{
	struct player *tgt;
	uint32_t tgt_id;
	char on_off, right;
	int priv_required;

	send_acknowledge(pl);		/* ACK */

	memcpy(&tgt_id, data + 24, 4);
	on_off = data[28];
	right = data[29];
	tgt = get_player_by_public_id(pl->in_chan->in_server, tgt_id);

	switch (1 << right) {
	case CHANNEL_PRIV_CHANADMIN:
		priv_required = (on_off == 0) ? SP_PL_GRANT_CA : SP_PL_REVOKE_CA;
		break;
	case CHANNEL_PRIV_OP:
		priv_required = (on_off == 0) ? SP_PL_GRANT_OP : SP_PL_REVOKE_OP;
		break;
	case CHANNEL_PRIV_VOICE:
		priv_required = (on_off == 0) ? SP_PL_GRANT_VOICE : SP_PL_REVOKE_VOICE;
		break;
	case CHANNEL_PRIV_AUTOOP:
		priv_required = (on_off == 0) ? SP_PL_GRANT_AUTOOP : SP_PL_REVOKE_AUTOOP;
		break;
	case CHANNEL_PRIV_AUTOVOICE:
		priv_required = (on_off == 0) ? SP_PL_GRANT_AUTOVOICE : SP_PL_REVOKE_AUTOVOICE;
		break;
	default:
		return NULL;
	}
	if (tgt != NULL && player_has_privilege(pl, priv_required, tgt->in_chan)) {
		logger(LOG_INFO, "Player priv before : 0x%x", player_get_channel_privileges(tgt, tgt->in_chan));
		if (on_off == 2)
			player_clr_channel_privilege(tgt, tgt->in_chan, (1 << right));
		else if(on_off == 0)
			player_set_channel_privilege(tgt, tgt->in_chan, (1 << right));
		logger(LOG_INFO, "Player priv after  : 0x%x", player_get_channel_privileges(tgt, tgt->in_chan));
		s_notify_player_ch_priv_changed(pl, tgt, right, on_off);
	}
	return NULL;
}

/**
 * Notify all players that a player's global flags
 * has been granted/revoked.
 *
 * @param pl the player who granted/revoked the privilege
 * @param tgt the player whose privileges are going to change
 * @param right the offset of the right (1 << right == GLOBAL_FLAG_XXX)
 * @param on_off switch this right on or off (0 = add, 2 = remove)
 */
void s_notify_player_sv_right_changed(struct player *pl, struct player *tgt, char right, char on_off)
{
	char *data, *ptr;
	struct player *tmp_pl;
	int data_size = 34;
	struct server *s = tgt->in_chan->in_server;
	size_t iter;

	data = (char *)calloc(data_size, sizeof(char));
	if (data == NULL) {
		logger(LOG_WARN, "s_notify_player_sv_right_changed, packet allocation failed : %s.", strerror(errno));
		return;
	}
	ptr = data;

	*(uint16_t *)ptr = PKT_TYPE_CTL;	ptr += 2;	/* */
	*(uint16_t *)ptr = CTL_CHANGE_PL_SVPRIV;ptr += 2;	/* */
	/* private ID */			ptr += 4;/* filled later */
	/* public ID */				ptr += 4;/* filled later */
	/* packet counter */			ptr += 4;/* filled later */
	/* packet version */			ptr += 4;/* not done yet */
	/* empty checksum */			ptr += 4;/* filled later */
	*(uint32_t *)ptr = tgt->public_id;	ptr += 4;/* ID of player whose global flags changed */
	*(uint8_t *)ptr = on_off;		ptr += 1;/* set or unset the flag */
	*(uint8_t *)ptr = right;		ptr += 1;/* offset of the flag (1 << right) */
	if (pl != NULL) {
		*(uint32_t *)ptr = pl->public_id;	ptr += 4;/* ID of player who changed the flag */
	} else {
		*(uint32_t *)ptr = 0;			ptr += 4;
	}

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
 * Handle a request to change a player's global flags
 *
 * @param data the request packet
 * @param len the length of data
 * @param pl the player asking for it
 */
void *c_req_change_player_sv_right(char *data, unsigned int len, struct player *pl)
{
	struct player *tgt;
	uint32_t tgt_id;
	char on_off, right;
	int priv_required;
	struct channel *ch;
	struct player_channel_privilege *priv;
	size_t iter, iter2;

	send_acknowledge(pl);		/* ACK */

	memcpy(&tgt_id, data + 24, 4);
	on_off = data[28];
	right = data[29];
	tgt = get_player_by_public_id(pl->in_chan->in_server, tgt_id);

	switch (1 << right) {
	case GLOBAL_FLAG_SERVERADMIN:
		priv_required = (on_off == 0) ? SP_PL_GRANT_SA : SP_PL_REVOKE_SA;
		break;
	case GLOBAL_FLAG_ALLOWREG:
		priv_required = (on_off == 0) ? SP_PL_GRANT_ALLOWREG : SP_PL_REVOKE_ALLOWREG;
		break;
	case GLOBAL_FLAG_REGISTERED:
		priv_required = (on_off == 0) ? SP_PL_ALLOW_SELF_REG : SP_PL_DEL_REGISTRATION;
		break;
	default:
		logger(LOG_WARN, "c_req_change_player_sv_right : not implemented for privilege : %i", 1<<right);
		return NULL;
	}
	if (tgt != NULL && player_has_privilege(pl, priv_required, tgt->in_chan)) {
		logger(LOG_INFO, "Player sv rights before : 0x%x", tgt->global_flags);
		if (on_off == 2) {
			tgt->global_flags &= (0xFF ^ (1 << right));
			/* special case : removing a registration */
			if (1 << right == GLOBAL_FLAG_REGISTERED) {
				db_del_registration(tgt->in_chan->in_server->conf, tgt->in_chan->in_server, tgt->reg);
				/* associate the player privileges to the player instead of the registration */
				ar_each(struct channel *, ch, iter, tgt->in_chan->in_server->chans)
					ar_each(struct player_channel_privilege *, priv, iter2, ch->pl_privileges)
						if (priv->reg == PL_CH_PRIV_REGISTERED && priv->pl_or_reg.reg == tgt->reg) {
							priv->reg = PL_CH_PRIV_UNREGISTERED;
							priv->pl_or_reg.pl = tgt;
						}
					ar_end_each;
				ar_end_each;
				free(tgt->reg);
				tgt->reg = NULL;
			}
		} else if(on_off == 0) {
			tgt->global_flags |= (1 << right);
		}

		/* special case : registration */
		logger(LOG_INFO, "Player sv rights after  : 0x%x", tgt->global_flags);
		s_notify_player_sv_right_changed(pl, tgt, right, on_off);
	}
	return NULL;
}

/**
 * Notify all players of a player's status change
 * has been granted/revoked.
 *
 * @param pl the player who granted/revoked the privilege
 * @param tgt the player whose privileges are going to change
 * @param right the offset of the right (1 << right == CHANNEL_PRIV_XXX)
 * @param on_off switch this right on or off
 */
static void s_notify_player_attr_changed(struct player *pl, uint16_t new_attr)
{
	char *data, *ptr;
	struct player *tmp_pl;
	int data_size = 30;
	struct server *s = pl->in_chan->in_server;
	size_t iter;

	data = (char *)calloc(data_size, sizeof(char));
	if (data == NULL) {
		logger(LOG_WARN, "s_notify_player_attr_changed, packet allocation failed : %s.", strerror(errno));
		return;
	}
	ptr = data;

	*(uint16_t *)ptr = PKT_TYPE_CTL;	ptr += 2;	/* */
	*(uint16_t *)ptr = CTL_CHANGE_PL_STATUS;ptr += 2;	/* */
	/* private ID */			ptr += 4;	/* filled later */
	/* public ID */				ptr += 4;	/* filled later */
	/* packet counter */			ptr += 4;	/* filled later */
	/* packet version */			ptr += 4;	/* not done yet */
	/* empty checksum */			ptr += 4;	/* filled later */
	*(uint32_t *)ptr = pl->public_id;	ptr += 4;	/* ID of player whose attr changed */
	*(uint16_t *)ptr = new_attr;		ptr += 2;	/* new attributes */

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
 * Handle a request to change a player's global flags
 *
 * @param data the request packet
 * @param len the length of data
 * @param pl the player asking for it
 */
void *c_req_change_player_attr(char *data, unsigned int len, struct player *pl)
{
	uint16_t attributes;

	send_acknowledge(pl);		/* ACK */

	memcpy(&attributes, data + 24, 2);
	logger(LOG_INFO, "Player sv rights before : 0x%x", pl->player_attributes);
	pl->player_attributes = attributes;
	logger(LOG_INFO, "Player sv rights after  : 0x%x", pl->player_attributes);
	s_notify_player_attr_changed(pl, attributes);
	return NULL;
}

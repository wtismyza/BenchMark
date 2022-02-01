/*
 * ox.h
 * vim: expandtab:ts=4:sts=4:sw=4
 *
 * Copyright (C) 2020 Stefan Kropp <stefan@debxwoody.de> 
 *
 * This file is part of Profanity.
 *
 * Profanity is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Profanity is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Profanity.  If not, see <https://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link the code of portions of this program with the OpenSSL library under
 * certain conditions as described in each individual source file, and
 * distribute linked combinations including the two.
 *
 * You must obey the GNU General Public License in all respects for all of the
 * code used other than OpenSSL. If you modify file(s) with this exception, you
 * may extend this exception to your version of the file(s), but you are not
 * obligated to do so. If you do not wish to do so, delete this exception
 * statement from your version. If you delete this exception statement from all
 * source files in the program, then also delete it here.
 *
 */

/*!
 * \page OX OX Implementation
 * 
 * \section OX XEP-0373: OpenPGP for XMPP
 * XEP-0373: OpenPGP for XMPP (OX) is the implementation of OpenPGP for XMPP
 * replace the XEP-0027.  
 *
 * https://xmpp.org/extensions/xep-0373.html
 */

/*!
 * \brief Announcing OpenPGP public key from file to PEP.
 *
 * Reads the public key from the given file. Checks the key-information and
 * pushes the key on PEP.
 *  
 * https://xmpp.org/extensions/xep-0373.html#announcing-pubkey
 *
 * \param filename name of the file with the public key 
 * \return TRUE: success; FALSE: failed
 */

gboolean ox_announce_public_key(const char* const filename);

/*!
 * \brief Discovering Public Keys of a User.
 *
 * Reads the public key from a JIDs PEP.
 *
 * \param jid JID
 */

void ox_discover_public_key(const char* const jid);

void ox_request_public_key(const char* const jid, const char* const fingerprint);

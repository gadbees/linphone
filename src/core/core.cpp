/*
 * core.cpp
 * Copyright (C) 2010-2018 Belledonne Communications SARL
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <mediastreamer2/mscommon.h>
#include <xercesc/util/PlatformUtils.hpp>

#include "address/address-p.h"
#include "call/call.h"
#include "chat/chat-room/chat-room.h"
#include "conference/handlers/local-conference-list-event-handler.h"
#include "conference/handlers/remote-conference-list-event-handler.h"
#include "core/core-listener.h"
#include "core/core-p.h"
#include "logger/logger.h"
#include "paths/paths.h"

// TODO: Remove me later.
#include "c-wrapper/c-wrapper.h"
#include "private.h"

#define LINPHONE_DB "linphone.db"

// =============================================================================

using namespace std;

LINPHONE_BEGIN_NAMESPACE

void CorePrivate::init () {
	L_Q();
	mainDb.reset(new MainDb(q->getSharedFromThis()));
	remoteListEventHandler = makeUnique<RemoteConferenceListEventHandler>(q->getSharedFromThis());
	localListEventHandler = makeUnique<LocalConferenceListEventHandler>(q->getSharedFromThis());

	AbstractDb::Backend backend;
	string uri = L_C_TO_STRING(lp_config_get_string(linphone_core_get_config(L_GET_C_BACK_PTR(q)), "storage", "uri", nullptr));
	if (!uri.empty())
		backend = strcmp(lp_config_get_string(linphone_core_get_config(L_GET_C_BACK_PTR(q)), "storage", "backend", nullptr), "mysql") == 0
			? MainDb::Mysql
			: MainDb::Sqlite3;
	else {
		backend = AbstractDb::Sqlite3;
		uri = q->getDataPath() + LINPHONE_DB;
	}

	lInfo() << "Opening linphone database: " << uri;
	if (!mainDb->connect(backend, uri))
		lFatal() << "Unable to open linphone database.";

	loadChatRooms();
}

void CorePrivate::registerListener (CoreListener *listener) {
	listeners.push_back(listener);
}

void CorePrivate::unregisterListener (CoreListener *listener) {
	listeners.remove(listener);
}

void CorePrivate::uninit () {
	L_Q();
	while (!calls.empty()) {
		calls.front()->terminate();
		linphone_core_iterate(L_GET_C_BACK_PTR(q));
		ms_usleep(10000);
	}

	chatRooms.clear();
	chatRoomsById.clear();
	noCreatedClientGroupChatRooms.clear();

	remoteListEventHandler = nullptr;
	localListEventHandler = nullptr;

	AddressPrivate::clearSipAddressesCache();
}

// -----------------------------------------------------------------------------

void CorePrivate::notifyGlobalStateChanged (LinphoneGlobalState state) {
	auto listenersCopy = listeners; // Allow removable of a listener in its own call
	for (const auto &listener : listenersCopy)
		listener->onGlobalStateChanged(state);
}

void CorePrivate::notifyNetworkReachable (bool sipNetworkReachable, bool mediaNetworkReachable) {
	auto listenersCopy = listeners; // Allow removable of a listener in its own call
	for (const auto &listener : listenersCopy)
		listener->onNetworkReachable(sipNetworkReachable, mediaNetworkReachable);
}

void CorePrivate::notifyRegistrationStateChanged (LinphoneProxyConfig *cfg, LinphoneRegistrationState state, const string &message) {
	auto listenersCopy = listeners; // Allow removable of a listener in its own call
	for (const auto &listener : listenersCopy)
		listener->onRegistrationStateChanged(cfg, state, message);
}

void CorePrivate::notifyEnteringBackground () {
	if (isInBackground)
		return;

	isInBackground = true;
	auto listenersCopy = listeners; // Allow removable of a listener in its own call
	for (const auto &listener : listenersCopy)
		listener->onEnteringBackground();
}

void CorePrivate::notifyEnteringForeground () {
	if (!isInBackground)
		return;

	isInBackground = false;
	auto listenersCopy = listeners; // Allow removable of a listener in its own call
	for (const auto &listener : listenersCopy)
		listener->onEnteringForeground();
}

// =============================================================================

Core::Core () : Object(*new CorePrivate) {
	xercesc::XMLPlatformUtils::Initialize();
}

Core::~Core () {
	lInfo() << "Destroying core: " << this;
	xercesc::XMLPlatformUtils::Terminate();
}

shared_ptr<Core> Core::create (LinphoneCore *cCore) {
	// Do not use `make_shared` => Private constructor.
	shared_ptr<Core> core = shared_ptr<Core>(new Core);
	L_SET_CPP_PTR_FROM_C_OBJECT(cCore, core);
	return core;
}

// ---------------------------------------------------------------------------
// Application lifecycle.
// ---------------------------------------------------------------------------

void Core::enterBackground () {
	L_D();
	d->notifyEnteringBackground();
}

void Core::enterForeground () {
	L_D();
	d->notifyEnteringForeground();
}

// ---------------------------------------------------------------------------
// C-Core.
// ---------------------------------------------------------------------------

LinphoneCore *Core::getCCore () const {
	return L_GET_C_BACK_PTR(this);
}

// -----------------------------------------------------------------------------
// Paths.
// -----------------------------------------------------------------------------

string Core::getDataPath () const {
	return Paths::getPath(Paths::Data, static_cast<PlatformHelpers *>(L_GET_C_BACK_PTR(this)->platform_helper));
}

string Core::getConfigPath () const {
	return Paths::getPath(Paths::Config, static_cast<PlatformHelpers *>(L_GET_C_BACK_PTR(this)->platform_helper));
}

// -----------------------------------------------------------------------------
// Misc.
// -----------------------------------------------------------------------------

int Core::getUnreadChatMessageCount () const {
	L_D();
	return d->mainDb->getUnreadChatMessageCount();
}

int Core::getUnreadChatMessageCount (const IdentityAddress &localAddress) const {
	L_D();
	int count = 0;
	for (const auto &chatRoom : d->chatRooms)
		if (chatRoom->getLocalAddress() == localAddress)
			count += chatRoom->getUnreadChatMessageCount();
	return count;
}

int Core::getUnreadChatMessageCountFromActiveLocals () const {
	L_D();

	set<IdentityAddress> localAddresses;
	{
		LinphoneAddress *address = linphone_core_get_primary_contact_parsed(getCCore());
		localAddresses.insert(*L_GET_CPP_PTR_FROM_C_OBJECT(address));
		linphone_address_unref(address);
	}

	for (const bctbx_list_t *it = linphone_core_get_proxy_config_list(getCCore()); it; it = bctbx_list_next(it))
		localAddresses.insert(*L_GET_CPP_PTR_FROM_C_OBJECT(static_cast<LinphoneProxyConfig *>(it->data)->identity_address));

	int count = 0;
	for (const auto &chatRoom : d->chatRooms) {
		auto it = localAddresses.find(chatRoom->getLocalAddress());
		if (it != localAddresses.end())
			count += chatRoom->getUnreadChatMessageCount();
	}
	return count;
}

LINPHONE_END_NAMESPACE

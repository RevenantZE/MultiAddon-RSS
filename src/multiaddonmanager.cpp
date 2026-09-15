/**
 * =============================================================================
 * MultiAddonManager
 * Copyright (C) 2024-2025 xen
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "networkbasetypes.pb.h"
#include "gameevents.pb.h"

#include <stdio.h>
#include "multiaddonmanager.h"
#include "module.h"
#include "utils/plat.h"
#include "networksystem/inetworkmessages.h"
#include "convar.h"
#include "hoststate.h"
#include "igameeventsystem.h"
#include "serversideclient.h"
#include "filesystem.h"
#include "steam/steam_gameserver.h"
#include <string>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <chrono>
#include "iserver.h"

#include "tier0/memdbgon.h"

CConVar<bool> mm_addon_mount_download("mm_addon_mount_download", FCVAR_NONE, "Whether to download an addon upon mounting even if it's installed", false);
CConVar<bool> mm_block_disconnect_messages("mm_block_disconnect_messages", FCVAR_NONE, "Whether to block \"loop shutdown\" disconnect messages", false);
CConVar<bool> mm_cache_clients_with_addons("mm_cache_clients_with_addons", FCVAR_NONE, "Whether to cache clients addon download list, this will prevent reconnects on mapchange/rejoin", false);
CConVar<float> mm_cache_clients_duration("mm_cache_clients_duration", FCVAR_NONE, "How long to cache clients' downloaded addons list in seconds, pass 0 for forever.", 0.0f);
CConVar<float> mm_addon_connection_timeout("mm_addon_connection_timeout", FCVAR_NONE, "How long until clients are timed out while downloading the first required addon (usually the current map), 0 disables", 30.f);
CConVar<float> mm_extra_addons_timeout("mm_extra_addons_timeout", FCVAR_NONE, "How long until clients are timed out in between connects for extra addons in seconds, requires mm_extra_addons to be used", 10.f);

CConVar<bool> mm_addon_debug("mm_addon_debug", FCVAR_NONE, "Whether to print some extra debug information", false);

static constexpr const char *g_RssAssetPreferencesPath = "addons/multiaddonmanager/data/rss_asset_preferences.jsonc";
static constexpr const char *g_RssAssetPreferencesTempPath = "addons/multiaddonmanager/data/rss_asset_preferences.jsonc.tmp";
static constexpr const char *g_RssAssetLegacyOptOutPath = "addons/multiaddonmanager/data/rss_asset_optout.txt";

void Message(const char *msg, ...)
{
	va_list args;
	va_start(args, msg);

	char buf[1024] = {};
	V_vsnprintf(buf, sizeof(buf) - 1, msg, args);

	LoggingSystem_Log(2, LS_MESSAGE, Color(0, 255, 200), "[MultiAddonManager] %s", buf);

	va_end(args);
}

void Panic(const char *msg, ...)
{
	va_list args;
	va_start(args, msg);

	char buf[1024] = {};
	V_vsnprintf(buf, sizeof(buf) - 1, msg, args);

	Warning("[MultiAddonManager] %s", buf);

	va_end(args);
}

void StringToVector(const char *pszString, CUtlVector<std::string> &vector)
{
	std::stringstream stream(pszString);

	vector.RemoveAll();

	while (stream.good())
	{
		std::string substr;
		getline(stream, substr, ',');

		if (!substr.empty())
			vector.AddToTail(substr);
	}
}

std::string VectorToString(CUtlVector<std::string> &vector)
{
	std::string result;

	FOR_EACH_VEC(vector, i)
	{
		result += vector[i];

		if (i + 1 < vector.Count())
			result += ',';
	}

	return result;
}

ISteamUGC *GetSteamUGC()
{
	if (g_pEngineServer->IsDedicatedServer())
		return SteamGameServerUGC();
	else
		return SteamUGC();
}

typedef bool (FASTCALL *SendNetMessage_t)(CServerSideClientBase *, const CNetMessage*, NetChannelBufType_t);
typedef void (FASTCALL *HostStateRequest_t)(CHostStateMgr*, CHostStateRequest*);
typedef void (FASTCALL *ReplyConnection_t)(INetworkGameServer *, CServerSideClient *);
typedef uint64 (FASTCALL *ScriptGetAddon_t)();

class GameSessionConfiguration_t { };

// Signatures

// "Discarding pending request '%s, %u'\n"
// "Sending S2C_CONNECTION to %s [addons:'%s']\n"
// First call without args in func with "Saving existing workshop save file from %s\n"
#ifdef PLATFORM_WINDOWS
constexpr const char *g_HostStateRequest_Sig = "48 89 74 24 ? 57 48 83 EC ? 33 F6 48 8B FA 48 39 35";
constexpr const char *g_ReplyConnection_Sig = "48 8B C4 55 41 55 41 56";
constexpr const char *g_ScriptGetAddon_Sig = "40 56 48 83 EC ? 48 8B 0D ? ? ? ? 48 8D 54 24";
#else
constexpr const char *g_HostStateRequest_Sig = "55 48 89 E5 41 56 41 55 41 54 49 89 F4 53 48 83 7F";
constexpr const char *g_ReplyConnection_Sig = "55 B9 ? ? ? ? 41 B8";
constexpr const char *g_ScriptGetAddon_Sig = "55 48 89 E5 41 55 41 54 48 8D 75 ? 53 48 83 EC ? 48 8D 05 ? ? ? ? 48 C7 45 ? ? ? ? ? 48 C7 45 ? ? ? ? ? 48 8B 38 48 8B 07 FF 90 ? ? ? ? 8B 55";
#endif


// Offsets
constexpr int g_iServerAddonsOffset = 344;
constexpr int g_iClientListOffset = 584;

#ifdef PLATFORM_WINDOWS
constexpr int g_iSendNetMessageOffset = 15;
#else
constexpr int g_iSendNetMessageOffset = 16;
#endif

/* 
The general workflow is defined as follows:
0. The server defines a list of server side addons and global client side addons to mount.
1. Client connects and request for the list of addons through ReplyConnection. MAM get the full list of addons to load.
2. If there is at least one addon to load, client will be prompted to download the first addon.
3. Once done, client reconnects and ClientConnect fires. If connected within the timeout interval, MAM marks this first addon as downloaded, 
	then check if there are other addons to download. If there is at least one, it will send a signon message to the client through SendNetMessage.
4. Client will be prompted to download the next addon. Client reconnects and ClientConnect fires again. 
	MAM marks the previous downloading addon (the one sent in the previous signon message) as done, and keep sending signon messages until all addons are downloaded.
5. Once all addons are downloaded, MAM stops sending custom signon messages.
Note: 
The list of addons to download does not have to be in order, but the addon list that the client uses to load is. This order is somewhat arbitrarily defined as follows: 
	- Original server workshop map (if any)
	- Server side *mounted* addons (m_MountedAddons)
	- Client side global addons (m_GlobalClientAddons)
	- Client side client-specific addons (addonsToLoad).
While plugins using the interface can add/remove addons at any time between these steps, it should be fine since the list of addon to load is newly checked every time the client connects.
*/

enum ClientConnectedState_t
{
	CLIENTCONN_NONE,
	CLIENTCONN_CONNECTING,
	CLIENTCONN_JOINED
};

struct ClientAddonInfo_t
{
	double lastActiveTime {};
	CUtlVector<std::string> addonsToLoad;
	CUtlVector<std::string> downloadedAddons;
	std::string currentPendingAddon;
	ClientConnectedState_t connectedState = CLIENTCONN_NONE;
	double connectionStartTime {};
	uint32 rssCacheGeneration = 0;
	RssAssetMode appliedRssMode = RssAssetMode::Disabled;
};

std::unordered_map<uint64, ClientAddonInfo_t> g_ClientAddons;

CUtlVector<CServerSideClient *> *GetClientList()
{
	if (!g_pNetworkServerService)
		return nullptr;
	INetworkGameServer *server = g_pNetworkServerService->GetIGameServer();
	if (!server)
		return nullptr;
	return (CUtlVector<CServerSideClient *> *)((char *)server + g_iClientListOffset);
}

static CServerSideClient *FindClientBySteamID(uint64 steamID64)
{
	CUtlVector<CServerSideClient *> *clients = GetClientList();
	if (!clients)
		return nullptr;

	CUtlVector<CServerSideClient *> &clientList = *clients;
	FOR_EACH_VEC(clientList, i)
	{
		CServerSideClient *client = clientList[i];
		if (client && client->GetClientSteamID().ConvertToUint64() == steamID64)
			return client;
	}

	return nullptr;
}

static rss_flow::Mode ToFlowMode(RssAssetMode mode)
{
	return static_cast<rss_flow::Mode>(static_cast<uint32>(mode));
}

static RssAssetFlowPhase ToPublicPhase(rss_flow::Phase phase)
{
	return static_cast<RssAssetFlowPhase>(static_cast<uint32>(phase));
}

static RssAssetResult ToPublicResult(rss_flow::Result result)
{
	return static_cast<RssAssetResult>(static_cast<uint32>(result));
}

static rss_prefs::Mode ToPreferenceMode(RssAssetMode mode)
{
	return static_cast<rss_prefs::Mode>(static_cast<uint32>(mode));
}

static RssAssetMode ToPublicMode(rss_prefs::Mode mode)
{
	return static_cast<RssAssetMode>(static_cast<uint32>(mode));
}

std::vector<std::string> MultiAddonManager::GetEffectiveRssAddons() const
{
	std::vector<std::string> extra;
	FOR_EACH_VEC(m_ExtraAddons, i)
		extra.push_back(m_ExtraAddons[i]);
	return rss_flow::EffectiveExtra(extra, GetCurrentWorkshopMap());
}

bool MultiAddonManager::IsRssAssetAddon(const char *pszAddon) const
{
	if (!pszAddon || !*pszAddon)
		return false;
	const std::vector<std::string> effective = GetEffectiveRssAddons();
	return std::find(effective.begin(), effective.end(), pszAddon) != effective.end();
}

void MultiAddonManager::SetCurrentWorkshopMap(const char *pszWorkshopID)
{
	const std::string next = pszWorkshopID ? pszWorkshopID : "";
	if (m_sCurrentWorkshopMap == next)
		return;
	m_sCurrentWorkshopMap = next;
	OnRssAddonConfigurationChanged(true);
}

void MultiAddonManager::ClearCurrentWorkshopMap()
{
	SetCurrentWorkshopMap("");
}

void MultiAddonManager::InvalidateRssAssetCache()
{
	const std::vector<std::string> current = GetEffectiveRssAddons();
	for (std::unordered_map<uint64, ClientAddonInfo_t>::iterator it = g_ClientAddons.begin();
		it != g_ClientAddons.end(); ++it)
	{
		ClientAddonInfo_t &info = it->second;
		for (int i = info.downloadedAddons.Count() - 1; i >= 0; --i)
		{
			const std::string &id = info.downloadedAddons[i];
			if (std::find(m_RssEffectiveAddons.begin(), m_RssEffectiveAddons.end(), id) != m_RssEffectiveAddons.end() ||
				std::find(current.begin(), current.end(), id) != current.end())
				info.downloadedAddons.Remove(i);
		}
		if (std::find(m_RssEffectiveAddons.begin(), m_RssEffectiveAddons.end(), info.currentPendingAddon) != m_RssEffectiveAddons.end() ||
			std::find(current.begin(), current.end(), info.currentPendingAddon) != current.end())
			info.currentPendingAddon.clear();
		info.rssCacheGeneration = 0;
	}
}

void MultiAddonManager::OnRssAddonConfigurationChanged(bool forceGeneration)
{
	const std::vector<std::string> current = GetEffectiveRssAddons();
	if (!forceGeneration && current == m_RssEffectiveAddons)
		return;
	InvalidateRssAssetCache();
	m_RssEffectiveAddons = current;
	++m_nRssAddonGeneration;
	if (!m_nRssAddonGeneration)
		m_nRssAddonGeneration = 1;
	const rss_flow::TimePoint now = rss_flow::Clock::now();
	for (std::unordered_map<uint64, rss_flow::Flow>::iterator it = m_RssAssetFlows.begin();
		it != m_RssAssetFlows.end(); ++it)
	{
		if (!rss_flow::IsTerminal(it->second.phase))
			rss_flow::Finish(it->second, rss_flow::Phase::Failed, rss_flow::Result::ConfigChanged, now);
	}
}

bool MultiAddonManager::GetCurrentRssSession(uint64 steamID64, RssClientSession &session,
	CServerSideClient **outClient)
{
	CServerSideClient *client = FindClientBySteamID(steamID64);
	if (!client || client->GetSignonState() != SIGNONSTATE_FULL || !client->GetNetChannel())
		return false;
	const CPlayerSlot slot = client->GetPlayerSlot();
	if (!g_pEngineServer->IsClientFullyAuthenticated(slot))
		return false;
	RssClientSession &stored = m_RssClientSessions[steamID64];
	const int slotValue = slot.Get();
	const int userId = client->GetUserID().Get();
	if (!stored.serial || stored.slot != slotValue || (stored.userId >= 0 && stored.userId != userId))
	{
		m_RssDisconnectEvidence.erase(steamID64);
		stored.slot = slotValue;
		stored.userId = userId;
		stored.serial = m_nNextRssSessionSerial++;
		if (!stored.serial)
			stored.serial = m_nNextRssSessionSerial++;
	}
	else if (stored.userId < 0)
	{
		stored.userId = userId;
		std::unordered_map<uint64, rss_flow::Flow>::iterator flow = m_RssAssetFlows.find(steamID64);
		if (flow != m_RssAssetFlows.end() && flow->second.session.serial == stored.serial &&
			flow->second.session.slot == stored.slot && flow->second.session.userId < 0)
			flow->second.session.userId = userId;
	}
	session = stored;
	if (outClient)
		*outClient = client;
	return true;
}

void MultiAddonManager::AddTimedOutClient(uint64 steamID64, CServerSideClient *client)
{
	if (!client)
		client = FindClientBySteamID(steamID64);
	if (!client || client->GetClientSteamID().ConvertToUint64() != steamID64)
		return;
	RssClientSession &stored = m_RssClientSessions[steamID64];
	const int slot = client->GetPlayerSlot().Get();
	const int userId = client->GetUserID().Get();
	if (!stored.serial || stored.slot != slot || stored.userId != userId)
	{
		stored.slot = slot;
		stored.userId = userId;
		stored.serial = m_nNextRssSessionSerial++;
		if (!stored.serial)
			stored.serial = m_nNextRssSessionSerial++;
	}
	TimedOutClientToken token;
	token.steamId = steamID64;
	token.providerEpoch = m_nRssProviderEpoch;
	token.addonGeneration = m_nRssAddonGeneration;
	token.sessionSerial = stored.serial;
	token.slot = stored.slot;
	token.userId = stored.userId;
	std::unordered_map<uint64, rss_flow::Flow>::const_iterator flow = m_RssAssetFlows.find(steamID64);
	if (flow != m_RssAssetFlows.end())
	{
		if (rss_flow::IsTerminal(flow->second.phase))
			return;
		token.flowId = flow->second.flowId;
	}
	m_TimedOutClients.push_back(token);
}

CConVar<CUtlString> mm_extra_addons("mm_extra_addons", FCVAR_NONE, "The workshop IDs of extra addons separated by commas, addons will be downloaded (if not present) and mounted", CUtlString(""),
	[](CConVar<CUtlString> *cvar, CSplitScreenSlot slot, const CUtlString *new_val, const CUtlString *old_val)
	{
		StringToVector(new_val->Get(), g_MultiAddonManager.m_ExtraAddons);
		g_MultiAddonManager.OnRssAddonConfigurationChanged();
		g_MultiAddonManager.RefreshAddons();
	});


CConVar<CUtlString> mm_client_extra_addons("mm_client_extra_addons", FCVAR_NONE, "The workshop IDs of extra client addons that will be applied to all clients, separated by commas", CUtlString(""),
	[](CConVar<CUtlString> *cvar, CSplitScreenSlot slot, const CUtlString *new_val, const CUtlString *old_val)
	{
		StringToVector(new_val->Get(), g_MultiAddonManager.m_GlobalClientAddons);
	});

INetworkGameServer *g_pNetworkGameServer = nullptr;
CGlobalVars *gpGlobals = nullptr;
IGameEventSystem *g_pGameEventSystem = nullptr;
IGameEventManager2 *g_pGameEventManager = nullptr;
IGameEventManager2 *g_pGameEventManagerVTable = nullptr;
CServerSideClientBase *g_pServerSideClientVTable = nullptr;
CServerSideClientBase *g_pHLTVClientVTable = nullptr;

MultiAddonManager g_MultiAddonManager;
PLUGIN_EXPOSE(MultiAddonManager, g_MultiAddonManager);

MultiAddonManager::MultiAddonManager() :
	m_hookGameFrame(&IServerGameDLL::GameFrame, this, nullptr, &MultiAddonManager::Hook_GameFrame),
	m_hookGameServerSteamAPIActivated(&IServerGameDLL::GameServerSteamAPIActivated, this, &MultiAddonManager::Hook_GameServerSteamAPIActivated, nullptr),
	m_hookStartupServer(&INetworkServerService::StartupServer, this, nullptr, &MultiAddonManager::Hook_StartupServer),
	m_hookClientConnect(&IServerGameClients::ClientConnect, this, &MultiAddonManager::Hook_ClientConnect, nullptr),
	m_hookCanHLTVClientConnect(&IServerGameClients::CanHLTVClientConnect, this, &MultiAddonManager::Hook_CanHLTVClientConnect, nullptr),
	m_hookClientDisconnect(&IServerGameClients::ClientDisconnect, this, &MultiAddonManager::Hook_ClientDisconnect, nullptr),
	m_hookClientActive(&IServerGameClients::ClientActive, this, nullptr, &MultiAddonManager::Hook_ClientActive),
	m_hookPostEventAbstract(&IGameEventSystem::PostEventAbstract, this, &MultiAddonManager::Hook_PostEvent, nullptr),
	m_hookLoadEventsFromFile(&IGameEventManager2::LoadEventsFromFile, this, &MultiAddonManager::Hook_LoadEventsFromFile, nullptr),
	m_hookSendNetMessage_ServerSideClient(&CServerSideClientBase::SendNetMessage, this, &MultiAddonManager::Hook_SendNetMessage_ServerSideClient, nullptr),
	m_hookSendNetMessage_HLTVClient(&CServerSideClientBase::SendNetMessage, this, &MultiAddonManager::Hook_SendNetMessage_HLTVClient, nullptr),
	m_hookDisconnectSource(&CServerSideClientBase::Disconnect, this, &MultiAddonManager::Hook_DisconnectSource, nullptr),
	m_hookSetPendingHostStateRequest(this, &MultiAddonManager::Hook_SetPendingHostStateRequest, nullptr),
	m_hookReplyConnection(this, &MultiAddonManager::Hook_ReplyConnection, nullptr),
	m_hookScriptGetAddon(this, &MultiAddonManager::Hook_ScriptGetAddon, nullptr)
{
}

bool MultiAddonManager::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, g_pEngineServer, IVEngineServer, INTERFACEVERSION_VENGINESERVER);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, g_pSource2GameClients, IServerGameClients, INTERFACEVERSION_SERVERGAMECLIENTS);
	GET_V_IFACE_ANY(GetServerFactory, g_pSource2Server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pNetworkServerService, INetworkServerService, NETWORKSERVERSERVICE_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pNetworkMessages, INetworkMessages, NETWORKMESSAGES_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pGameEventSystem, IGameEventSystem, GAMEEVENTSYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);

	// Required to get the IMetamodListener events
	g_SMAPI->AddListener( this, this );

	CModule engineModule(ROOTBIN, "engine2");
	CModule serverModule(GAMEBIN, "server");

	bool g_bRequiredInitLoaded = true;

	// R2: look up all three function signatures and all three vtables first.
	// No Function Configure and no Virtual Add runs unless every lookup succeeds.
	auto pfnSetPendingHostStateRequest = (HostStateRequest_t)engineModule.LookupSignature(g_HostStateRequest_Sig);

	if (!pfnSetPendingHostStateRequest)
	{
		Panic("Failed to lookup the signature for HostStateRequest\n");
		g_bRequiredInitLoaded = false;
	}

	auto pfnReplyConnection = (ReplyConnection_t)engineModule.LookupSignature(g_ReplyConnection_Sig);

	if (!pfnReplyConnection)
	{
		Panic("Failed to lookup the signature for ReplyConnection\n");
		g_bRequiredInitLoaded = false;
	}

	auto pfnScriptGetAddon = (ScriptGetAddon_t)serverModule.LookupSignature(g_ScriptGetAddon_Sig);

	if (!pfnScriptGetAddon)
	{
		Panic("Failed to lookup the signature for ScriptGetAddon\n");
		g_bRequiredInitLoaded = false;
	}

	if (!(g_pGameEventManagerVTable = (IGameEventManager2 *)serverModule.FindVirtualTable("CGameEventManager")))
	{
		Panic("Failed to lookup the vtable for CGameEventManager\n");
		g_bRequiredInitLoaded = false;
	}

	if (!(g_pServerSideClientVTable = (CServerSideClientBase *)engineModule.FindVirtualTable("CServerSideClient")))
	{
		Panic("Failed to lookup the vtable for CServerSideClient\n");
		g_bRequiredInitLoaded = false;
	}

	if (!(g_pHLTVClientVTable = (CServerSideClientBase *)engineModule.FindVirtualTable("CHLTVClient")))
	{
		Panic("Failed to lookup the vtable for CHLTVClient\n");
		g_bRequiredInitLoaded = false;
	}

	if (!g_bRequiredInitLoaded)
	{
		V_snprintf(error, maxlen, "One or more address lookups failed, please refer to startup logs for more information");
		return false;
	}

	// R2: publish the full RSS preference set/latch before the first Function
	// Configure, because a callback is possible immediately after Configure.
	m_nRssProviderEpoch = static_cast<uint64>(std::chrono::steady_clock::now().time_since_epoch().count());
	if (!m_nRssProviderEpoch)
		m_nRssProviderEpoch = 1;
	m_nRssServerEpoch = 1;
	LoadRssAssetPreferences();

	m_hookSetPendingHostStateRequest.Configure(pfnSetPendingHostStateRequest);
	m_hookReplyConnection.Configure(pfnReplyConnection);
	m_hookScriptGetAddon.Configure(pfnScriptGetAddon);

	m_hookClientConnect.Add(g_pSource2GameClients);
	m_hookCanHLTVClientConnect.Add(g_pSource2GameClients);
	m_hookClientDisconnect.Add(g_pSource2GameClients);
	m_hookClientActive.Add(g_pSource2GameClients);
	m_hookGameServerSteamAPIActivated.Add(g_pSource2Server);
	m_hookGameFrame.Add(g_pSource2Server);
	m_hookStartupServer.Add(g_pNetworkServerService);
	m_hookPostEventAbstract.Add(g_pGameEventSystem);
	m_hookLoadEventsFromFile.AddGlobal((IGameEventManager2*)&g_pGameEventManagerVTable);
	m_hookSendNetMessage_ServerSideClient.AddGlobal((CServerSideClientBase*)&g_pServerSideClientVTable);
	m_hookSendNetMessage_HLTVClient.AddGlobal((CServerSideClientBase*)&g_pHLTVClientVTable);
	m_hookDisconnectSource.AddGlobal((CServerSideClientBase*)&g_pServerSideClientVTable);

	if (late)
	{
		g_pNetworkGameServer = g_pNetworkServerService->GetIGameServer();
		gpGlobals = g_pEngineServer->GetServerGlobals();
		if (g_pEngineServer->IsDedicatedServer())
		{
			m_CallbackDownloadItemResult.Register(this, &MultiAddonManager::OnAddonDownloaded);
		}
	}

	META_CONVAR_REGISTER(FCVAR_RELEASE);

	ParseCfg();
	m_bRssProviderReady = true;

	Message("Plugin loaded successfully!\n");

	return true;
}

bool MultiAddonManager::Unload(char *error, size_t maxlen)
{
	m_bRssProviderReady = false;
	m_RssAssetFlows.clear();
	m_RssClientSessions.clear();
	m_RssDisconnectEvidence.clear();
	m_TimedOutClients.clear();
	ClearAddons();

	m_hookClientConnect.Remove(g_pSource2GameClients);
	m_hookCanHLTVClientConnect.Remove(g_pSource2GameClients);
	m_hookClientDisconnect.Remove(g_pSource2GameClients);
	m_hookClientActive.Remove(g_pSource2GameClients);
	m_hookGameServerSteamAPIActivated.Remove(g_pSource2Server);
	m_hookGameFrame.Remove(g_pSource2Server);
	m_hookStartupServer.Remove(g_pNetworkServerService);
	m_hookPostEventAbstract.Remove(g_pGameEventSystem);
	m_hookLoadEventsFromFile.RemoveGlobal((IGameEventManager2*)&g_pGameEventManagerVTable);
	m_hookSendNetMessage_ServerSideClient.RemoveGlobal((CServerSideClientBase *)&g_pServerSideClientVTable);
	m_hookSendNetMessage_HLTVClient.RemoveGlobal((CServerSideClientBase *)&g_pHLTVClientVTable);
	m_hookDisconnectSource.RemoveGlobal((CServerSideClientBase *)&g_pServerSideClientVTable);
	
	return true;
}

void *MultiAddonManager::OnMetamodQuery(const char *iface, int *ret)
{
	const bool bLegacyInterface = !V_strcmp(iface, MULTIADDONMANAGER_INTERFACE);
	const bool bRssAssetsInterface = !V_strcmp(iface, MULTIADDONMANAGER_RSS_ASSETS_INTERFACE);
	const bool bRssAssetsV2Interface = !V_strcmp(iface, MULTIADDONMANAGER_RSS_ASSETS_V2_INTERFACE);

	if (!bLegacyInterface && !bRssAssetsInterface && !bRssAssetsV2Interface)
	{
		if (ret)
			*ret = META_IFACE_FAILED;

		return nullptr;
	}

	if (ret)
		*ret = META_IFACE_OK;

	if (bRssAssetsV2Interface)
		return static_cast<IMultiAddonManager005*>(&g_MultiAddonManager);

	if (bRssAssetsInterface)
		return static_cast<IMultiAddonManager004*>(&g_MultiAddonManager);

	return static_cast<IMultiAddonManager*>(&g_MultiAddonManager);
}

void MultiAddonManager::ParseCfg()
{
	char szPath[MAX_PATH];
	V_snprintf(szPath, sizeof(szPath), "%s/csgo/cfg/multiaddonmanager/multiaddonmanager.cfg", Plat_GetGameDirectory());
	std::ifstream cfgFile(szPath);

	if (!cfgFile.is_open())
	{
		Message("Unable to open & execute custom cfg file \"multiaddonmanager/multiaddonmanager\"\n");
		return;
	}

	Message("Executing custom cfg file \"multiaddonmanager/multiaddonmanager\"\n");

	std::string strCommand;

	while (std::getline(cfgFile, strCommand))
	{
		if (!strCommand.empty() && strCommand.back() == '\r')
			strCommand.pop_back();

		if (!strCommand.empty())
			g_pEngineServer->ServerCommand(strCommand.c_str());
	}
}

void MultiAddonManager::BuildAddonPath(const char *pszAddon, char *buf, size_t len, bool bLegacy = false)
{
	// The workshop on a dedicated server is stored relative to the working directory for whatever reason
	static CBufferStringGrowable<MAX_PATH> s_sWorkingDir;
	ExecuteOnce(g_pFullFileSystem->GetSearchPath("EXECUTABLE_PATH", GET_SEARCH_PATH_ALL, s_sWorkingDir, 1));

	V_snprintf(buf, len, "%ssteamapps/workshop/content/730/%s/%s%s.vpk", s_sWorkingDir.Get(), pszAddon, pszAddon, bLegacy ? "" : "_dir");
}

bool MultiAddonManager::MountAddon(const char *pszAddon, bool bAddToTail = false)
{
	if (!pszAddon || !*pszAddon)
		return false;

	CUtlVector<std::string> serverMountedAddons;
	StringToVector(this->m_sCurrentWorkshopMap.c_str(), serverMountedAddons);
	if (serverMountedAddons.Find(pszAddon) != -1)
	{
		Message("%s: Addon %s is already mounted by the server\n", __func__, pszAddon);
		return false;
	}

	PublishedFileId_t iAddon = V_StringToUint64(pszAddon, 0);
	uint32 iAddonState = GetSteamUGC()->GetItemState(iAddon);

	if (iAddonState & k_EItemStateLegacyItem)
	{
		Message("%s: Addon %s is not compatible with Source 2, skipping\n", __func__, pszAddon);
		return false;
	}

	if (!(iAddonState & k_EItemStateInstalled))
	{
		Message("%s: Addon %s is not installed, queuing a download\n", __func__, pszAddon);
		DownloadAddon(pszAddon, true, true);
		return false;
	}
	else if (mm_addon_mount_download.Get())
	{
		// Queue a download anyway in case the addon got an update and the server desires this, but don't reload the map when done
		DownloadAddon(pszAddon, false, true);
	}

	char pszPath[MAX_PATH];
	BuildAddonPath(pszAddon, pszPath, sizeof(pszPath));

	if (!g_pFullFileSystem->FileExists(pszPath))
	{
		// This might be a legacy addon (before mutli-chunk was introduced), try again without the _dir
		BuildAddonPath(pszAddon, pszPath, sizeof(pszPath), true);

		if (!g_pFullFileSystem->FileExists(pszPath))
		{
			Panic("%s: Addon %s not found at %s\n", __func__, pszAddon, pszPath);
			return false;
		}
	}
	else
	{
		// We still need it without _dir anyway because the filesystem will append suffixes if needed
		BuildAddonPath(pszAddon, pszPath, sizeof(pszPath), true);
	}

	if (m_MountedAddons.Find(pszAddon) != -1)
	{
		Panic("%s: Addon %s is already mounted\n", __func__, pszAddon);
		return false;
	}

	Message("Adding search path: %s\n", pszPath);

	g_pFullFileSystem->AddSearchPath(pszPath, "GAME", bAddToTail ? PATH_ADD_TO_TAIL : PATH_ADD_TO_HEAD, SEARCH_PATH_PRIORITY_VPK);
	m_MountedAddons.AddToTail(pszAddon);

	return true;
}

bool MultiAddonManager::UnmountAddon(const char *pszAddon)
{
	if (!pszAddon || !*pszAddon)
		return false;

	char path[MAX_PATH];
	BuildAddonPath(pszAddon, path, sizeof(path));

	if (!g_pFullFileSystem->RemoveSearchPath(path, "GAME"))
		return false;

	m_MountedAddons.FindAndFastRemove(pszAddon);

	Message("Removing search path: %s\n", path);

	return true;
}

void MultiAddonManager::PrintDownloadProgress()
{
	if (m_DownloadQueue.Count() == 0)
		return;

	uint64 iBytesDownloaded = 0;
	uint64 iTotalBytes = 0;

	if (!GetSteamUGC()->GetItemDownloadInfo(m_DownloadQueue.Head(), &iBytesDownloaded, &iTotalBytes) || !iTotalBytes)
		return;

	double flMBDownloaded = (double)iBytesDownloaded / 1024 / 1024;
	double flTotalMB = (double)iTotalBytes / 1024 / 1024;

	double flProgress = (double)iBytesDownloaded / (double)iTotalBytes;
	flProgress *= 100.f;

	Message("Downloading addon %lli: %.2f/%.2f MB (%.2f%%)\n", m_DownloadQueue.Head(), flMBDownloaded, flTotalMB, flProgress);
}

// bImportant adds downloads to the pending list, which will reload the current map once the list is exhausted
// bForce will initiate a download even if the addon already exists and is updated
// Internally, downloads are queued up and processed one at a time
bool MultiAddonManager::DownloadAddon(const char *pszAddon, bool bImportant, bool bForce)
{
	if (!GetSteamUGC())
	{
		Panic("%s: Cannot download addons as the Steam API is not initialized\n", __func__);
		return false;
	}

	PublishedFileId_t addon = V_StringToUint64(pszAddon, 0);

	if (addon == 0)
	{
		Panic("%s: Invalid addon %s\n", __func__, pszAddon);
		return false;
	}

	if (m_DownloadQueue.Check(addon))
	{
		Panic("%s: Addon %s is already queued for download!\n", __func__, pszAddon);
		return false;
	}

	uint32 nItemState = GetSteamUGC()->GetItemState(addon);

	if (!bForce && (nItemState & k_EItemStateInstalled))
	{
		Message("Addon %lli is already installed\n", addon);
		return true;
	}

	if (!GetSteamUGC()->DownloadItem(addon, false))
	{
		Panic("%s: Addon download for %lli failed to start, addon ID is invalid or server is not logged on Steam\n", __func__, addon);
		return false;
	}
	
	if (bImportant && m_ImportantDownloads.Find(addon) == -1)
		m_ImportantDownloads.AddToTail(addon);

	m_DownloadQueue.Insert(addon);

	Message("Addon download started for %lli\n", addon);

	return true;
}

void MultiAddonManager::RefreshAddons(bool bReloadMap)
{
	if (!GetSteamUGC())
		return;

	Message("Refreshing addons (%s)\n", VectorToString(m_ExtraAddons).c_str());

	// Remove our paths first in case addons were switched
	FOR_EACH_VEC_BACK(m_MountedAddons, i)
		UnmountAddon(m_MountedAddons[i].c_str());

	bool bAllAddonsMounted = true;

	FOR_EACH_VEC(m_ExtraAddons, i)
	{
		if (!MountAddon(m_ExtraAddons[i].c_str()))
			bAllAddonsMounted = false;
	}

	if (bAllAddonsMounted && bReloadMap)
		ReloadMap();
}

void MultiAddonManager::ClearAddons()
{
	m_ExtraAddons.RemoveAll();
	OnRssAddonConfigurationChanged();

	// Update the convar to reflect the new addon list, but don't trigger the callback
	mm_extra_addons.GetConVarData()->Value(0)->m_StringValue = "";
	
	FOR_EACH_VEC_BACK(m_MountedAddons, i)
		UnmountAddon(m_MountedAddons[i].c_str());
}

KHook::Return<void> MultiAddonManager::Hook_GameServerSteamAPIActivated(IServerGameDLL *pThis)
{
	// This is only intended for dedicated servers
	if (!g_pEngineServer->IsDedicatedServer())
		return {KHook::Action::Ignore};

	Message("Steam API Activated\n");

	m_CallbackDownloadItemResult.Register(this, &MultiAddonManager::OnAddonDownloaded);

	RefreshAddons(true);

	return {KHook::Action::Ignore};
}

void MultiAddonManager::ReloadMap()
{
	char cmd[MAX_PATH];

	// Using the concommand here as g_pEngineServer->ChangeLevel doesn't unmount workshop maps and we wanna be clean.
	// See Hook_SetPendingHostStateRequest's comment for more details.
	// Community maps are treated like workshop maps but they should still be loaded using changelevel
	if (m_sCurrentWorkshopMap.empty() || g_pFullFileSystem->IsDirectory(m_sCurrentWorkshopMap.c_str(), "OFFICIAL_ADDONS"))
		V_snprintf(cmd, sizeof(cmd), "changelevel %s", gpGlobals->mapname.ToCStr());
	else
		V_snprintf(cmd, sizeof(cmd), "host_workshop_map %s", m_sCurrentWorkshopMap.c_str());

	g_pEngineServer->ServerCommand(cmd);
}

void MultiAddonManager::OnAddonDownloaded(DownloadItemResult_t *pResult)
{
	if (pResult->m_eResult == k_EResultOK)
		Message("Addon %lli downloaded successfully\n", pResult->m_nPublishedFileId);
	else
		Panic("Addon %lli download failed with reason \"%s\" (%i)\n", pResult->m_nPublishedFileId, g_SteamErrorMessages[pResult->m_eResult], pResult->m_eResult);

	// This download isn't triggered by us, don't do anything
	if (!m_DownloadQueue.Check(pResult->m_nPublishedFileId))
		return;

	m_DownloadQueue.RemoveAtHead();
	
	bool bFound = m_ImportantDownloads.FindAndRemove(pResult->m_nPublishedFileId);
	
	// That was the last important download, now reload the map
	if (bFound && m_ImportantDownloads.Count() == 0)
	{
		Message("All addon downloads finished, reloading map %s\n", gpGlobals->mapname);
		ReloadMap();
	}
}

bool MultiAddonManager::AddAddon(const char *pszAddon, bool bRefresh)
{
	if (m_ExtraAddons.Find(pszAddon) != -1)
	{
		Panic("Addon %s is already in the list!\n", pszAddon);
		return false;
	}

	Message("Adding %s to addon list\n", pszAddon);

	m_ExtraAddons.AddToTail(pszAddon);
	OnRssAddonConfigurationChanged();

	// Update the convar to reflect the new addon list, but don't trigger the callback
	mm_extra_addons.GetConVarData()->Value(0)->m_StringValue = VectorToString(m_ExtraAddons).c_str();

	Message("Clearing client cache due to addons changing");

	if (bRefresh)
		RefreshAddons();

	return true;
}

bool MultiAddonManager::RemoveAddon(const char *pszAddon, bool bRefresh)
{
	int index = m_ExtraAddons.Find(pszAddon);

	if (index == -1)
	{
		Panic("Addon %s is not in the list!\n", pszAddon);
		return false;
	}

	Message("Removing %s from addon list\n", pszAddon);

	m_ExtraAddons.Remove(index);
	OnRssAddonConfigurationChanged();

	// Update the convar to reflect the new addon list, but don't trigger the callback
	mm_extra_addons.GetConVarData()->Value(0)->m_StringValue = VectorToString(m_ExtraAddons).c_str();

	if (bRefresh)
		RefreshAddons();

	return true;
}

CNetMessagePB<CNETMsg_SignonState> *GetAddonSignonStateMessage(const char *pszAddon)
{
	if (!gpGlobals)
		return nullptr;
	CUtlVector<CServerSideClient *> *clients = GetClientList();
	if (!clients)
		return nullptr;

	INetworkMessageInternal *pNetMsg = g_pNetworkMessages->FindNetworkMessagePartial("SignonState");
	if (!pNetMsg)
		return nullptr;
	CNetMessagePB<CNETMsg_SignonState> *pMsg = pNetMsg->AllocateMessage()->ToPB<CNETMsg_SignonState>();
	if (!pMsg)
		return nullptr;
	pMsg->set_spawn_count(gpGlobals->serverCount);
	pMsg->set_signon_state(SIGNONSTATE_CHANGELEVEL);
	pMsg->set_addons(pszAddon ? pszAddon : "");
	pMsg->set_num_server_players(clients->Count());
	for (int i = 0; i < clients->Count(); i++)
	{
		auto client = clients->Element(i);
		if (!client)
			continue;

		char const *szNetworkId = g_pEngineServer->GetPlayerNetworkIDString(client->GetPlayerSlot());

		pMsg->add_players_networkids(szNetworkId);
	}

	return pMsg;
}

bool MultiAddonManager::HasUGCConnection()
{
	return GetSteamUGC() != nullptr;
}

void MultiAddonManager::AddClientAddon(const char *pszAddon, uint64 steamID64, bool bRefresh)
{
	if (!steamID64)
	{
		if (m_GlobalClientAddons.Find(pszAddon) != -1)
		{
			Panic("Addon %s is already in the list!\n", pszAddon);
			return;
		}
	
		m_GlobalClientAddons.AddToTail(pszAddon);
		mm_client_extra_addons.GetConVarData()->Value(0)->m_StringValue = VectorToString(m_GlobalClientAddons).c_str();	
	}
	else
	{
		if (g_ClientAddons[steamID64].addonsToLoad.Find(pszAddon) != -1)
		{
			Panic("Addon %s is already in the list!\n", pszAddon);
			return;
		}

		ClientAddonInfo_t &clientInfo = g_ClientAddons[steamID64];
		clientInfo.addonsToLoad.AddToTail(pszAddon);
	}
	
	if (bRefresh)
	{
		CUtlVector<CServerSideClient*> &clients = *GetClientList();
		auto pMsg = GetAddonSignonStateMessage(pszAddon);
		if (!pMsg)
		{
			Panic("Failed to create signon state message for %s\n", pszAddon);
			return;
		}
		FOR_EACH_VEC(clients, i)
		{
			CServerSideClient *pClient = clients[i];
			if (steamID64 == 0 || pClient->GetClientSteamID().ConvertToUint64() == steamID64)
			{
				// Client is already loading, telling them to reload now will actually just disconnect them. ("Received signon %i when at %i\n" in client console)
				if (pClient->GetSignonState() == SIGNONSTATE_CHANGELEVEL)
					break;
				// Client still has addons to load anyway, they don't need to be told to reload
				if (!g_ClientAddons[steamID64].currentPendingAddon.empty())
					break;
				ClientAddonInfo_t &clientInfo = g_ClientAddons[steamID64];

				CUtlVector<std::string> addons;
				GetClientAddons(addons, steamID64, AddonListPurpose::Download);
				
				FOR_EACH_VEC(clientInfo.downloadedAddons, j)
				{
					addons.FindAndRemove(clientInfo.downloadedAddons[j]);
				}

				if (!addons.Count())
				{
					break;
				}
				clientInfo.currentPendingAddon = addons.Head();
				
				pClient->GetNetChannel()->SendNetMessage(pMsg, BUF_RELIABLE);

				if (steamID64)
				{
					break;
				}
			}
		}
		delete pMsg;
	}
}

void MultiAddonManager::RemoveClientAddon(const char *pszAddon, uint64 steamID64)
{
	if (!steamID64)
	{
		m_GlobalClientAddons.FindAndRemove(pszAddon);
		mm_client_extra_addons.GetConVarData()->Value(0)->m_StringValue = VectorToString(m_GlobalClientAddons).c_str();	
	}
	else
	{
		ClientAddonInfo_t &clientInfo = g_ClientAddons[steamID64];
		clientInfo.addonsToLoad.FindAndRemove(pszAddon);
	}
}

void MultiAddonManager::ClearClientAddons(uint64 steamID64)
{
	if (!steamID64)
	{
		m_GlobalClientAddons.RemoveAll();
		mm_client_extra_addons.GetConVarData()->Value(0)->m_StringValue = VectorToString(m_GlobalClientAddons).c_str();	
	}
	else
	{
		ClientAddonInfo_t &clientInfo = g_ClientAddons[steamID64];
		clientInfo.addonsToLoad.RemoveAll();
	}
}

RssAssetMode MultiAddonManager::GetClientRssAssetMode(uint64 steamID64) const
{
	std::map<uint64, RssAssetMode>::const_iterator it = m_RssAssetModes.find(steamID64);
	return rss_prefs::ResolveConfiguredMode(
		it == m_RssAssetModes.end() ? nullptr : &it->second, m_bRssAssetPreferencesWritable);
}

bool MultiAddonManager::IsClientRssAssetsEnabled(uint64 steamID64) const
{
	return !steamID64 || GetClientRssAssetMode(steamID64) == RssAssetMode::DownloadAndMount;
}

bool MultiAddonManager::SetClientRssAssetsEnabled(uint64 steamID64, bool bEnabled)
{
	if (!steamID64)
		return false;
	return SetClientRssAssetMode(steamID64,
		bEnabled ? RssAssetMode::DownloadAndMount : RssAssetMode::MountOnly) == RssAssetResult::Ok;
}

RssAssetResult MultiAddonManager::SetClientRssAssetMode(uint64 steamID64, RssAssetMode mode)
{
	if (!steamID64 || static_cast<uint32>(mode) > static_cast<uint32>(RssAssetMode::DownloadAndMount))
		return RssAssetResult::InvalidArgument;
	if (!m_bRssAssetPreferencesWritable)
		return RssAssetResult::StoreReadOnly;
	std::unordered_map<uint64, rss_flow::Flow>::const_iterator active = m_RssAssetFlows.find(steamID64);
	if (active != m_RssAssetFlows.end() && !rss_flow::IsTerminal(active->second.phase))
		return RssAssetResult::Busy;

	rss_prefs::ModeMap modes;
	for (std::map<uint64, RssAssetMode>::const_iterator it = m_RssAssetModes.begin();
		it != m_RssAssetModes.end(); ++it)
		modes[static_cast<rss_prefs::RssSteamId>(it->first)] = ToPreferenceMode(it->second);

	const bool ok = rss_prefs::ApplyRssPreferenceChange(m_bRssAssetPreferencesWritable, modes,
		static_cast<rss_prefs::RssSteamId>(steamID64), ToPreferenceMode(mode),
		[this](const rss_prefs::ModeMap &candidate)
		{
			std::map<uint64, RssAssetMode> converted;
			for (rss_prefs::ModeMap::const_iterator it = candidate.begin(); it != candidate.end(); ++it)
				converted[static_cast<uint64>(it->first)] = ToPublicMode(it->second);
			return SaveRssAssetModesLocked(converted);
		},
		[]() {});

	if (!ok)
		return RssAssetResult::PersistFailed;

	m_RssAssetModes.clear();
	for (rss_prefs::ModeMap::const_iterator it = modes.begin(); it != modes.end(); ++it)
		m_RssAssetModes[static_cast<uint64>(it->first)] = ToPublicMode(it->second);

	ClientAddonInfo_t &info = g_ClientAddons[steamID64];
	if (mode == RssAssetMode::DownloadAndMount)
	{
		for (int i = info.downloadedAddons.Count() - 1; i >= 0; --i)
			if (IsRssAssetAddon(info.downloadedAddons[i].c_str()))
				info.downloadedAddons.Remove(i);
		info.rssCacheGeneration = 0;
	}
	if (IsRssAssetAddon(info.currentPendingAddon.c_str()))
		info.currentPendingAddon.clear();

	Message("RSS asset mode set to %u for %llu; refresh is a separate operation\n",
		static_cast<uint32>(mode), steamID64);
	return RssAssetResult::Ok;
}

void MultiAddonManager::GetClientAddons(CUtlVector<std::string> &addons, uint64 steamID64,
	AddonListPurpose purpose, const std::string &currentAddon, int replySlot, int replyUserId)
{
	addons.RemoveAll();
	std::vector<std::string> base;
	if (!GetCurrentWorkshopMap().empty())
		rss_flow::AddUnique(base, GetCurrentWorkshopMap());
	FOR_EACH_VEC(m_MountedAddons, i)
		rss_flow::AddUnique(base, m_MountedAddons[i]);
	FOR_EACH_VEC(m_GlobalClientAddons, i)
		rss_flow::AddUnique(base, m_GlobalClientAddons[i]);
	if (steamID64)
		FOR_EACH_VEC(g_ClientAddons[steamID64].addonsToLoad, i)
			rss_flow::AddUnique(base, g_ClientAddons[steamID64].addonsToLoad[i]);

	std::vector<std::string> extra;
	FOR_EACH_VEC(m_ExtraAddons, i)
		extra.push_back(m_ExtraAddons[i]);
	std::vector<std::string> completed;
	if (steamID64)
	{
		ClientAddonInfo_t &clientInfo = g_ClientAddons[steamID64];
		FOR_EACH_VEC(clientInfo.downloadedAddons, i)
		{
			const std::string &downloaded = clientInfo.downloadedAddons[i];
			if (!IsRssAssetAddon(downloaded.c_str()) ||
				clientInfo.rssCacheGeneration == m_nRssAddonGeneration)
				rss_flow::AddUnique(completed, downloaded);
		}
		std::unordered_map<uint64, rss_flow::Flow>::const_iterator flow = m_RssAssetFlows.find(steamID64);
		std::unordered_map<uint64, RssClientSession>::const_iterator stored =
			m_RssClientSessions.find(steamID64);
		rss_flow::Session bound;
		const rss_flow::Session *boundPtr = nullptr;
		if (stored != m_RssClientSessions.end())
		{
			bound.slot = stored->second.slot;
			bound.userId = stored->second.userId;
			bound.serial = stored->second.serial;
			boundPtr = &bound;
		}
		if (purpose == AddonListPurpose::ReplyMount && flow != m_RssAssetFlows.end() &&
			rss_flow::CanUseFlowLocalCompleted(flow->second, m_nRssProviderEpoch,
				m_nRssAddonGeneration, boundPtr, replySlot, replyUserId, rss_flow::Clock::now()))
		{
			const std::size_t count = std::min<std::size_t>(flow->second.completed, flow->second.addons.size());
			for (std::size_t i = 0; i < count; ++i)
				rss_flow::AddUnique(completed, flow->second.addons[i]);
		}
	}

	rss_flow::ListPurpose flowPurpose = rss_flow::ListPurpose::Download;
	if (purpose == AddonListPurpose::ReplyMount)
		flowPurpose = rss_flow::ListPurpose::ReplyMount;
	else if (purpose == AddonListPurpose::SignonFilter ||
		purpose == AddonListPurpose::GenericSignon)
		flowPurpose = rss_flow::ListPurpose::SignonFilter;
	rss_flow::Mode listMode = ToFlowMode(GetClientRssAssetMode(steamID64));
	std::string listCurrentAddon = currentAddon;
	std::unordered_map<uint64, rss_flow::Flow>::const_iterator activeFlow =
		m_RssAssetFlows.find(steamID64);
	rss_flow::Session currentBound;
	const rss_flow::Session *currentBoundPtr = nullptr;
	std::unordered_map<uint64, RssClientSession>::const_iterator currentStored =
		m_RssClientSessions.find(steamID64);
	if (currentStored != m_RssClientSessions.end())
	{
		currentBound.slot = currentStored->second.slot;
		currentBound.userId = currentStored->second.userId;
		currentBound.serial = currentStored->second.serial;
		currentBoundPtr = &currentBound;
	}
	const rss_flow::TimePoint listNow = rss_flow::Clock::now();
	if (activeFlow != m_RssAssetFlows.end() && rss_flow::CanUseFlowCurrentAddon(
		activeFlow->second, m_nRssProviderEpoch, m_nRssAddonGeneration, currentBoundPtr,
		replySlot, replyUserId, listNow))
		listCurrentAddon = activeFlow->second.currentAddon;
	const bool genericPath = purpose == AddonListPurpose::GenericSignon ||
		purpose == AddonListPurpose::GenericDownload;
	listMode = rss_flow::ResolveListMode(listMode,
		purpose == AddonListPurpose::ReplyMount, genericPath,
		activeFlow == m_RssAssetFlows.end() ? nullptr : &activeFlow->second,
		m_nRssProviderEpoch, m_nRssAddonGeneration, currentBoundPtr,
		replySlot, replyUserId, listNow);
	const std::vector<std::string> built = rss_flow::BuildList(base, extra, GetCurrentWorkshopMap(),
		listMode, flowPurpose, completed, listCurrentAddon);
	for (std::size_t i = 0; i < built.size(); ++i)
		addons.AddToTail(built[i]);
}

bool MultiAddonManager::AllowExplicitRssChangeLevel(uint64 steamID64,
	CServerSideClientBase *client, const char *addon)
{
	if (!client || !addon || !*addon)
		return false;
	std::unordered_map<uint64, rss_flow::Flow>::iterator flow = m_RssAssetFlows.find(steamID64);
	if (flow == m_RssAssetFlows.end())
		return true;
	std::unordered_map<uint64, RssClientSession>::const_iterator stored =
		m_RssClientSessions.find(steamID64);
	if (rss_flow::IsTerminal(flow->second.phase))
	{
		if (stored == m_RssClientSessions.end())
			return true;
		rss_flow::Session current;
		current.slot = stored->second.slot;
		current.userId = stored->second.userId;
		current.serial = stored->second.serial;
		return !rss_flow::SameSession(flow->second.session, current);
	}
	if (stored == m_RssClientSessions.end() || stored->second.slot != client->GetPlayerSlot().Get() ||
		stored->second.userId != client->GetUserID().Get())
		return false;
	rss_flow::Session exact;
	exact.slot = stored->second.slot;
	exact.userId = stored->second.userId;
	exact.serial = stored->second.serial;
	return rss_flow::MarkStageMessageDelivered(flow->second, m_nRssProviderEpoch,
		m_nRssAddonGeneration, exact, addon, rss_flow::Clock::now());
}

void MultiAddonManager::LoadRssAssetPreferences()
{
	m_RssAssetModes.clear();
	m_bRssAssetPreferencesWritable = false;

	const bool mainExists = g_pFullFileSystem->FileExists(g_RssAssetPreferencesPath, "GAME");
	if (mainExists)
	{
		FileHandle_t file = g_pFullFileSystem->Open(g_RssAssetPreferencesPath, "rb", "GAME");
		if (file == FILESYSTEM_INVALID_HANDLE)
		{
			Panic("Failed to open RSS asset preference JSONC; defaults remain Disabled and read-only\n");
			return;
		}
		const unsigned int fileSize = g_pFullFileSystem->Size(file);
		if (fileSize > 1024 * 1024)
		{
			g_pFullFileSystem->Close(file);
			Panic("RSS asset preference JSONC is unexpectedly large; defaults remain Disabled and read-only\n");
			return;
		}
		std::string json(fileSize, '\0');
		const int bytesRead = fileSize ? g_pFullFileSystem->Read(json.data(), fileSize, file) : 0;
		g_pFullFileSystem->Close(file);
		if (bytesRead != static_cast<int>(fileSize))
		{
			Panic("Failed to read RSS asset preference JSONC; defaults remain Disabled and read-only\n");
			return;
		}
		rss_prefs::ParsedPreferences parsed;
		if (!rss_prefs::ParseRssAssetPreferences(json, parsed))
		{
			Panic("Failed to parse RSS asset preference JSONC; no fallback or overwrite performed\n");
			return;
		}
		std::map<uint64, RssAssetMode> candidate;
		for (rss_prefs::ModeMap::const_iterator it = parsed.clients.begin(); it != parsed.clients.end(); ++it)
			candidate[static_cast<uint64>(it->first)] = ToPublicMode(it->second);
		if (parsed.sourceVersion == 1 && !SaveRssAssetModesLocked(candidate))
		{
			Panic("Failed to migrate RSS asset preference v1; defaults remain Disabled and read-only\n");
			return;
		}
		m_RssAssetModes.swap(candidate);
		m_bRssAssetPreferencesWritable = true;
		Message("Loaded %d RSS asset preferences (source version %u)\n",
			static_cast<int>(m_RssAssetModes.size()), parsed.sourceVersion);
		return;
	}

	if (g_pFullFileSystem->FileExists(g_RssAssetLegacyOptOutPath, "GAME"))
	{
		FileHandle_t file = g_pFullFileSystem->Open(g_RssAssetLegacyOptOutPath, "rt", "GAME");
		if (file == FILESYSTEM_INVALID_HANDLE)
		{
			Panic("Failed to open RSS legacy preference TXT\n");
			return;
		}
		std::vector<std::string> lines;
		char line[64];
		while (g_pFullFileSystem->ReadLine(line, sizeof(line), file))
			lines.push_back(std::string(line));
		const bool readOk = g_pFullFileSystem->IsOk(file) ? true : false;
		g_pFullFileSystem->Close(file);
		rss_prefs::ModeMap parsed;
		if (!rss_prefs::CollectLegacyOptOutIds(lines, readOk, parsed))
		{
			Panic("Failed to read RSS legacy preference TXT; not migrating\n");
			return;
		}
		std::map<uint64, RssAssetMode> candidate;
		for (rss_prefs::ModeMap::const_iterator it = parsed.begin(); it != parsed.end(); ++it)
			candidate[static_cast<uint64>(it->first)] = ToPublicMode(it->second);
		if (!SaveRssAssetModesLocked(candidate))
		{
			Panic("Failed to migrate RSS legacy preference TXT\n");
			return;
		}
		m_RssAssetModes.swap(candidate);
		m_bRssAssetPreferencesWritable = true;
		Message("Migrated %d RSS asset preferences from TXT\n", static_cast<int>(m_RssAssetModes.size()));
		return;
	}

	std::map<uint64, RssAssetMode> empty;
	if (!SaveRssAssetModesLocked(empty))
	{
		Panic("Failed to create RSS asset preference v2; defaults remain Disabled and read-only\n");
		return;
	}
	m_bRssAssetPreferencesWritable = true;
	Message("Created RSS asset preference v2; missing clients default to MountOnly\n");
}

bool MultiAddonManager::SaveRssAssetModesLocked(const std::map<uint64, RssAssetMode> &modes) const
{
	rss_prefs::ModeMap converted;
	for (std::map<uint64, RssAssetMode>::const_iterator it = modes.begin(); it != modes.end(); ++it)
		converted[static_cast<rss_prefs::RssSteamId>(it->first)] = ToPreferenceMode(it->second);
	const std::string json = rss_prefs::BuildRssAssetPreferencesJson(converted);
	g_pFullFileSystem->CreateDirHierarchyForFile(g_RssAssetPreferencesPath, "GAME");
	FileHandle_t file = g_pFullFileSystem->Open(g_RssAssetPreferencesTempPath, "wt", "GAME");
	if (file == FILESYSTEM_INVALID_HANDLE)
		return false;
	const bool success = rss_prefs::RunPreferenceWriteTransaction(
		[&]()
		{
			return g_pFullFileSystem->Write(json.data(), json.size(), file) ==
				static_cast<int>(json.size());
		},
		[&]()
		{
			g_pFullFileSystem->Flush(file);
			return g_pFullFileSystem->IsOk(file) ? true : false;
		},
		[&]()
		{
			g_pFullFileSystem->Close(file);
			return true;
		},
		[&]()
		{
			return g_pFullFileSystem->RenameFile(g_RssAssetPreferencesTempPath,
				g_RssAssetPreferencesPath, "GAME");
		});
	if (!success)
	{
		g_pFullFileSystem->RemoveFile(g_RssAssetPreferencesTempPath, "GAME");
		return false;
	}
	return true;
}

RssAssetResult MultiAddonManager::RefreshClientRssAssets(uint64 steamID64, RssAssetMode mode,
	uint32 maxFlowSeconds, uint64 &outFlowId)
{
	outFlowId = 0;
	if (!steamID64 || static_cast<uint32>(mode) > static_cast<uint32>(RssAssetMode::DownloadAndMount) ||
		mode == RssAssetMode::Disabled)
		return RssAssetResult::InvalidArgument;
	if (!m_bRssProviderReady || !g_pEngineServer->IsDedicatedServer())
		return RssAssetResult::Unavailable;
	if (GetClientRssAssetMode(steamID64) != mode)
		return RssAssetResult::InvalidArgument;
	if (maxFlowSeconds && (maxFlowSeconds < 30 || maxFlowSeconds > 1200))
		return RssAssetResult::InvalidArgument;
	std::unordered_map<uint64, rss_flow::Flow>::const_iterator existing = m_RssAssetFlows.find(steamID64);
	if (existing != m_RssAssetFlows.end() && !rss_flow::IsTerminal(existing->second.phase))
		return RssAssetResult::Busy;
	std::size_t activeCount = 0;
	for (std::unordered_map<uint64, rss_flow::Flow>::const_iterator it = m_RssAssetFlows.begin();
		it != m_RssAssetFlows.end(); ++it)
		if (!rss_flow::IsTerminal(it->second.phase))
			++activeCount;
	if (activeCount >= 4)
		return RssAssetResult::Busy;

	const std::vector<std::string> effective = GetEffectiveRssAddons();
	if (effective.empty())
		return RssAssetResult::NoAddons;
	for (std::size_t i = 0; i < effective.size(); ++i)
		if (m_MountedAddons.Find(effective[i].c_str()) == -1)
			return RssAssetResult::NotReady;

	RssClientSession current;
	CServerSideClient *client = nullptr;
	if (!GetCurrentRssSession(steamID64, current, &client) || !client)
		return RssAssetResult::NotReady;
	CSteamID steamId(steamID64);
	if (!steamId.IsValid() || !steamId.BIndividualAccount())
		return RssAssetResult::InvalidArgument;

	rss_flow::Flow flow;
	flow.providerEpoch = m_nRssProviderEpoch;
	flow.flowId = m_nNextRssFlowId++;
	if (!flow.flowId)
		flow.flowId = m_nNextRssFlowId++;
	flow.addonGeneration = m_nRssAddonGeneration;
	flow.mode = ToFlowMode(mode);
	flow.phase = rss_flow::Phase::Queued;
	flow.result = rss_flow::Result::Queued;
	flow.session.slot = current.slot;
	flow.session.userId = current.userId;
	flow.session.serial = current.serial;
	flow.addons = effective;
	const uint32 seconds = maxFlowSeconds ? maxFlowSeconds : 1200;
	flow.hardDeadline = rss_flow::Clock::now() + std::chrono::seconds(seconds);
	m_RssAssetFlows[steamID64] = flow;
	outFlowId = flow.flowId;
	return RssAssetResult::Queued;
}

bool MultiAddonManager::GetClientRssAssetStatus(uint64 steamID64, RssAssetStatus &inOutStatus) const
{
	if (inOutStatus.structSize != sizeof(RssAssetStatus))
		return false;
	RssAssetStatus status = {};
	status.structSize = sizeof(RssAssetStatus);
	status.configuredMode = GetClientRssAssetMode(steamID64);
	status.appliedMode = RssAssetMode::Disabled;
	status.phase = RssAssetFlowPhase::None;
	status.result = RssAssetResult::Ok;
	status.slot = -1;
	status.userId = -1;
	status.providerEpoch = m_nRssProviderEpoch;
	std::unordered_map<uint64, ClientAddonInfo_t>::const_iterator clientInfo = g_ClientAddons.find(steamID64);
	if (clientInfo != g_ClientAddons.end())
		status.appliedMode = clientInfo->second.appliedRssMode;
	std::unordered_map<uint64, rss_flow::Flow>::const_iterator it = m_RssAssetFlows.find(steamID64);
	if (it != m_RssAssetFlows.end())
	{
		const rss_flow::Flow &flow = it->second;
		status.phase = ToPublicPhase(flow.phase);
		status.result = ToPublicResult(flow.result);
		status.addonGeneration = flow.addonGeneration;
		status.stagesCompleted = flow.completed;
		status.stagesTotal = static_cast<uint32>(flow.addons.size());
		status.remainingSeconds = rss_flow::RemainingSeconds(flow, rss_flow::Clock::now());
		status.slot = flow.session.slot;
		status.userId = flow.session.userId;
		status.providerEpoch = flow.providerEpoch;
		status.flowId = flow.flowId;
		status.sessionSerial = flow.session.serial;
		if (flow.reconnectRequested)
			status.flags |= RssAssetStatusFlag_ReconnectRequested;
		if (flow.mountListDelivered)
			status.flags |= RssAssetStatusFlag_MountListDelivered;
		if (flow.clientActiveObserved)
			status.flags |= RssAssetStatusFlag_ClientActiveObserved;
	}
	if (m_bRssAssetPreferencesWritable)
		status.flags |= RssAssetStatusFlag_ModePersisted;
	inOutStatus = status;
	return true;
}

bool MultiAddonManager::CancelClientRssAssetFlow(uint64 steamID64, uint64 flowId)
{
	std::unordered_map<uint64, rss_flow::Flow>::iterator it = m_RssAssetFlows.find(steamID64);
	if (it == m_RssAssetFlows.end() || it->second.flowId != flowId)
		return false;
	if (!rss_flow::IsTerminal(it->second.phase))
		rss_flow::Finish(it->second, rss_flow::Phase::Cancelled, rss_flow::Result::Cancelled,
			rss_flow::Clock::now());
	return true;
}

void MultiAddonManager::PublishRssAssetCompletion(uint64 steamID64, const rss_flow::Flow &flow)
{
	if (!rss_flow::CanPublishRssCompletion(flow, m_nRssProviderEpoch, m_nRssAddonGeneration))
		return;
	ClientAddonInfo_t &info = g_ClientAddons[steamID64];
	if (!mm_cache_clients_with_addons.Get())
	{
		info.rssCacheGeneration = 0;
		return;
	}
	for (std::size_t i = 0; i < flow.addons.size(); ++i)
		if (info.downloadedAddons.Find(flow.addons[i].c_str()) == -1)
			info.downloadedAddons.AddToTail(flow.addons[i]);
	info.rssCacheGeneration = flow.addonGeneration;
}

void MultiAddonManager::FinishRssAssetFlow(uint64 steamID64, RssAssetFlowPhase phase,
	RssAssetResult result)
{
	std::unordered_map<uint64, rss_flow::Flow>::iterator it = m_RssAssetFlows.find(steamID64);
	if (it == m_RssAssetFlows.end() || rss_flow::IsTerminal(it->second.phase))
		return;
	rss_flow::Finish(it->second, static_cast<rss_flow::Phase>(static_cast<uint32>(phase)),
		static_cast<rss_flow::Result>(static_cast<uint32>(result)), rss_flow::Clock::now());
}

void MultiAddonManager::AdvanceRssAssetFlows()
{
	const rss_flow::TimePoint now = rss_flow::Clock::now();
	for (std::unordered_map<uint64, rss_flow::Flow>::iterator it = m_RssAssetFlows.begin();
		it != m_RssAssetFlows.end();)
	{
		const uint64 steamID64 = it->first;
		rss_flow::Flow &flow = it->second;
		if (rss_flow::IsTerminal(flow.phase))
		{
			std::unordered_map<uint64, ClientAddonInfo_t>::iterator clientInfo =
				g_ClientAddons.find(steamID64);
			std::string *sharedPendingAddon = clientInfo == g_ClientAddons.end() ?
				nullptr : &clientInfo->second.currentPendingAddon;
			const rss_flow::TerminalRetentionResult retention =
				rss_flow::ApplyTerminalRetention(flow, now, std::chrono::seconds(60),
					sharedPendingAddon);
			if (retention == rss_flow::TerminalRetentionResult::EraseHistory)
			{
				it = m_RssAssetFlows.erase(it);
				continue;
			}
			++it;
			continue;
		}
		if (flow.providerEpoch != m_nRssProviderEpoch || flow.addonGeneration != m_nRssAddonGeneration)
		{
			rss_flow::Finish(flow, rss_flow::Phase::Failed, rss_flow::Result::ConfigChanged, now);
			++it;
			continue;
		}
		if (now >= flow.hardDeadline ||
			(flow.phase != rss_flow::Phase::Queued && flow.actionDeadline != rss_flow::TimePoint{} && now >= flow.actionDeadline))
		{
			rss_flow::Finish(flow, rss_flow::Phase::Failed, rss_flow::Result::TimedOut, now);
			++it;
			continue;
		}
		if (flow.phase != rss_flow::Phase::Queued && flow.phase != rss_flow::Phase::Staging)
		{
			++it;
			continue;
		}

		RssClientSession current;
		CServerSideClient *client = nullptr;
		if (!GetCurrentRssSession(steamID64, current, &client) || !client)
		{
			// A reconnecting client is not FULL while the engine is still loading it.
			// Keep the stage bounded by actionDeadline instead of misclassifying it as a new session.
			if (flow.phase == rss_flow::Phase::Staging)
			{
				++it;
				continue;
			}
			rss_flow::Finish(flow, rss_flow::Phase::Failed, rss_flow::Result::SessionChanged, now);
			++it;
			continue;
		}
		if (current.serial != flow.session.serial || current.slot != flow.session.slot ||
			current.userId != flow.session.userId)
		{
			rss_flow::Finish(flow, rss_flow::Phase::Failed, rss_flow::Result::SessionChanged, now);
			++it;
			continue;
		}

		const bool first = flow.phase == rss_flow::Phase::Queued;
		if (first)
		{
			if (!rss_flow::StartDeferred(flow, m_nRssProviderEpoch, m_nRssAddonGeneration,
				flow.session, now, std::chrono::seconds(15)))
			{
				rss_flow::Finish(flow, rss_flow::Phase::Failed, rss_flow::Result::SessionChanged, now);
				++it;
				continue;
			}
		}
		else
		{
			if (!rss_flow::QueueNextStage(flow, flow.session, now, std::chrono::seconds(15)))
			{
				rss_flow::Finish(flow, rss_flow::Phase::Failed, rss_flow::Result::SessionChanged, now);
				++it;
				continue;
			}
		}

		const char *addon = flow.mode == rss_flow::Mode::MountOnly ? "" : flow.currentAddon.c_str();
		// flow.currentAddon exclusively owns explicit RSS staging. The shared
		// pending field is reserved for the generic addon pipeline.
		CNetMessagePB<CNETMsg_SignonState> *message = GetAddonSignonStateMessage(addon);
		if (!message || !client->GetNetChannel()->SendNetMessage(message, BUF_RELIABLE))
		{
			delete message;
			rss_flow::Finish(flow, rss_flow::Phase::Failed, rss_flow::Result::SendFailed, now);
			++it;
			continue;
		}
		delete message;
		++it;
	}
}

CON_COMMAND_F(mm_add_client_addon, "Add a workshop ID to the global client-only addon list", FCVAR_SPONLY)
{
	if (args.ArgC() < 2)
	{
		Message("Usage: %s <ID>\n", args[0]);
		return;
	}
	g_MultiAddonManager.AddClientAddon(args[1]);
}

CON_COMMAND_F(mm_remove_client_addon, "Remove a workshop ID from the global client-only addon list", FCVAR_SPONLY)
{
	if (args.ArgC() < 2)
	{
		Message("Usage: %s <ID>\n", args[0]);
		return;
	}
	g_MultiAddonManager.RemoveClientAddon(args[1]);
}

CON_COMMAND_F(mm_add_addon, "Add a workshop ID to the extra addon list", FCVAR_SPONLY)
{
	if (args.ArgC() < 2)
	{
		Message("Usage: %s <ID>\n", args[0]);
		return;
	}

	g_MultiAddonManager.AddAddon(args[1]);
}

CON_COMMAND_F(mm_remove_addon, "Remove a workshop ID from the extra addon list", FCVAR_SPONLY)
{
	if (args.ArgC() < 2)
	{
		Message("Usage: %s <ID>\n", args[0]);
		return;
	}

	g_MultiAddonManager.RemoveAddon(args[1]);
}

CON_COMMAND_F(mm_download_addon, "Download an addon manually", FCVAR_SPONLY)
{
	if (args.ArgC() != 2)
	{
		Message("Usage: %s <ID>\n", args[0]);
		return;
	}

	g_MultiAddonManager.DownloadAddon(args[1], false, true);
}

CON_COMMAND_F(mm_print_searchpaths, "Print search paths", FCVAR_SPONLY)
{
	g_pFullFileSystem->PrintSearchPaths();
}

CON_COMMAND_F(mm_print_searchpaths_client, "Print search paths client-side, only usable if you're running the plugin on a listenserver", FCVAR_CLIENTDLL)
{
	g_pFullFileSystem->PrintSearchPaths();
}

KHook::Return<void> MultiAddonManager::Hook_StartupServer(INetworkServerService *pThis, const GameSessionConfiguration_t &config, ISource2WorldSession *session, const char *mapname)
{
	++m_nRssServerEpoch;
	if (!m_nRssServerEpoch)
		m_nRssServerEpoch = 1;
	m_RssDisconnectEvidence.clear();
	gpGlobals = g_pEngineServer->GetServerGlobals();
	g_pNetworkGameServer = g_pNetworkServerService->GetIGameServer();

	m_TimedOutClients.clear();

	// Remove empty paths added when there are 2+ addons, they screw up file writes
	g_pFullFileSystem->RemoveSearchPath("", "GAME");
	g_pFullFileSystem->RemoveSearchPath("", "DEFAULT_WRITE_PATH");

	// This has to be done here to replicate the behavior on clients, where they mount addons in the string order
	// So if the current map is ID 1 and extra addons are IDs 2 and 3, they would be mounted in that order with ID 3 at the top
	// Note that the actual map VPK(s) and any sub-maps like team_select will be even higher, but those usually don't contain any assets that concern us
	RefreshAddons();

	return {KHook::Action::Ignore};
}

bool Hook_SendNetMessage(CServerSideClientBase *pClient, const CNetMessage *pData, NetChannelBufType_t bufType, SendNetMessage_t pOriginalFunc)
{
	NetMessageInfo_t *info = pData->GetNetMessage()->GetNetMessageInfo();
	
	uint64 steamID64 = pClient->GetClientSteamID().ConvertToUint64();
	ClientAddonInfo_t &clientInfo = g_ClientAddons[steamID64];
	
	// If we are sending a message to the client, that means the client is still active.
	clientInfo.lastActiveTime = Plat_FloatTime();

	if (info->m_MessageId != net_SignonState || !g_pEngineServer->IsDedicatedServer())
		return pOriginalFunc(pClient, pData, bufType);

	auto pMsg = const_cast<CNetMessage*>(pData)->ToPB<CNETMsg_SignonState>();

	CUtlVector<std::string> addons;
	const AddonListPurpose signonPurpose = pMsg->signon_state() == SIGNONSTATE_CHANGELEVEL ?
		AddonListPurpose::SignonFilter : AddonListPurpose::GenericSignon;
	g_MultiAddonManager.GetClientAddons(addons, steamID64, signonPurpose,
		clientInfo.currentPendingAddon, pClient->GetPlayerSlot().Get(),
		pClient->GetUserID().Get());
	
	if (pMsg->signon_state() == SIGNONSTATE_CHANGELEVEL)
	{
		// When switching to another map, the signon message might contain more than 1 addon.
		// This puts the client in limbo because client doesn't know how to handle multiple addons at the same time.
		CUtlVector<std::string> addonsList;
		StringToVector(pMsg->addons().c_str(), addonsList);
		CUtlVector<std::string> filteredAddonsList;
		const std::string workshopMap = g_MultiAddonManager.GetCurrentWorkshopMap();
		FOR_EACH_VEC(addonsList, i)
		{
			// Workshop maps are never filtered even if an ID collides with the RSS allowlist.
			if (!workshopMap.empty() && addonsList[i] == workshopMap)
				filteredAddonsList.AddToTail(addonsList[i]);
			else if (!g_MultiAddonManager.IsRssAssetAddon(addonsList[i].c_str()) ||
				(addons.Find(addonsList[i].c_str()) != -1 &&
					g_MultiAddonManager.AllowExplicitRssChangeLevel(steamID64, pClient,
						addonsList[i].c_str())))
				filteredAddonsList.AddToTail(addonsList[i]);
		}
		if (filteredAddonsList.Count() > 1)
		{
			// If there's more than one addon, ensure that it takes the first addon (which should be the workshop map or the first custom addon)
			pMsg->set_addons(filteredAddonsList.Head());
			// Since the client will download the addon contained inside this messsage, we might as well add it to the list of client's downloaded addons.
			if (!g_MultiAddonManager.IsRssAssetAddon(filteredAddonsList.Head().c_str()))
				clientInfo.currentPendingAddon = filteredAddonsList.Head();
			else if (g_MultiAddonManager.IsRssAssetAddon(clientInfo.currentPendingAddon.c_str()))
				clientInfo.currentPendingAddon.clear();
		}
		else if (filteredAddonsList.Count() == 1)
		{
			// Nothing to do here, the rest of the required addons can be sent later.
			if (!g_MultiAddonManager.IsRssAssetAddon(filteredAddonsList.Head().c_str()))
				clientInfo.currentPendingAddon = filteredAddonsList.Head().c_str();
			else if (g_MultiAddonManager.IsRssAssetAddon(clientInfo.currentPendingAddon.c_str()))
				clientInfo.currentPendingAddon.clear();
			pMsg->set_addons(filteredAddonsList.Head().c_str());
		}
		else
		{
			clientInfo.currentPendingAddon.clear();
			pMsg->set_addons("");
		}
		
		return pOriginalFunc(pClient, pData, bufType);
	}
	FOR_EACH_VEC(clientInfo.downloadedAddons, i)
	{
		addons.FindAndRemove(clientInfo.downloadedAddons[i]);
	}
	
	// Check if client has downloaded everything.
	if (addons.Count() == 0)
	{
		// Complete only the generic pending item. Explicit RSS completion is
		// flow-local and requires its exact authenticated ClientActive path.
		g_MultiAddonManager.CompleteGenericPendingAddon(steamID64, true);
		return pOriginalFunc(pClient, pData, bufType);
	}

	if (mm_addon_debug.Get())
		Message("%s: Number of addons remaining to download for %lli: %d\n", __func__, steamID64, addons.Count());

	// Otherwise, send the next addon to the client.
	clientInfo.currentPendingAddon = addons.Head();
	pMsg->set_addons(addons.Head().c_str());
	pMsg->set_signon_state(SIGNONSTATE_CHANGELEVEL);

	return pOriginalFunc(pClient, pData, bufType);
}

KHook::Return<bool> MultiAddonManager::Hook_SendNetMessage_ServerSideClient(CServerSideClientBase *pClient, const CNetMessage *pData, NetChannelBufType_t bufType)
{
	return {KHook::Action::Supersede, Hook_SendNetMessage(pClient, pData, bufType, (SendNetMessage_t)KHook::GetOriginalFunction())};
}

KHook::Return<bool> MultiAddonManager::Hook_SendNetMessage_HLTVClient(CServerSideClientBase *pClient, const CNetMessage *pData, NetChannelBufType_t bufType)
{
	return {KHook::Action::Supersede, Hook_SendNetMessage(pClient, pData, bufType, (SendNetMessage_t)KHook::GetOriginalFunction())};
}

KHook::Return<void> MultiAddonManager::Hook_DisconnectSource(CServerSideClientBase *pClient,
	ENetworkDisconnectionReason reason, const char *pszInternalReason)
{
	if (!pClient)
		return {KHook::Action::Ignore};
	const uint64 steamID64 = pClient->GetClientSteamID().ConvertToUint64();
	std::unordered_map<uint64, RssClientSession>::const_iterator stored =
		m_RssClientSessions.find(steamID64);
	if (!steamID64 || stored == m_RssClientSessions.end() || !stored->second.serial ||
		stored->second.slot != pClient->GetPlayerSlot().Get() ||
		stored->second.userId != pClient->GetUserID().Get())
		return {KHook::Action::Ignore};
	rss_flow::DisconnectEvidence evidence;
	evidence.providerEpoch = m_nRssProviderEpoch;
	evidence.serverEpoch = m_nRssServerEpoch;
	evidence.steamId = steamID64;
	evidence.session.slot = stored->second.slot;
	evidence.session.userId = stored->second.userId;
	evidence.session.serial = stored->second.serial;
	m_RssDisconnectEvidence[steamID64] = evidence;
	return {KHook::Action::Ignore};
}

// pMgrDoNotUse is named as such because the variable is optimized out in Windows builds and will not be passed to the function.
// The original Windows function just uses the global singleton instead.
KHook::Return<void> MultiAddonManager::Hook_SetPendingHostStateRequest(CHostStateMgr* pMgrDoNotUse, CHostStateRequest *pRequest)
{
	// When IVEngineServer::ChangeLevel is called by the plugin or the server code,
	// (which happens at the end of a map), the server-defined addon does not change.
	// Also, host state requests coming from that function will always have "ChangeLevel" in its KV's name.
	// We can use this information to always be aware of what the original addon is.
	
	if (!pRequest->m_pKV)
	{
		// g_pEngineServer->IsMapValid takes into account mounted addons so we have to do this instead
		char szFileName[MAX_PATH];
		V_snprintf(szFileName, sizeof(szFileName), "maps/%s.vpk", pRequest->m_LevelName.Get());
		bool bValveMap = g_pFullFileSystem->FileExists(szFileName, "MOD");

		// Workshop map changes from end of match votes have null keyvalues
		// ...and when such votes lead to reloading the CURRENT map, m_Addons will also be null, in which case we want to keep the workshop map unchanged
		if (!pRequest->m_Addons.IsEmpty())
			SetCurrentWorkshopMap(pRequest->m_Addons);
		else if (bValveMap) // Sadly this will include any workshop maps that share names with shipped Valve maps, but at this point there's no way to tell
			ClearCurrentWorkshopMap();
	}
	else if (V_stricmp(pRequest->m_pKV->GetName(), "ChangeLevel"))
	{
		if (!V_stricmp(pRequest->m_pKV->GetName(), "map_workshop"))
			SetCurrentWorkshopMap(pRequest->m_pKV->GetString("customgamemode", ""));
		else
			ClearCurrentWorkshopMap();
	}

	// Valve changed the way community maps (like de_dogtown) are loaded
	// Now their content lives in addons and they're mounted internally somehow (m_Addons is already set to it by this point)
	// So check if the addon is indeed one of the community maps and keep it, otherwise clients would error out due to missing assets
	// Each map has its own folder under game/csgo_community_addons which is mounted as "OFFICIAL_ADDONS"
	if (!pRequest->m_Addons.IsEmpty() && g_pFullFileSystem->IsDirectory(pRequest->m_Addons.String(), "OFFICIAL_ADDONS"))
		SetCurrentWorkshopMap(pRequest->m_Addons);

	if (m_ExtraAddons.Count() == 0)
		return {KHook::Action::Ignore};

	// Rebuild the addon list. We always start with the original addon.
	if (GetCurrentWorkshopMap().empty())
	{
		pRequest->m_Addons = VectorToString(m_ExtraAddons).c_str();
	}
	else
	{
		// Don't add the same addon twice. Hopefully no server owner is diabolical enough to do things like `map de_dust2 customgamemode=1234,5678`.
		CUtlVector<std::string> newAddons;
		newAddons.CopyArray(m_ExtraAddons.Base(), m_ExtraAddons.Count());
		newAddons.FindAndRemove(GetCurrentWorkshopMap().c_str());
		newAddons.AddToHead(GetCurrentWorkshopMap().c_str());
		pRequest->m_Addons = VectorToString(newAddons).c_str();
	}

	return {KHook::Action::Ignore};
}

bool MultiAddonManager::CompleteGenericPendingAddon(uint64 steamID64, bool allowGenericRss)
{
	ClientAddonInfo_t &clientInfo = g_ClientAddons[steamID64];
	if (clientInfo.currentPendingAddon.empty())
		return false;
	const bool rssPending = IsRssAssetAddon(clientInfo.currentPendingAddon.c_str());
	std::unordered_map<uint64, rss_flow::Flow>::const_iterator flow =
		m_RssAssetFlows.find(steamID64);
	const bool hasExplicitFlow = flow != m_RssAssetFlows.end() &&
		!rss_flow::IsTerminal(flow->second.phase);
	const bool withinTimeout = Plat_FloatTime() - clientInfo.lastActiveTime <=
		mm_extra_addons_timeout.Get();
	const rss_flow::GenericCompletionAction action = rss_flow::DecideGenericCompletion(
		ToFlowMode(GetClientRssAssetMode(steamID64)), rssPending, hasExplicitFlow,
		allowGenericRss, withinTimeout);
	if (action == rss_flow::GenericCompletionAction::KeepPending)
		return false;
	if (action == rss_flow::GenericCompletionAction::RecordAndClear &&
		clientInfo.downloadedAddons.Find(clientInfo.currentPendingAddon.c_str()) == -1)
		clientInfo.downloadedAddons.AddToTail(clientInfo.currentPendingAddon);
	if (rss_flow::PublishesGenericRssCache(action, rssPending))
		clientInfo.rssCacheGeneration = m_nRssAddonGeneration;
	clientInfo.currentPendingAddon.clear();
	return action == rss_flow::GenericCompletionAction::RecordAndClear;
}

void MultiAddonManager::CheckClientAddons(uint64 steamID64)
{
	ClientAddonInfo_t &clientInfo = g_ClientAddons[steamID64];
	clientInfo.connectedState = CLIENTCONN_JOINED;
	const rss_flow::TimePoint now = rss_flow::Clock::now();
	std::unordered_map<uint64, rss_flow::Flow>::iterator explicitFlow = m_RssAssetFlows.find(steamID64);

	if (explicitFlow != m_RssAssetFlows.end() &&
		!rss_flow::IsTerminal(explicitFlow->second.phase) &&
		explicitFlow->second.phase == rss_flow::Phase::AwaitingReconnect &&
		explicitFlow->second.disconnectObserved)
	{
		std::unordered_map<uint64, RssClientSession>::const_iterator session = m_RssClientSessions.find(steamID64);
		if (session != m_RssClientSessions.end())
		{
			rss_flow::Session next;
			next.slot = session->second.slot;
			next.userId = session->second.userId;
			next.serial = session->second.serial;
			const float configured = mm_extra_addons_timeout.Get();
			const int seconds = configured <= 0.0f ? 180 :
				(configured >= 1200.0f ? 1200 : std::max(1, static_cast<int>(configured)));
			if (rss_flow::ObserveReconnect(explicitFlow->second, m_nRssProviderEpoch,
				m_nRssAddonGeneration, next, now, std::chrono::seconds(seconds)))
			{
				if (explicitFlow->second.phase == rss_flow::Phase::AwaitingActive)
					rss_flow::PromotePendingMountList(explicitFlow->second, next, now);
			}
		}
	}

	CompleteGenericPendingAddon(steamID64, true);
	clientInfo.lastActiveTime = Plat_FloatTime();
}

KHook::Return<bool> MultiAddonManager::Hook_ClientConnect(IServerGameClients *pThis, CPlayerSlot slot,
	const char *pszName, uint64 steamID64, const char *pszNetworkID, bool unk1,
	CBufferString *pRejectReason)
{
	// Preserve the legacy generic pipeline's reconnect receipt for non-RSS
	// addons only. Explicit RSS flow/session consumption remains ClientActive-only.
	CompleteGenericPendingAddon(steamID64, true);
	return {KHook::Action::Ignore};
}

KHook::Return<bool> MultiAddonManager::Hook_CanHLTVClientConnect(IServerGameClients *pThis,
	int index, const CSteamID &steamID, int *pRejectReason)
{
	return {KHook::Action::Ignore};
}

KHook::Return<void> MultiAddonManager::Hook_ClientDisconnect(IServerGameClients *pThis,
	CPlayerSlot slot, ENetworkDisconnectionReason reason, const char *pszName,
	uint64 steamID64, const char *pszNetworkID)
{
	// Consume only the immutable token captured from the concrete client object's
	// Disconnect frame. Never reconstruct an ended session from the current slot.
	std::unordered_map<uint64, RssClientSession>::const_iterator session = m_RssClientSessions.find(steamID64);
	rss_flow::Session stored;
	const rss_flow::Session *storedPtr = nullptr;
	if (session != m_RssClientSessions.end())
	{
		stored.slot = session->second.slot;
		stored.userId = session->second.userId;
		stored.serial = session->second.serial;
		storedPtr = &stored;
	}
	rss_flow::DisconnectEvidence evidence;
	const rss_flow::DisconnectConsumeResult consume =
		rss_flow::TryConsumeDisconnectEvidence(m_RssDisconnectEvidence,
			m_nRssProviderEpoch, m_nRssServerEpoch, steamID64, slot.Get(),
			storedPtr, &evidence);
	if (consume != rss_flow::DisconnectConsumeResult::Consumed)
		return {KHook::Action::Ignore};

	ClientAddonInfo_t &info = g_ClientAddons[steamID64];
	info.lastActiveTime = Plat_FloatTime();
	info.connectedState = CLIENTCONN_NONE;
	std::unordered_map<uint64, rss_flow::Flow>::iterator flow = m_RssAssetFlows.find(steamID64);
	if (flow != m_RssAssetFlows.end())
	{
		const float configured = mm_extra_addons_timeout.Get();
		const int seconds = configured <= 0.0f ? 180 :
			(configured >= 1200.0f ? 1200 : std::max(1, static_cast<int>(configured)));
		rss_flow::ObserveDisconnect(flow->second, m_nRssProviderEpoch,
			m_nRssAddonGeneration, evidence.session, rss_flow::Clock::now(),
			std::chrono::seconds(seconds));
	}
	m_RssClientSessions.erase(session);
	return {KHook::Action::Ignore};
}

KHook::Return<void> MultiAddonManager::Hook_ClientActive(IServerGameClients *pThis,
	CPlayerSlot slot, bool bLoadGame, const char *pszName, uint64 steamID64)
{
	RssClientSession session;
	CServerSideClient *client = nullptr;
	if (GetCurrentRssSession(steamID64, session, &client) && client && session.slot == slot.Get())
	{
		CheckClientAddons(steamID64);
		std::unordered_map<uint64, rss_flow::Flow>::iterator flow = m_RssAssetFlows.find(steamID64);
		if (flow != m_RssAssetFlows.end() && !rss_flow::IsTerminal(flow->second.phase))
		{
			rss_flow::Session current;
			current.slot = session.slot;
			current.userId = session.userId;
			current.serial = session.serial;
			if (rss_flow::MarkClientActive(flow->second, current, rss_flow::Clock::now()) &&
				flow->second.phase == rss_flow::Phase::Active)
				PublishRssAssetCompletion(steamID64, flow->second);
		}
	}

	if (!mm_cache_clients_with_addons.Get())
	{
		g_ClientAddons[steamID64].downloadedAddons.RemoveAll();
		g_ClientAddons[steamID64].rssCacheGeneration = 0;
	}
	return {KHook::Action::Ignore};
}

KHook::Return<void> MultiAddonManager::Hook_GameFrame(IServerGameDLL *pThis, bool simulating, bool bFirstTick, bool bLastTick)
{
	// Explicit refresh never starts network work in the API call stack.
	AdvanceRssAssetFlows();

	static double s_flTime = 0.0f;

	// Print download progress every second
	if (Plat_FloatTime() - s_flTime > 1.f)
	{
		s_flTime = Plat_FloatTime();
		PrintDownloadProgress();
	}

	if (m_TimedOutClients.empty())
		return {KHook::Action::Ignore};

	CUtlVector<CServerSideClient *> *clients = GetClientList();
	for (std::size_t i = 0; i < m_TimedOutClients.size();)
	{
		const TimedOutClientToken token = m_TimedOutClients[i];
		bool exact = token.providerEpoch == m_nRssProviderEpoch &&
			token.addonGeneration == m_nRssAddonGeneration && clients != nullptr;
		std::unordered_map<uint64, RssClientSession>::const_iterator stored =
			m_RssClientSessions.find(token.steamId);
		if (exact && (stored == m_RssClientSessions.end() || stored->second.serial != token.sessionSerial ||
			stored->second.slot != token.slot || stored->second.userId != token.userId))
			exact = false;
		std::unordered_map<uint64, rss_flow::Flow>::const_iterator flow =
			m_RssAssetFlows.find(token.steamId);
		if (exact && token.flowId)
		{
			rss_flow::Session timeoutSession;
			timeoutSession.slot = token.slot;
			timeoutSession.userId = token.userId;
			timeoutSession.serial = token.sessionSerial;
			if (flow == m_RssAssetFlows.end() || !rss_flow::MatchesTimeoutToken(flow->second,
				token.providerEpoch, token.addonGeneration, token.flowId, timeoutSession))
				exact = false;
		}
		else if (exact && !rss_flow::CanRunLegacyTimeout(flow != m_RssAssetFlows.end()))
		{
			// A legacy timeout token cannot be reinterpreted after an explicit flow
			// (including a terminal flow) has appeared for the same exact session.
			exact = false;
		}

		CServerSideClient *client = nullptr;
		if (exact)
		{
			FOR_EACH_VEC(*clients, clientIndex)
			{
				CServerSideClient *candidate = (*clients)[clientIndex];
				if (candidate && candidate->GetClientSteamID().ConvertToUint64() == token.steamId &&
					candidate->GetPlayerSlot().Get() == token.slot && candidate->GetUserID().Get() == token.userId)
				{
					client = candidate;
					break;
				}
			}
		}
		if (client)
		{
			client->Disconnect(NETWORK_DISCONNECT_TIMEDOUT,
				"Required Workshop addon download was not accepted in time");
			g_ClientAddons[token.steamId].connectedState = CLIENTCONN_NONE;
		}
		m_TimedOutClients.erase(m_TimedOutClients.begin() + i);
	}

	return {KHook::Action::Ignore};
}

KHook::Return<void> MultiAddonManager::Hook_PostEvent(IGameEventSystem *pThis, CSplitScreenSlot nSlot, bool bLocalOnly, int nClientCount, const uint64 *clients,
	INetworkMessageInternal *pEvent, const CNetMessage *pData, unsigned long nSize, NetChannelBufType_t bufType)
{
	NetMessageInfo_t *info = pEvent->GetNetMessageInfo();

	if (mm_block_disconnect_messages.Get() && info->m_MessageId == GE_Source1LegacyGameEvent)
	{
		auto pMsg = pData->ToPB<CMsgSource1LegacyGameEvent>();

		static int sDisconnectId = g_pGameEventManager->LookupEventId("player_disconnect");

		if (pMsg->eventid() == sDisconnectId)
		{
			IGameEvent *pEvent = g_pGameEventManager->UnserializeEvent(*pMsg);

			// This will prevent "loop shutdown" messages in the chat when clients reconnect
			// As far as we're aware, there are no other cases where this reason is used
			if (pEvent->GetInt("reason") == NETWORK_DISCONNECT_LOOPSHUTDOWN)
				*(uint64*)clients = 0;
		}
	}

	return {KHook::Action::Ignore};
}

KHook::Return<int> MultiAddonManager::Hook_LoadEventsFromFile(IGameEventManager2 *pThis, const char *filename, bool bSearchAll)
{
	if (!g_pGameEventManager)
		g_pGameEventManager = pThis;

	return {KHook::Action::Ignore};
}

KHook::Return<void> MultiAddonManager::Hook_ReplyConnection(INetworkGameServer *pThis, CServerSideClient *pClient)
{
	uint64 steamID64 = pClient->GetClientSteamID().ConvertToUint64();
	const int replySlot = pClient->GetPlayerSlot().Get();
	const int replyUserId = pClient->GetUserID().Get();
	ClientAddonInfo_t &clientInfo = g_ClientAddons[steamID64];
	if (mm_cache_clients_with_addons.Get() && mm_cache_clients_duration.Get() != 0 && Plat_FloatTime() - clientInfo.lastActiveTime > mm_cache_clients_duration.Get())
	{
		if (mm_addon_debug.Get())
			Message("%s: Client %lli has not connected for a while, clearing the cache\n", __func__, steamID64);

		clientInfo.currentPendingAddon.clear();
		clientInfo.downloadedAddons.RemoveAll();
		clientInfo.rssCacheGeneration = 0;
	}
	clientInfo.lastActiveTime = Plat_FloatTime();

	CUtlString *addons = (CUtlString *)((uintptr_t)pThis + g_iServerAddonsOffset);
	CUtlString originalAddons = *addons;

	CUtlVector<std::string> stagedAddons;
	GetClientAddons(stagedAddons, steamID64, AddonListPurpose::GenericDownload);

	if (stagedAddons.Count() != 0 && clientInfo.connectedState != CLIENTCONN_CONNECTING)
	{
		clientInfo.connectionStartTime = Plat_FloatTime();
		clientInfo.connectedState = CLIENTCONN_CONNECTING;
	}
	else if (mm_addon_connection_timeout.Get() > 0 &&
		clientInfo.connectedState == CLIENTCONN_CONNECTING && 
		Plat_FloatTime() - clientInfo.connectionStartTime > mm_addon_connection_timeout.Get())
	{
		// Can't kick right now as this will crash on windows, so defer to the next frame
		AddTimedOutClient(steamID64, pClient);
		return {KHook::Action::Supersede};
	}

	if (stagedAddons.Count() != 0 && clientInfo.downloadedAddons.Find(stagedAddons[0]) == -1 &&
		clientInfo.currentPendingAddon.empty())
		clientInfo.currentPendingAddon = stagedAddons[0];

	// Preserve the original one-at-a-time signature-check mitigation for every
	// non-RSS addon and for DownloadAndMount's current RSS stage.
	FOR_EACH_VEC_BACK(stagedAddons, i)
	{
		if (clientInfo.downloadedAddons.Find(stagedAddons[i]) != -1 ||
			stagedAddons[i] == clientInfo.currentPendingAddon)
			continue;
		stagedAddons.Remove(i);
	}
	std::unordered_map<uint64, rss_flow::Flow>::iterator pendingFlow =
		m_RssAssetFlows.find(steamID64);
	if (pendingFlow != m_RssAssetFlows.end() &&
		pendingFlow->second.providerEpoch == m_nRssProviderEpoch &&
		pendingFlow->second.addonGeneration == m_nRssAddonGeneration)
		rss_flow::ObservePendingReply(pendingFlow->second, replySlot, replyUserId,
			rss_flow::Clock::now(), std::chrono::seconds(30));

	CUtlVector<std::string> requestedMountAddons;
	GetClientAddons(requestedMountAddons, steamID64, AddonListPurpose::ReplyMount,
		clientInfo.currentPendingAddon, replySlot, replyUserId);
	CUtlVector<std::string> replyAddons;
	FOR_EACH_VEC(requestedMountAddons, i)
	{
		if (IsRssAssetAddon(requestedMountAddons[i].c_str()) ||
			stagedAddons.Find(requestedMountAddons[i]) != -1)
			replyAddons.AddToTail(requestedMountAddons[i]);
	}

	// Even an empty Disabled result must override the server's original addon
	// string while the original ReplyConnection implementation is called.
	*addons = VectorToString(replyAddons).c_str();

	if (mm_addon_debug.Get())
		Message("%s: Sending addons %s to steamID64 %lli\n", __func__, addons->Get(), steamID64);

	m_hookReplyConnection.CallOriginal(pThis, pClient);

	const RssAssetMode mode = GetClientRssAssetMode(steamID64);
	std::vector<std::string> delivered;
	FOR_EACH_VEC(replyAddons, deliveredIndex)
		delivered.push_back(replyAddons[deliveredIndex]);
	clientInfo.appliedRssMode = static_cast<RssAssetMode>(static_cast<uint32>(
		rss_flow::AppliedModeForDeliveredList(ToFlowMode(mode), GetEffectiveRssAddons(),
			delivered)));
	std::unordered_map<uint64, rss_flow::Flow>::iterator flow = m_RssAssetFlows.find(steamID64);
	std::unordered_map<uint64, RssClientSession>::const_iterator session = m_RssClientSessions.find(steamID64);
	if (flow != m_RssAssetFlows.end() && !rss_flow::IsTerminal(flow->second.phase))
	{
		bool completeMountList = true;
		for (std::size_t i = 0; i < flow->second.addons.size(); ++i)
			if (replyAddons.Find(flow->second.addons[i].c_str()) == -1)
				completeMountList = false;
		if (completeMountList)
		{
			if (flow->second.phase == rss_flow::Phase::AwaitingActive &&
				session != m_RssClientSessions.end() && session->second.slot == replySlot &&
				session->second.userId == replyUserId)
			{
				rss_flow::Session exact;
				exact.slot = session->second.slot;
				exact.userId = session->second.userId;
				exact.serial = session->second.serial;
				if (rss_flow::MarkMountListDelivered(flow->second, exact, rss_flow::Clock::now(),
					std::chrono::seconds(30)) && flow->second.phase == rss_flow::Phase::Active)
					PublishRssAssetCompletion(steamID64, flow->second);
			}
			else if (flow->second.phase == rss_flow::Phase::AwaitingReconnect &&
				flow->second.disconnectObserved)
			{
				// ReplyConnection may precede ClientActive. Mark delivery only for the
				// concrete candidate observed before the original Reply call.
				rss_flow::MarkPendingMountListDelivered(flow->second, replySlot,
					replyUserId, rss_flow::Clock::now());
			}
		}
	}

	*addons = originalAddons;

	return {KHook::Action::Supersede};
}

KHook::Return<uint64> MultiAddonManager::Hook_ScriptGetAddon()
{
	if (!m_ExtraAddons.Count())
		return {KHook::Action::Ignore};

	uint64 iAddon = V_StringToUint64(GetCurrentWorkshopMap().c_str(), 0);
	
	if (!iAddon)
		return {KHook::Action::Ignore};

	return {KHook::Action::Supersede, iAddon};
}

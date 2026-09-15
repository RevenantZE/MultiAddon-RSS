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

#pragma once

#define MULTIADDONMANAGER_INTERFACE "MultiAddonManager003"
#define MULTIADDONMANAGER_RSS_ASSETS_INTERFACE "MultiAddonManager004"
#define MULTIADDONMANAGER_RSS_ASSETS_V2_INTERFACE "MultiAddonManager005"
class IMultiAddonManager
{
public:
	// These add/remove to the internal list without reloading anything
	// pszWorkshopID is the workshop ID in string form (e.g. "3157463861")
	virtual bool AddAddon(const char *pszWorkshopID, bool bRefresh = false) = 0;
	virtual bool RemoveAddon(const char *pszWorkshopID,  bool bRefresh = false) = 0;
	
	// Returns true if the given addon is mounted in the filesystem. 
	// Pass 'true' to bCheckWorkshopMap to check from the server mounted workshop map as well.
	virtual bool IsAddonMounted(const char *pszWorkshopID, bool bCheckWorkshopMap = false) = 0;

	// Start an addon download of the given workshop ID
	// Returns true if the download successfully started or the addon already exists, and false otherwise
	// bImportant: If set, the map will be reloaded once the download finishes 
	// bForce: If set, will start the download even if the addon already exists
	virtual bool DownloadAddon(const char *pszWorkshopID, bool bImportant = false, bool bForce = true) = 0;

	// Refresh addons, applying any changes from add/remove
	// This will trigger a map reload once all addons are updated and mounted
	virtual void RefreshAddons(bool bReloadMap = false) = 0;

	// Clear the internal list and unmount all addons excluding the current workshop map
	virtual void ClearAddons() = 0;
	
	// Check whether the server is connected to the game coordinator, and therefore is capable of downloading addons.
	// Should be called before calling DownloadAddon.
	virtual bool HasUGCConnection() = 0;
	
	// Functions to manage addons to be loaded only by a client. 
	// Pass a steamID value of 0 to perform the operation on a global list instead, and bRefresh to 'true' to trigger a reconnect if necessary.
	virtual void AddClientAddon(const char *pszAddon, uint64 steamID64 = 0, bool bRefresh = false) = 0;
	virtual void RemoveClientAddon(const char *pszAddon, uint64 steamID64 = 0) = 0;
	virtual void ClearClientAddons(uint64 steamID64 = 0) = 0;
};

// RSS extension. The original 003 interface remains available for compatibility.
// Compatibility view over the cfg-defined RSS asset set. Workshop maps and
// every non-RSS addon keep the required flow.
class IMultiAddonManager004 : public IMultiAddonManager
{
public:
	virtual bool IsClientRssAssetsEnabled(uint64 steamID64) const = 0;
	// Main/game-thread-only. Must be called serialized on the server game thread
	// (CS2Fixes menu/command dispatch path). No internal locking is provided and
	// KHook internal locking does not protect RSS preference state.
	virtual bool SetClientRssAssetsEnabled(uint64 steamID64, bool bEnabled) = 0;
};

enum class RssAssetMode : uint32
{
	Disabled = 0,
	MountOnly = 1,
	DownloadAndMount = 2
};

enum class RssAssetFlowPhase : uint32
{
	None = 0,
	Queued = 1,
	AwaitingReconnect = 2,
	Staging = 3,
	AwaitingActive = 4,
	Active = 5,
	Failed = 6,
	Cancelled = 7
};

enum class RssAssetResult : uint32
{
	Ok = 0,
	Queued = 1,
	InvalidArgument = 2,
	Unavailable = 3,
	NotReady = 4,
	Busy = 5,
	NoAddons = 6,
	StoreReadOnly = 7,
	PersistFailed = 8,
	SendFailed = 9,
	TimedOut = 10,
	Cancelled = 11,
	ConfigChanged = 12,
	SessionChanged = 13,
	AuthenticationFailed = 14
};

enum RssAssetStatusFlag : uint32
{
	RssAssetStatusFlag_None = 0,
	RssAssetStatusFlag_ModePersisted = 1u << 0,
	RssAssetStatusFlag_ReconnectRequested = 1u << 1,
	RssAssetStatusFlag_MountListDelivered = 1u << 2,
	RssAssetStatusFlag_ClientActiveObserved = 1u << 3
};

struct RssAssetStatus
{
	uint32 structSize;
	RssAssetMode configuredMode;
	RssAssetMode appliedMode;
	RssAssetFlowPhase phase;
	RssAssetResult result;
	uint32 flags;
	uint32 addonGeneration;
	uint32 stagesCompleted;
	uint32 stagesTotal;
	uint32 remainingSeconds;
	int32 slot;
	int32 userId;
	uint64 providerEpoch;
	uint64 flowId;
	uint64 sessionSerial;
};

static_assert(sizeof(RssAssetStatus) == 72, "RssAssetStatus ABI size changed");

class IMultiAddonManager005 : public IMultiAddonManager004
{
public:
	virtual RssAssetMode GetClientRssAssetMode(uint64 steamID64) const = 0;
	virtual RssAssetResult SetClientRssAssetMode(uint64 steamID64, RssAssetMode mode) = 0;
	virtual RssAssetResult RefreshClientRssAssets(uint64 steamID64, RssAssetMode mode,
		uint32 maxFlowSeconds, uint64 &outFlowId) = 0;
	virtual bool GetClientRssAssetStatus(uint64 steamID64, RssAssetStatus &inOutStatus) const = 0;
	virtual bool CancelClientRssAssetFlow(uint64 steamID64, uint64 flowId) = 0;
};

#pragma once

namespace VFTIndexes
{
	namespace IClientEngine
	{
		constexpr int GetClientUser = 7;
	}

	namespace IClientApps
	{
		constexpr int GetAppData = 0;
		constexpr int GetAppDataSection = 5;
		constexpr int RequestAppInfoUpdate = 7;
		constexpr int GetDLCCount = 8;
		constexpr int GetDLCDataByIndex = 9;
		constexpr int GetAppType = 10;
	}

	namespace IClientAppManager
	{
		constexpr int InstallApp = 0;
		constexpr int UninstallApp = 1;
		constexpr int LaunchApp = 2;
		constexpr int GetAppInstallState = 4;
		constexpr int IsAppDlcInstalled = 9;
		constexpr int BIsDlcEnabled = 11;
		constexpr int GetUpdateInfo = 20;
	}

	namespace IClientRemoteStorage
	{
		// Manual RE from the IClientRemoteStorage IPC method-name table.
		constexpr int IsCloudEnabledForApp = 24;
		// idx25: void(this, appId, bEnabled). The per-app cloud toggle the UI
		// Properties->General "Keep game saves in the Steam Cloud" switch calls. It writes the
		// value into the in-memory config store (and updates the cloud-enabled map node
		// appId@+0x10/bEnabled@+0x14). The lua-hot-reload "Steam Cloud out of date" badge is
		// painted by the post-reload AutoCloud re-evaluation, which reads that in-memory config
		// value — so calling SetCloudEnabledForApp(appId,false) makes the re-eval see the app as
		// cloud-disabled and paint no badge (verified on device). The in-memory write is
		// REQUIRED (skipping it re-introduces the badge). It also dirties the roaming config
		// store, which would normally flush cloudenabled into the cloud-synced sharedconfig.vdf;
		// SLSsteam strips only its own injected cloudenabled entries at CConfigStore::WriteVdfFile
		// before Steam writes that serialized UserRoamingConfigStore buffer. idx confirmed via
		// frida (UI switch fired slot 25).
		constexpr int SetCloudEnabledForApp = 25;
	}

	namespace IClientUser
	{
		constexpr int BLoggedOn = 4;
		constexpr int GetSteamID = 10;
	}

	namespace IClientUtils
	{
		constexpr int GetOfflineMode = 17;
		constexpr int GetAppId = 19;
	}
}

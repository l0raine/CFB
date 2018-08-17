#include <Windows.h>
#include <VersionHelpers.h>

#include "stdafx.h"

#include "../Common/common.h"
#include "client.h"
#include "device.h"
#include "utils.h"

#pragma comment(lib, "Advapi32.lib") // for privilege check and driver/service {un}loading

static SC_HANDLE g_hService = NULL;
static SC_HANDLE g_hSCManager = NULL;
static HANDLE g_hNamedPipe = INVALID_HANDLE_VALUE;



/*++

--*/
BOOL AssignPrivilegeToSelf(LPWSTR lpPrivilegeName)
{
	HANDLE hToken;
	BOOL bRes = FALSE;
	LUID Luid;

	bRes = OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &hToken);

	if (bRes)
	{
		bRes = LookupPrivilegeValue(NULL, lpPrivilegeName, &Luid);

		if (bRes)
		{
			LUID_AND_ATTRIBUTES Privilege = {
				.Luid = Luid,
				.Attributes = SE_PRIVILEGE_ENABLED
			};

			TOKEN_PRIVILEGES NewPrivs = {
				.PrivilegeCount = 1,
				.Privileges[0].Luid = Luid
			};

			bRes = AdjustTokenPrivileges(hToken,
				FALSE,
				&NewPrivs,
				sizeof(TOKEN_PRIVILEGES),
				(PTOKEN_PRIVILEGES)NULL,
				(PDWORD)NULL) != 0;
		}

		CloseHandle(hToken);
	}

	return bRes;
}


/*++

--*/
BOOL HasPrivilege(LPWSTR lpPrivName, PBOOL lpHasPriv)
{
	LUID Luid;
	HANDLE hToken;
	BOOL bRes, bHasPriv;

	do {

		bRes = LookupPrivilegeValue(NULL, lpPrivName, &Luid);
		if (!bRes)
			break;

		LUID_AND_ATTRIBUTES Privilege = {
			.Luid = Luid,
			.Attributes = SE_PRIVILEGE_ENABLED | SE_PRIVILEGE_ENABLED_BY_DEFAULT
		};
		PRIVILEGE_SET PrivSet = {
			.PrivilegeCount = 1,
			.Privilege[0] = Privilege
		};

		bRes = OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &hToken);
		if (!bRes)
			break;

		bRes = PrivilegeCheck(hToken, &PrivSet, &bHasPriv);
		if (!bRes)
			break;

		*lpHasPriv = bHasPriv;
		bRes = TRUE;

	} while (0);

	return bRes;
}


/*++

--*/
BOOL RunInitializationChecks()
{
	BOOL IsSeDebugEnabled;

	xlog(LOG_INFO, L"Checking for Debug privilege...\n");

	if (!HasPrivilege(L"SeDebugPrivilege", &IsSeDebugEnabled))
	{
		PrintError(L"HasPrivilege()");
		return FALSE;
	}

	if (!IsSeDebugEnabled)
	{
		xlog(LOG_WARNING, L"SeDebugPrivilege is not enabled, trying to enable...\n");
		if (AssignPrivilegeToSelf(L"SeDebugPrivilege") == FALSE)
		{
			xlog(LOG_CRITICAL, L"SeDebugPrivilege is required for %s to run\n", CFB_PROGRAM_NAME_SHORT);
			xlog(LOG_INFO, L"Hint: Are you running as Administrator?\n");
			PrintError(L"AssignPrivilegeToSelf");
			return FALSE;
		}
	}

	xlog(LOG_SUCCESS, L"Got SeDebugPrivilege, resuming...\n");

	return TRUE;
}



/*++

Starts a thread that will run the command line interpreter to interact with the driver.

--*/
VOID StartInterpreter()
{
	RunInterpreter();
	/*
	DWORD dwCliTid;

	HANDLE hCli = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)RunInterpreter, NULL, 0, &dwCliTid);
	if (!hCli)
	{
		xlog(LOG_CRITICAL, L"Fatal! Failed to start interpreter... Reason: %lu\n", GetLastError());
		ExitProcess(-1);
	}

	xlog(LOG_SUCCESS, L"Interpreter thread started as TID=%d\n", dwCliTid);
	WaitForSingleObject(hCli, INFINITE);
	*/
	return;
}


/*++

--*/
BOOL CreateCfbPipe()
{
	xlog(LOG_DEBUG, L"Creating named pipe '%s'...\n", CFB_PIPE_NAME);

	g_hNamedPipe = CreateNamedPipe(
		CFB_PIPE_NAME,
		PIPE_ACCESS_DUPLEX,
		PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
		CFB_PIPE_MAXCLIENTS,
		CFB_PIPE_INBUFLEN,
		CFB_PIPE_OUTBUFLEN,
		0,
		NULL
	);

	if (g_hNamedPipe == INVALID_HANDLE_VALUE)
	{
		PrintError(L"CreateNamedPipe()");
		return FALSE;
	}

	return TRUE;
}


/*++

--*/
BOOL CloseCfbPipe()
{
	xlog(LOG_DEBUG, L"Closing named pipe '%s'...\n", CFB_PIPE_NAME);

	//
	// Wait until all data was consumed
	//
	FlushFileBuffers(g_hNamedPipe);

	//
	// Then close down the named pipe
	//
	if (!DisconnectNamedPipe(g_hNamedPipe))
	{
		PrintError(L"DisconnectNamedPipe()");
		return FALSE;
	}

	return TRUE;
}


/*++

Creates and starts a service for the driver.

--*/
BOOL LoadDriver()
{
	xlog(LOG_DEBUG, L"Loading '%s'\n", CFB_DRIVER_NAME);

	g_hSCManager = OpenSCManager(L"", SERVICES_ACTIVE_DATABASE, SC_MANAGER_CREATE_SERVICE);

	if (!g_hSCManager)
	{
		PrintError(L"OpenSCManager()");
		return FALSE;
	}

	WCHAR lpPath[MAX_PATH] = { 0, };

	GetCurrentDirectory(MAX_PATH-(DWORD)wcslen(CFB_DRIVER_NAME)*sizeof(WCHAR), lpPath);
	wcscat_s(lpPath, MAX_PATH, L"\\");
	wcscat_s(lpPath, MAX_PATH, CFB_DRIVER_NAME);

	xlog(LOG_DEBUG, L"Create the service '%s' for kernel driver '%s'\n", CFB_SERVICE_NAME, lpPath);

	g_hService = CreateService(g_hSCManager,
		CFB_SERVICE_NAME,
		CFB_SERVICE_DESCRIPTION,
		SERVICE_START | DELETE | SERVICE_STOP,
		SERVICE_KERNEL_DRIVER,
		SERVICE_DEMAND_START,
		SERVICE_ERROR_IGNORE,
		lpPath,
		NULL, NULL, NULL, NULL, NULL);

	//
	// if the service was already registered, just open it
	//
	if (!g_hService)
	{
		if (GetLastError() != ERROR_SERVICE_EXISTS)
		{
			PrintError(L"CreateService()");
			return FALSE;
		}

		g_hService = OpenService(g_hSCManager, CFB_SERVICE_NAME, SERVICE_START | DELETE | SERVICE_STOP);
		if (!g_hService)
		{
			PrintError(L"CreateService()");
			return FALSE;
		}
	}

	//
	// start the service
	//

	xlog(LOG_DEBUG, L"Starting service '%s'\n", CFB_SERVICE_NAME);

	if (!StartService(g_hService, 0, NULL))
	{
		PrintError(L"StartService()");
		return FALSE;
	}

	xlog(LOG_DEBUG, L"Success...\n");

	return TRUE;
}


/*++

Stops and unloads the service to the driver.

--*/
BOOL UnloadDriver()
{
	SERVICE_STATUS ServiceStatus;

	xlog(LOG_DEBUG, L"Stopping service '%s'\n", CFB_SERVICE_NAME);

	if (!ControlService(g_hService, SERVICE_CONTROL_STOP, &ServiceStatus))
	{
		PrintError(L"ControlService");
		return FALSE;
	}

	xlog(LOG_DEBUG, L"Service '%s' stopped\n", CFB_SERVICE_NAME);

	if (!DeleteService(g_hService))
	{
		PrintError(L"DeleteService");
		return FALSE;
	}

	xlog(LOG_DEBUG, L"Service '%s' deleted\n", CFB_SERVICE_NAME);

	CloseServiceHandle(g_hService);

	CloseServiceHandle(g_hSCManager);

	return TRUE;
}


/*++

--*/
VOID InitializeCfbContext()
{
	do
	{
		if (!OpenCfbDevice())
		{
			xlog(LOG_CRITICAL, L"Failed to get a handle to '%s'\n", CFB_USER_DEVICE_NAME);
			break;
		}

		return;

	} while (0);

	ExitProcess(-1);
}


/*++

--*/
VOID CleanupCfbContext()
{
	CloseCfbDevice();

	return;
}


/*++

The entrypoint for the loader.

--*/
int main()
{
	xlog(LOG_INFO, L"Starting %s (v%.02f) - by <%s>\n", CFB_PROGRAM_NAME_SHORT, CFB_VERSION, CFB_AUTHOR);
#ifdef _DEBUG
	xlog(LOG_DEBUG, L"DEBUG mode on\n");
#endif

	//
	// Check the OS (must be Vista+)
	//
	if ( !IsWindowsVistaOrGreater() )
		return -1;

	//
	// Check the privileges
	//
	if ( !RunInitializationChecks() )
		return -1;


	//
	// Setup the pipe and load the driver, the service
	//
	if (!CreateCfbPipe())
		return -1;

	if (!LoadDriver())
		return -1;


	InitializeCfbContext();


	//
	// Launch the prompt
	//
	StartInterpreter();


	CleanupCfbContext();


	//
	// Flush and stop the pipe, then unload the service, and the driver
	//
	if (!CloseCfbPipe())
		return -1;

	if (!UnloadDriver())
		return -1;

	xlog(LOG_INFO, L"Thanks for using %s, have a nice day! - %s\n", CFB_PROGRAM_NAME_SHORT, CFB_AUTHOR);

    return 0;
}


#include <Windows.h>

#include "stdafx.h"

#include "../Common/common.h"
#include "client.h"
#include "device.h"
#include "utils.h"

#pragma comment(lib, "Advapi32.lib") // for PrivilegeCheck()


/**
 *
 */
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

/**
 *
 */
BOOL HasPrivilege(LPWSTR lpPrivName, PBOOL lpHasPriv)
{
	LUID Luid;
	HANDLE hToken;
	BOOL bRes, bHasPriv;

	bRes = LookupPrivilegeValue(NULL, lpPrivName, &Luid);
	if (!bRes)
		goto fail;

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
		goto fail;

	bRes = PrivilegeCheck(hToken, &PrivSet, &bHasPriv);
	if (!bRes)
		goto fail;

	*lpHasPriv = bHasPriv;
	return TRUE;

fail:
	return FALSE;
}


/**
 *
 */
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



/**
*
*/
VOID StartInterpreter()
{
	if (!OpenCfbDevice())
	{
		xlog(LOG_CRITICAL, L"Failed to get a handle to '%s'\n", CFB_USER_DEVICE_NAME);
		xlog(LOG_INFO, L"Hint: is the driver registered?\n");
		return;
	}
	/*
	DWORD dwCliTid;

	HANDLE hCli = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)RunInterpreter, NULL, 0, &dwCliTid);
	if (!hCli)
	{
		xlog(LOG_CRITICAL, L"Fatal! Failed to start interpreter... Reason: %lu\n", GetLastError());
	}
	else
	{
		xlog(LOG_SUCCESS, L"Interpreter thread started as TID=%d\n", dwCliTid);
		WaitForSingleObject(hCli, INFINITE);
	}
	*/

	RunInterpreter();

	CloseCfbDevice();

	return;
}


/**
*
*/
int main()
{
	xlog(LOG_INFO, L"Starting %s (v%.02f) - by <%s>\n", CFB_PROGRAM_NAME_SHORT, CFB_VERSION, CFB_AUTHOR);
#ifdef _DEBUG
	xlog(LOG_DEBUG, L"DEBUG mode on\n");
#endif

	if ( !RunInitializationChecks() )
		return -1;

	StartInterpreter();

	xlog(LOG_INFO, L"Thanks for using %s, have a nice day! - %s\n", CFB_PROGRAM_NAME_SHORT, CFB_AUTHOR);

    return 0;
}


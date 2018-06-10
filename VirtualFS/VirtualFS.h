#pragma once
#ifndef _WIN32_WINNT		// Allow use of features specific to Windows XP or later.                   
#define _WIN32_WINNT 0x0501	// Change this to the appropriate value to target other versions of Windows.
#endif

#include <windows.h>
#include "../lib/minhook/include/MinHook.h"
#include <experimental/filesystem>

#define DllExport __declspec( dllexport )

BOOL (WINAPI* TrueFindClose)(HANDLE hFindFile);

HANDLE (WINAPI* TrueFindFirstFileW)(LPCWSTR lpFileName, WIN32_FIND_DATAW* lpFindFileData);

BOOL (WINAPI* TrueFindNextFileW)(HANDLE hFindFile, WIN32_FIND_DATAW* lpFindFileData);

BOOL (WINAPI* TrueGetFileAttributesExW)(LPCWSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId,
                                        LPVOID lpFileInformation);

DWORD (WINAPI* TrueGetFileAttributesW)(LPCWSTR lpFileName);

HANDLE (WINAPI* TrueCreateFileW)(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                                 LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                                 DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);

BOOL (WINAPI* TrueCreateDirectoryW)(LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes);

BOOL (WINAPI* TrueDeleteFileW)(LPCWSTR lpFileName);

BOOL (WINAPI* TrueRemoveDirectoryW)(LPCWSTR lpPathName);

BOOL (WINAPI* TrueSetCurrentDirectoryW)(LPCWSTR lpPathName);

BOOL (WINAPI* TrueGetCurrentDirectoryW)(DWORD nBufferLength, LPWSTR buffer);

HANDLE (WINAPI* TrueFindFirstFileExW)(LPCWSTR lpFileName, FINDEX_INFO_LEVELS fInfoLevelId, LPVOID lpFindFileData,
                                      FINDEX_SEARCH_OPS fSearchOp, LPVOID lpSearchFilter, DWORD dwAdditionalFlags);

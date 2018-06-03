#pragma once
#ifndef _WIN32_WINNT		// Allow use of features specific to Windows XP or later.                   
#define _WIN32_WINNT 0x0501	// Change this to the appropriate value to target other versions of Windows.
#endif

#include <windows.h>
#include "../lib/minhook/include/MinHook.h"
#include <regex>
#include <experimental/filesystem>
#include <list>

#define DllExport __declspec( dllexport )

#define BUFSIZE 4096
#define DEBUG false

#if DEBUG
#define print(x, y) _cwprintf_s(x, y)
#else
#define print(x, y)
#endif


//Represents a "result" of a filesystem query (FindFirstFile/FindNextFile).
typedef struct FileInfo
{
	//WIN32_FIND_DATAW provides more information than name/attributes, but this seems sufficient.
	std::wstring cFileName = L"";
	DWORD dwFileAttributes = 0;

	//Operators used for sorting.
	bool operator==(const FileInfo& f) const { return cFileName == f.cFileName; }
	bool operator<(const FileInfo& f) const { return cFileName < f.cFileName; }

	FileInfo()
	{
	}
} FileInfo;

//We don't have any way to determine the original parameters when hooking FindNextFile.
//By establishing a HANDLE -> Metadata mapping, we can better handle the hook.
//No longer needs to be a struct after being slimmed down.
typedef struct HandleMap
{
	std::list<FileInfo> Results;

	HandleMap()
	{
	}
} HandleMap;


// Definitions for the original functions

BOOL (WINAPI* TrueFindClose)(HANDLE hFindFile);

HANDLE (WINAPI* TrueFindFirstFileW)(LPCWSTR lpFileName, WIN32_FIND_DATAW* lpFindFileData);

BOOL (WINAPI* TrueFindNextFileW)(HANDLE hFindFile, WIN32_FIND_DATAW* lpFindFileData);

BOOL (WINAPI* TrueGetFileAttributesExW)(LPCWSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId,
                                       LPVOID lpFileInformation);

DWORD (WINAPI* TrueGetFileAttributesW)(LPCWSTR lpFileName);

HANDLE (WINAPI* TrueCreateFileW)(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                                 LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                                 DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);

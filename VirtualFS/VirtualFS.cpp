// VirtualFS.cpp : Defines the exported functions for the DLL application.


#include "VirtualFS.h"
#include <fstream>
#include "vfs_data.h"

static vfs::vfs_folder RootFolder;
static std::wstring GamePath;
static FILETIME dummyTime;
static std::map<HANDLE, std::vector<std::wstring>*> ActiveSearchHandles;

// Split string by delimeter
std::vector<std::wstring> split(const std::wstring& s, wchar_t delim)
{
	std::wstringstream ss(s);
	std::wstring item;
	std::vector<std::wstring> elems;
	while (std::getline(ss, item, delim))
		elems.push_back(std::move(item));
	return elems;
}

// Split the last path separator and return a pair (path, file)
std::pair<std::wstring, std::wstring> split_last(const std::wstring& s)
{
	const auto index = s.rfind(L"\\");
	if (index == s.npos)
		return std::pair<std::wstring, std::wstring>(s, L"");
	return std::pair<std::wstring, std::wstring>(s.substr(0, index), s.substr(index + 1));
}

// Walk the VFS by the fiven path
// Return a pointer to a JSON key that is the found file/folder
vfs::vfs_object* walk_vfs(std::wstring path)
{
	auto parts = split(path, L'\\');

	vfs::vfs_object* p = &RootFolder;

	for (auto part = parts.begin(); part != parts.end(); ++part)
	{
		auto folder = static_cast<vfs::vfs_folder*>(p);
		auto folder_contents = folder->get_contents();
		const auto result = folder_contents.find(*part);

		if (result == folder_contents.end())
			return nullptr;

		p = &*result->second;

		if (p->is_file() && part + 1 != parts.end())
			return nullptr;
	}

	return p;
}

// Match a path against common Windows globbing pattern
// This is O(nm) on average because of backtracking. Oh well
bool match_path(const wchar_t* pattern, const wchar_t* str)
{
	for (; *pattern != L'\0'; ++pattern)
	{
		switch (*pattern)
		{
		case '?':
			if (*str == L'\0')
				return false;
			++str;
			break;
		case '*':
			{
				if (pattern[1] == L'\0')
					return true;
				const auto max = wcslen(str);
				for (size_t i = 0; i < max; i++)
					if (match_path(pattern + 1, str + i))
						return true;
				return false;
			}
		default:
			if (*str != *pattern)
				return false;
			++str;
		}
	}
	return *str == L'\0';
}

// Fill the WIN32_FIND_DATA with the info of the provided file
void fill_info(std::wstring file, WIN32_FIND_DATAW* lpFindFileData)
{
	WIN32_FILE_ATTRIBUTE_DATA data;

	// Go through hooks because our path is virtual!
	GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data);

	wchar_t name[_MAX_FNAME + 1];
	wchar_t ext[_MAX_EXT + 1];

	_wsplitpath_s(file.c_str(), nullptr, 0, nullptr, 0, name, _MAX_FNAME + 1, ext, _MAX_EXT + 1);

	lpFindFileData->dwFileAttributes = data.dwFileAttributes;
	wmemset(lpFindFileData->cFileName, L'\0', MAX_PATH);
	wcscpy_s(lpFindFileData->cFileName, MAX_PATH, name);
	wcscat_s(lpFindFileData->cFileName, MAX_PATH, ext);
}

// Get the canonical full path from a string
std::wstring CanonicalPath(LPCWSTR input)
{
	return std::experimental::filesystem::canonical(input);
}

// Called when a file handle is created
// Basically every time something (file/folder) is created, opened, read
HANDLE hook_CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                        LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                        DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
#define ORIGINAL TrueCreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile)

	auto fileName(CanonicalPath(lpFileName));

	if (fileName.compare(0, GamePath.length(), GamePath) != 0)
		return ORIGINAL;

	const auto gamePath = fileName.substr(GamePath.length());

	const auto item = walk_vfs(gamePath);

	if (item == nullptr)
		return ORIGINAL;

	if (item->is_file())
		return TrueCreateFileW(static_cast<vfs::vfs_file*>(item)->get_real_file().c_str(), dwDesiredAccess, dwShareMode,
		                       lpSecurityAttributes,
		                       dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);

	return ORIGINAL;
}

// Closes file search handle an frees the resources
BOOL hook_FindClose(HANDLE hFindFile)
{
	if (ActiveSearchHandles.find(hFindFile) == ActiveSearchHandles.end())
		return TrueFindClose(hFindFile);

	auto list = ActiveSearchHandles[hFindFile];
	ActiveSearchHandles.erase(hFindFile);
	list->clear();
	delete list;
	delete static_cast<char*>(hFindFile);
	hFindFile = nullptr;

	return TRUE;
}

// Finds the first file based on the pattern and returns a file search handle to find more files
HANDLE hook_FindFirstFileW(LPCWSTR lpFileName, WIN32_FIND_DATAW* lpFindFileData)
{
	auto fileName(CanonicalPath(lpFileName));

	if (fileName.compare(0, GamePath.length(), GamePath) != 0)
		return TrueFindFirstFileW(lpFileName, lpFindFileData);

	const auto gamePath = fileName.substr(GamePath.length());

	auto parts = split_last(gamePath); // Split into (path, pattern) pair

	auto item = walk_vfs(parts.first);

	if (item == nullptr || item->is_file())
		return TrueFindFirstFileW(lpFileName, lpFindFileData);

	// Basically we'll walk the vfs and match the files manually
	// TODO: Take in account game's folders in the search

	const auto pattern = parts.second.c_str();

	auto foundFiles = new std::vector<std::wstring>;

	auto folder_items = static_cast<vfs::vfs_folder*>(item)->get_contents();

	for (auto it = folder_items.begin(); it != folder_items.end(); ++it)
	{
		if (match_path(pattern, it->first.c_str()))
		{
			if (it->second->is_file())
				foundFiles->push_back(static_cast<vfs::vfs_file*>(it->second)->get_real_file());
			else
			{
				wchar_t path[MAX_PATH + 1];
				swprintf_s(path, MAX_PATH + 1, L"%s%s\\%s", GamePath.c_str(), parts.first.c_str(), it->first.c_str());
				const std::wstring dirPath = path;
				foundFiles->push_back(dirPath);
			}
		}
	}

	if (foundFiles->empty())
	{
		delete foundFiles;
		SetLastError(ERROR_FILE_NOT_FOUND);
		return INVALID_HANDLE_VALUE;
	}

	const auto handle = new char;
	ActiveSearchHandles[handle] = foundFiles;

	fill_info(foundFiles->back(), lpFindFileData);
	foundFiles->pop_back();

	return handle;
}

// Finds the next file for the handle
BOOL hook_FindNextFileW(HANDLE hFindFile, WIN32_FIND_DATAW* lpFindFileData)
{
	if (ActiveSearchHandles.find(hFindFile) == ActiveSearchHandles.end())
		return TrueFindNextFileW(hFindFile, lpFindFileData);

	auto list = ActiveSearchHandles[hFindFile];

	if (list->empty())
	{
		SetLastError(ERROR_NO_MORE_FILES);
		return FALSE;
	}

	fill_info(list->back(), lpFindFileData);
	list->pop_back();

	return TRUE;
}

// Gets file's extended attributes
BOOL hook_GetFileAttributesExW(LPCWSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation)
{
	auto fileName(CanonicalPath(lpFileName));

	if (fileName.compare(0, GamePath.length(), GamePath) != 0)
		return TrueGetFileAttributesExW(lpFileName, fInfoLevelId, lpFileInformation);

	const auto gamePath = fileName.substr(GamePath.length());

	auto item = walk_vfs(gamePath);

	if (item == nullptr)
		return TrueGetFileAttributesExW(lpFileName, fInfoLevelId, lpFileInformation);

	if (item->is_file())
		return TrueGetFileAttributesExW(static_cast<vfs::vfs_file*>(item)->get_real_file().c_str(), fInfoLevelId,
		                                lpFileInformation);

	if (fInfoLevelId == GetFileExInfoStandard)
	{
		// We're nice and fill everything for a folder
		auto data = static_cast<LPWIN32_FILE_ATTRIBUTE_DATA>(lpFileInformation);
		data->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
		data->ftCreationTime = dummyTime;
		data->ftLastAccessTime = dummyTime;
		data->ftLastWriteTime = dummyTime;
		data->nFileSizeHigh = 0;
		data->nFileSizeLow = 0;
	}

	return TRUE;
}

// Gets basic file attributes
DWORD hook_GetFileAttributesW(LPCWSTR lpFileName)
{
	auto fileName(CanonicalPath(lpFileName));

	if (fileName.compare(0, GamePath.length(), GamePath) != 0)
		return TrueGetFileAttributesW(lpFileName);

	const auto gamePath = fileName.substr(GamePath.length());

	auto item = walk_vfs(gamePath);

	if (item == nullptr)
		return TrueGetFileAttributesW(lpFileName);

	if (item->is_file())
		return TrueGetFileAttributesW(static_cast<vfs::vfs_file*>(item)->get_real_file().c_str());

	return FILE_ATTRIBUTE_DIRECTORY;
}

#define HOOK(function) \
		auto orig_##function = GetProcAddress(GetModuleHandle(L"kernel32"), #function); \
		MH_CreateHook(orig_##function, &hook_##function, reinterpret_cast<LPVOID*>(&True##function)); \
		MH_EnableHook(orig_##function);

extern "C" DllExport void init_vfs(wchar_t* vfss_json_path, wchar_t* game_path)
{
	GamePath = game_path;
	dummyTime.dwHighDateTime = 0;
	dummyTime.dwLowDateTime = 0;

	std::ifstream vfif(vfss_json_path);
	RootFolder.parse(vfif);

	MH_Initialize();

	HOOK(FindClose);
	HOOK(FindFirstFileW);
	HOOK(FindNextFileW);
	HOOK(GetFileAttributesExW);
	HOOK(GetFileAttributesW);
	HOOK(CreateFileW);
}

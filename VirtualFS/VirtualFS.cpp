// VirtualFS.cpp : Defines the exported functions for the DLL application.


#include "VirtualFS.h"
#include <fstream>
#include "vfs_data.h"

namespace fs = std::experimental::filesystem;

static vfs::vfs_folder RootFolder;
static std::wstring GamePath;
static fs::path TempFolderPath;
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
	// std::filesystem::canonical uses GetFullPathName that handles all path conversions (like "/" to "\")
	// More info: https://googleprojectzero.blogspot.com/2016/02/the-definitive-guide-on-win32-to-nt.html
	// This version should also handle long path names automagically
	return fs::canonical(input);
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

	// If the file does not exist, we might need to do some work
	if (item == nullptr)
	{
		// If file exists in the game's dir, open it
		if (TrueGetFileAttributesW(lpFileName) != INVALID_FILE_ATTRIBUTES)
			return ORIGINAL;

		// File does not exist in the game's folder. Now it's time to do work!

		if (dwCreationDisposition == OPEN_EXISTING || dwCreationDisposition == TRUNCATE_EXISTING)
		{
			SetLastError(ERROR_FILE_NOT_FOUND);
			return INVALID_HANDLE_VALUE;
		}

		vfs::vfs_object* p = &RootFolder;
		auto real_path = TempFolderPath;
		std::vector<std::wstring> path;
		std::wstring name;

		const auto parts = split_last(gamePath);
		if (parts.second != L"")
		{
			path = split(parts.first, L'\\');
			name = parts.second;
		}
		else
		{
			name = parts.first;
		}

		// Walk down the VFS, creating folders in TEMP as needed
		// We do not create new folders in the VFS here, partially because
		// we want to bail if we detect the folder might be game-related (e.g. saves folder).
		// This *should* minimize the amount of unnecessary redirection
		for (auto part = path.begin(); part != path.end(); ++part)
		{
			auto folder = static_cast<vfs::vfs_folder*>(p);
			auto folder_contents = folder->get_contents();
			const auto result = folder_contents.find(*part);

			// If we don't have the folder in the VFS, most likely it's the game's folder
			// Therefore we route the thing back to the game
			if (result == folder_contents.end())
				return ORIGINAL;

			p = &*result->second;

			if (p->is_file())
				return INVALID_HANDLE_VALUE;

			real_path /= *part;

			// Only create folders in the temp folder if they don't exist already
			if (!TrueCreateDirectoryW(real_path.c_str(), nullptr) && GetLastError() == ERROR_PATH_NOT_FOUND)
				return INVALID_HANDLE_VALUE;
		}

		// Finally, update VFS and physically create a file

		auto file_path = TempFolderPath / gamePath;

		auto new_file = new vfs::vfs_file(file_path);
		new_file->set_parent(p);
		static_cast<vfs::vfs_folder*>(p)->get_contents()[name] = new_file;

		return TrueCreateFileW(file_path.c_str(), dwDesiredAccess, dwShareMode, lpSecurityAttributes,
		                       dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
	}


	if (item->is_file())
		return TrueCreateFileW(static_cast<vfs::vfs_file*>(item)->get_real_file().c_str(), dwDesiredAccess, dwShareMode,
		                       lpSecurityAttributes,
		                       dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);

	return ORIGINAL;
}

BOOL hook_CreateDirectoryW(LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes)
{
	auto fileName(CanonicalPath(lpPathName));

	if (fileName.compare(0, GamePath.length(), GamePath) != 0)
		return TrueCreateDirectoryW(lpPathName, lpSecurityAttributes);

	const auto gamePath = fileName.substr(GamePath.length());

	const auto object = walk_vfs(gamePath);

	if (object != nullptr)
	{
		SetLastError(ERROR_ALREADY_EXISTS);
		return FALSE;
	}

	// Check if the folder already exists in the game's directory
	if (TrueGetFileAttributesW(lpPathName) != INVALID_FILE_ATTRIBUTES)
		return TrueCreateDirectoryW(lpPathName, lpSecurityAttributes);

	vfs::vfs_object* p = &RootFolder;
	auto real_path = TempFolderPath;
	std::vector<std::wstring> path;
	std::wstring name;

	const auto parts = split_last(gamePath);
	if (parts.second != L"")
	{
		path = split(parts.first, L'\\');
		name = parts.second;
	}
	else
	{
		name = parts.first;
	}

	// TODO: We might want to check that the parts.first/root path actually exists before continuing
	// This is mainly to keep behaviour consistent

	// Walk down the VFS, creating folders in TEMP as needed
	// We do not create new folders in the VFS here, partially because
	// we want to bail if we detect the folder might be game-related (e.g. saves folder).
	// This *should* minimize the amount of unnecessary redirection
	for (auto part = path.begin(); part != path.end(); ++part)
	{
		auto folder = static_cast<vfs::vfs_folder*>(p);
		auto folder_contents = folder->get_contents();
		const auto result = folder_contents.find(*part);

		if (result == folder_contents.end())
		{
			// Create the folder if it does not exist
			auto new_subfolder = new vfs::vfs_folder;
			new_subfolder->set_parent(p);
			folder_contents[*part] = new_subfolder;

			p = new_subfolder;
			//return FALSE;
		}
		else
			p = &*result->second;

		if (p->is_file())
			return FALSE;

		real_path /= *part;

		// Only create folders in the temp folder if they don't exist already
		if (!TrueCreateDirectoryW(real_path.c_str(), nullptr) && GetLastError() == ERROR_PATH_NOT_FOUND)
			return FALSE;
	}

	auto file_path = TempFolderPath / gamePath;
	auto new_folder = new vfs::vfs_folder;
	new_folder->set_parent(p);
	static_cast<vfs::vfs_folder*>(p)->get_contents()[name] = new_folder;

	return TrueCreateDirectoryW(file_path.c_str(), lpSecurityAttributes);
}

BOOL hook_DeleteFileW(LPCWSTR lpFileName)
{
	auto fileName(CanonicalPath(lpFileName));

	if (fileName.compare(0, GamePath.length(), GamePath) != 0)
		return TrueDeleteFileW(lpFileName);

	const auto gamePath = fileName.substr(GamePath.length());

	const auto item = walk_vfs(gamePath);

	if (item == nullptr || !item->is_file())
		return TrueDeleteFileW(lpFileName);

	const auto parent = item->get_parent();

	if (parent == nullptr)
		return TrueDeleteFileW(lpFileName);

	const auto parts = split_last(gamePath);
	std::wstring name;
	if (parts.second == L"")
		name = parts.first;
	else
		name = parts.second;

	static_cast<vfs::vfs_folder*>(parent)->get_contents().erase(name);
	delete item;

	auto file_path = TempFolderPath / gamePath;
	return TrueDeleteFileW(file_path.c_str());
}

BOOL hook_RemoveDirectoryW(LPCWSTR lpPathName)
{
	auto fileName(CanonicalPath(lpPathName));

	if (fileName.compare(0, GamePath.length(), GamePath) != 0)
		return TrueRemoveDirectoryW(lpPathName);

	const auto gamePath = fileName.substr(GamePath.length());

	const auto item = walk_vfs(gamePath);

	if (item == nullptr || !item->is_folder())
		return TrueRemoveDirectoryW(lpPathName);

	if (!static_cast<vfs::vfs_folder*>(item)->get_contents().empty())
		return FALSE;

	const auto parts = split_last(gamePath);
	std::wstring name;
	if (parts.second == L"")
		name = parts.first;
	else
		name = parts.second;

	const auto parent = item->get_parent();

	if (parent == nullptr)
		return TrueRemoveDirectoryW(lpPathName);

	auto folder = static_cast<vfs::vfs_folder*>(parent);

	folder->get_contents().erase(name);
	delete item;

	auto folder_path = TempFolderPath / gamePath;
	return TrueRemoveDirectoryW(folder_path.c_str());
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

extern "C" DllExport void init_vfs(wchar_t* vfs_path, wchar_t* game_path)
{
	GamePath = game_path;
	dummyTime.dwHighDateTime = 0;
	dummyTime.dwLowDateTime = 0;

	const fs::path vfs_root(vfs_path);
	const auto vfs_tree_file = vfs_root / L"vfs.json";
	TempFolderPath = vfs_root / L"__temp__";

	std::ifstream vfif(vfs_tree_file);
	RootFolder.parse(vfif);

	MH_Initialize();

	HOOK(FindClose);
	HOOK(FindFirstFileW);
	HOOK(FindNextFileW);
	HOOK(GetFileAttributesExW);
	HOOK(GetFileAttributesW);
	HOOK(CreateFileW);
	HOOK(DeleteFileW);
	HOOK(CreateDirectoryW);
	HOOK(RemoveDirectoryW);
}

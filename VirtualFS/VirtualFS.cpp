// VirtualFS.cpp : Defines the exported functions for the DLL application.


#include "VirtualFS.h"
#include <fstream>
#include "vfs_data.h"
#include "logging.h"
#include <optional>

namespace fs = std::experimental::filesystem;

static vfs::vfs_folder RootFolder;
static std::wstring GamePath;
static fs::path TempFolderPath;
static FILETIME DummyTime;
static std::map<HANDLE, std::vector<std::wstring>*> ActiveSearchHandles;

static bool ProxyEnabled = false;
static std::wstring ProxyCurrentDirectory;
static std::wstring RealCurrentDirectory;
static vfs::vfs_folder* CurrentRoot = &RootFolder;

inline bool starts_with(std::wstring const& str, std::wstring const& start)
{
	return str.length() >= start.length() &&
		CompareStringW(
			LOCALE_INVARIANT,
			NORM_IGNORECASE,
			str.c_str(),
			start.length(),
			start.c_str(),
			start.length()) == CSTR_EQUAL;
}

inline std::optional<std::wstring> get_main_path(std::wstring const& full_path)
{
	if (ProxyEnabled && starts_with(full_path, ProxyCurrentDirectory))
		return std::optional<std::wstring>{full_path.substr(ProxyCurrentDirectory.length())};
	if (starts_with(full_path, GamePath))
		return std::optional<std::wstring>{full_path.substr(GamePath.length())};
	return std::nullopt;
}


inline std::wstring get_original_path(std::wstring const& path_part)
{
	return GamePath + path_part;
}

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
		return {L"", s};
	return {s.substr(0, index), s.substr(index + 1)};
}

// Walk the VFS by the fiven path
// Return a pointer to a JSON key that is the found file/folder
vfs::vfs_object* walk_vfs(std::wstring const& path, vfs::vfs_object* root = CurrentRoot)
{
	auto parts = split(path, L'\\');

	auto p = root;

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
void fill_info(std::wstring const& file, WIN32_FIND_DATAW* lpFindFileData)
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

DWORD hook_GetCurrentDirectoryW(DWORD nBufferLength, LPWSTR buffer)
{
	LOG("GetCurrentDirectoryW");
	if (ProxyEnabled)
	{
		if (buffer == nullptr && nBufferLength == 0)
			return RealCurrentDirectory.length() + 1;

		if (nBufferLength < RealCurrentDirectory.length() + 1)
			return 0;

		wmemset(buffer, L'\0', RealCurrentDirectory.length() + 1);
		wmemcpy_s(buffer, nBufferLength, RealCurrentDirectory.c_str(), RealCurrentDirectory.length());
		LOG("    Returning " << N(buffer));
		return RealCurrentDirectory.length();
	}

	LOG("    Returning original!");
	return TrueGetCurrentDirectoryW(nBufferLength, buffer);
}

// Sets the current directory
BOOL hook_SetCurrentDirectoryW(LPCWSTR lpPathName)
{
	std::wstring path(lpPathName);
	std::replace(path.begin(), path.end(), L'/', L'\\'); // Do a hobo version of canonical_path

	if (path.back() != L'\\')
		path += L'\\';

	LOG("SetCurrentDirectoryW: " << N(path));

	if (starts_with(path, GamePath))
	{
		const auto gamePath = path.substr(GamePath.length());

		if (gamePath.empty())
		{
			ProxyEnabled = false;
			CurrentRoot = &RootFolder;
			LOG("    Disabled proxy because gamePath is empty!");
			return TrueSetCurrentDirectoryW(lpPathName);
		}

		const auto folder = walk_vfs(gamePath, &RootFolder);

		if (folder == nullptr || !folder->is_folder())
		{
			ProxyEnabled = false;
			CurrentRoot = &RootFolder;
			LOG("    Disabled proxy because failed to walk VFS!");
			return TrueSetCurrentDirectoryW(lpPathName);
		}

		ProxyEnabled = true;
		CurrentRoot = static_cast<vfs::vfs_folder*>(folder);
		RealCurrentDirectory = lpPathName;

		LOG("    Enabled directory proxy for " << N(RealCurrentDirectory));

		auto parts = split(gamePath, L'\\');

		auto p = TempFolderPath;
		for (auto& part : parts)
		{
			p /= part;
			TrueCreateDirectoryW(p.c_str(), nullptr);
		}

		ProxyCurrentDirectory = p / "\\";
		LOG("    Setting current dir to: " << N(ProxyCurrentDirectory));

		return TrueSetCurrentDirectoryW(p.c_str());
	}

	ProxyEnabled = false;
	CurrentRoot = &RootFolder;
	LOG("    Disabled proxy + calling original function!");

	return TrueSetCurrentDirectoryW(lpPathName);
}

// Called when a file handle is created
// Basically every time something (file/folder) is created, opened, read
HANDLE hook_CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                        LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                        DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
#define ORIGINAL TrueCreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile)
#define ORIGINAL_P TrueCreateFileW(get_original_path(game_path.value()).c_str(), dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile)

	const auto file_name = fs::canonical(lpFileName);

	auto game_path = get_main_path(file_name);

	if (!game_path)
		return ORIGINAL;

	LOG("CreateFileW: " << N(file_name));

	const auto item = walk_vfs(game_path.value());

	// If the file does not exist, we might need to do some work
	if (item == nullptr)
	{
		// If file exists in the game's dir, open it
		if (TrueGetFileAttributesW(lpFileName) != INVALID_FILE_ATTRIBUTES)
			return ORIGINAL_P;

		// File does not exist in the game's folder. Now it's time to do work!

		if (dwCreationDisposition == OPEN_EXISTING || dwCreationDisposition == TRUNCATE_EXISTING)
		{
			SetLastError(ERROR_FILE_NOT_FOUND);
			return INVALID_HANDLE_VALUE;
		}

		vfs::vfs_object* p = CurrentRoot;
		auto real_path = TempFolderPath;

		const auto parts = split_last(game_path.value());

		const auto path = split(parts.first, L'\\');
		const auto name = parts.second;

		// Walk down the VFS, creating folders in TEMP as needed
		// We do not create new folders in the VFS here, partially because
		// we want to bail if we detect the folder might be game-related (e.g. saves folder).
		// This *should* minimize the amount of unnecessary redirection
		for (auto& part : path)
		{
			auto folder = static_cast<vfs::vfs_folder*>(p);
			auto folder_contents = folder->get_contents();
			const auto result = folder_contents.find(part);

			// If we don't have the folder in the VFS, most likely it's the game's folder
			// Therefore we route the thing back to the game
			if (result == folder_contents.end())
				return ORIGINAL_P;

			p = &*result->second;

			if (p->is_file())
				return INVALID_HANDLE_VALUE;

			real_path /= part;

			// Only create folders in the temp folder if they don't exist already
			if (!TrueCreateDirectoryW(real_path.c_str(), nullptr) && GetLastError() == ERROR_PATH_NOT_FOUND)
				return INVALID_HANDLE_VALUE;
		}

		// Finally, update VFS and physically create a file

		auto file_path = TempFolderPath / game_path.value();

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

	return ORIGINAL_P;
}

BOOL hook_CreateDirectoryW(LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes)
{
	const auto file_name = fs::canonical(lpPathName);
	const auto game_path = get_main_path(file_name);

	if (!game_path)
		return TrueCreateDirectoryW(lpPathName, lpSecurityAttributes);

	LOG("CreateDirectoryW: " << N(file_name));

	const auto object = walk_vfs(game_path.value());

	if (object != nullptr)
	{
		SetLastError(ERROR_ALREADY_EXISTS);
		return FALSE;
	}

	auto original_path = get_original_path(game_path.value());

	// Check if the folder already exists in the game's directory
	if (TrueGetFileAttributesW(original_path.c_str()) != INVALID_FILE_ATTRIBUTES)
		return TrueCreateDirectoryW(original_path.c_str(), lpSecurityAttributes);

	vfs::vfs_object* p = &RootFolder;
	auto real_path = TempFolderPath;

	const auto parts = split_last(game_path.value());
	const auto path = split(parts.first, L'\\');
	const auto name = parts.second;

	// TODO: We might want to check that the parts.first/root path actually exists before continuing
	// This is mainly to keep behaviour consistent

	// Walk down the VFS, creating folders in TEMP as needed
	// We do not create new folders in the VFS here, partially because
	// we want to bail if we detect the folder might be game-related (e.g. saves folder).
	// This *should* minimize the amount of unnecessary redirection
	for (auto& part : path)
	{
		auto folder = static_cast<vfs::vfs_folder*>(p);
		auto folder_contents = folder->get_contents();
		const auto result = folder_contents.find(part);

		if (result == folder_contents.end())
		{
			// Create the folder if it does not exist
			auto new_subfolder = new vfs::vfs_folder;
			new_subfolder->set_parent(p);
			folder_contents[part] = new_subfolder;

			p = new_subfolder;
			//return FALSE;
		}
		else
			p = &*result->second;

		if (p->is_file())
			return FALSE;

		real_path /= part;

		// Only create folders in the temp folder if they don't exist already
		if (!TrueCreateDirectoryW(real_path.c_str(), nullptr) && GetLastError() == ERROR_PATH_NOT_FOUND)
			return FALSE;
	}

	auto file_path = TempFolderPath / game_path.value();
	auto new_folder = new vfs::vfs_folder;
	new_folder->set_parent(p);
	static_cast<vfs::vfs_folder*>(p)->get_contents()[name] = new_folder;

	return TrueCreateDirectoryW(file_path.c_str(), lpSecurityAttributes);
}

BOOL hook_DeleteFileW(LPCWSTR lpFileName)
{
	const auto file_name = fs::canonical(lpFileName);

	const auto game_path = get_main_path(file_name);

	if (!game_path)
		return TrueDeleteFileW(lpFileName);

	LOG("DeleteFileW: " << N(file_name));

	const auto item = walk_vfs(game_path.value());

	if (item == nullptr || !item->is_file())
		return TrueDeleteFileW(get_original_path(game_path.value()).c_str());

	const auto parent = item->get_parent();

	if (parent == nullptr)
		return TrueDeleteFileW(get_original_path(game_path.value()).c_str());

	const auto parts = split_last(game_path.value());
	const auto name = parts.second;

	static_cast<vfs::vfs_folder*>(parent)->get_contents().erase(name);
	delete item;

	auto file_path = TempFolderPath / game_path.value();
	return TrueDeleteFileW(file_path.c_str());
}

BOOL hook_RemoveDirectoryW(LPCWSTR lpPathName)
{
	const auto file_name = fs::canonical(lpPathName);

	const auto game_path = get_main_path(file_name);

	if (!game_path)
		return TrueRemoveDirectoryW(lpPathName);

	LOG("RemoveDirectoryW: " << N(file_name));

	const auto item = walk_vfs(game_path.value());

	if (item == nullptr || !item->is_folder())
		return TrueRemoveDirectoryW(get_original_path(game_path.value()).c_str());

	if (!static_cast<vfs::vfs_folder*>(item)->get_contents().empty())
		return FALSE;

	const auto [_, name] = split_last(game_path.value());
	const auto parent = item->get_parent();

	if (parent == nullptr)
		return TrueRemoveDirectoryW(get_original_path(game_path.value()).c_str());

	auto folder = static_cast<vfs::vfs_folder*>(parent);

	folder->get_contents().erase(name);
	delete item;

	auto folder_path = TempFolderPath / game_path.value();
	return TrueRemoveDirectoryW(folder_path.c_str());
}

HANDLE create_search_handle(std::wstring const& game_path, WIN32_FIND_DATAW* find_file_data)
{
	auto [path, pattern] = split_last(game_path); // Split into (path, pattern) pair

	const auto item = walk_vfs(path);

	if (item == nullptr || item->is_file())
		return nullptr;

	// Basically we'll walk the vfs and match the files manually
	// TODO: Take in account game's folders in the search

	auto found_files = new std::vector<std::wstring>;

	auto folder_items = static_cast<vfs::vfs_folder*>(item)->get_contents();

	for (auto& [name, object] : folder_items)
	{
		if (match_path(pattern.c_str(), name.c_str()))
		{
			if (object->is_file())
			{
				LOG("    " << N(static_cast<vfs::vfs_file*>(object)->get_real_file()));
				found_files->push_back(static_cast<vfs::vfs_file*>(object)->get_real_file());
			}
			else
			{
				fs::path dir = get_original_path(path);
				dir /= name;
				LOG("    " << N(dir));
				found_files->push_back(dir);
			}
		}
	}

	if (found_files->empty())
	{
		delete found_files;
		SetLastError(ERROR_FILE_NOT_FOUND);
		return INVALID_HANDLE_VALUE;
	}

	const auto handle = new char;
	ActiveSearchHandles[handle] = found_files;

	fill_info(found_files->back(), find_file_data);
	found_files->pop_back();

	return handle;
}

// Finds the first file based on the pattern and returns a file search handle to find more files
HANDLE hook_FindFirstFileW(LPCWSTR lpFileName, WIN32_FIND_DATAW* lpFindFileData)
{
	const auto file_name = fs::canonical(lpFileName);

	const auto game_path = get_main_path(file_name);

	if (!game_path)
		return TrueFindFirstFileW(lpFileName, lpFindFileData);

	LOG("FindFirstFileW: " << N(file_name));

	const auto handle = create_search_handle(game_path.value(), lpFindFileData);
	if (handle != nullptr)
		return handle;

	return TrueFindFirstFileW(get_original_path(game_path.value()).c_str(), lpFindFileData);
}

HANDLE hook_FindFirstFileExW(LPCWSTR lpFileName, FINDEX_INFO_LEVELS fInfoLevelId, LPVOID lpFindFileData,
                             FINDEX_SEARCH_OPS fSearchOp, LPVOID lpSearchFilter, DWORD dwAdditionalFlags)
{
	const auto file_name = fs::canonical(lpFileName);

	const auto game_path = get_main_path(file_name);

	if (!game_path)
		return TrueFindFirstFileExW(lpFileName, fInfoLevelId, lpFindFileData, fSearchOp, lpSearchFilter, dwAdditionalFlags);

	LOG("FindFirstFileExW: " << N(file_name));

	const auto handle = create_search_handle(game_path.value(), static_cast<WIN32_FIND_DATAW*>(lpFindFileData));
	if (handle != nullptr)
		return handle;

	return TrueFindFirstFileExW(get_original_path(game_path.value()).c_str(), fInfoLevelId, lpFindFileData, fSearchOp,
	                            lpSearchFilter, dwAdditionalFlags);
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

// Gets file's extended attributes
BOOL hook_GetFileAttributesExW(LPCWSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation)
{
	const auto file_name = fs::canonical(lpFileName);

	const auto game_path = get_main_path(file_name);

	if (!game_path)
		return TrueGetFileAttributesExW(lpFileName, fInfoLevelId, lpFileInformation);

	LOG("GetFileAttributesExW: " << N(file_name));

	const auto item = walk_vfs(game_path.value());

	if (item == nullptr)
		return TrueGetFileAttributesExW(get_original_path(game_path.value()).c_str(), fInfoLevelId, lpFileInformation);

	if (item->is_file())
		return TrueGetFileAttributesExW(static_cast<vfs::vfs_file*>(item)->get_real_file().c_str(), fInfoLevelId,
		                                lpFileInformation);

	if (fInfoLevelId == GetFileExInfoStandard)
	{
		// We're nice and fill everything for a folder
		auto data = static_cast<LPWIN32_FILE_ATTRIBUTE_DATA>(lpFileInformation);
		data->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
		data->ftCreationTime = DummyTime;
		data->ftLastAccessTime = DummyTime;
		data->ftLastWriteTime = DummyTime;
		data->nFileSizeHigh = 0;
		data->nFileSizeLow = 0;
	}

	return TRUE;
}

// Gets basic file attributes
DWORD hook_GetFileAttributesW(LPCWSTR lpFileName)
{
	const auto file_name = fs::canonical(lpFileName);

	const auto game_path = get_main_path(file_name);

	if (!game_path)
		return TrueGetFileAttributesW(lpFileName);

	LOG("GetFileAttributesW: " << N(file_name));

	const auto item = walk_vfs(game_path.value());

	if (item == nullptr)
		return TrueGetFileAttributesW(get_original_path(game_path.value()).c_str());

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
	DummyTime.dwHighDateTime = 0;
	DummyTime.dwLowDateTime = 0;

	const fs::path vfs_root(vfs_path);
	const auto vfs_tree_file = vfs_root / L"vfs.json";
	TempFolderPath = vfs_root / L"__temp__";

	std::ifstream vfif(vfs_tree_file);
	RootFolder.parse(vfif);

	init_log(vfs_root);

	MH_Initialize();

	HOOK(GetCurrentDirectoryW);
	HOOK(SetCurrentDirectoryW);
	HOOK(FindFirstFileW);
	HOOK(FindFirstFileExW);
	HOOK(FindNextFileW);
	HOOK(FindClose);
	HOOK(GetFileAttributesExW);
	HOOK(GetFileAttributesW);
	HOOK(CreateFileW);
	HOOK(DeleteFileW);
	HOOK(CreateDirectoryW);
	HOOK(RemoveDirectoryW);
}

#pragma once

#include <windows.h>
#include <fstream>
#include <experimental/filesystem>


#ifdef _DEBUG

static std::ofstream LogStream;

#define LOG(msg) LogStream << msg << std::endl
#define N(str) narrow(str)

inline void init_log(std::experimental::filesystem::path const& vfs_root)
{
	const auto log_file = vfs_root / L"vfs_log.log";
	LogStream.open(log_file, std::ios_base::out | std::ios_base::binary);
}

static std::string narrow(std::wstring const& str)
{
	const auto char_len = WideCharToMultiByte(CP_UTF8, 0, str.c_str(), str.length(), nullptr, 0, nullptr, nullptr);

	if (char_len == 0)
		return "";

	std::string res;
	res.resize(char_len);
	WideCharToMultiByte(CP_UTF8, 0, str.c_str(), str.size(), const_cast<char*>(res.c_str()), char_len, nullptr, nullptr);
	return res;
}

#else

#define LOG(msg)
#define N(str) ""

inline void init_log(std::experimental::filesystem::path const&) { }

#endif

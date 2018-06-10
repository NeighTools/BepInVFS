#pragma once

#include <windows.h>
#include <fstream>
#include <experimental/filesystem>
#include "wideutils.h"


#ifdef _DEBUG

static std::ofstream LogStream;

#define LOG(msg) LogStream << msg << std::endl
#define N(str) narrow(str)

inline void init_log(std::experimental::filesystem::path const& vfs_root)
{
	const auto log_file = vfs_root / L"vfs_log.log";
	LogStream.open(log_file, std::ios_base::out | std::ios_base::binary);
}

#else

#define LOG(msg)
#define N(str) ""

inline void init_log(std::experimental::filesystem::path const&) { }

#endif

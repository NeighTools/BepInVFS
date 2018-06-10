#pragma once
#include <windows.h>
#include <string>

static std::string narrow(std::wstring const& str)
{
	const auto char_len = WideCharToMultiByte(CP_UTF8, 0, str.c_str(), str.length(), nullptr, 0, nullptr, nullptr);

	if (char_len == 0)
		return "";

	std::string result;
	result.resize(char_len);
	WideCharToMultiByte(CP_UTF8, 0, str.c_str(), str.size(), const_cast<char*>(result.c_str()), char_len, nullptr, nullptr);
	return result;
}

static std::wstring widen(std::string const& str)
{
	const auto char_len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), str.length(), nullptr, 0);

	if (char_len == 0)
		return L"";

	std::wstring result;
	result.resize(char_len);
	MultiByteToWideChar(CP_UTF8, 0, str.c_str(), str.length(), const_cast<wchar_t*>(result.c_str()), char_len);
	return result;
}
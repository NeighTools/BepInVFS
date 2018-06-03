#pragma once

#include "stdafx.h"
#include <map>
#include <fstream>
#include <sstream>

namespace vfs
{
	namespace details
	{
		inline bool skip_until(std::wistream& stream, wchar_t ch)
		{
			while (!stream.eof())
			{
				if (stream.get() == ch)
					return true;
			}
			return false;
		}

		inline bool skip_until_block(std::wistream& stream, wchar_t ch, wchar_t block_end)
		{
			while (!stream.eof())
			{
				const wchar_t c = stream.get();
				if (c == ch || c == block_end)
				{
					if (c == block_end)
						stream.unget();
					return true;
				}
			}
			return false;
		}

		inline std::wstring read_until(std::wistream& stream, wchar_t ch)
		{
			std::wstringbuf buf;
			bool is_escape = false;
			wchar_t c;
			while (!stream.eof() && (c = stream.get()) != ch)
			{
				if (c == L'\\' && !is_escape)
				{
					is_escape = true;
					continue;
				}

				if (is_escape)
				{
					is_escape = false;
					switch (c)
					{
					case L'\\':
						c = L'\\';
						break;
					case L'\"':
						c = L'\"';
						break;
					case L't':
						c = L'\t';
						break;
					case L'n':
						c = L'\n';
						break;
					case L'r':
						c = L'\r';
						break;
					default:
						continue;
					}
				}

				buf.sputc(c);
			}
			return buf.str();
		}

		inline bool skip_whitespace(std::wistream& stream)
		{
			while (!stream.eof())
			{
				if (iswspace(stream.peek()))
					stream.get();
				else
					return true;
			}
			return false;
		}

		struct wstr_comp_ci
		{
			bool operator()(const std::wstring& s1, const std::wstring& s2) const
			{
				return CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, s1.c_str(), -1, s2.c_str(), -1) == CSTR_LESS_THAN;
			}
		};
	}

	// A helper to easily distiguish a type of VFS object
	// We use an enum because we only have two VFS objects (file and folder)
	enum VFSObjectType
	{
		None,
		Folder,
		File
	};

	// A common VFS object
	class vfs_object
	{
	public:
		virtual ~vfs_object()
		{
			parent = nullptr;
		}

		bool is_folder() const
		{
			return type_ == Folder;
		}

		bool is_file() const
		{
			return type_ == File;
		}

	protected:
		VFSObjectType type_ = None;

		vfs_object* parent = nullptr;
	};

	// A VFS file. Contains a path to the original file.
	class vfs_file : public vfs_object
	{
	public:
		vfs_file(const std::wstring str)
		{
			type_ = File;
			original_file = str;
		}

		std::wstring& get_real_file()
		{
			return original_file;
		}

	private:
		std::wstring original_file;
	};

	// VFS Folder. Contains a case-insensitive map of VFS objects
	class vfs_folder : public vfs_object
	{
	public:
		using folder_t = std::map<std::wstring, vfs_object*, details::wstr_comp_ci>;

		vfs_folder()
		{
			type_ = Folder;
		}

		virtual ~vfs_folder()
		{
			for (auto v : contents_)
			{
				delete v.second;
			}
			contents_.clear();
		}

		folder_t& get_contents()
		{
			return contents_;
		}

		// Parses the provided stream into a VFS folder
		// We use a simlified FSM with the help of gotos (I know, shame on me; didn't bother with proper states)
		void parse(std::wistream& stream)
		{
			vfs_folder* new_folder;
			vfs_file* new_file;
			auto folder = this;

			while (true)
			{
			folder_start:
				if (!details::skip_until(stream, L'{'))
					return;

			parse_folder:
				while (!stream.eof() && stream.peek() != L'}')
				{
					if (!details::skip_until(stream, L'"'))
						return;

					const auto key = details::read_until(stream, L'"');

					if (!details::skip_until(stream, L':') || !details::skip_whitespace(stream))
						return;

					switch (stream.peek())
					{
					case L'{':
						new_folder = new vfs_folder;
						folder->contents_[key] = new_folder;
						new_folder->parent = folder;
						folder = new_folder;
						goto folder_start;
					case L'"':
						new_file = new vfs_file(details::read_until(stream, L'"'));
						folder->contents_[key] = new_file;
						break;
					default:
						return;
					}

					if (!details::skip_until_block(stream, L',', L'}') || !details::skip_whitespace(stream))
						return;
				}

				if (!stream.eof())
					stream.get();

				if (folder->parent != nullptr)
				{
					folder = dynamic_cast<vfs_folder*>(folder->parent);

					if (!details::skip_until_block(stream, L',', L'}') || !details::skip_whitespace(stream))
						return;

					goto parse_folder;
				}
				return;
			}
		}

	private:
		folder_t contents_;
	};
}

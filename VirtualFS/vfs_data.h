/*
 * vfs_data.h -- Simplified JSON parser optimized for VFS trees.
 * 
 * This is a very basic and barebones implementation of JSON parser for use with VFS.
 * 
 * The parser can only handle objects and string values (which is enough for our case anyway).
 * No caching, everything is parsed into memory at once.
 * 
 * The JSON parser reads a normal UTF-8 file (as a ifstream, not wifstream) and converts all char strings
 * to wchar strings via codecvt.
 */

#pragma once

#include <map>
#include <sstream>

namespace vfs
{
	namespace details
	{
		inline bool skip_until(std::istream& stream, char ch)
		{
			while (!stream.eof())
			{
				if (stream.get() == ch)
					return true;
			}
			return false;
		}

		inline bool skip_until_block(std::istream& stream, char ch, char block_end)
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

		// Use codecvt to convert UTF8 to UTF16.
		// We have to that in order to both work with W-functions and have valid encoding of
		// Japanese characters (especially when they are handled on the managed side).
		static std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t> converterX;
		inline std::wstring AStringToWString(const std::string& str)
		{
			return converterX.from_bytes(str);
		}

		inline std::wstring read_until(std::istream& stream, char ch)
		{
			std::stringbuf buf;
			bool is_escape = false;

			while (!stream.eof())
			{

				char c = stream.get();
				if (c == ch && !is_escape)
					break;

				if (c == '\\' && !is_escape)
				{
					is_escape = true;
					continue;
				}

				if (is_escape)
				{
					is_escape = false;
					switch (c)
					{
					case '\\':
						c = '\\';
						break;
					case '\"':
						c = '\"';
						break;
					case 't':
						c = '\t';
						break;
					case 'n':
						c = '\n';
						break;
					case 'r':
						c = '\r';
						break;
					default:
						continue;
					}
				}

				buf.sputc(c);
			}
			return AStringToWString(buf.str());
		}

		inline bool skip_whitespace(std::istream& stream)
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
		void parse(std::istream& stream)
		{
			vfs_folder* new_folder;
			vfs_file* new_file;
			auto folder = this;

			while (true)
			{
			folder_start:
				if (!details::skip_until(stream, '{'))
					return;

			parse_folder:
				while (!stream.eof() && stream.peek() != '}')
				{
					if (!details::skip_until(stream, '"'))
						return;

					const auto key = details::read_until(stream, '"');

					if (!details::skip_until(stream, ':') || !details::skip_whitespace(stream))
						return;

					switch (stream.peek())
					{
					case '{':
						new_folder = new vfs_folder;
						folder->contents_[key] = new_folder;
						new_folder->parent = folder;
						folder = new_folder;
						goto folder_start;
					case '"':
						stream.get();
						new_file = new vfs_file(details::read_until(stream, '"'));
						folder->contents_[key] = new_file;
						break;
					default:
						return;
					}

					if (!details::skip_until_block(stream, ',', '}') || !details::skip_whitespace(stream))
						return;
				}

				if (!stream.eof())
					stream.get();

				if (folder->parent != nullptr)
				{
					folder = dynamic_cast<vfs_folder*>(folder->parent);

					if (!details::skip_until_block(stream, ',', '}') || !details::skip_whitespace(stream))
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

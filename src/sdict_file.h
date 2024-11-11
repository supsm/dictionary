#ifndef FILE_IO_H
#define FILE_IO_H

#include <algorithm>
#include <array>
#include <cassert>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// file containing dictionary info (words and definitions)
// magic bytes: SDICT[0x01][0x00] or 53 44 49 43 54 01 00 in ASCII
// 0x01 is the current file version number
// File format:
// [Magic Bytes]
// reserved_words words_sect_size
// num_words
// (inds section)
// WInd WInd WInd WInd ... (reserved_words in total, only first num_words have a useful value; unsigned 32-bit (4-byte LE) integer offset after word section)
// DInd DInd DInd DInd ... (reserved_words in total, only first num_words have a useful value; unsigned 32-bit (4-byte LE) integer offset after defs section)
// note that these indices start at 1. 0 is used to denote "no index"
// (words section)
//     word word word word ... (num_words in total, null terminated; occupies words_size bytes)
// (defs section)
//     def def def def ... (num_words in total; each def contains a unsigned 32-bit (4-byte LE) integer as size and unsigned 64-bit (8-byte LE) int as hash)
class dictionary_file
{
private:
	constexpr static std::uint32_t init_reserved_words = 32;
	constexpr static std::uint32_t init_words_sect_size = 256;

	// batch size for batched def reads
	constexpr static std::size_t batch_size = 4096;

	// convert string literal to array, removing the null delimiter
	constexpr static auto strlit_to_array = []<std::size_t N>(const char(&a)[N])
		{ std::array<char, N - 1> arr; std::copy_n(a, N - 1, arr.begin()); return arr; };

	constexpr static std::array magic_bytes = strlit_to_array("SDICT\x01\x00");

	std::string filename;
	// main file object. all public member functions except close() and add_word<false>()
	// will leave it as read only (add_word without flush will leave it as read/write)
	std::fstream file;
	enum class open_type { no_file, none, read, write, read_write };
	open_type file_open_type = open_type::no_file;

	std::uint32_t reserved_words, words_sect_size;
	struct word_info
	{
		std::string word;
		std::uint32_t def_ind;
		constexpr std::strong_ordering operator<=>(const word_info& other) const
			{ return word <=> other.word; }
		constexpr bool operator==(const word_info& other) const
			{ return word == other.word; }
		constexpr friend std::strong_ordering operator<=>(const word_info& lhs, std::string_view rhs)
			{ return lhs.word <=> rhs; }
		constexpr friend bool operator==(const word_info& lhs, std::string_view rhs)
			{ return lhs.word == rhs; }
		constexpr friend std::strong_ordering operator<=>(std::string_view lhs, const word_info& rhs)
			{ return lhs <=> rhs.word; }
		constexpr friend bool operator==(std::string_view lhs, const word_info& rhs)
			{ return lhs == rhs.word; }
	};
	// sorted
	// uint32_t is offset from the start of the defs section
	// (i.e. starts from 0, despite indices starting from 1 on disk)
	std::vector<word_info> words;
	std::size_t first_new_word = -1;
	
	// map of def size and hash to inds
	std::unordered_map<std::uint32_t, std::unordered_map<std::uint64_t, std::vector<std::uint32_t>>> existing_defs;
	bool do_dedup = true;

public:
	// true if a file was created on construction, false if it was read from
	bool created_file = false;
	
	// creates a dictionary_file object with no associated file
	// open(string_view) must be called before anything else
	dictionary_file() {}

	// @param create_if_not_exists  whether to create a new file if an existing one is not found
	// @param deduplicate  whether to enable deduplication
	// @param check_defs  whether to verify definition hashes (expensive)
	// @throws std::runtime_error  on file i/o error or if parsing receives an unexpected value
	dictionary_file(std::string_view filename_, bool create_if_not_exists = true, bool deduplicate = true, bool check_defs = true)
	{
		open(filename_, create_if_not_exists, deduplicate, check_defs);
	}

	~dictionary_file()
	{
		if (first_new_word != -1)
		{
			try
				{ flush(); }
			catch (...) {}
		}
	}
	
	// associate given filename with this object and open as input (reading contents or creating if not exists)
	// TODO: update complexity
	// Complexity: O(reserved_words + total_words_len + N*log(N)), where N is number of words
	// File Access: Read, magic_bytes.size() + 8 + reserved_words * 8 + total_words_len
	// @throws std::runtime_error  on file i/o or parsing error
	void open(std::string_view filename_, bool create_if_not_exists = true, bool deduplicate = true, bool check_defs = true)
	{
		filename = filename_;
		file_open_type = open_type::none;
		do_dedup = deduplicate;
		
		if (!std::filesystem::is_regular_file(filename))
		{
			if (std::filesystem::exists(filename))
				{ throw std::runtime_error(std::string(filename) + " exists but is not a regular file"); }
			if (!create_if_not_exists)
				{ throw std::runtime_error(std::string(filename) + " does not exist, not creating"); }
			
			create_file();
			created_file = true;
		}
		else
		{
			open();
			if (deduplicate || check_defs)
			{
				for (const auto& [word, def_ind] : words)
				{
					auto [size, hash] = get_def_size_and_hash(def_ind);
					assert(size != 0);
					if (deduplicate)
						{ existing_defs[size][hash].push_back(def_ind); } // keep old def_ind value if exists
					if (check_defs)
					{
						if (hash != hash_existing_def(def_ind, size))
							{ throw std::runtime_error("Definition hash does not match. File may be corrupted"); }
					}
				}
			}
			
			created_file = false;
		}
	}

	// open file as input and read contents
	// Complexity: O(reserved_words + total_words_len + N*log(N)), where N is number of words
	// File Access: Read, magic_bytes.size() + 8 + reserved_words * 8 + total_words_len
	// @throws std::runtime_error  on file i/o or parsing error
	// @throws std::logic_error  if there is no associated file
	void open()
	{
		if (file_open_type == open_type::no_file)
			{ throw std::logic_error("No associated file. Call open(string_view) first"); }
		open_in();
		read_file();
	}

	// Complexity: O(1)
	// @throws std::logic_error  if there is no associated file
	void close()
	{
		if (file_open_type == open_type::no_file)
			{ throw std::logic_error("No associated file. Call open(string_view) first"); }
		if (file.is_open())
			{ file.close(); }
		file_open_type = open_type::none;
	}
	
	// flush words & indices
	// expects defs to already be written
	// If no rewrite is necessary (word inds and words fit in their respective sections):
	//   Complexity: O(N*log(N)), where N is number of words
	//   File Acess: Write, total_new_words_len + n_new_words * 4
	// Otherwise:
	//   Complexity: O(n_words + total_words_len + total_defs_size)
	//   File Access: Create; Read, reserved_words * 8 + words_sect_size + total_defs_size bytes;
	//       Write, new_reserved_words * 8 + new_words_sect_size + total_defs_size bytes; Rename; Delete
	//         where new_reserved_words and new_words_sect_size are the nearest power-of-two-multiple of
	//         reserved_words and words_sect_size greater than n_words and total_words_len, respectively
	// @throws std::runtime_error  on file i/o or parsing error
	// @throws std::logic_error  if there is no associated file
	// @return whether file was modified
	bool flush()
	{
		if (file_open_type == open_type::no_file)
			{ throw std::logic_error("No associated file. Call open(string_view) first"); }
		if (first_new_word == -1)
			{ open_in(); return false; }

		open_in_out();
		
		static constexpr auto reduce_fn =
			[](std::size_t init, const word_info& elem) -> std::size_t
				{ return init + elem.word.size() + 1; }; // +1 for null character
		std::size_t cur_words_total_len = std::reduce(words.begin(), words.begin() + first_new_word, std::size_t(0), reduce_fn); // TODO: replace with uz following support
		std::size_t words_total_len = std::reduce(words.begin() + first_new_word, words.end(), cur_words_total_len, reduce_fn);
		auto old_words_sect_size = words_sect_size;
		while (words_sect_size < words_total_len)
			{ words_sect_size *= 2; }
		if (words_sect_size != old_words_sect_size || reserved_words < words.size())
		{
			sort_words();
			const auto old_reserved_words = reserved_words;
			while (reserved_words < words.size())
				{ reserved_words *= 2; }
			rewrite_file(old_reserved_words, old_words_sect_size);
			return true;
		}

		std::vector<std::streamoff> inds;
		inds.resize(words.size() - first_new_word);

		// write num words
		file.seekp(inds_section_offset() - 4, std::ios::beg);
		write_uint32_LE(words.size());

		// write new words
		{
			file.seekp(words_section_offset() + cur_words_total_len, std::ios::beg);
			std::size_t bytes_written = 0;
			for (std::size_t i = first_new_word; i < words.size(); i++)
			{
				inds[i - first_new_word] = cur_words_total_len + bytes_written;
				const auto& word = words[i].word;
				file.write(word.c_str(), word.size() + 1); // write the null as well
				bytes_written += word.size() + 1;
			}
		}

		// write word inds
		file.seekp(inds_section_offset() + first_new_word * 4, std::ios::beg);
		for (const auto i : inds)
			{ write_uint32_LE(i + 1); }

		// write def inds
		file.seekp(inds_section_offset() + (reserved_words + first_new_word) * 4, std::ios::beg);
		for (const auto [word, def_ind] : words | std::views::drop(first_new_word))
			{ write_uint32_LE(def_ind + 1); }

		sort_words();
		
		// file will be flushed when closed and reopened
		open_in();
		return true;
	}
	
	// TODO: something to add a stream of data (with part of definition added at a time)
	// TODO: override def instead of ignoring if word exists
	// If flush_words:
	//   Complexity equals that of flush(), File Access equals that of flush() plus def_len
	// Otherwise:
	//   Complexity: O(def_len)
	//   File Access: Write, def_len bytes
	// @tparam flush_words  whether to flush and update words (defs will always be written).
	//     if false, contains(), num_words(), get_def(), and add_word<_, false>() will be slow, until flush() or add_word<true>() is called
	//     repeatedly calling add_word<false, false> will result in minor slowdowns from dup checking, consider using add_word<false, true> instead
	// @tparam skip_dup_check  whether to skip checking for duplicates. note that flushing is significantly slower than duplicate checking
	// @throws std::runtime_error  on file i/o or parsing error
	// @throws std::logic_error  if there is no associated file
	// @return whether the word/def was successfully inserted
	template<bool flush_words = true, bool skip_dup_check = false>
	bool add_word(std::string_view word, std::span<const std::byte> def)
	{
		if (file_open_type == open_type::no_file)
			{ throw std::logic_error("No associated file. Call open(string_view) first"); }
		if constexpr (!skip_dup_check)
		{
			if (find_def_ind(word) != -1)
				{ return false; }
		}

		if (first_new_word == -1)
			{ first_new_word = words.size(); }
		
		if (auto def_ind = (do_dedup ? get_existing_def_ind(def) : std::nullopt))
		{
			words.emplace_back(std::string(word), def_ind.value());
		}
		else
		{
			open_in_out();
			
			// add def to words
			file.seekg(0, std::ios::end);
			std::streamoff cur_def_offset = file.tellg();
			assert(cur_def_offset >= defs_section_offset());
			cur_def_offset -= defs_section_offset();
			if (cur_def_offset < 0)
				{ throw std::runtime_error("Incorrect file size (too small)"); }
			words.emplace_back(std::string(word), cur_def_offset);
			
			auto hash = fnv1a(def);

			if (do_dedup)
			{
				// add def to existing_defs
				existing_defs[def.size()][hash].push_back(cur_def_offset);
			}
			
			// add def to file
			write_uint32_LE(def.size());
			write_uint64_LE(hash);
			file.write(reinterpret_cast<const char*>(def.data()), def.size());
		}
		
		if constexpr (flush_words)
			{ flush(); }
		return true;
	}
	template<bool flush_words = true, bool skip_dup_check = false>
	bool add_word(std::string_view word, std::span<const char> def) { return add_word<flush_words, skip_dup_check>(word, std::as_bytes(def)); }
	
	// Complexity: O(log(n_words))
	// File Access: No
	bool contains(std::string_view word) const
	{
		std::uint32_t ind = find_def_ind(word);
		return (ind != -1);
	}

	// Complexity: O(1)
	// File Access: no
	std::size_t num_words() const noexcept
	{
		return words.size();
	}

	// Complexity: O(def_size)
	// File Access: Read, def_size + 4 bytes
	// @throws std::runtime_error  on file i/o error
	std::optional<std::vector<char>> find(std::string_view word, bool check_def = false)
	{
		std::uint32_t ind = find_def_ind(word);
		if (ind == -1)
			{ return {}; }
		return read_def_whole(ind, check_def);
	}

private:
	// oper file as input
	// leaves file as read only
	void open_in()
	{
		if (file_open_type == open_type::read)
			{ return; }
		if (file.is_open())
			{ file.close(); }
		file.open(filename, std::ios::in | std::ios::binary);
		check_file();
		file_open_type = open_type::read;
	}

	// open file as output
	// FILE CONTENTS WILL BE CLEARED IF IT EXISTS
	// leaves file as write only
	void open_out()
	{
		if (file.is_open())
			{ file.close(); }
		file.open(filename, std::ios::out | std::ios::binary);
		check_file();
		file_open_type = open_type::write;
	}

	// open file as input and output
	// file will not be created if it doesn't exist
	// allows for overwriting output instead of having it deleted
	// leaves file as read + write
	void open_in_out()
	{
		if (file_open_type == open_type::read_write)
			{ return; }
		if (file.is_open())
			{ file.close(); }
		file.open(filename, std::ios::in | std::ios::out | std::ios::binary);
		check_file();
		file_open_type = open_type::read_write;
	}
	
	// expects file to be readable
	static std::uint32_t read_uint32_LE(std::fstream& fin)
	{
		std::uint32_t val = 0;
		char chars[4];
		fin.read(chars, 4);
		val += static_cast<std::uint32_t>(static_cast<unsigned char>(chars[0]));
		val += static_cast<std::uint32_t>(static_cast<unsigned char>(chars[1])) << 8;
		val += static_cast<std::uint32_t>(static_cast<unsigned char>(chars[2])) << 16;
		val += static_cast<std::uint32_t>(static_cast<unsigned char>(chars[3])) << 24;
		return val;
	}
	std::uint32_t read_uint32_LE() { return read_uint32_LE(file); }
	// expects file to be writable
	static void write_uint32_LE(std::uint32_t num, std::fstream& fout)
	{
		fout.put(static_cast<char>(static_cast<unsigned char>(num & 0xFF)));
		fout.put(static_cast<char>(static_cast<unsigned char>((num >> 8) & 0xFF)));
		fout.put(static_cast<char>(static_cast<unsigned char>((num >> 16) & 0xFF)));
		fout.put(static_cast<char>(static_cast<unsigned char>((num >> 24) & 0xFF)));
	}
	void write_uint32_LE(std::uint32_t num) { write_uint32_LE(num, file); }
	// expects file to be readable
	static std::uint64_t read_uint64_LE(std::fstream& fin)
	{
		std::uint64_t val = 0;
		char chars[8];
		fin.read(chars, 8);
		val += static_cast<std::uint64_t>(static_cast<unsigned char>(chars[0]));
		val += static_cast<std::uint64_t>(static_cast<unsigned char>(chars[1])) << 8;
		val += static_cast<std::uint64_t>(static_cast<unsigned char>(chars[2])) << 16;
		val += static_cast<std::uint64_t>(static_cast<unsigned char>(chars[3])) << 24;
		val += static_cast<std::uint64_t>(static_cast<unsigned char>(chars[4])) << 32;
		val += static_cast<std::uint64_t>(static_cast<unsigned char>(chars[5])) << 40;
		val += static_cast<std::uint64_t>(static_cast<unsigned char>(chars[6])) << 48;
		val += static_cast<std::uint64_t>(static_cast<unsigned char>(chars[7])) << 56;
		return val;
	}
	std::uint64_t read_uint64_LE() { return read_uint64_LE(file); }
	// expects file to be writable
	static void write_uint64_LE(std::uint64_t num, std::fstream& fout)
	{
		fout.put(static_cast<char>(static_cast<unsigned char>(num & 0xFF)));
		fout.put(static_cast<char>(static_cast<unsigned char>((num >> 8) & 0xFF)));
		fout.put(static_cast<char>(static_cast<unsigned char>((num >> 16) & 0xFF)));
		fout.put(static_cast<char>(static_cast<unsigned char>((num >> 24) & 0xFF)));
		fout.put(static_cast<char>(static_cast<unsigned char>((num >> 32) & 0xFF)));
		fout.put(static_cast<char>(static_cast<unsigned char>((num >> 40) & 0xFF)));
		fout.put(static_cast<char>(static_cast<unsigned char>((num >> 48) & 0xFF)));
		fout.put(static_cast<char>(static_cast<unsigned char>((num >> 56) & 0xFF)));
	}
	void write_uint64_LE(std::uint64_t num) { write_uint64_LE(num, file); }

	// expects file to be writable
	static void write_nulls(std::size_t count, std::fstream& fout)
	{
		for (std::size_t i = 0; i < count; i++)
			{ fout.put(0); }
	}
	void write_nulls(std::size_t count) { write_nulls(count, file); }
	
	constexpr static std::streamoff inds_section_offset()
	{
		return magic_bytes.size() + 4 + 4 + 4;
	}
	constexpr static std::streamoff words_section_offset(std::uint32_t reserved_words_)
	{
		return inds_section_offset() + reserved_words_ * 4 * 2;
	}
	constexpr std::streamoff words_section_offset() const
		{ return words_section_offset(reserved_words); }
	constexpr static std::streamoff defs_section_offset(std::uint32_t reserved_words_, std::uint32_t words_sect_size_)
	{
		return words_section_offset(reserved_words_) + words_sect_size_;
	}
	constexpr std::streamoff defs_section_offset() const
		{ return defs_section_offset(reserved_words, words_sect_size); }
	
	constexpr static std::uint64_t fnv_init = 0xcbf29ce484222325;
	
	// @param init  value to use for hash initialization. can be used to continue calculating a hash with additional data
	constexpr static std::uint64_t fnv1a(std::span<const std::byte> data, std::uint64_t init = fnv_init)
	{
		std::uint64_t hash = init;
		for (std::byte b : data)
		{
			hash ^= std::to_integer<std::uint64_t>(b);
			hash *= 0x100000001b3;
		}
		return hash;
	}

	// @param expected_size  expected size, or 0 to skip size checking
	// @return pair of size and hash or { 0, 0 } if size does not match expected
	std::pair<std::uint32_t, std::uint64_t> get_def_size_and_hash(std::uint32_t def_ind, std::uint32_t expected_size, std::streamoff defs_section_offset_)
	{
		auto def_off = defs_section_offset_ + def_ind;
		file.seekg(def_off, std::ios::beg);
		std::uint32_t size = read_uint32_LE();
		check_file();
		if (size == 0)
			{ throw std::runtime_error("Read 0 definition size. File may be corrupted"); }
		if (expected_size != 0 && size != expected_size)
			{ return {0, 0}; }
		std::uint64_t hash = read_uint64_LE();
		check_file();
		return { size, hash };
	}
	std::pair<std::uint32_t, std::uint64_t> get_def_size_and_hash(std::uint32_t def_ind, std::uint32_t expected_size = 0) { return get_def_size_and_hash(def_ind, expected_size, defs_section_offset()); }
	
	// retrieve hash of definition from file (fast)
	// @param expected_size  expected size, or 0 to skip size checking
	// @return hash, or nullopt if size does not match expected_size
	std::optional<std::uint64_t> get_def_hash(std::uint32_t def_ind, std::uint32_t expected_size, std::streamoff defs_section_offset_)
	{
		auto [size, hash] = get_def_size_and_hash(def_ind, expected_size, defs_section_offset_);
		if (size == 0)
			{ return {}; }
		return hash;
	}
	std::optional<std::uint64_t> get_def_hash(std::uint32_t def_ind, std::uint32_t expected_size = 0) { return get_def_hash(def_ind, expected_size, defs_section_offset()); }
	
	// hash definition found in file (slow)
	// should only be used to verify hashes on open()
	// @param expected_size  expected size, or 0 to skip size checking
	// @return hash, or nullopt if size does not match expected_size
	std::optional<std::uint64_t> hash_existing_def(std::uint32_t def_ind, std::uint32_t expected_size = 0)
	{
		std::uint64_t hash = fnv_init;

		auto def_off = defs_section_offset() + def_ind;
		file.seekg(def_off, std::ios::beg);
		std::uint32_t size = read_uint32_LE();
		check_file();
		if (size == 0)
			{ throw std::runtime_error("Read 0 definition size. File may be corrupted"); }

		if (expected_size != 0 && size != expected_size)
			{ return {}; }
		
		file.ignore(8); // skip reading existing hash

		for (int i = 0; i < (size - 1) / batch_size + 1; i++) // (size / batch_size) rounded up
		{
			const auto [data, read_amt] = read_def_batched(i, size, def_off + 12);
			check_file();
			hash = fnv1a(std::as_bytes(std::span(data).subspan(0, read_amt)), hash);
		}
		return hash;
	}
	
	std::optional<std::uint32_t> get_existing_def_ind(std::span<const std::byte> def)
	{
		assert(!def.empty() && static_cast<std::uint32_t>(def.size()) == def.size());
		const std::uint32_t size = def.size();
		const auto it = existing_defs.find(size);
		if (it != existing_defs.end())
		{
			const auto hash = fnv1a(def);
			const auto it2 = it->second.find(hash);
			if (it2 != it->second.end())
			{
				for (const auto def_ind : it2->second)
				{
					if (get_def_hash(def_ind, size) == hash)
						{ return def_ind; }
				}
			}
		}
		return {};
	};
	
	// @throws std::runtime_error  if file is invalid
	static void check_file(std::fstream& f)
	{
		if (f.bad())
			{ throw std::runtime_error("Unrecoverable file I/O error"); }
		if (f.eof())
			{ throw std::runtime_error("Unexpected EOF"); }
		if (f.fail())
			{ throw std::runtime_error("File I/O error"); }
	}
	void check_file() { check_file(file); }

	// leaves file as read only
	void create_file()
	{
		assert(words.empty());

		reserved_words = init_reserved_words;
		words_sect_size = init_words_sect_size;

		open_out();

		// magic bytes
		file.write(magic_bytes.data(), magic_bytes.size());

		write_uint32_LE(reserved_words);
		write_uint32_LE(words_sect_size);
		write_uint32_LE(0); // no words yet

		write_nulls(reserved_words * 4 * 2); // 4 bytes per index, once for words and once for defs

		write_nulls(words_sect_size); // no words yet

		// we don't actually need to fill the defs section with nulls
		// since defs will simply be appended to the end

		open_in(); // re-open as read only
	}

	// expects file to be readable
	// Complexity: O(reserved_words + total_words_len + N*log(N)), where N is number of words
	// File Access: Read, magic_bytes.size() + 8 + reserved_words * 8 + total_words_len
	// @throws std::runtime_error  on file i/o error or if parsing receives an unexpected value
	void read_file()
	{
		if (!file)
			{ throw std::runtime_error("Error reading from file"); }
		
		std::uintmax_t file_size = std::filesystem::file_size(filename);
		
		file.seekg(0, std::ios::beg);
		{
			std::array<char, magic_bytes.size()> arr;
			file.read(arr.data(), arr.size());
			check_file();
			if (!std::ranges::equal(magic_bytes, arr))
				{ throw std::runtime_error("Incorrect magic bytes. File may be corrupted"); }
		}
		reserved_words = read_uint32_LE();
		check_file();
		if (reserved_words == 0)
			{ throw std::runtime_error("Read 0 reserved words. File may be corrupted"); }
		words_sect_size = read_uint32_LE();
		check_file();
		if (words_sect_size == 0)
			{ throw std::runtime_error("Read 0 word section size. File may be corrupted"); }
		std::size_t num_words = read_uint32_LE();
		check_file();
		if (num_words > reserved_words)
			{ throw std::runtime_error("Number of words is greater than total reserved words. File may be corrupted"); }
		
		if (static_cast<std::uintmax_t>(reserved_words) * 8 + static_cast<std::uintmax_t>(num_words) > file_size)
			{ throw std::runtime_error("Reported indices + words section sizes is greater than file size. File may be corrupted"); }
		
		static const auto sort_and_find_dup = [](auto& v) -> bool
		{
			std::ranges::sort(v);
			auto it = std::ranges::adjacent_find(v);
			return (it != std::ranges::end(v));
		};
		// sort by first range and find duplicates in first range only
		static const auto sort_and_find_dup_zipped = [](auto&&... args) -> bool
		{
			auto v = std::views::zip(std::forward<decltype(args)>(args)...);
			std::ranges::sort(v, [](const auto& a, const auto& b) { return (std::get<0>(a) < std::get<0>(b)); });
			auto it = std::ranges::adjacent_find(v, [](const auto& a, const auto& b) { return (std::get<0>(a) == std::get<0>(b)); });
			return (it != std::ranges::end(v));
		};

		{
			std::vector<std::uint32_t> word_inds, def_inds;
			for (std::uint32_t i = 0; i < reserved_words; i++)
			{
				std::uint32_t ind = read_uint32_LE();
				check_file();
				if (ind != 0)
					{ word_inds.emplace_back(ind - 1); }
			}
			for (std::uint32_t i = 0; i < reserved_words; i++)
			{
				std::uint32_t ind = read_uint32_LE();
				check_file();
				if (ind != 0)
					{ def_inds.emplace_back(ind - 1); }
			}
			if (word_inds.size() != num_words || def_inds.size() != num_words)
				{ throw std::runtime_error("Incorrect number of valid indices. File may be corrupted"); }

			// multiple words can share the same def_ind but word_inds must be unique
			if (sort_and_find_dup_zipped(word_inds, def_inds))
				{ throw std::runtime_error("Found repeated indices. File may be corrupted"); }

			words.clear();
			std::streamoff section_off = words_section_offset();
			for (const auto [word_off, def_off] : std::views::zip(word_inds, def_inds))
			{
				file.seekg(section_off + word_off, std::ios::beg);
				check_file();
				std::string word;
				for (std::size_t j = 0; j < words_sect_size - word_off; j++)
				{
					char c;
					file.get(c);
					check_file();
					if (c == '\0')
						{ break; }
					word += c;
				}
				words.emplace_back(std::move(word), def_off);
			}
		}

		if (sort_and_find_dup(words))
			{ throw std::runtime_error("Found repeated words. File may be corrupted"); }
	}
	
	// expects file to be readable
	// leaves file as read only
	// Complixity: O(n_words + total_words_len + total_defs_size)
	// File Access: Create; Read, old_reserved_words * 8 + old_words_sect_size + total_defs_size bytes;
	//     Write, reserved_words * 8 + words_sect_size + total_defs_size bytes; Rename; Delete
	void rewrite_file(std::uint32_t old_reserved_words, std::uint32_t old_words_sect_size)
	{
		assert(reserved_words >= words.size());
		assert(words_sect_size >= std::reduce(words.begin(), words.end(), std::size_t(0), // TODO: replace with uz following support
			[](std::size_t init, const word_info& elem) -> std::size_t
				{ return init + elem.word.size() + 1; }));

		// create new file for output and swap with current file
		const std::string new_file = filename + ".tmp";
		std::fstream file2(new_file, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		
		// magic bytes
		file2.write(magic_bytes.data(), magic_bytes.size());

		write_uint32_LE(reserved_words, file2);
		write_uint32_LE(words_sect_size, file2);
		write_uint32_LE(words.size(), file2);
		
		// inds section
		std::uint32_t bytes_written = 0;
		for (const auto& [word, def_ind] : words)
		{
			write_uint32_LE(bytes_written + 1, file2);
			bytes_written += word.size() + 1;
		}
		write_nulls((reserved_words - words.size()) * 4, file2);
		// defs will be rearranged, just use a placeholder for now
		write_nulls(reserved_words * 4, file2);
		
		// words section
		bytes_written = 0;
		for (const auto& [word, def_ind] : words)
		{
			file2.write(word.c_str(), word.size() + 1); // write the null as well
			bytes_written += word.size() + 1;
		}
		write_nulls(words_sect_size - bytes_written, file2);

		// defs section
		{
			if (do_dedup)
			{
				existing_defs.clear();
			}

			std::streampos defs_sect_start = file2.tellp();
			assert(defs_sect_start == defs_section_offset());
			std::streamoff old_defs_sect_off = defs_section_offset(old_reserved_words, old_words_sect_size);
			
			for (auto& [word, def_ind] : words)
			{
				std::streamoff cur_def_off = old_defs_sect_off + def_ind;
				file.seekg(cur_def_off, std::ios::beg);
				std::uint32_t size = read_uint32_LE();
				check_file();
				if (size == 0)
					{ throw std::runtime_error("Read 0 definition size. File may be corrupted"); }
				
				std::optional<std::uint64_t> old_hash;
				if (do_dedup)
				{
					const auto it = existing_defs.find(size);
					if (it != existing_defs.end())
					{
						const auto res = get_def_size_and_hash(def_ind, size, old_defs_sect_off);
						const auto size2 = res.first;
						old_hash = res.second;
						if (size2 == size)
						{
							const auto it2 = it->second.find(old_hash.value());
							if (it2 != it->second.end())
							{
								bool found_match = false;
								for (const std::streamoff found_def_ind : it2->second)
								{
									file.seekg(cur_def_off, std::ios::beg);
									auto read_size = read_uint32_LE();
									check_file();
									if (read_size != size)
										{ continue; }
									file2.seekg(defs_sect_start + found_def_ind, std::ios::beg);
									read_size = read_uint32_LE(file2);
									check_file(file2);
									if (read_size != size)
										{ continue; }
									// we've already read the hash from `file` through get_def_hash
									const auto hash2 = read_uint64_LE(file2);
									if (hash2 != old_hash.value())
										{ continue; }

									static constexpr std::size_t cmp_batch_size = 1024;
									bool mismatch = false;
									for (int i = 0; i < (size - 1) / batch_size + 1; i++) // (size / cmp_batch_size) rounded up
									{
										const auto [data, read_amt] = read_def_batched(i, size, cur_def_off + 12);
										check_file();
										const auto [data2, read_amt2] = read_def_batched(i, size, defs_sect_start + (found_def_ind + 12), file2);
										check_file(file2);
										if (read_amt != read_amt2 || data != data2)
											{ mismatch = true; break; }
									}
									if (!mismatch)
										{ def_ind = found_def_ind; found_match = true; break; }
								}
								if (found_match)
									{ continue; }
							}
						}
					}
				}

				std::uint64_t hash = [old_hash, cur_def_off, this]()
				{
					if (!do_dedup)
						{ return fnv_init; } // will be written to the file but we don't care (will be overwritten)
					if (old_hash)
						{ return old_hash.value(); }
					file.seekg(cur_def_off + 4, std::ios::beg);
					std::uint64_t hash = read_uint64_LE();
					check_file();
					return hash;
				}();
				
				file2.seekp(0, std::ios::end);
				assert(file2.tellp() >= defs_sect_start);
				def_ind = file2.tellp() - defs_sect_start;

				write_uint32_LE(size, file2);
				write_uint64_LE(hash, file2);
				
				for (int i = 0; i < (size - 1) / batch_size + 1; i++) // (size / batch_size) rounded up
				{
					const auto [data, read_amt] = read_def_batched(i, size, cur_def_off + 12);
					check_file();
					file2.write(data.data(), read_amt);
					if (!do_dedup)
					{
						hash = fnv1a(std::as_bytes(std::span(data).subspan(0, read_amt)), hash);
					}
				}
				
				if (!do_dedup)
				{
					file2.seekp(defs_sect_start + static_cast<std::streamoff>(def_ind), std::ios::beg);
					write_uint64_LE(hash, file2);
				}
				else
				{
					assert(!std::ranges::contains(existing_defs[size][hash], def_ind));
					existing_defs[size][hash].push_back(def_ind);
				}
			}
		}

		// update def inds
		file2.seekp(inds_section_offset() + reserved_words * 4, std::ios::beg);
		for (const auto& [word, def_ind] : words)
			{ write_uint32_LE(def_ind + 1, file2); }
		write_nulls((reserved_words - words.size()) * 4, file2);

		file.close();
		file2.close();
		std::filesystem::rename(new_file, filename);

		open_in();
	}

	// Complexity: O(N*log(N)), where N is number of words
	// File Access: No
	void sort_words()
	{
		assert(0 <= first_new_word && first_new_word <= words.size());
		std::sort(words.begin() + first_new_word, words.end());
		if (std::adjacent_find(words.begin() + first_new_word, words.end()) != words.end())
			{ throw std::logic_error("Repeated words were inserted"); }
		std::inplace_merge(words.begin(), words.begin() + first_new_word, words.end());
		first_new_word = -1;
		assert(std::ranges::is_sorted(words));
	}
	
	// find def_ind corresponding to a word in `words`,
	// using a binary search followed by linear search
	// Complexity: O(log(n_words))
	// File Access: No
	std::uint32_t find_def_ind(std::string_view word) const
	{
		const auto end_it = ((first_new_word == -1) ? words.end() : words.begin() + first_new_word);
		auto it = std::lower_bound(words.begin(), end_it, word);
		if (it == end_it || it->word != word)
		{
			if (end_it == words.end())
				{ return -1; }
			it = std::find(end_it, words.end(), word);
			if (it == words.end())
				{ return -1; }
		}
		return it->def_ind;
	}

	// `batch_ind` * `batch_size` must be less than `size`
	// expects file to be readable
	// Complexity: O(1)
	// File Access: Read, 4096 bytes
	// @param batch_ind  index of current batch (starting from batch 0)
	// @param size  total size of definition
	// @param data_start_pos  start position of DATA ONLY (EXCLUDES DATA SIZE)
	// @return pair of data array and number of bytes read
	static std::pair<std::array<char, batch_size>, std::size_t> read_def_batched(int batch_ind, std::uint32_t size, std::streamoff data_start_pos, std::fstream& fin)
	{
		std::array<char, batch_size> arr;
		auto read_amt = std::min<std::streamsize>(size - batch_ind * batch_size, batch_size);
		fin.seekg(data_start_pos + batch_ind * batch_size, std::ios::beg);
		fin.read(arr.data(), read_amt);
		check_file(fin);
		return { arr, read_amt };
	}
	std::pair<std::array<char, batch_size>, std::size_t> read_def_batched(int batch_ind, std::uint32_t size, std::streamoff data_start_pos) { return read_def_batched(batch_ind, size, data_start_pos, file); }
	
	// expects file to be readable
	// Complexity: O(def_size)
	// File Access: Read, 4 + def_size bytes
	// @param def_ind  start position of definition (including data size)
	std::vector<char> read_def_whole(std::uint32_t def_ind, bool check_def = false)
	{
		file.seekg(defs_section_offset() + def_ind, std::ios::beg);
		const auto size = read_uint32_LE();
		check_file();
		const auto hash = read_uint64_LE();
		check_file();
		
		std::vector<char> v(size);
		file.read(v.data(), size);
		check_file();
		
		if (check_def && hash != fnv1a(std::as_bytes(std::span(v))))
		{
			throw std::runtime_error("Definition hash does not match. File may be corrupted");
		}
		return v;
	}
};

#endif

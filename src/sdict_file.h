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
//     def def def def ... (num_words in total; each def contains a unsigned 32-bit (4-byte LE) integer as size)
class dictionary_file
{
private:
	constexpr static std::uint32_t init_reserved_words = 32;
	constexpr static std::uint32_t init_words_sect_size = 256;

	// batch size for batched def reads
	constexpr static std::size_t batch_size = 4096;

	// convert string literal to array, removing the null delimiter
	template<std::size_t N>
	constexpr static auto strlit_to_array(const char (&a)[N])
		{ std::array<char, N - 1> arr; std::copy_n(a, N - 1, arr.begin()); return arr; }

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

public:
	// true if a file was created on construction, false if it was read from
	bool created_file = false;
	
	// creates a dictionary_file object with no associated file
	// open(string_view) must be called before anything else
	dictionary_file() {}

	// @throws std::runtime_error  on file i/o error or if parsing receives an unexpected value
	dictionary_file(std::string_view filename) : filename(filename), file_open_type(open_type::none)
	{
		if (!std::filesystem::is_regular_file(filename))
		{
			if (std::filesystem::exists(filename))
				{ throw std::runtime_error(std::string(filename) + " exists but is not a regular file"); }
			create_file();
			created_file = true;
		}
		else
		{
			open();
		}
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
	
	// associate given filename with this object and open as input (reading contents)
	// Complexity: O(reserved_words + total_words_len + N*log(N)), where N is number of words
	// File Access: Read, magic_bytes.size() + 8 + reserved_words * 8 + total_words_len
	// @throws std::runtime_error  on file i/o or parsing error
	void open(std::string_view filename_)
	{
		filename = filename_;
		file_open_type = open_type::none;
		open();
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
		
		open_in_out();
		
		// add def to words
		file.seekp(0, std::ios::end);
		std::streamoff cur_def_offset = file.tellg();
		cur_def_offset -= defs_section_offset();
		if (cur_def_offset < 0)
			{ throw std::runtime_error("Incorrect file size (too small)"); }
		words.emplace_back(std::string(word), cur_def_offset);
		
		// add def to file
		write_uint32_LE(def.size());
		file.write(reinterpret_cast<const char*>(def.data()), def.size());
		
		if constexpr (flush_words)
			{ flush(); }
		return true;
	}
	// see other overload
	template<bool flush_words = true, bool skip_dup_check = false>
	bool add_word(std::string_view word, std::span<const char> def)
		{ return add_word<flush_words, skip_dup_check>(word, std::as_bytes(def)); }
	
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
	std::optional<std::vector<char>> find(std::string_view word)
	{
		std::uint32_t ind = find_def_ind(word);
		if (ind == -1)
			{ return {}; }
		return read_def_whole(ind);
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
	std::uint32_t read_uint32_LE()
	{
		std::uint32_t val = 0;
		char chars[4];
		file.read(chars, 4);
		val += static_cast<unsigned char>(chars[0]);
		val += static_cast<unsigned char>(chars[1]) << 8;
		val += static_cast<unsigned char>(chars[2]) << 16;
		val += static_cast<unsigned char>(chars[3]) << 24;
		return val;
	}
	// expects file to be writable
	void write_uint32_LE(std::uint32_t num)
	{
		file.put(static_cast<char>(static_cast<unsigned char>(num & 0xFF)));
		file.put(static_cast<char>(static_cast<unsigned char>((num >> 8) & 0xFF)));
		file.put(static_cast<char>(static_cast<unsigned char>((num >> 16) & 0xFF)));
		file.put(static_cast<char>(static_cast<unsigned char>((num >> 24) & 0xFF)));
	}

	// expects file to be writable
	void write_nulls(std::size_t count)
	{
		for (std::size_t i = 0; i < count; i++)
			{ file.put(0); }
	}
	
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
	std::streamoff defs_section_offset() const
		{ return defs_section_offset(reserved_words, words_sect_size); }
	
	// @throws std::runtime_error  if file is invalid
	void check_file()
	{
		if (file.bad())
			{ throw std::runtime_error("Unrecoverable file I/O error"); }
		if (file.eof())
			{ throw std::runtime_error("Unexpected EOF"); }
		if (file.fail())
			{ throw std::runtime_error("File I/O error"); }
	}

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
		file.seekg(0, std::ios::beg);
		{
			std::array<char, magic_bytes.size()> arr;
			file.read(arr.data(), arr.size());
			check_file();
			if (!std::ranges::equal(magic_bytes, arr))
				{ throw std::runtime_error("Incorrect magic bytes. File may be corrupted"); }
		}
		// TODO: validate these sizes with the size of the file
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
		
		static const auto sort_and_find_dup = [](auto& v) -> bool
		{
			std::ranges::sort(v);
			auto it = std::ranges::adjacent_find(v);
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

			if (sort_and_find_dup(word_inds) || sort_and_find_dup(def_inds))
				{ throw std::runtime_error("Found repeated indices. File may be corrupted"); }

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
		std::fstream file2(new_file, std::ios::out | std::ios::binary);
		file.swap(file2); // file = new file (write)
		
		// magic bytes
		file.write(magic_bytes.data(), magic_bytes.size());

		write_uint32_LE(reserved_words);
		write_uint32_LE(words_sect_size);
		write_uint32_LE(words.size());
		
		// inds section
		std::uint32_t bytes_written = 0;
		for (const auto& [word, def_ind] : words)
		{
			write_uint32_LE(bytes_written + 1);
			bytes_written += word.size() + 1;
		}
		write_nulls((reserved_words - words.size()) * 4);
		// defs will be rearranged, just use a placeholder for now
		write_nulls(reserved_words * 4);
		
		// words section
		bytes_written = 0;
		for (const auto& [word, def_ind] : words)
		{
			file.write(word.c_str(), word.size() + 1); // write the null as well
			bytes_written += word.size() + 1;
		}
		write_nulls(words_sect_size - bytes_written);

		// defs section
		{
			file.swap(file2); // file = old file (read)
			std::streampos defs_sect_start = file2.tellp();
			std::streamoff old_sect_off = defs_section_offset(old_reserved_words, old_words_sect_size);
			for (auto& [word, def_ind] : words)
			{
				std::streamoff cur_def_off = old_sect_off + def_ind;
				file.seekg(cur_def_off, std::ios::beg);
				std::uint32_t size = read_uint32_LE();
				check_file();
				if (size == 0)
					{ throw std::runtime_error("Read 0 definition size. File may be corrupted"); }

				def_ind = file2.tellp() - defs_sect_start;

				file.swap(file2); // file = new file (write)
				write_uint32_LE(size);
				file.swap(file2); // file = old file (read)
				for (int i = 0; i < (size - 1) / batch_size + 1; i++) // (size / batch_size) rounded up
				{
					const auto [data, read_amt] = read_def_batched(i, size, cur_def_off + 4);
					check_file();
					file2.write(data.data(), read_amt);
				}
			}
			file.swap(file2); // file = new file (write)
		}

		// update def inds
		file.seekp(inds_section_offset() + reserved_words * 4, std::ios::beg);
		for (const auto& [word, def_ind] : words)
			{ write_uint32_LE(def_ind + 1); }
		write_nulls((reserved_words - words.size()) * 4);

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
	std::pair<std::array<char, batch_size>, std::size_t> read_def_batched(int batch_ind, std::uint32_t size, std::streamoff data_start_pos)
	{
		std::array<char, batch_size> arr;
		auto read_amt = std::min<std::streamsize>(size - batch_ind * batch_size, batch_size);
		file.seekg(data_start_pos + batch_ind * batch_size, std::ios::beg);
		file.read(arr.data(), read_amt);
		check_file();
		return { arr, read_amt };
	}
	
	// expects file to be readable
	// Complexity: O(def_size)
	// File Access: Read, 4 + def_size bytes
	// @param def_ind  start position of definition (including data size)
	std::vector<char> read_def_whole(std::uint32_t def_ind)
	{
		file.seekg(defs_section_offset() + def_ind, std::ios::beg);
		auto size = read_uint32_LE();
		check_file();
		std::vector<char> v(size);
		file.read(v.data(), size);
		check_file();
		return v;
	}
};

#endif

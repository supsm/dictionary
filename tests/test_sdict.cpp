#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "sdict_file.h"
#include <Catch2/catch_test_macros.hpp>
#include <Catch2/matchers/catch_matchers.hpp>
#include <Catch2/matchers/catch_matchers_string.hpp>

static bool files_are_equal(std::string_view f1, std::string_view f2)
{
	auto size = std::filesystem::file_size(f1);
	if (size != std::filesystem::file_size(f2))
		{ return false; }
	std::vector<char> v1(size), v2(size);
	std::ifstream fin{std::string(f1)};
	fin.read(v1.data(), size);
	fin.open(f2);
	fin.read(v2.data(), size);
	return (v1 == v2);
}

template<std::ranges::contiguous_range T1, std::ranges::contiguous_range T2>
static bool cmp_as_bytes(const T1& r1, const T2& r2)
{
	return std::ranges::equal(std::as_bytes(std::span<const std::ranges::range_value_t<T1>>(r1)), std::as_bytes(std::span<const std::ranges::range_value_t<T2>>(r2)));
}

template<typename ContainerT = std::string>
static ContainerT random_string(std::size_t min_length, std::size_t max_length, unsigned char min_char, unsigned char max_char)
{
	std::mt19937 mt(std::random_device{}());
	std::uniform_int_distribution<std::size_t> len_dist(min_length, max_length);
	std::uniform_int_distribution<unsigned short> char_dist(min_char, max_char);
	ContainerT s;
	s.resize(len_dist(mt), 0);
	std::generate(s.begin(), s.end(), [&]() { return static_cast<unsigned char>(char_dist(mt)); });
	return s;
}

template<typename ContainerT = std::vector<std::byte>>
static ContainerT random_bytes(std::size_t min_length, std::size_t max_length, unsigned char min_char, unsigned char max_char)
{
	std::mt19937 mt(std::random_device{}());
	std::uniform_int_distribution<std::size_t> len_dist(min_length, max_length);
	std::uniform_int_distribution<unsigned short> char_dist(min_char, max_char);
	ContainerT s;
	s.resize(len_dist(mt), std::byte(0));
	std::generate(s.begin(), s.end(), [&]() { return std::byte(char_dist(mt)); });
	return s;
}

TEST_CASE("create when exists", "[sdict]")
{
	constexpr std::string_view filename = "test.sdict";
	if (std::filesystem::exists(filename))
		{ std::filesystem::remove(filename); }

	SECTION("regular file")
	{
		std::ofstream fout{std::string(filename)};
		fout.close();
		REQUIRE_THROWS_WITH(dictionary_file(filename), "Unexpected EOF");
		std::filesystem::remove(filename);
	}
	SECTION("directory")
	{
		std::filesystem::create_directory(filename);
		using Catch::Matchers::EndsWith;
		REQUIRE_THROWS_WITH(dictionary_file(filename), EndsWith("exists but is not a regular file"));
		std::filesystem::remove(filename);
	}
	SECTION("symlink")
	{
		constexpr std::string_view filename2 = "test2.sdict";
		std::ofstream fout{std::string(filename2)};
		fout.close();
		try
			{ std::filesystem::create_symlink(filename2, filename); }
		catch (const std::filesystem::filesystem_error& e)
			{ SKIP("Could not create symlink: " << e.what()); }
		using Catch::Matchers::EndsWith;
		REQUIRE_THROWS_WITH(dictionary_file(filename), EndsWith("exists but is not a regular file"));
		std::filesystem::remove(filename);
		std::filesystem::remove(filename2);
	}
}

TEST_CASE("read fixed", "[sdict]")
{
	constexpr std::string_view filename = "assets/test1.sdict";
	if (!std::filesystem::is_regular_file(filename))
		{ SKIP("test file not found"); }

	dictionary_file file(filename);
	REQUIRE_FALSE(file.created_file);
	REQUIRE(file.num_words() == 2);

	REQUIRE(file.contains("testword1"));
	constexpr std::string_view def1 = "This is the definition for the first test word.";
	REQUIRE(std::ranges::equal(file.find("testword1").value(), def1));

	REQUIRE(file.contains("testword2"));
	constexpr std::string_view def2 = "This is the definition for the second test word.";
	REQUIRE(std::ranges::equal(file.find("testword2").value(), def2));
}

TEST_CASE("read+write fixed", "[sdict]")
{
	constexpr std::string_view filename = "test.sdict";
	if (std::filesystem::exists(filename))
		{ std::filesystem::remove(filename); }

	static constexpr std::array<std::pair<std::string_view, std::string_view>, 33> words_defs =
	{ {
		{ "word1", "definition1" }, { "word2", "definition2" }, { "word3", "definition3" }, { "word4", "definition4" },
		{ "word5", "definition1" }, { "word6", "definition1" }, { "word7", "definition2" }, { "word8", "definition2" },
		{ "word9", "definition2" }, { "word10", "definition3" }, { "word11", "definition3" }, { "word12", "definition3" },
		{ "word13", "definition4" }, { "word14", "definition4" }, { "word15", "definition1" }, { "word16", "definition1" },
		{ "word17", "definition1" }, { "word18", "definition1" }, { "word19", "definition3" }, { "word20", "definition3" },
		{ "word21", "definition2" }, { "word22", "definition2" }, { "word23", "definition4" }, { "word24", "definition2" },
		{ "word25", "definition1" }, { "word26", "definition4" }, { "word27", "definition1" }, { "word28", "definition3" },
		{ "word29", "definition2" }, { "word30", "definition5" }, { "word31", "definition1" }, { "word32", "definition6" },
		{ "word33", "definition2" }
	} };
	{
		dictionary_file file(filename);
		REQUIRE(file.created_file);
		REQUIRE(file.num_words() == 0);

		
		SECTION("always flush")
		{
			for (const auto [word, def] : words_defs)
			{
				file.add_word(word, def);
			}
		}
		SECTION("flush at end")
		{
			for (const auto [word, def] : words_defs)
			{
				file.add_word<false, true>(word, def);
			}
		}
	}

	REQUIRE(std::filesystem::is_regular_file(filename));

	{
		dictionary_file file(filename);
		REQUIRE_FALSE(file.created_file);
		REQUIRE(file.num_words() == words_defs.size());
		for (const auto& [word, def] : words_defs)
		{
			REQUIRE(file.contains(word));
			REQUIRE(cmp_as_bytes(def, file.find(word).value()));
		}
	}

	std::filesystem::remove(filename);
}

TEST_CASE("read+write dynamic", "[sdict]")
{
	constexpr std::string_view filename = "test.sdict";
	if (std::filesystem::exists(filename))
		{ std::filesystem::remove(filename); }

	std::unordered_map<std::string, std::vector<std::byte>> words;

	{
		dictionary_file file(filename);
		REQUIRE(file.created_file);
		REQUIRE(file.num_words() == 0);

		auto total_duration = std::chrono::microseconds::zero();
		std::size_t num_added = 0;
		SECTION("no dup check")
		{
			for (std::size_t i = 0; i < 65536; i++)
			{
				std::string word = random_string(1, 32, ' ', '~');
				if (words.contains(word))
					{ continue; }
				auto def = random_bytes<std::vector<std::byte>>(1, 256, 0, 255);

				const auto start = std::chrono::steady_clock::now();
				file.add_word<false, true>(word, def);
				const auto end = std::chrono::steady_clock::now();
				total_duration += std::chrono::duration_cast<std::chrono::microseconds>(end - start);
				num_added++;

				words.emplace(std::move(word), std::move(def));
			}
			WARN("Avg add_word (no flush, no dup check, 65536) time: " << total_duration / num_added);
			const auto start = std::chrono::steady_clock::now();
			file.flush();
			const auto end = std::chrono::steady_clock::now();
			WARN("Flush time (no dup check, 65536): " << std::chrono::duration_cast<std::chrono::milliseconds>(end - start));
		}
		SECTION("dup check")
		{
			for (std::size_t i = 0; i < 16384; i++)
			{
				std::string word = random_string(1, 32, ' ', '~');
				auto def = random_bytes<std::vector<std::byte>>(1, 256, 0, 255);

				const auto start = std::chrono::steady_clock::now();
				bool res = file.add_word<false>(word, def);
				const auto end = std::chrono::steady_clock::now();
				total_duration += std::chrono::duration_cast<std::chrono::microseconds>(end - start);
				num_added++;

				REQUIRE(res == !words.contains(word));

				words.emplace(std::move(word), std::move(def));
			}
			WARN("Avg add_word (no flush, yes dup check, 16384) time: " << total_duration / num_added);
			const auto start = std::chrono::steady_clock::now();
			file.flush();
			const auto end = std::chrono::steady_clock::now();
			WARN("Flush time (dup check, 16384): " << std::chrono::duration_cast<std::chrono::milliseconds>(end - start));
		}
	}

	REQUIRE(std::filesystem::is_regular_file(filename));

	{
		dictionary_file file(filename);
		REQUIRE_FALSE(file.created_file);
		REQUIRE(file.num_words() == words.size());
		for (const auto& [word, def] : words)
		{
			REQUIRE(file.contains(word));
			REQUIRE(cmp_as_bytes(def, file.find(word).value()));
		}
	}

	std::filesystem::remove(filename);
}

TEST_CASE("create and add", "[sdict]")
{
	constexpr std::string_view filename = "test.sdict";
	if (std::filesystem::exists(filename))
		{ std::filesystem::remove(filename); }

	{
		dictionary_file file(filename);
		REQUIRE(file.created_file);
		REQUIRE(file.num_words() == 0);

		SECTION("large def")
		{
			std::string word = random_string(1, 32, ' ', '~');
			auto def = random_bytes<std::vector<std::byte>>(2048, 4096, 0, 255);
			file.add_word(word, def);
			REQUIRE(file.num_words() == 1);
			REQUIRE(file.contains(word));
			REQUIRE(cmp_as_bytes(def, file.find(word).value()));
		}
		SECTION("large word")
		{
			std::string word = random_string(512, 1024, ' ', '~');
			auto def = random_bytes<std::vector<std::byte>>(1, 256, 0, 255);
			file.add_word(word, def);
			REQUIRE(file.num_words() == 1);
			REQUIRE(file.contains(word));
			REQUIRE(cmp_as_bytes(def, file.find(word).value()));
		}
		SECTION("multiple")
		{
			std::unordered_map<std::string, std::vector<std::byte>> words;
			auto total_duration = std::chrono::microseconds::zero();
			std::size_t num_added = 0;
			for (std::size_t i = 0; i < 1024; i++)
			{
				std::string word = random_string(1, 32, ' ', '~');
				auto def = random_bytes<std::vector<std::byte>>(1, 256, 0, 255);

				const auto start = std::chrono::steady_clock::now();
				bool res = file.add_word(word, def);
				const auto end = std::chrono::steady_clock::now();
				total_duration += std::chrono::duration_cast<std::chrono::microseconds>(end - start);
				num_added++;

				REQUIRE(res == !words.contains(word));
				REQUIRE(file.num_words() == words.size() + (res ? 1 : 0));
				REQUIRE(file.contains(word));
				if (res)
					{ REQUIRE(cmp_as_bytes(def, file.find(word).value())); }
				words.emplace(std::move(word), std::move(def));
			}
			WARN("Avg add_word (with flush, 1024) time: " << total_duration / num_added);
		}
	}
	REQUIRE(std::filesystem::is_regular_file(filename));
	std::filesystem::remove(filename);
}

// TODO: test def with unsigned char and char vector (or string)
// and also with span

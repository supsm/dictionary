#include "sdict_file.h"
#include <atomic>
#include <fstream>
#include <filesystem>
#include <thread>
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
#include <jsoncons/json.hpp>
#include <jsoncons/json_cursor.hpp>
#include <jsoncons_ext/cbor/cbor.hpp>

constexpr std::size_t num_http_workers = 16;

constexpr std::size_t word_buf_size = 64, def_buf_size = 8;
std::array<std::pair<std::string, std::string>, def_buf_size> def_buf;
std::atomic<std::size_t> def_buf_start = 0, def_buf_end = 0;
std::array<std::string, word_buf_size> word_buf;
std::atomic<std::size_t> word_buf_start = 0, word_buf_end = 0;
std::mutex word_lock;
std::atomic_flag word_finished, def_finished;

std::string api_key;

// can have multiple http workers
void http_worker()
{
	httplib::SSLClient http_client("www.dictionaryapi.com");
	while (!word_finished.test() || word_buf_start != word_buf_end)
	{
		static constexpr auto url_encode = [](std::string_view in)
		{
			std::string s;
			for (char c : in)
			{
				if (('A' <= c && c <= 'Z') ||
					('a' <= c && c <= 'z') ||
					('0' <= c && c <= '9') ||
					c == '-' || c == '.' || c == '_' || c == '~')
					{ s += c; }
				else
					{ s += std::format("%{:X}", c); }
			}
			return s;
		};

		std::size_t ind;

		do
		{
			ind = word_buf_start;
			// wait if buf_end == buf_start (buffer is empty)
			word_buf_end.wait(ind);
		}
		while (!word_lock.try_lock());

		const std::string word = std::move(word_buf[ind]);
		word_buf_start = (ind + 1) % word_buf_size;
		word_buf_start.notify_one();

		word_lock.unlock();

		auto res = http_client.Get(httplib::append_query_params("/api/v3/references/collegiate/json/" + url_encode(word), { { "key", api_key } }));
		if (!res || res->status != 200)
		{
			std::cerr << word << std::endl;
			std::exit(-1);
		}

		std::size_t next = (def_buf_end + 1) % def_buf_size;

		// wait if buf_start == buf_end + 1 (buffer is full)
		def_buf_start.wait(next);
		if (def_finished.test())
			{ return; }

		def_buf[def_buf_end] = { std::move(word), std::move(res->body) };
		def_buf_end = next;
		def_buf_end.notify_one();
	}
	def_finished.test_and_set();
	def_buf_start.notify_all(); // unblock and end all other http worker threads
}

// should have only 1 file read worker
void file_read_worker()
{
	std::ifstream fin("words.txt"); // NB: words.txt should have no duplicates!
	std::string word;
	while (std::getline(fin, word))
	{
		std::ranges::transform(word, word.begin(), [](unsigned char c) { return std::tolower(c); });

		std::size_t next = (word_buf_end + 1) % word_buf_size;

		// wait if buf_start == buf_end + 1 (buffer is full)
		word_buf_start.wait(next);

		word_buf[word_buf_end] = std::move(word);
		word_buf_end = next;
		word_buf_end.notify_one();
	}
	word_finished.test_and_set();
}

int main()
{
	if (std::filesystem::exists("data.sdict")) { std::filesystem::remove("data.sdict"); }
	dictionary_file dict_file("data.sdict");
	{
		std::ifstream fin("api_key.txt");
		if (!fin)
			{ return -1; }
		fin >> api_key;
	}
	
	std::jthread t(file_read_worker);
	std::array<std::jthread, num_http_workers> ts;
	for (auto& e : ts)
		{ e = std::jthread(http_worker); }
	
	std::size_t num = 0;
	while (!def_finished.test() || def_buf_start != def_buf_end)
	{
		std::vector<std::uint8_t> cbor_bytes;
		
		// wait if buf_end == buf_start (buffer is empty)
		def_buf_end.wait(def_buf_start);
		
		const std::pair<std::string, std::string> p = std::move(def_buf[def_buf_start]);
		def_buf_start = (def_buf_start + 1) % def_buf_size;
		def_buf_start.notify_one();
		
		jsoncons::json_string_cursor cursor(p.second);
		jsoncons::cbor::cbor_bytes_encoder encoder(cbor_bytes);
		
		using jsoncons::staj_event_type;
		
		for (; !cursor.done(); cursor.next())
		{
			const auto& event = cursor.current();
			switch (event.event_type())
			{
				case staj_event_type::begin_array:
					encoder.begin_array();
					break;
				case staj_event_type::end_array:
					encoder.end_array();
					break;
				case staj_event_type::begin_object:
					encoder.begin_object();
					break;
				case staj_event_type::end_object:
					encoder.end_object();
					break;
				case staj_event_type::key:
					encoder.key(event.get<jsoncons::string_view>());
					break;
				case staj_event_type::string_value:
					encoder.string_value(event.get<jsoncons::string_view>());
					break;
				case staj_event_type::null_value:
					encoder.null_value();
					break;
				case staj_event_type::bool_value:
					encoder.bool_value(event.get<bool>());
					break;
				case staj_event_type::int64_value:
					encoder.int64_value(event.get<int64_t>());
					break;
				case staj_event_type::uint64_value:
					encoder.uint64_value(event.get<uint64_t>());
					break;
				case staj_event_type::double_value:
					encoder.double_value(event.get<double>());
					break;
				default:
					std::cerr << "Unhandled event type: " << event.event_type() << " " << "\n";
					break;
			}
		}
		
		dict_file.add_word<false, true>(p.first, std::as_bytes(std::span(cbor_bytes)));
		
		num++;
		if (num % 10 == 0)
			{ std::cout << num << std::endl; }
	}
	dict_file.flush();
}

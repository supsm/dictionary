#include "sdict_file.h"
#include <fstream>
#include <filesystem>
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
#include <jsoncons/json.hpp>
#include <jsoncons/json_cursor.hpp>
#include <jsoncons_ext/cbor/cbor.hpp>

int main()
{
	std::ifstream fin("words.txt");
	if (std::filesystem::exists("data.sdict")) { std::filesystem::remove("data.sdict"); }
	dictionary_file dict_file("data.sdict");
	httplib::SSLClient http_client("www.dictionaryapi.com");
	std::string api_key;
	{
		std::ifstream fin("api_key.txt");
		if (!fin)
			{ return -1; }
		fin >> api_key;
	}
	
	std::string word;
	while (std::getline(fin, word))
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
					{ s += std::format("{:X}", c); }
			}
			return s;
		};
		auto res = http_client.Get(httplib::append_query_params("/api/v3/references/collegiate/json/" + url_encode(word), { { "key", api_key } }));
		if (!res || res->status != 200)
		{
			std::cerr << word << std::endl;
			return -1;
		}
		
		std::vector<std::uint8_t> cbor_bytes;
		
		jsoncons::json_string_cursor cursor(res->body);
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
		
		dict_file.add_word<false, true>(word, std::as_bytes(std::span(cbor_bytes)));
	}
	dict_file.flush();
}

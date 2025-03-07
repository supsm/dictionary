#include <cstdlib>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
#define FMT_HEADER_ONLY
#define FMT_UNICODE false
#include <fmt/ranges.h>
#include <jsoncons_ext/cbor/cbor.hpp>
#include <FL/fl_ask.H>

#include "ui.h"
#include "styles.h"
#include "json_coro_cursor.h"
#include "dict_parse.h"
#include "text_parse.h"
#include "links.h"
#include "util.h"
#include "sdict_file.h"

FLTK_UI ui;
std::string api_key;
httplib::SSLClient http_client("www.dictionaryapi.com");
std::string last_word = "";
dictionary_file dict_file;
bool online_mode = true, offline_mode = true; // TODO: indicators for whether each of these are available; maybe indicator for whether search is online or not
// TODO: offline search completion (maybe Fl_Input_Choice)?

// TODO: save/restore scroll location and selections?
struct cached_def
{
	struct buf_data
	{
		// fltk uses malloc/free so we need custom deleter
		std::unique_ptr<char, decltype([](char* p) { std::free(p); })> buf;
		int length, gap_start, gap_end;
	};
	std::string word;
	buf_data def_text, def_style;
	decltype(links) def_links;
};

// TODO: deduplicate?
std::vector<cached_def> cached_defs;
std::size_t cur_cached_ind; // index of last used cached def, or size of cached_defs if none

// make forward/back buttons active/inactive based on cur_cached_ind and cached_defs.size()
void update_nav_buttons()
{
	const bool is_first = (cur_cached_ind == 0);
	const bool is_last = (cur_cached_ind + 1 >= cached_defs.size()); // if cached_defs is empty then -1 will underflow
	if (is_first)
		{ ui.button_back.deactivate(); }
	else
		{ ui.button_back.activate(); }
	if (is_last)
		{ ui.button_forward.deactivate(); }
	else
		{ ui.button_forward.activate(); }
}

// clears last_word, links, and optionally text/style buffers, and caches the current
// definition in cached_defs
// NB: if do_reset is false, text_buf.mBuf and style_buf.mBuf MUST be changed immediately after,
// or the buffers (whose addresses are cached) may be freed, leading to dangling cached buffers
// @tparam do_reset  whether to reset text_buffers and style_buffers to a new empty buffer
// @tparam do_cache  whether to cache to cached_defs
template<bool do_reset = true, bool do_cache = true>
void clear_and_cache()
{
	static_assert(do_reset || do_cache, "must either reset or cache (or both)");

	Fl_Text_Buffer& text_buf = ui.text_buf;
	Fl_Text_Buffer& style_buf = ui.style_buf;

	char*& text_buf_mBuf = text_buf.*get(Fl_Text_Buffer_m<"mBuf", char*>());
	char*& style_buf_mBuf = style_buf.*get(Fl_Text_Buffer_m<"mBuf", char*>());
	int& text_buf_mLength = text_buf.*get(Fl_Text_Buffer_m<"mLength", int>());
	int& style_buf_mLength = style_buf.*get(Fl_Text_Buffer_m<"mLength", int>());
	int& text_buf_mGapStart = text_buf.*get(Fl_Text_Buffer_m<"mGapStart", int>());
	int& style_buf_mGapStart = style_buf.*get(Fl_Text_Buffer_m<"mGapStart", int>());
	int& text_buf_mGapEnd = text_buf.*get(Fl_Text_Buffer_m<"mGapEnd", int>());
	int& style_buf_mGapEnd = style_buf.*get(Fl_Text_Buffer_m<"mGapEnd", int>());
	int& text_buf_mPreferredGapSize = text_buf.*get(Fl_Text_Buffer_m<"mPreferredGapSize", int>());
	int& style_buf_mPreferredGapSize = style_buf.*get(Fl_Text_Buffer_m<"mPreferredGapSize", int>());

	// copy old data (buffers are NOT copied, just the pointer address)
	cached_def::buf_data text_buf_data = { decltype(cached_def::buf_data::buf)(text_buf_mBuf), text_buf_mLength, text_buf_mGapStart, text_buf_mGapEnd };
	cached_def::buf_data style_buf_data = { decltype(cached_def::buf_data::buf)(style_buf_mBuf), style_buf_mLength, style_buf_mGapStart, style_buf_mGapEnd };

	if constexpr (do_reset)
	{
		(text_buf.*get(Fl_Text_Buffer_m<"call_predelete_callbacks", void(int, int) const>()))(0, text_buf_data.length);
		(style_buf.*get(Fl_Text_Buffer_m<"call_predelete_callbacks", void(int, int) const>()))(0, style_buf_data.length);

		// allocate new empty (but with gap) buffer and set members accordingly
		text_buf_mBuf = static_cast<char*>(std::malloc(text_buf_mPreferredGapSize));
		style_buf_mBuf = static_cast<char*>(std::malloc(style_buf_mPreferredGapSize));
		text_buf_mLength = 0;
		style_buf_mLength = 0;
		text_buf_mGapStart = 0;
		style_buf_mGapStart = 0;
		text_buf_mGapEnd = text_buf_mPreferredGapSize;
		style_buf_mGapEnd = style_buf_mPreferredGapSize;

		(text_buf.*get(Fl_Text_Buffer_m<"update_selections", void(int, int, int)>()))(0, text_buf_data.length, 0);
		(style_buf.*get(Fl_Text_Buffer_m<"update_selections", void(int, int, int)>()))(0, style_buf_data.length, 0);

		// TODO: do we need to remove the gap for this?
		(text_buf.*get(Fl_Text_Buffer_m<"call_modify_callbacks", void(int, int, int, int, const char*) const>()))(0, text_buf_data.length, 0, 0, text_buf_data.buf.get());
		(style_buf.*get(Fl_Text_Buffer_m<"call_modify_callbacks", void(int, int, int, int, const char*) const>()))(0, style_buf_data.length, 0, 0, style_buf_data.buf.get());
	}

	if constexpr (do_cache)
	{
		if (cur_cached_ind != cached_defs.size())
			{ cached_defs.resize(cur_cached_ind + 1); }
		cached_defs.emplace_back(std::move(last_word), std::move(text_buf_data), std::move(style_buf_data), std::move(links));
		cur_cached_ind = cached_defs.size();
		update_nav_buttons();
	}
}

void restore_from_cache(std::size_t ind)
{
	if (ind >= cached_defs.size())
	{
		fl_alert("Trying to restore from invalid cache index %uz (cache size %uz)", ind, cached_defs.size());
		return;
	}

	Fl_Text_Buffer& text_buf = ui.text_buf;
	Fl_Text_Buffer& style_buf = ui.style_buf;

	char*& text_buf_mBuf = text_buf.*get(Fl_Text_Buffer_m<"mBuf", char*>());
	char*& style_buf_mBuf = style_buf.*get(Fl_Text_Buffer_m<"mBuf", char*>());
	int& text_buf_mLength = text_buf.*get(Fl_Text_Buffer_m<"mLength", int>());
	int& style_buf_mLength = style_buf.*get(Fl_Text_Buffer_m<"mLength", int>());
	int& text_buf_mGapStart = text_buf.*get(Fl_Text_Buffer_m<"mGapStart", int>());
	int& style_buf_mGapStart = style_buf.*get(Fl_Text_Buffer_m<"mGapStart", int>());
	int& text_buf_mGapEnd = text_buf.*get(Fl_Text_Buffer_m<"mGapEnd", int>());
	int& style_buf_mGapEnd = style_buf.*get(Fl_Text_Buffer_m<"mGapEnd", int>());
	
	if (cur_cached_ind == cached_defs.size())
		{ clear_and_cache<false, true>(); }
	else
	{
		// bufs would have been released when restored, we need to cache them again
		cached_defs[cur_cached_ind].def_text.buf.reset(text_buf_mBuf);
		cached_defs[cur_cached_ind].def_style.buf.reset(style_buf_mBuf);
	}

	char* old_text_buf = text_buf_mBuf;
	char* old_style_buf = style_buf_mBuf;
	int old_text_length = text_buf_mLength;
	int old_style_length = style_buf_mLength;

	(text_buf.*get(Fl_Text_Buffer_m<"call_predelete_callbacks", void(int, int) const>()))(0, old_text_length);
	(style_buf.*get(Fl_Text_Buffer_m<"call_predelete_callbacks", void(int, int) const>()))(0, old_style_length);

	cached_def& cached = cached_defs[ind];
	text_buf_mBuf = cached.def_text.buf.release();
	style_buf_mBuf = cached.def_style.buf.release();
	text_buf_mLength = cached.def_text.length;
	style_buf_mLength = cached.def_style.length;
	text_buf_mGapStart = cached.def_text.gap_start;
	style_buf_mGapStart = cached.def_style.gap_start;
	text_buf_mGapEnd = cached.def_text.gap_end;
	style_buf_mGapEnd = cached.def_style.gap_end;
	links = cached.def_links;

	(text_buf.*get(Fl_Text_Buffer_m<"update_selections", void(int, int, int)>()))(0, old_text_length, 0);
	(style_buf.*get(Fl_Text_Buffer_m<"update_selections", void(int, int, int)>()))(0, old_style_length, 0);

	(text_buf.*get(Fl_Text_Buffer_m<"call_modify_callbacks", void(int, int, int, int, const char*) const>()))(0, old_text_length, 0, 0, old_text_buf);
	(style_buf.*get(Fl_Text_Buffer_m<"call_modify_callbacks", void(int, int, int, int, const char*) const>()))(0, old_style_length, 0, 0, old_style_buf);

	cur_cached_ind = ind;
	last_word = cached.word;
	update_nav_buttons();
}

void search_word(std::string_view word)
{
	std::string word_lower(word.size(), 0);
	std::ranges::transform(word, word_lower.begin(), [](unsigned char c) { return std::tolower(c); });

	const auto word_colon = word_lower.rfind(':');
	std::vector<word_info> data;

	{
		std::string_view word_only = word_lower;
		if (word_colon != std::string_view::npos)
			{ word_only = word_lower.substr(0, word_colon); }

		std::optional<std::vector<char>> dict_res;
		if (offline_mode)
		{
			try
				{ dict_res = dict_file.find(word_lower); }
			catch (const std::exception& e)
				{ fl_alert("%s", e.what()); return; }
		}

		try
		{
			const auto parse_data = [](auto& parse_task, const char* data, std::size_t data_len)
			{
				try
					{ parse_task.add_data({ data, data_len }); }
				catch (const std::exception& e)
				{
					// TODO: replace with std::format once range formatter is more mature
					throw std::runtime_error(fmt::format("JSON parse error: {}\nOccured in section:\n{:s}", e.what(), std::string_view(data, data_len) | std::views::chunk(128) | std::views::join_with('\n')));
				}
				if (parse_task.coro_handle.done())
				{
					throw std::runtime_error(fmt::format("JSON parsing ended prematurely.\nOccured in section:\n{:s}", std::string_view(data, data_len) | std::views::chunk(128) | std::views::join_with('\n')));
				}
			};


			if (dict_res)
			{
				auto cursor = cursor_coro_wrapper<jsoncons::cbor::cbor_bytes_cursor>(dict_res.value());
				try
				{
					task<void> parse_task = begin_parse(cursor, data);
					if (!parse_task.coro_handle.done())
						{ throw std::runtime_error("CBOR parsing did not finish"); }
				}
				catch (const std::exception& e)
					{ throw std::runtime_error(std::format("CBOR parse error: {}", e.what())); }
			}
			else if (online_mode)
			{
				json_coro_cursor cursor;
				task<void> parse_task = begin_parse(cursor, data);

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
				auto res = http_client.Get(httplib::append_query_params("/api/v3/references/collegiate/json/" + url_encode(word_only), { { "key", api_key } }),
					[&parse_data, &parse_task](const char* cur_data, std::size_t data_len) { parse_data(parse_task, cur_data, data_len); return true; });
				if (!res)
				{
					throw std::runtime_error("HTTP Error: " + httplib::to_string(res.error()));
				}
				if (res->status != 200)
				{
					throw std::runtime_error(std::format("Unexpected HTTP Status {}", res->status));
				}
			}
			else
			{
				throw std::runtime_error(std::format("Unable to find \"{}\" in offline dictionary", word_lower));
			}
		}
		catch (const std::runtime_error& e)
		{
			fl_alert("%s", e.what());
			return;
		}
	}

	if (!last_word.empty() && cur_cached_ind == cached_defs.size())
		{ clear_and_cache(); }
	else
	{
		clear_and_cache<true, false>();
		cur_cached_ind = cached_defs.size();
		update_nav_buttons();
	}
	last_word = word_lower;

	// we keep a separate buffer instead of using ui.text_buf and ui.style_buf
	// to prevent calling modify callbacks excessively. This results in a
	// speedup for larger definitions despite additional copy
	// (which might be optimized out anyway)
	std::vector<char> text_buf, style_buf;
	std::pair<std::size_t, std::size_t> target_word = { -1, -1 };

	const auto add = [&text_buf, &style_buf](std::string_view text, char style = get_style())
	{
		text_buf.append_range(text);
		style_buf.append_range(std::views::repeat(style, text.size()));
	};

	for (const auto& w : data)
	{
		const std::size_t start_len = text_buf.size();

		add(w.id, get_style(style::title));
		add("\n");

		if (word_colon != std::string_view::npos && word_lower == w.id)
			{ target_word = { start_len, text_buf.size() }; }

		for (const auto& sense : w.defs)
		{
			const auto add_sense = [&text_buf, &add](this auto self, const auto& val)
			{
				if (val.number)
				{
					add(val.number.value(), get_style(style_bold));
					add(" ");
				}
				if constexpr (std::is_base_of_v<basic_def_sense_data, std::remove_reference_t<decltype(val)>>)
				{
					if constexpr (std::is_same_v<div_sense_data, std::remove_cvref_t<decltype(val)>>)
					{
						add(val.sense_div, get_style(style_italic));
					}
					parse_def_text(val.def_text, add, [&text_buf]() -> int { return text_buf.size(); });
					add("\n");
					if constexpr (std::is_same_v<sense_data, std::remove_cvref_t<decltype(val)>>)
					{
						if (val.sdsense)
						{
							self(val.sdsense.value());
						}
					}
				}
				else
					{ add("\n"); }
			};
			std::visit(add_sense, sense);
		}
		add("\n");
	}

	ui.text_buf.append(text_buf.data(), text_buf.size());
	ui.style_buf.append(style_buf.data(), style_buf.size());

	if (target_word.first != -1)
	{
		const int lines = ui.text_buf.count_lines(0, target_word.first);
		// TODO: scroll is not correct
		ui.text_display.scroll(lines + 1, 0);
		ui.text_buf.select(target_word.first, target_word.second - 1);
	}
	else
		{ ui.text_display.scroll(0, 0); }
}

void search_word(Fl_Widget*)
{
	const char* word = ui.search_bar.value();
	search_word(word);
}

void nav_back(Fl_Widget*)
{
	if (cur_cached_ind == 0)
		{ return; }
	restore_from_cache(cur_cached_ind - 1);
}
void nav_forward(Fl_Widget*)
{
	if (cur_cached_ind + 1 >= cached_defs.size())
		{ return; }
	restore_from_cache(cur_cached_ind + 1);
}

int main()
{
	http_client.set_connection_timeout(0, 500'000); // 500 ms
	http_client.set_read_timeout(2);  // 2 s
	http_client.set_write_timeout(2); // 2 s
	http_client.set_url_encode(false);

	Fl::get_system_colors();

	std::string sdict_error_msg;
	try
	{
		dict_file.open("data.sdict", false);
	}
	catch (const std::exception& e)
	{
		sdict_error_msg = e.what();
		offline_mode = false;
	}

	{
		std::ifstream fin("api_key.txt");
		if (!fin)
		{
			if (!offline_mode) // online and offline both unavailable
			{
				fl_alert(std::format("Unable to open offline dictionary data.sdict ({}) and API key not found (place key in api_key.txt). Quitting", sdict_error_msg).c_str());
				return -1;
			}
			fl_alert("API key not found (place key in api_key.txt). Using offline-only mode");
			online_mode = false;
		}
		else
			{ fin >> api_key; }
	}

	if (!offline_mode) // online-only mode
	{
		fl_alert(std::format("Unable to open offline dictionary (data.sdict): {}. Using online-only mode", sdict_error_msg).c_str());
	}
	
	ui.window.show();
	Fl::run();
}

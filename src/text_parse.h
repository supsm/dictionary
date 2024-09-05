#ifndef TEXT_PARSE_H
#define TEXT_PARSE_H

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "styles.h"
#include "links.h"

constexpr std::array<std::string_view, 40> tokens =
{ "/b", "/dx", "/dx_def", "/dx_ety", "/gloss", "/inf", "/it", "/ma", "/parahw", "/phrase", "/qword", "/sc", "/sup", "/wi",
	"a_link", "b", "bc", "d_link", "ds", "dx", "dx_def", "dx_ety", "dxt", "et_link", "gloss", "i_link", "inf", "it", "ldquo", "ma", "mat", "p_br", "parahw", "phrase", "qword", "rdquo", "sc", "sup", "sx", "wi" };

static_assert([]()
	{
		std::string_view last = "";
		for (auto s : tokens)
		{
			if (s <= last)
				{ return false; }
			last = s;
		}
		return true;
	}(), "tokens must be sorted");

consteval std::size_t token_ind(std::string_view token)
{
	for (auto [i, s] : tokens | std::views::enumerate)
	{
		if (s == token)
		{
			return i;
		}
	}
	return -1;
}

template<auto val_>
struct check_struct
{
	static_assert(val_ != -1, "token not valid");
	static constexpr auto val = val_;
};
template<auto val>
constexpr auto check = check_struct<val>::val;

// constructs a flat trie from sorted `tokens`, where all children of a node are stored sequentially.
// {0, 0} denotes the end of a list of children
// and {0, n}, where n > 0, denotes the end of a key, with n - 1 being its index in `tokens`
// @return pair of trie array and max token length
template<typename index_type = std::size_t>
constexpr auto build_trie()
{
	constexpr std::size_t max_len = []()
	{
		std::size_t max_size = 0;
		for (auto s : tokens)
		{
			max_size = std::max(max_size, s.size());
		}
		return max_size;
	}();
	constexpr std::size_t trie_size = []()
	{
		std::size_t size = 0;

		constexpr auto requal = [](std::string_view lhs, std::string_view rhs)
			{ return std::equal(lhs.rbegin(), lhs.rend(), rhs.rbegin(), rhs.rend()); };

		for (std::size_t i = 0; i < max_len; i++)
		{
			char prev = 0;
			std::string_view prev_par;
			bool added = false;
			for (auto s : tokens)
			{

				if (i != 0 && s.size() >= i && !requal(s.substr(0, i), prev_par))
				{
					prev_par = s.substr(0, i);
					if (prev != 0)
						{ size++; }
					prev = 0;

					if (s.size() == i)
						{ size++; added = true; }
				}
				if (s.size() > i && s[i] != prev)
				{
					prev = s[i];
					size++;
					added = true;
				}
			}
			if (added)
				{ size++; }
		}
		std::string_view prev_par;
		for (auto [tokens_ind, s] : tokens | std::views::enumerate)
		{
			if (s.size() == max_len && !requal(s, prev_par))
			{
				prev_par = s;
				size++;
			}
		}
		return size;
	}();
	static_assert(trie_size <= std::numeric_limits<index_type>::max(), "index type too small");
	constexpr auto t = []()
	{
		std::array<std::pair<char, index_type>, trie_size> t{};
		std::array<index_type, max_len - 1> offsets;
		std::size_t cur_ind = 0;
		
		const auto update_parent_ref = [&t, &cur_ind](std::size_t i, std::string_view s)
		{
			std::size_t start_ind = 0;
			for (std::size_t j = 0; j < i; j++)
			{
				for (std::size_t k = start_ind; k < t.size(); k++)
				{
					auto& [c, ind] = t[k];
					if (c == 0 && ind == 0)
						{ return; }
					else if (c == s[j])
					{
						if (j == i - 1)
						{
							if (ind == 0)
								{ ind = cur_ind; }
							return;
						}
						start_ind = ind;
						break;
					}
				}
			}
		};

		constexpr auto requal = [](std::string_view lhs, std::string_view rhs)
			{ return std::equal(lhs.rbegin(), lhs.rend(), rhs.rbegin(), rhs.rend()); };

		for (std::size_t i = 0; i < max_len; i++)
		{
			if (i != 0)
				{ offsets[i - 1] = cur_ind; }
			char prev = 0;
			std::string_view prev_par;
			bool added = false;
			for (auto [tokens_ind, s] : tokens | std::views::enumerate)
			{

				if (i != 0 && s.size() >= i && !requal(s.substr(0, i), prev_par))
				{
					prev_par = s.substr(0, i);
					if (prev != 0)
						{ cur_ind++; }
					update_parent_ref(i, s);
					prev = 0;

					if (s.size() == i)
					{
						t[cur_ind] = { 0, tokens_ind + 1 };
						cur_ind++;
						added = true;
					}
				}
				if (s.size() > i && s[i] != prev)
				{
					prev = s[i];
					t[cur_ind] = { prev, 0 };
					cur_ind++;
					added = true;
				}
			}
			if (added)
			{ cur_ind++; }
		}
		std::string_view prev_par;
		for (auto [tokens_ind, s] : tokens | std::views::enumerate)
		{
			if (s.size() == max_len && !requal(s, prev_par))
			{
				prev_par = s;
				update_parent_ref(max_len, s);

				t[cur_ind] = { 0, tokens_ind + 1 };
				cur_ind++;
			}
		}
		return t;
	}();
	constexpr std::string_view error_msg = [&t]() -> std::string_view
	{
		for (auto [c, i] : t)
		{
			if (c == 0)
			{
				if (i > tokens.size())
					{ return "delimiter has invalid index"; }
			}
			else
			{
				if (i == 0 || i >= t.size())
					{ return "character has invalid index"; }
				if (t[i].first == 0 && t[i].second == 0)
					{ return "child cannot be separator"; }
			}
		}
		return {};
	}();
	static_assert(error_msg == "", "invalid trie, see error_msg for more details");
	return std::make_pair(t, max_len);
}

// searches a flat trie created by `create_trie`
// @return -1 for partial match, 0 for no match, or index + 1 for full match
template<typename index_type, std::size_t size>
constexpr int search_trie(const std::array<std::pair<char, index_type>, size>& arr, std::string_view key)
{
	index_type start_ind = 0;
	for (index_type i = 0; i <= key.size(); i++)
	{
		if (i == key.size())
		{
			if (arr[start_ind].first == 0 && arr[start_ind].second > 0)
				{ return arr[start_ind].second; }
			return -1;
		}
		for (index_type j = start_ind; j < arr.size(); j++)
		{
			const auto [c, ind] = arr[j];
			if (c == 0)
			{
				if (ind == 0)
					{ return 0; }
			}
			else if (c == key[i])
			{
				start_ind = ind;
				break;
			}
			else if (c > key[i])
				{ return 0; }
		}
	}
	return 0;
}

constexpr auto tokens_data = build_trie();
constexpr auto tokens_trie = tokens_data.first;
constexpr auto max_token_len = tokens_data.second;

// @param add  callable with arguments (string_view, char) to add text with specified formatting
// @param get_pos  callable with no arguments to get current position
void parse_def_text(std::string_view text, const auto& add, const auto& get_pos)
{
	// TODO: {inf}, {p_br}, {sup}, {gloss},
	// {dx}, {dx_def}, {dx_ety}, {ma}, {dxt}, {ds}
	std::size_t start_ind = 0, brace_start = 0;
	bool in_brace = false, found_token = false;
	int last_search_res = 0;
	std::vector<std::string_view> token_fields; // fields of a token with multiple fields. first element is token itself (e.g. "a_link")

	style base_style = style::normal;
	int num_bold = 0, num_italic = 0, num_small = 0, num_allcaps = 0;
	
	const auto reset_state = [&in_brace, &found_token, &token_fields]()
		{ in_brace = false; found_token = false; token_fields.clear(); };
	
	const auto sub = [](auto& i)
		{ i = std::max<std::remove_cvref_t<decltype(i)>>(i - 1, 0); };
	
	const auto get_cur_style_mod = [&num_bold, &num_italic, &num_small]() -> unsigned char
	{
		return (num_bold > 0 ? 0b001 : 0) +
			(num_italic > 0 ? 0b010 : 0) +
			(num_small  > 0 ? 0b100 : 0);
	};

	const auto get_cur_style = [&base_style, &get_cur_style_mod]()
	{
		return get_style(base_style, get_cur_style_mod());
	};

	const auto add_rich = [&num_allcaps, &add, &get_cur_style](std::string_view str, char style = 0, bool force_caps = false)
	{
		if (style == 0)
			{ style = get_cur_style(); }
		if (num_allcaps <= 0 && !force_caps)
			{ add(str, style); } 
		else
		{
			std::string str_caps;
			str_caps.resize(str.size());
			std::transform(str.begin(), str.end(), str_caps.begin(), [](unsigned char c)
				{ return std::toupper(c); });
			add(str_caps, style);
		}
	};

	for (auto [i, c] : text | std::views::enumerate)
	{
		if (in_brace)
		{
			switch (c)
			{
			case '|':
				found_token = true;
				token_fields.emplace_back(text.substr(start_ind + 1, i - start_ind - 1));
				start_ind = i;
				break;
			case '}':
				if (last_search_res > 0) // must be full match
				{
					switch (last_search_res - 1)
					{
					case check<token_ind("bc")>:
						add(": ", get_style(style_bold));
						break;
					case check<token_ind("ldquo")>: [[fallthrough]];
					case check<token_ind("rdquo")>:
						add_rich("\"");
						break;
					case check<token_ind("b")>:
						num_bold++;
						break;
					case check<token_ind("/b")>:
						sub(num_bold);
						break;
					case check<token_ind("wi")>: [[fallthrough]];
					case check<token_ind("qword")>: [[fallthrough]];
					case check<token_ind("it")>:
						num_italic++;
						break;
					case check<token_ind("/wi")>: [[fallthrough]];
					case check<token_ind("/qword")>: [[fallthrough]];
					case check<token_ind("/it")>:
						sub(num_italic);
						break;
					case check<token_ind("sc")>:
						num_small++;
						num_allcaps++;
						break;
					case check<token_ind("/sc")>:
						sub(num_small);
						sub(num_allcaps);
						break;
					case check<token_ind("phrase")>:
						num_bold++;
						num_italic++;
						break;
					case check<token_ind("/phrase")>:
						sub(num_bold);
						sub(num_italic);
						break;
					case check<token_ind("parahw")>:
						num_bold++;
						num_small++;
						num_allcaps++;
						break;
					case check<token_ind("/parahw")>:
						sub(num_bold);
						sub(num_small);
						sub(num_allcaps);
						break;
					
					case check<token_ind("a_link")>:
						if (token_fields.size() != 1)
							{ start_ind = brace_start; break; }
						{
							const auto cur_pos = get_pos();
							std::string_view target = text.substr(start_ind + 1, i - start_ind - 1); // last token field
							links.emplace_back<link_bounds, std::string>({ cur_pos, cur_pos + static_cast<int>(target.size()) }, std::string(target));
						}
						add_rich(text.substr(start_ind + 1, i - start_ind - 1), get_style(style::link));
						break;
					case check<token_ind("d_link")>:
						if (token_fields.size() != 2)
							{ start_ind = brace_start; break; }
						{
							const auto cur_pos = get_pos();
							std::string_view target = text.substr(start_ind + 1, i - start_ind - 1);
							if (target.empty())
								{ target = token_fields[1]; }
							links.emplace_back<link_bounds, std::string>({ cur_pos, cur_pos + static_cast<int>(target.size()) }, std::string(target));
						}
						add_rich(token_fields[1], get_style(style::link));
						break;
					case check<token_ind("i_link")>:
						if (token_fields.size() != 2)
							{ start_ind = brace_start; break; }
						{
							const auto cur_pos = get_pos();
							std::string_view target = text.substr(start_ind + 1, i - start_ind - 1);
							if (target.empty())
								{ target = token_fields[1]; }
							links.emplace_back<link_bounds, std::string>({ cur_pos, cur_pos + static_cast<int>(target.size()) }, std::string(target));
						}
						add_rich(token_fields[1], get_style(style::link, style_italic));
						break;
					case check<token_ind("et_link")>:
						if (token_fields.size() != 2)
							{ start_ind = brace_start; break; }
						{
							const auto cur_pos = get_pos();
							std::string_view target = text.substr(start_ind + 1, i - start_ind - 1);
							if (target.empty())
								{ target = token_fields[1]; }
							links.emplace_back<link_bounds, std::string>({ cur_pos, cur_pos + static_cast<int>(target.size()) }, std::string(target));
						}
						add_rich(token_fields[1], get_style(style::link, style_small), true);
						break;
					case check<token_ind("mat")>:
						if (token_fields.size() != 2)
							{ start_ind = brace_start; break; }
						{
							const auto cur_pos = get_pos();
							std::string_view target = text.substr(start_ind + 1, i - start_ind - 1);
							if (target.empty())
								{ target = token_fields[1]; }
							links.emplace_back<link_bounds, std::string>({ cur_pos, cur_pos + static_cast<int>(target.size()) }, std::string(target));
						}
						add_rich(token_fields[1], get_style(style::link, style_small), true);
						break;
					case check<token_ind("sx")>:
						if (token_fields.size() != 3)
							{ start_ind = brace_start; break; }
						{
							const auto cur_pos = get_pos();
							std::string_view target = token_fields[2];
							if (target.empty())
								{ target = token_fields[1]; }
							links.emplace_back<link_bounds, std::string>({ cur_pos, cur_pos + static_cast<int>(target.size()) }, std::string(target));
						}
						add_rich(token_fields[1], get_style(style::link, style_small), true);
						break;
					/*case check<token_ind("dxt")>:
						if (token_fields.size() != 4)
							{ start_ind = brace_start; break; }
						add_rich(token_fields[1], get_style(style::link, style_small), true);
						break;*/
						
					default:
						start_ind = brace_start;
						break;
					}
					// TODO: sometimes we don't want to add 1
					start_ind = i + 1; // consume token and braces
				}
				else
				{
					start_ind = brace_start;
				}
				reset_state();
				break;
			default:
				if (!found_token)
				{
					if (i - start_ind > max_token_len)
						{ reset_state(); }

					auto search_res = search_trie(tokens_trie, text.substr(start_ind + 1, i - start_ind));
					last_search_res = search_res;
					if (search_res == 0) // partial or full match ok
						{ reset_state(); }
				}
				break;
			}
		}

		if (!in_brace && c == '{')
		{
			in_brace = true;
			last_search_res = 0;
			if (i != start_ind)
				{ add_rich(text.substr(start_ind, i - start_ind)); }
			start_ind = i;
			brace_start = i;
		}
	}
	if (start_ind != text.size() - 1)
	{
		add_rich(text.substr(start_ind));
	}
}

#endif

#ifndef DICT_PARSE_H
#define DICT_PARSE_H

#include <stdexcept>
#include <vector>

#include "co_util.h"
#include "dict_def.h"
#include "json_coro_cursor.h"
#include "json_util.h"

// parse the `meta` field for `id`, `stems`, `offensive`
// expects cursor to be at begin_object of `meta`, but does not validate
// leaves cursor after end_object
task<void> parse_meta(json_coro_cursor& cursor, word_info& data)
{
	using json_type = jsoncons::staj_event_type;
	CO_CALL(cursor.next_); // consume begin object
	
	const json_obj_callbacks<3> meta_callbacks =
    { {
        { "id", json_type::string_value, [&cursor, &data]() -> task<void> { data.id = cursor.current().get<std::string>(); CO_CALL(cursor.next_); } },
        { "stems", json_type::begin_array, [&cursor, &data]() -> task<void>
			{
				CO_CALL(cursor.next_); // skip begin array
				for (; !cursor.done();)
				{
					const auto& cur_event = cursor.current();
					if (cur_event.event_type() == json_type::end_array)
						{ CO_CALL(cursor.next_); co_return; }
					data.stems.push_back(cur_event.get<std::string>());
					
					CO_CALL(cursor.next_);
				}
			} },
        { "offensive", json_type::bool_value, [&cursor, &data]() -> task<void> { data.offensive = cursor.current().get<bool>(); CO_CALL(cursor.next_); } }
    } };
	
	CO_WHILE(recursive_skip_until_obj, cursor, meta_callbacks) /* { */ }
}

task<void> parse_sdsense(json_coro_cursor& cursor, word_info& data)
{
	using json_type = jsoncons::staj_event_type;
	CO_CALL(cursor.next_); // consume begin object
	auto& this_sense = std::get<sense_data>(data.defs.back()).sdsense;
	this_sense = div_sense_data(); // only full `sense` contains sdsense
	
	// TODO: merge this with parse_sense to avoid duplication ?
	const json_obj_callbacks<3> sense_callbacks =
	{ {
		{ "sd", json_type::string_value, [&this_sense, &cursor]() -> task<void>
			{
				this_sense.value().sense_div = cursor.current().get<std::string>();
				CO_CALL(cursor.next_);
			} },
		{ "sn", json_type::string_value, [&this_sense, &cursor]() -> task<void>
			{
				this_sense.value().number = cursor.current().get<std::string>();
				CO_CALL(cursor.next_);
			} },
		{ "dt", json_type::begin_array, [&this_sense, &cursor]() -> task<void>
			{
				CO_CALL(cursor.next_); // consume begin array
				bool val;
				CO_CALL(recursive_skip_until_key_arr, cursor, "text") >> val;
				if (val)
				{
					this_sense.value().def_text = cursor.current().get<std::string>();
					CO_CALL(recursive_skip, cursor); // exit "text" array
					CO_CALL(recursive_skip, cursor); // exit dt array
				}
			} },
	} };
	
	CO_WHILE(recursive_skip_until_obj, cursor, sense_callbacks) /* { */ }
}

// parse a `sense` or `sen` object
// expects cursor to be after key, but does not validate
// leaves cursor after end_object
template<bool is_trunc = false>
task<void> parse_sense(json_coro_cursor& cursor, word_info& data)
{
	using json_type = jsoncons::staj_event_type;
	CO_CALL(cursor.next_); // consume begin object
	
	using sense_type = std::conditional_t<is_trunc, trunc_sense_data, sense_data>;
	data.defs.push_back(sense_type()); // new (truncated) sense
	
	json_obj_callbacks<1> basic_sense_callbacks =
	{ {
		{ "sn", json_type::string_value, [&cursor, &data]() -> task<void>
			{
				std::get<sense_type>(data.defs.back()).number = cursor.current().get<std::string>();
				CO_CALL(cursor.next_);
			} }
	} };
	
	if constexpr (!is_trunc)
	{
		constexpr std::size_t sense_only_callbacks_size = 2, size2 = basic_sense_callbacks.size();
		
		json_obj_callbacks<sense_only_callbacks_size + size2> sense_callbacks =
		{ {
			{ "dt", json_type::begin_array, [&cursor, &data]() -> task<void>
				{
					CO_CALL(cursor.next_); // consume begin array
					bool val;
					CO_CALL(recursive_skip_until_key_arr, cursor, "text") >> val;
					if (val)
					{
						std::get<sense_data>(data.defs.back()).def_text = cursor.current().get<std::string>();
						CO_CALL(recursive_skip, cursor); // exit "text" array
						CO_CALL(recursive_skip, cursor); // exit dt array
					}
				} },
			{ "sdsense", json_type::begin_object, CO_BIND_VOID(parse_sdsense, cursor, data) }
		} };
		
		std::move(basic_sense_callbacks.begin(), basic_sense_callbacks.end(),
			sense_callbacks.begin() + sense_only_callbacks_size);
		
		CO_WHILE(recursive_skip_until_obj, cursor, sense_callbacks) /* { */ }
	}
	else
	{
		CO_WHILE(recursive_skip_until_obj, cursor, basic_sense_callbacks) /* { */ }
	}
}

constexpr auto& parse_sen = parse_sense<true>;

task<void> parse_bs(json_coro_cursor& cursor, word_info& data)
{
	CO_CALL(cursor.next_); // consume begin object (bs)
	CO_CALL(cursor.next_); // consume "sense" key
	CO_CALL(parse_sense, cursor, data);
	CO_CALL(recursive_skip, cursor); // consume end object (bs)
	CO_CALL(recursive_skip, cursor); // consume sub-array (in pseq)
}

task<void> parse_pseq(json_coro_cursor& cursor, word_info& data)
{
	CO_CALL(cursor.next_); // consume begin array
	
	const json_key_arr_callbacks<2> pseq_callbacks =
	{ {
		{ "sense", [&cursor, &data]() -> task<void>
			{
				CO_CALL(parse_sense, cursor, data);
				CO_CALL(recursive_skip, cursor); // consume sub-array
			} },
		{ "bs", CO_BIND_VOID(parse_bs, cursor, data) }
	} };
	
	CO_WHILE(recursive_skip_until_key_arr, cursor, pseq_callbacks) /* { */ }
	
	CO_CALL(recursive_skip, cursor); // TODO: why is this necessary? shouldn't it already be consumed?
}

// parse an array element from `sseq`
// which contains `sense`, `sen`, `pseq`, `bs`
// expects cursor to be at begin_array, but does not validate
// leaves cursor after end_array
task<void> parse_sseq_element(json_coro_cursor& cursor, word_info& data)
{
	CO_CALL(cursor.next_); // consume begin array
	
	const json_key_arr_callbacks<4> sseq_callbacks =
	{ {
		{ "sense", [&cursor, &data]() -> task<void>
			{
				CO_CALL(parse_sense, cursor, data);
				CO_CALL(recursive_skip, cursor); // consume sub-array
			} },
		{ "sen", [&cursor, &data]() -> task<void>
			{
				CO_CALL(parse_sen, cursor, data);
				CO_CALL(recursive_skip, cursor); // consume sub-array
			} },
		{ "pseq", CO_BIND_VOID(parse_pseq, cursor, data) },
		{ "bs", CO_BIND_VOID(parse_bs, cursor, data) }
	} };
	
	CO_WHILE(recursive_skip_until_key_arr, cursor, sseq_callbacks) /* { */ }
}

// parse a `sseq` array from a def object
// expects cursor to be at begin_array, but does not validate
// leaves cursor after end_array
task<void> parse_sseq(json_coro_cursor& cursor, word_info& data)
{
	CO_CALL(cursor.next_); // consume begin array
	
	// parse array
	using json_type = jsoncons::staj_event_type;
	CO_WHILE(recursive_skip_until_arr, cursor, json_type::begin_array)
	// {
		CO_CALL(parse_sseq_element, cursor, data);
	}
}

// parse a single object from the `def` array
// expects cursor to be at begin_object, but does not validate
// leaves cursor after end_object
task<void> parse_single_def(json_coro_cursor& cursor, word_info& data)
{
	using json_type = jsoncons::staj_event_type;
	CO_CALL(cursor.next_); // consume begin object
	
	const json_obj_callbacks<1> def_callbacks =
	{ {
		{ "sseq", json_type::begin_array, CO_BIND_VOID(parse_sseq, cursor, data) }
	} };
	
	CO_WHILE(recursive_skip_until_obj, cursor, def_callbacks) /* { */ }
}

// parse the `def` field
// expects cursor to be at begin_array of `def`, but does not validate
// leaves cursor after end_array
task<void> parse_def(json_coro_cursor& cursor, word_info& data)
{
	using json_type = jsoncons::staj_event_type;
	CO_CALL(cursor.next_); // consume begin array
	
	// parse array
	const json_arr_callbacks<1> def_callbacks =
	{ {
		{ json_type::begin_object, CO_BIND_VOID(parse_single_def, cursor, data) }
	} };
	
	CO_WHILE(recursive_skip_until_arr, cursor, def_callbacks) /* { */ }
}

task<void> begin_parse(json_coro_cursor& cursor, std::vector<word_info>& data)
{
	CO_CALL(cursor.init);
	
	using json_type = jsoncons::staj_event_type;
		
	// TODO: detect "invalid key" condition
	if (cursor.current().event_type() != json_type::begin_array)
		{ throw std::runtime_error("Definition does not begin with an array"); }
	CO_CALL(cursor.next_); // consume begin array
	
	switch (cursor.current().event_type())
	{
	case json_type::begin_object:
		break;
	case json_type::string_value:
		// TODO: "no word found" condition
		throw std::runtime_error("No word found. Possible alternatives: ");
	default:
		throw std::runtime_error("Expected word definition object");
	}
	
	CO_WHILE(recursive_skip_until_arr, cursor, json_type::begin_object)
	// {
		data.emplace_back();
		CO_CALL(cursor.next_); // consume begin object

		const json_obj_callbacks<2> root_callbacks =
		{ {
			{ "meta", json_type::begin_object, CO_BIND_VOID(parse_meta, cursor, data.back()) },
			{ "def", json_type::begin_array, CO_BIND_VOID(parse_def, cursor, data.back()) }
		} };
		
		CO_WHILE(recursive_skip_until_obj, cursor, root_callbacks) /* { */ }
	}
}

#endif

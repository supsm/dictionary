#ifndef JSON_UTIL_H
#define JSON_UTIL_H

#include <array>
#include <concepts>
#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>
#include <jsoncons/json.hpp>

#include "co_util.h"
#include "json_coro_cursor.h"

template<typename T>
concept JsonObjectCondition = requires(T f, jsoncons::staj_event e, std::string_view s)
{
	{ f(e, s) } -> std::same_as<task<bool>>;
};
template<typename T>
concept JsonArrayCondition = requires(T f, jsoncons::staj_event e)
{
	{ f(e) } -> std::same_as<task<bool>>;
};
template<typename T>
concept JsonKeyArrayCondition = requires(T f, std::string_view s)
{
	{ f(s) } -> std::same_as<task<bool>>;
};

// array type that contains key and type information for json object callbacks
template<std::size_t size>
using json_obj_callbacks = std::array<std::tuple<std::string_view, jsoncons::staj_event_type, std::function<task<void>()>>, size>;

// array type that contains type information for json array callbacks
template<std::size_t size>
using json_arr_callbacks = std::array<std::tuple<jsoncons::staj_event_type, std::function<task<void>()>>, size>;

// array type that contains key information for json array callbacks
// where every element is a sub-array beginning with a string key
template<std::size_t size>
using json_key_arr_callbacks = std::array<std::tuple<std::string_view, std::function<task<void>()>>, size>;

// skip events until current object/array is consumed
// expects cursor to have consumed the begin obj/arr
// leaves cursor after the end obj/arr
task<void> recursive_skip(coro_cursor auto& cursor)
{
	int num_levels = 0;
	for (; !cursor.done();)
	{
		using json_type = jsoncons::staj_event_type;
		const auto& cur_event = cursor.current();
		
		switch (cur_event.event_type())
		{
		case json_type::begin_array: [[fallthrough]];
		case json_type::begin_object:
			num_levels++;
			break;
		case json_type::end_array: [[fallthrough]];
		case json_type::end_object:
			num_levels--;
			break;
		}
		
		if (num_levels < 0) // end array/object is consumed
			{ CO_CALL(cursor.next_); break; }
		
		CO_CALL(cursor.next_);
	}
}

// skip events until a condition is met, or entire json object is consumed
// does not verify json and assumes all arrays/objects are started and ended correctly
// expects cursor to be before a key, in an object
// leaves cursor directly BEFORE value (key is consumed but value is not)
// or AFTER end object if entire object is consumed
// @param condition  callable that accepts a jsoncons event and key (string_view) and returns true to end
// @return false if entire object is consumed, true if condition is met
task<bool> recursive_skip_until_obj(coro_cursor auto& cursor, JsonObjectCondition auto condition)
{
	std::string last_key;
	int num_levels = 0;
	for (; !cursor.done();)
	{
		using json_type = jsoncons::staj_event_type;
		const auto& cur_event = cursor.current();
		
        // TODO: filter?
		if (num_levels == 0)
		{
			if (cur_event.event_type() == json_type::key)
			{
				// last_key needs to be a copy here as cur_event could change (which would change the contents of a string_view)
				last_key = cur_event.get<std::string>();
			}
			else
			{
				bool val;
				CO_CALL(condition, cur_event, last_key) >> val;
				if (val)
				{
					co_return true;
				}
				else
				{
					last_key = {};
				}
			}
		}
		
		switch (cur_event.event_type())
		{
		case json_type::begin_array: [[fallthrough]];
		case json_type::begin_object:
			num_levels++;
			break;
		case json_type::end_array: [[fallthrough]];
		case json_type::end_object:
			num_levels--;
			break;
		}
		
		if (num_levels < 0) // end array/object is consumed
			{ CO_CALL(cursor.next_); break; }
		
		CO_CALL(cursor.next_);
	}
	co_return false;
}

// skip events until a desired field is found on the current level, or entire json object is consumed
// see `condition` overload
task<bool> recursive_skip_until_obj(coro_cursor auto& cursor, std::string_view key, jsoncons::staj_event_type event_type)
{
	bool val;
	CO_CALL(recursive_skip_until_obj, cursor, [event_type, key](const auto& cur_event, std::string_view last_key) -> task<bool>
		{ co_return cur_event.event_type() == event_type && last_key == key; }) >> val;
	co_return val;
}

// skip events until any one of many desired fields is found on the current level, or entire json object is consumed
// @param callbacks  array of {key, event type, callback}; callback is called when key and event type matches.
//                   each key and event type should be unique, and the callback should consume the value fully
// see `condition` overload
template<std::size_t size>
task<bool> recursive_skip_until_obj(coro_cursor auto& cursor, const json_obj_callbacks<size>& callbacks)
{
	bool val;
	CO_CALL(recursive_skip_until_obj, cursor, [&callbacks](const auto& cur_event, std::string_view last_key) -> task<bool>
		{
			for (const auto& [key, event_type, callback] : callbacks)
			{
				if (cur_event.event_type() == event_type && last_key == key)
				{
					CO_CALL(callback);
					co_return true;
				}
			}
			co_return false;
		}) >> val;
	co_return val;
}

// skip events until a condition is met, or until entire json array is consumed
// does not verify json and assumes all arrays/objects are started and ended correctly
// expects cursor to be before an element, in an array
// leaves cursor directly BEFORE desired element
// or AFTER end array if entire array is consumed
// @param condition  callable that accepts a jsoncons event and returns true to end
// @return false if entire array is consumed, true if condition is met
task<bool> recursive_skip_until_arr(coro_cursor auto& cursor, JsonArrayCondition auto condition)
{
	int num_levels = 0;
	for (; !cursor.done();)
	{
		using json_type = jsoncons::staj_event_type;
		const auto& cur_event = cursor.current();
		
		if (num_levels == 0)
		{
			bool val;
			CO_CALL(condition, cur_event) >> val;
			if (val)
			{
				co_return true;
			}
		}
		
		switch (cur_event.event_type())
		{
		case json_type::begin_array: [[fallthrough]];
		case json_type::begin_object:
			num_levels++;
			break;
		case json_type::end_array: [[fallthrough]];
		case json_type::end_object:
			num_levels--;
			break;
		}
		
		if (num_levels < 0) // end array is consumed
			{ CO_CALL(cursor.next_); break; }
		
		CO_CALL(cursor.next_);
	}
	co_return false;
}

// skip events until a desired type is found on the current level, or entire json array is consumed
// see `condition` overload
task<bool> recursive_skip_until_arr(coro_cursor auto& cursor, jsoncons::staj_event_type event_type)
{
	bool val;
	CO_CALL(recursive_skip_until_arr, cursor, [event_type](const auto& cur_event) -> task<bool>
		{ co_return cur_event.event_type() == event_type; }) >> val;
	co_return val;
}

// skip events until one of many desired types is found on the current level, or entire json array is consumed
// @param callbacks  array of {event type, callback}; callback is called when event type matches.
//                   each event type should be unique, and the callback should consume the element fully
// see `condition` overload
template<std::size_t size>
task<bool> recursive_skip_until_arr(coro_cursor auto& cursor, const json_arr_callbacks<size>& callbacks)
{
	bool val;
	CO_CALL(recursive_skip_until_arr, cursor, [&callbacks](const auto& cur_event) -> task<bool>
		{
			for (const auto& [event_type, callback] : callbacks)
			{
				if (cur_event.event_type() == event_type)
				{
					CO_CALL(callback);
					co_return true;
				}
			}
			co_return false;
		}) >> val;
	co_return val;
}

// skip elements in an array (main) until a condition is met regarding the first value of a sub-array iff it is a string ("key"),
// or until entire main array is consumed
// does not verify json and assumes all arrays/objects are started and ended correctly
// expects cursor to be before an element in main array (i.e. after begin_array of main array)
// leaves cursor directly AFTER string "key" in sub-array if `consume_after_cond`,
// BEFORE string "key" if not `consume_after_cond`,
// or AFTER end of main array if entire array is consumed
// @param condition  callable that accepts a jsoncons event and returns true to end
// @return false if entire main array is consumed, true if condition is met
template<bool consume_after_cond = true>
task<bool> recursive_skip_until_key_arr(coro_cursor auto& cursor, JsonKeyArrayCondition auto condition)
{
	using json_type = jsoncons::staj_event_type;
	CO_WHILE(recursive_skip_until_arr, cursor, json_type::begin_array)
	// {
		CO_CALL(cursor.next_); // consume begin array
		const auto& cur_event = cursor.current();
		if (cur_event.event_type() == json_type::string_value)
		{
			bool val;
			CO_CALL(condition, cur_event.get<std::string_view>()) >> val;
			if (val)
			{
				if constexpr (consume_after_cond)
				{
					CO_CALL(cursor.next_); // consume key string
				}
				co_return true;
			}
		}
		// consume sub-array
		CO_CALL(recursive_skip, cursor);
	}
	co_return false;
}

// skip events until a sub-array is found containing `key` as its first element, or entire json array is consumed
// see `condition` overload
task<bool> recursive_skip_until_key_arr(coro_cursor auto& cursor, std::string_view key)
{
	bool val;
	CO_CALL(recursive_skip_until_key_arr, cursor, [key](std::string_view cur_key) -> task<bool>
		{ co_return cur_key == key; }) >> val;
	co_return val;
}

// skip events until one of many desired "keys" is found on the current level, or entire json array is consumed
// @param callbacks  array of {key, callback}; callback is called when key matches first element of a sub-array.
//                   each key should be unique, and the callback should consume the sub-array
// see `condition` overload
template<std::size_t size>
task<bool> recursive_skip_until_key_arr(coro_cursor auto& cursor, const json_key_arr_callbacks<size>& callbacks)
{
	// placing this within CO_CALL below causes
	// compiler confusion with __LINE__
	const auto callback = [&callbacks, &cursor](std::string_view cur_key) -> task<bool>
	{
		for (const auto& [key, callback] : callbacks)
		{
			if (key == cur_key)
			{
				CO_CALL(cursor.next_); // consume key string (don't consume after condition)
				CO_CALL(callback);
				co_return true;
			}
		}
		co_return false;
	};

	bool val;
	CO_CALL(recursive_skip_until_key_arr<false>, cursor, callback) >> val;
	co_return val;
}

#endif

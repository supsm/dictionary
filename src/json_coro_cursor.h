// code based on danielaparker/jsoncons/include/jsoncons/json_cursor.hpp
// Copyright 2013-2024 Daniel Parker
// Distributed under the Boost license, Version 1.0.
// (See http://www.boost.org/LICENSE_1_0.txt)

#ifndef JSON_CORO_CURSOR_H
#define JSON_CORO_CURSOR_H

#include <jsoncons/json.hpp>
#include <jsoncons/staj_cursor.hpp>
#include <jsoncons/json_parser.hpp>
#include <jsoncons/json_visitor.hpp>

#include "co_util.h"

#include <concepts>

template<typename T>
concept coro_cursor =
requires(T cursor,
	jsoncons::basic_json_visitor<typename T::char_type> vis,
	std::error_code ec,
	std::function<bool(const jsoncons::basic_staj_event<typename T::char_type>&, const jsoncons::ser_context&)> pred)
{
	{ cursor.init() } -> std::same_as<task<void>>;
	{ cursor.reset() } -> std::same_as<void>;
	{ cursor.read_to_(vis) } -> std::same_as<task<void>>;
	{ cursor.read_to_(vis, ec) } -> std::same_as<task<void>>;
	{ cursor.next_() } -> std::same_as<task<void>>;
	{ cursor.next_(ec) } -> std::same_as<task<void>>;
	{ cursor | pred } -> std::same_as<jsoncons::basic_staj_filter_view<typename T::char_type>>;
} &&
requires(const T cursor)
{
	{ cursor.done() } -> std::same_as<bool>;
	{ cursor.current() } -> std::same_as<const jsoncons::basic_staj_event<typename T::char_type>&>;
	{ cursor.context() } -> std::same_as<const jsoncons::ser_context&>;
};

namespace jsoncons
{
	// modified json cursor using coroutines
	// will only parse and create events when updated
	// via task::add_data
	template<typename CharT, typename Allocator = std::allocator<CharT>>
	class basic_json_coro_cursor : public basic_staj_cursor<CharT>, private virtual ser_context
	{
	public:
		using char_type = CharT;
		using allocator_type = Allocator;
	
	private:
		basic_json_parser<CharT, Allocator> parser_;
		basic_staj_visitor<CharT> cursor_visitor_;
		bool done_;
		
		// Noncopyable and nonmoveable
		basic_json_coro_cursor(const basic_json_coro_cursor&) = delete;
		basic_json_coro_cursor& operator=(const basic_json_coro_cursor&) = delete;
		
		static bool accept_all(const basic_staj_event<CharT>&, const ser_context&) 
		{
			return true;
		}

	public:
		basic_json_coro_cursor(const basic_json_decode_options<CharT>& options = basic_json_decode_options<CharT>(),
			std::function<bool(json_errc, const ser_context&)> err_handler = default_json_parsing(),
			const Allocator& alloc = Allocator())
			: parser_(options, err_handler, alloc), cursor_visitor_(accept_all), done_(false) {}
		
		task<void> init()
		{
			if (!done())
			{
				CO_CALL(next_);
			}
		}
		
		void reset()
		{
			parser_.reset();
			cursor_visitor_.reset();
			done_ = false;
		}
		
		bool done() const override
		{
			return parser_.done() || done_;
		}

		const basic_staj_event<CharT>& current() const override
		{
			return cursor_visitor_.event();
		}

private:
		void read_to(basic_json_visitor<CharT>&) override {}
		void read_to(basic_json_visitor<CharT>&, std::error_code&) override {}
	
public:
		task<void> read_to_(basic_json_visitor<CharT>& visitor)
		{
			std::error_code ec;
			CO_CALL(read_to_, visitor, ec);
			if (ec)
			{
				JSONCONS_THROW(ser_error(ec,parser_.line(),parser_.column()));
			}
		}

		task<void> read_to_(basic_json_visitor<CharT>& visitor, std::error_code& ec)
		{
			if (send_json_event(cursor_visitor_.event(), visitor, *this, ec))
			{
				CO_CALL(read_next, visitor, ec);
			}
		}

private:
		// use next_ instead
		void next() override {}
		// use next_ instead
		void next(std::error_code&) override {}

public:
		task<void> next_()
		{
			std::error_code ec;
			CO_CALL(next_, ec);
			if (ec)
			{
				JSONCONS_THROW(ser_error(ec,parser_.line(),parser_.column()));
			}
		}

		task<void> next_(std::error_code& ec)
		{
			CO_CALL(read_next, ec);
		}

		bool source_exhausted() const
		{
			return parser_.source_exhausted();
		}

		const ser_context& context() const override
		{
			return *this;
		}
		
		std::size_t line() const override
		{
			return parser_.line();
		}

		std::size_t column() const override
		{
			return parser_.column();
		}

		friend basic_staj_filter_view<CharT> operator|(basic_json_coro_cursor& cursor, 
			std::function<bool(const basic_staj_event<CharT>&, const ser_context&)> pred)
		{
			return basic_staj_filter_view<CharT>(cursor, pred);
		}
		
	private:
		task<void> read_next()
		{
			std::error_code ec;
			CO_CALL(read_next, cursor_visitor_, ec);
			if (ec)
			{
				JSONCONS_THROW(ser_error(ec,parser_.line(),parser_.column()));
			}
		}

		task<void> read_next(std::error_code& ec)
		{
			CO_CALL(read_next, cursor_visitor_, ec);
		}

		task<void> read_next(basic_json_visitor<CharT>& visitor, std::error_code& ec)
		{
			parser_.restart();
			while (!parser_.stopped())
			{
				if (parser_.source_exhausted())
				{
					auto s = co_await std::string_view{};
					parser_.update(s.data(),s.size());
					if (ec) co_return;
				}
				parser_.parse_some(visitor, ec);
				if (ec) co_return;
				if (parser_.done())
				{
					done_ = true;
					co_return;
				}
			}
		}
	};
}

using json_coro_cursor = jsoncons::basic_json_coro_cursor<char>;

// wraps a regular cursor in a coroutine-friendly interface
// THE CURSOR WILL NOT BE SUSPENDABLE/RESUMABLE
// cursor can be constructed in-place; constructor args for this class
// are forwarded directly to cursor constructor
template<typename CursorT>
class cursor_coro_wrapper : public jsoncons::basic_staj_cursor<typename CursorT::char_type>
{
public:
	using char_type = CursorT::char_type;

private:
	CursorT cursor;

public:
	template<typename... Args>
	cursor_coro_wrapper(Args&&... args) : cursor(std::forward<Args>(args)...) {}

	task<void> init() { co_return; }
	void reset() { cursor.reset(); }
	bool done() const override { return cursor.done(); }
	const jsoncons::basic_staj_event<char_type>& current() const override { return cursor.current(); }
	task<void> read_to_(jsoncons::basic_json_visitor<char_type>& vis) { cursor.read_to(vis); co_return; }
	task<void> read_to_(jsoncons::basic_json_visitor<char_type>& vis, std::error_code& ec) { cursor.read_to(vis, ec); co_return; }
	task<void> next_() { cursor.next(); co_return; }
	task<void> next_(std::error_code& ec) { cursor.next(ec); co_return; }
	const jsoncons::ser_context& context() const override { return cursor.context(); }
	friend jsoncons::basic_staj_filter_view<char_type> operator|(cursor_coro_wrapper& cursor,
		std::function<bool(const jsoncons::basic_staj_event<char_type>&, const jsoncons::ser_context&)> pred)
		{ return operator|(cursor.cursor, pred); }
private:
	void next() override {}
	void next(std::error_code& ec) override {}
	void read_to(jsoncons::basic_json_visitor<char_type>&) override {}
	void read_to(jsoncons::basic_json_visitor<char_type>&, std::error_code&) override {}
};

#endif

#ifndef CO_UTIL_H
#define CO_UTIL_H

#include <coroutine>
#include <string_view>
#include <utility>

template<typename T>
struct promise_type;

template<typename T = void>
struct task
{
	using promise_type = promise_type<T>;

	using handle = std::coroutine_handle<promise_type>;
	handle coro_handle;

	constexpr explicit task(promise_type* p) : coro_handle(handle::from_promise(*p)) {}
	constexpr task(task&& rhs) : coro_handle(std::move(rhs.coro_handle)) {}

	constexpr ~task()
	{
		if (coro_handle)
			{ coro_handle.destroy(); }
	}

	constexpr void add_data(std::string_view msg)
	{
		coro_handle.promise().data_in = msg;
		if (!coro_handle.done())
		{
			coro_handle.resume();
		}
	}
};

template<typename Derived, typename T>
struct basic_promise_type
{
	std::string_view data_in;
	void unhandled_exception() { throw; }
	constexpr task<T> get_return_object() { return task<T>(static_cast<Derived*>(this)); }
	constexpr std::suspend_never initial_suspend() noexcept { return {}; }
	constexpr std::suspend_always final_suspend() noexcept { return {}; }
	constexpr auto await_transform(std::string_view) noexcept
	{
		struct awaiter
		{
			promise_type<T>& p;
			constexpr bool await_ready() const noexcept { return !p.data_in.empty(); }
			constexpr std::string_view await_resume() const noexcept { return std::exchange(p.data_in, {}); }
			constexpr void await_suspend(std::coroutine_handle<>) const noexcept {}
		};
		return awaiter(*static_cast<Derived*>(this));
	}
	template<typename T2>
	constexpr auto await_transform(task<T2>& t)
	{
		struct awaiter
		{
			bool data_empty;
			constexpr bool await_ready() const noexcept { return !data_empty; }
			constexpr void await_resume() const noexcept {}
			constexpr void await_suspend(std::coroutine_handle<>) const noexcept {}
		};
		bool data_empty = data_in.empty();
		if (!data_empty)
			{ t.add_data(std::exchange(data_in, {})); }
		return awaiter(data_empty);
	}
};

template<typename T>
struct promise_type : basic_promise_type<promise_type<T>, T>
{
	T data_out;
	constexpr void return_value(const T& val) noexcept { data_out = val; }
};

template<>
struct promise_type<void> : basic_promise_type<promise_type<void>, void>
{
	void* data_out; // placeholder
	constexpr void return_void() const noexcept {}
};

// wrapper for right-hand assignment
// stores a value internally; use `obj >> var`
// to assign obj.val to var
template<typename T>
class rha_wrapper
{
private:
	T val;

public:
	constexpr rha_wrapper(const T& val_) : val(val_) {}

	constexpr void operator>>(auto& other) { other = val; }
};

#define CONCAT_(lhs, rhs) lhs##rhs
#define CONCAT(lhs, rhs) CONCAT_(lhs, rhs)

// call `func` with params
// return type can be accessed with rha_wrapper::operator>>
#define CO_CALL(func, ...) \
decltype(func(__VA_ARGS__).coro_handle.promise().data_out) CONCAT(detail_t_ret_, __LINE__); \
{ auto t = func(__VA_ARGS__); \
while (!t.coro_handle.done()) { co_await t; } \
CONCAT(detail_t_ret_, __LINE__) = t.coro_handle.promise().data_out; } \
rha_wrapper(std::move(CONCAT(detail_t_ret_, __LINE__)))

// TODO: CO_IF (using c++17 if init statement)

// use return value of `func` as the condition of a while loop
// OPEN BRACE IS APPLIED IN MACRO AND NEEDS TO BE CLOSED
// Example usage:
// @code
// CO_WHILE(foo, arg1, arg2, arg3)
//     loop_body
// }
// @endcode
// note: it's possible to make this accept a tuple-like structure for args
//       and have the loop body as another macro arg (such that the closing
//       paren of the macro denotes loop end), but that makes debugging a pain
#define CO_WHILE(func, ...) \
while (true) { \
decltype(func(__VA_ARGS__).coro_handle.promise().data_out) CONCAT(detail_t_ret_, __LINE__); \
{ auto t = func(__VA_ARGS__); \
while (!t.coro_handle.done()) { co_await t; } \
CONCAT(detail_t_ret_, __LINE__) = t.coro_handle.promise().data_out; } \
if (!CONCAT(detail_t_ret_, __LINE__)) { break; }

#define CO_BIND_VOID(func, ...) \
[&]() -> task<void> { CO_CALL(func, __VA_ARGS__); }

#endif

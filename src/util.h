#ifndef UTIL_H
#define UTIL_H

#include <algorithm>

#include <FL/Fl.H>
#include <FL/Fl_Text_Buffer.H>

template<std::size_t size>
struct string_literal_wrapper
{
	char data[size];
	consteval string_literal_wrapper(const char (&str)[size])
		{ std::copy_n(str, size, data); }
};

template<string_literal_wrapper key, typename T>
struct Fl_Text_Buffer_m
{
	using type = T Fl_Text_Buffer::*;
	friend type get(Fl_Text_Buffer_m);
};

template<typename Tag, Tag::type M>
struct access_helper
{
	friend Tag::type get(Tag)
		{ return M; }
};

template struct access_helper<Fl_Text_Buffer_m<"mBuf", char*>, &Fl_Text_Buffer::mBuf>;
template struct access_helper<Fl_Text_Buffer_m<"mLength", int>, &Fl_Text_Buffer::mLength>;
template struct access_helper<Fl_Text_Buffer_m<"mGapStart", int>, &Fl_Text_Buffer::mGapStart>;
template struct access_helper<Fl_Text_Buffer_m<"mGapEnd", int>, &Fl_Text_Buffer::mGapEnd>;
template struct access_helper<Fl_Text_Buffer_m<"mPreferredGapSize", int>, &Fl_Text_Buffer::mPreferredGapSize>;
template struct access_helper<Fl_Text_Buffer_m<"call_modify_callbacks", void(int, int, int, int, const char*) const>, &Fl_Text_Buffer::call_modify_callbacks>;
template struct access_helper<Fl_Text_Buffer_m<"call_predelete_callbacks", void(int, int) const>, &Fl_Text_Buffer::call_predelete_callbacks>;
template struct access_helper<Fl_Text_Buffer_m<"update_selections", void(int, int, int)>, &Fl_Text_Buffer::update_selections>;

#endif

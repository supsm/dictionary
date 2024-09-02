#ifndef STYLES_H
#define STYLES_H

#include <array>
#include <utility>

#include <FL/Fl.H>
#include <FL/Fl_Text_Display.H>

enum style_modifier : unsigned char
{
	style_bold   = 0b00000001,
	style_italic = 0b00000010,
	style_small  = 0b00000100
};

enum class style : unsigned char
{
	normal = 0b00000000,
	link   = 0b00001000,
	title  = 0b00001101,
};

const Fl_Fontsize SMALL_SIZE = static_cast<Fl_Fontsize>(0.8 * FL_NORMAL_SIZE);
const Fl_Fontsize TITLE_SIZE = static_cast<Fl_Fontsize>(1.5 * FL_NORMAL_SIZE);

const std::array<Fl_Text_Display::Style_Table_Entry, 14> styles =
{ {
	{ FL_BLACK, FL_HELVETICA,             FL_NORMAL_SIZE }, // 00000000 - normal
	{ FL_BLACK, FL_HELVETICA_BOLD,        FL_NORMAL_SIZE }, // 00000001 - bold
	{ FL_BLACK, FL_HELVETICA_ITALIC,      FL_NORMAL_SIZE }, // 00000010 - italic
	{ FL_BLACK, FL_HELVETICA_BOLD_ITALIC, FL_NORMAL_SIZE }, // 00000011 - bold+italic
	{ FL_BLACK, FL_HELVETICA,             SMALL_SIZE     }, // 00000100 - small
	{ FL_BLACK, FL_HELVETICA_BOLD,        SMALL_SIZE     }, // 00000101 - bold small
	{ FL_BLACK, FL_HELVETICA_ITALIC,      SMALL_SIZE     }, // 00000110 - italic small
	{ FL_BLACK, FL_HELVETICA_BOLD_ITALIC, SMALL_SIZE     }, // 00000111 - bold+italic small
	{ FL_BLUE,  FL_COURIER,               FL_NORMAL_SIZE }, // 00001000 - link
	{ FL_BLUE,  FL_COURIER_BOLD,          FL_NORMAL_SIZE }, // 00001001 - link bold
	{ FL_BLUE,  FL_COURIER_ITALIC,        FL_NORMAL_SIZE }, // 00001010 - link italic
	{ FL_BLUE,  FL_COURIER_BOLD_ITALIC,   FL_NORMAL_SIZE }, // 00001011 - link bold+italic
	{ FL_BLUE,  FL_COURIER,               SMALL_SIZE     }, // 00001100 - link small
	{ FL_BLACK, FL_HELVETICA,             TITLE_SIZE     }  // 00001101 - title
} };

constexpr char get_style(style base_style = style::normal, unsigned char modifiers = 0)
{
	unsigned char style = std::to_underlying(base_style);
	if (base_style == style::normal)
	{
		style |= modifiers;
	}
	else if (base_style == style::link)
	{
		// do not apply bold/italic if small
		style |= (modifiers & ~((modifiers & 0b100) * 0b011));
	}
	return style + 'A';
}

constexpr char get_style(unsigned char modifiers)
{
	return get_style(style::normal, modifiers);
}

#endif

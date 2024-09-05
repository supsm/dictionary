#ifndef LINKS_H
#define LINKS_H

#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

#include <FL/Fl.H>
#include <FL/Fl_Text_Display.H>

#include <iostream>

extern void search_word(std::string_view word);

struct link_bounds
{
	int low, high;
};

inline std::vector<std::pair<link_bounds, std::string>> links;

class Linked_Text_Display : public Fl_Text_Display
{
private:
	std::size_t link_ind = -1;
	
	static std::size_t search_links(const int pos)
	{
		for (const auto& [i, p] : links | std::views::enumerate)
		{
			const auto& [bounds, str] = p;
			if (pos >= bounds.low && pos <= bounds.high)
			{
				return i;
			}
		}
		return -1;
	}

protected:
	using Fl_Text_Display::mMaxsize;

public:
	using Fl_Text_Display::Fl_Text_Display;

	int handle(int event) override
	{
		if (mMaxsize == 0) // causes xy_to_position to div by 0
			{ return Fl_Text_Display::handle(event); }

		switch (event)
		{
		case FL_ENTER: [[fallthrough]];
		case FL_MOVE: // TODO: is xy_to_position/search_links too expensive for move and drag events?
			{
				const int pos = xy_to_position(Fl::event_x(), Fl::event_y());
				if (search_links(pos) != -1)
					{ window()->cursor(FL_CURSOR_HAND); return 1; }
				// else: fallthrouh to fltk
			}
			break;
			
		case FL_PUSH:
			{
				const int pos = xy_to_position(Fl::event_x(), Fl::event_y());
				const auto cur_ind = search_links(pos);
				if (cur_ind != -1)
				{
					link_ind = cur_ind;
					// we do want a "fallthrough" to fltk behavior since otherwise trying to select
					// from a link will not work properly
				}
			}
			break;
		case FL_DRAG:
			if (link_ind != -1)
			{
				// make sure cursor is still on the same link
				const int pos = xy_to_position(Fl::event_x(), Fl::event_y());
				const auto [low, high] = links[link_ind].first;
				if (pos < low || pos > high)
					{ link_ind = -1; window()->cursor(FL_CURSOR_INSERT); } // fallthrough to fltk behavior; this is now a selection
				else
					{ return 1; } // still within the word, still a link click and not a selection
			}
		case FL_RELEASE:
			if (link_ind != -1)
			{
				// links will be cleared in search_word so we must
				// extend the lifetime of the word to avoid illegal access
				std::string word = std::move(links[link_ind].second);
				search_word(word);
				link_ind = -1;
				return 1;
			}
			break;
		}
		const int ret = Fl_Text_Display::handle(event);
		// on FL_PUSH, fltk will change the cursor, which is not always what we want
		// not ideal... but we don't want to turn into an insert cursor on clicking a link
		if (event == FL_PUSH && link_ind != -1)
			{ window()->cursor(FL_CURSOR_HAND); }
		return ret;
	}
};

#endif

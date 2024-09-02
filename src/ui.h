#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Text_Buffer.H>

#include "links.h"
#include "styles.h"

void search_word(Fl_Widget*);
void nav_back(Fl_Widget*);
void nav_forward(Fl_Widget*);

class FLTK_UI
{
private:
	struct end_group
	{
		end_group(Fl_Group& group)
		{
			group.end();
		}
	};

public:
	Fl_Double_Window window;
	Fl_Group top_bar;
	Fl_Input search_bar;
	Fl_Button search_button;
	Fl_Button button_back;
	Fl_Button button_forward;
	end_group end_top_bar;
	Linked_Text_Display text_display;
	Fl_Text_Buffer text_buf, style_buf;

	FLTK_UI() :
		window(480, 320, "Dictionary"),
		top_bar(0, 10, 480, 25),
		search_bar(80, 10, 310, 25),
		search_button(395, 10, 70, 25, "Search"),
		button_back(15, 10, 25, 25, "\342\206\220"),
		button_forward(45, 10, 25, 25, "\342\206\222"),
		end_top_bar(top_bar),
		text_display(15, 45, 450, 260),
		text_buf(), style_buf()
	{
		window.resizable(text_display);
		top_bar.resizable(search_bar);
		search_bar.callback(search_word);
		search_bar.when(FL_WHEN_ENTER_KEY | FL_WHEN_NOT_CHANGED);
		search_button.callback(search_word);
		button_back.deactivate();
		button_back.callback(nav_back);
		button_forward.deactivate();
		button_forward.callback(nav_forward);
		text_display.wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
		text_display.buffer(text_buf);
		text_display.highlight_data(&style_buf, styles.data(), styles.size(), 0, [](int, void*){}, nullptr);
		text_buf.canUndo(0); style_buf.canUndo(0);
		window.end();
	}
	
	~FLTK_UI()
	{
		text_display.buffer(nullptr);
	}
};
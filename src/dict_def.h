#ifndef DICT_DEF_H
#define DICT_DEF_H

#include <optional>
#include <string>
#include <variant>
#include <vector>

struct basic_sense_data
{
	std::optional<std::string> etymology;
	std::optional<std::vector<std::string>> inflections;
	std::optional<std::vector<std::string>> labels;
	std::optional<std::vector<std::string>> pronunciations;
	std::optional<bool> transitive_verb;
	std::optional<std::vector<std::string>> subj_status;
	std::optional<std::string> number;
	// variants
};

struct basic_def_sense_data : basic_sense_data
{
	std::string def_text;	
};

using trunc_sense_data = basic_sense_data;

struct div_sense_data : basic_def_sense_data
{
	std::string sense_div;
};

struct sense_data : basic_def_sense_data
{
	std::optional<div_sense_data> sdsense;
};

struct word_info
{
	std::string id;
	std::vector<std::string> stems;
	bool offensive;
	
	std::vector<std::variant<sense_data, trunc_sense_data>> defs;
};

#endif

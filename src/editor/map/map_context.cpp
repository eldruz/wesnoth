/*
   Copyright (C) 2008 - 2014 by Tomasz Sniatowski <kailoran@gmail.com>
   Part of the Battle for Wesnoth Project http://www.wesnoth.org/

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY.

   See the COPYING file for more details.
*/
#define GETTEXT_DOMAIN "wesnoth-editor"

#include "editor/action/action.hpp"
#include "map_context.hpp"

#include "display.hpp"
#include "filesystem.hpp"
#include "gettext.hpp"
#include "map_exception.hpp"
#include "map_label.hpp"
#include "resources.hpp"
#include "serialization/binary_or_text.hpp"
#include "serialization/parser.hpp"
#include "team.hpp"
#include "unit.hpp"
#include "wml_exception.hpp"


#include "formula_string_utils.hpp"

#include <boost/regex.hpp>
#include <boost/foreach.hpp>


namespace editor {

const size_t map_context::max_action_stack_size_ = 100;

map_context::map_context(const editor_map& map, const display& disp, bool pure_map, const config& schedule)
	: filename_()
	, map_data_key_()
	, embedded_(false)
	, pure_map_(pure_map)
	, map_(map)
	, undo_stack_()
	, redo_stack_()
	, actions_since_save_(0)
	, starting_position_label_locs_()
	, needs_reload_(false)
	, needs_terrain_rebuild_(false)
	, needs_labels_reset_(false)
	, changed_locations_()
	, everything_changed_(false)
	, scenario_id_()
	, scenario_name_()
	, scenario_description_()
	, xp_mod_(70)
    , victory_defeated_(true)
	, random_time_(false)
	, active_area_(-1)
	, labels_(disp, NULL)
	, units_()
	, teams_()
	, tod_manager_(new tod_manager(schedule))
	, mp_settings_()
	, game_classification_()
	, music_tracks_()
{
}

map_context::map_context(const config& game_config, const std::string& filename, const display& disp)
	: filename_(filename)
	, map_data_key_()
	, embedded_(false)
	, pure_map_(false)
	, map_(game_config)
	, undo_stack_()
	, redo_stack_()
	, actions_since_save_(0)
	, starting_position_label_locs_()
	, needs_reload_(false)
	, needs_terrain_rebuild_(false)
	, needs_labels_reset_(false)
	, changed_locations_()
	, everything_changed_(false)
	, scenario_id_()
	, scenario_name_()
	, scenario_description_()
	, xp_mod_(70)
	, victory_defeated_(true)
	, random_time_(false)
	, active_area_(-1)
	, labels_(disp, NULL)
	, units_()
	, teams_()
	, tod_manager_(new tod_manager(game_config.find_child("editor_times", "id", "default")))
	, mp_settings_()
	, game_classification_()
	, music_tracks_()
{
	/*
	 * Overview of situations possibly found in the file:
	 *
	 * 0. Not a scenario or map file.
	 *    0.1 File not found
	 *    0.2 Map file empty
	 *    0.3 No valid data
	 * 1. It's a file containing only pure map data.
	 *    * embedded_ = false
	 *    * pure_map_ = true
	 * 2. A scenario embedding the map
	 *    * embedded_ = true
	 *    * pure_map_ = true
	 *    The data/scenario-test.cfg for example.
	 *    The map is written back to the file.
	 * 3. The map file is referenced by map_data={MACRO_ARGUEMENT}.
	 *    * embedded_ = false
	 *    * pure_map_ = true
	 * 4. The file contains an editor generated scenario file.
	 *    * embedded_ = false
	 *    * pure_map_ = false
	 */

	log_scope2(log_editor, "Loading file " + filename);

	// 0.1 File not found
	if (!filesystem::file_exists(filename) || filesystem::is_directory(filename))
		throw editor_map_load_exception(filename, _("File not found"));

	std::string file_string = filesystem::read_file(filename);

	// 0.2 Map file empty
	if (file_string.empty()) {
		std::string message = _("Empty file");
		throw editor_map_load_exception(filename, message);
	}

	// 1.0 Pure map data
	boost::regex rexpression_map_data("map_data\\s*=\\s*\"(.+?)\"");
	boost::smatch matched_map_data;
	if (!boost::regex_search(file_string, matched_map_data, rexpression_map_data,
			boost::regex_constants::match_not_dot_null)) {
		map_ = editor_map::from_string(game_config, file_string); //throws on error
		pure_map_ = true;
		return;
	}

	// 2.0 Embedded map
	const std::string& map_data = matched_map_data[1];
	boost::regex rexpression_macro("\\{(.+?)\\}");
	boost::smatch matched_macro;
	if (!boost::regex_search(map_data, matched_macro, rexpression_macro)) {
		// We have a map_data string but no macro ---> embedded or scenario

		boost::regex rexpression_scenario("\\[scenario\\]|\\[test\\]|\\[multiplayer\\]");
		if (!boost::regex_search(file_string, rexpression_scenario)) {
			LOG_ED << "Loading generated scenario file" << std::endl;
			// 4.0 editor generated scenario
			try {
				load_scenario(game_config);
			} catch (config::error & e) {
				throw editor_map_load_exception("load_scenario", e.message); //we already caught and rethrew this exception in load_scenario
			}
			return;
		} else {
			LOG_ED << "Loading embedded map file" << std::endl;
			embedded_ = true;
			pure_map_ = true;
			map_ = editor_map::from_string(game_config, map_data);
			return;
		}
	}

	// 3.0 Macro referenced pure map
	const std::string& macro_argument = matched_macro[1];
	LOG_ED << "Map looks like a scenario, trying {" << macro_argument << "}" << std::endl;
	std::string new_filename = filesystem::get_wml_location(macro_argument,
			filesystem::directory_name(macro_argument));
	if (new_filename.empty()) {
		std::string message = _("The map file looks like a scenario, "
				"but the map_data value does not point to an existing file")
												+ std::string("\n") + macro_argument;
		throw editor_map_load_exception(filename, message);
	}
	LOG_ED << "New filename is: " << new_filename << std::endl;
	filename_ = new_filename;
	file_string = filesystem::read_file(filename_);
	map_ = editor_map::from_string(game_config, file_string);
	pure_map_ = true;
}

void map_context::set_side_setup(int side, const std::string& team_name, const std::string& user_team_name,
		int gold, int income, int village_gold, int village_support,
		bool fog, bool share_view, bool shroud, bool share_maps,
		team::CONTROLLER controller, bool hidden, bool no_leader)
{
	assert(teams_.size() > static_cast<unsigned int>(side));
	team& t = teams_[side];
//	t.set_save_id(id);
//	t.set_name(name);
	t.change_team(team_name, user_team_name);
	t.have_leader(!no_leader);
	t.change_controller(controller);
	t.set_gold(gold);
	t.set_base_income(income);
	t.set_hidden(hidden);
	t.set_fog(fog);
	t.set_share_maps(share_maps);
	t.set_shroud(shroud);
	t.set_share_view(share_view);
	t.set_village_gold(village_gold);
	t.set_village_support(village_support);
	actions_since_save_++;
}

void map_context::set_scenario_setup(const std::string& id, const std::string& name, const std::string& description,
		int turns, int xp_mod, bool victory_defeated, bool random_time)
{
	scenario_id_ = id;
	scenario_name_ = name;
	scenario_description_ = description;
	random_time_ = random_time;
	victory_defeated_ = victory_defeated;
	tod_manager_->set_number_of_turns(turns);
	xp_mod_ = xp_mod;
	actions_since_save_++;
}

void map_context::set_starting_time(int time)
{
	tod_manager_->set_current_time(time);
	if (!pure_map_)
		actions_since_save_++;
}

void map_context::remove_area(int index)
{
	tod_manager_->remove_time_area(index);
	active_area_--;
	actions_since_save_++;
}

void map_context::replace_schedule(const std::vector<time_of_day>& schedule)
{
	tod_manager_->replace_schedule(schedule);
	if (!pure_map_)
		actions_since_save_++;
}

void map_context::replace_local_schedule(const std::vector<time_of_day>& schedule)
{
	tod_manager_->replace_local_schedule(schedule, active_area_);
	if (!pure_map_)
		actions_since_save_++;
}

void map_context::load_scenario(const config& game_config)
{
	config scenario;

	try {
		read(scenario, *(preprocess_file(filename_)));
	} catch (config::error & e) {
		LOG_ED << "Caught a config error while parsing file: '" << filename_ << "'\n" << e.message << std::endl;
		throw e;
	}

	scenario_id_   = scenario["id"].str();
	scenario_name_ = scenario["name"].str();
	scenario_description_ = scenario["description"].str();

	xp_mod_ = scenario["experience_modifier"].to_int();

	victory_defeated_ = scenario["victory_when_enemies_defeated"].to_bool(true);
	random_time_ = scenario["random_start_time"].to_bool(false);

	map_ = editor_map::from_string(game_config, scenario["map_data"]); //throws on error

	labels_.read(scenario);

	tod_manager_.reset(new tod_manager(scenario));
	BOOST_FOREACH(const config &time_area, scenario.child_range("time_area")) {
		tod_manager_->add_time_area(map_,time_area);
	}

	BOOST_FOREACH(const config& item, scenario.child_range("item")) {
		const map_location loc(item);
		overlays_.insert(std::pair<map_location,
				overlay>(loc, overlay(item) ));
	}

	BOOST_FOREACH(const config& music, scenario.child_range("music")) {
		music_tracks_.insert(std::pair<std::string, sound::music_track>(music["name"], sound::music_track(music)));
	}

	resources::teams = &teams_;

	int i = 1;
	BOOST_FOREACH(config &side, scenario.child_range("side"))
	{
		team t;
		side["side"] = i;
		t.build(side, map_, NULL); //It's okay to pass a NULL gamedata ptr here, it is only used ultimately in map_location ctor and checked for null.
		teams_.push_back(t);

		BOOST_FOREACH(config &a_unit, side.child_range("unit")) {
			map_location loc(a_unit, NULL);
			a_unit["side"] = i;
			units_.add(loc, unit(a_unit, true) );
		}
		i++;
	}
}

map_context::~map_context()
{
	clear_stack(undo_stack_);
	clear_stack(redo_stack_);
}

bool map_context::select_area(int index)
{
	return map_.set_selection(tod_manager_->get_area_by_index(index));
}

void map_context::draw_terrain(const t_translation::t_terrain & terrain,
	const map_location& loc, bool one_layer_only)
{
	t_translation::t_terrain full_terrain = one_layer_only ? terrain :
		map_.get_terrain_info(terrain).terrain_with_default_base();

	draw_terrain_actual(full_terrain, loc, one_layer_only);
}

void map_context::draw_terrain_actual(const t_translation::t_terrain & terrain,
	const map_location& loc, bool one_layer_only)
{
	if (!map_.on_board_with_border(loc)) {
		//requests for painting off the map are ignored in set_terrain anyway,
		//but ideally we should not have any
		LOG_ED << "Attempted to draw terrain off the map (" << loc << ")\n";
		return;
	}
	t_translation::t_terrain old_terrain = map_.get_terrain(loc);
	if (terrain != old_terrain) {
		if (terrain.base == t_translation::NO_LAYER) {
			map_.set_terrain(loc, terrain, gamemap::OVERLAY);
		} else if (one_layer_only) {
			map_.set_terrain(loc, terrain, gamemap::BASE);
		} else {
			map_.set_terrain(loc, terrain);
		}
		add_changed_location(loc);
	}
}

void map_context::draw_terrain(const t_translation::t_terrain & terrain,
	const std::set<map_location>& locs, bool one_layer_only)
{
	t_translation::t_terrain full_terrain = one_layer_only ? terrain :
		map_.get_terrain_info(terrain).terrain_with_default_base();

	BOOST_FOREACH(const map_location& loc, locs) {
		draw_terrain_actual(full_terrain, loc, one_layer_only);
	}
}

void map_context::clear_changed_locations()
{
	everything_changed_ = false;
	changed_locations_.clear();
}

void map_context::add_changed_location(const map_location& loc)
{
	if (!everything_changed())
		changed_locations_.insert(loc);
}

void map_context::add_changed_location(const std::set<map_location>& locs)
{
	if (!everything_changed())
		changed_locations_.insert(locs.begin(), locs.end());
}

void map_context::set_everything_changed()
{
	everything_changed_ = true;
}

bool map_context::everything_changed() const
{
	return everything_changed_;
}

void map_context::clear_starting_position_labels(display& disp)
{
	disp.labels().clear_all();
	starting_position_label_locs_.clear();
}

void map_context::set_starting_position_labels(display& disp)
{
	std::set<map_location> new_label_locs = map_.set_starting_position_labels(disp);
	starting_position_label_locs_.insert(new_label_locs.begin(), new_label_locs.end());
}

void map_context::reset_starting_position_labels(display& disp)
{
	clear_starting_position_labels(disp);
	set_starting_position_labels(disp);
	set_needs_labels_reset(false);
}

config map_context::to_config()
{
	config scenario;

	scenario["id"] = scenario_id_;
	scenario["name"] = t_string(scenario_name_);
	scenario["description"] = scenario_description_;

	scenario["experience_modifier"] = xp_mod_;
	scenario["victory_when_enemies_defeated"] = victory_defeated_;
	scenario["random_starting_time"] = random_time_;

	scenario.append(tod_manager_->to_config());
	scenario.remove_attribute("turn_at");

	scenario["map_data"] = map_.write();

	labels_.write(scenario);

	overlay_map::const_iterator it;
	for (it = overlays_.begin(); it != overlays_.end(); ++it) {

			config& item = scenario.add_child("item");
			it->first.write(item);
			item["image"] = it->second.image;
			item["id"] = it->second.id;
			item["halo"] = it->second.halo;
			item["visible_in_fog"] = it->second.visible_in_fog;
			item["name"] = it->second.name;
			item["team_name"] = it->second.team_name;
	}

	BOOST_FOREACH(const music_map::value_type& track, music_tracks_) {
		track.second.write(scenario, true);
	}

	for(std::vector<team>::const_iterator t = teams_.begin(); t != teams_.end(); ++t) {
		int side_num = t - teams_.begin() + 1;

		config& side = scenario.add_child("side");

		side["side"] = side_num;
		side["hidden"] = t->hidden();

		side["controller"] = team::CONTROLLER_to_string (t->controller());
		side["no_leader"] = t->no_leader();

		side["team_name"] = t->team_name();
		side["user_team_name"] = t->user_team_name();

		// TODO
		// side["allow_player"] = "yes";

		side["fog"] = t->uses_fog();
		side["share_view"] = t->share_view();
		side["shroud"] = t->uses_shroud();
		side["share_maps"] = t->share_maps();

		side["gold"] = t->gold();
		side["income"] = t->base_income();

		BOOST_FOREACH(const map_location& village, t->villages()) {
			village.write(side.add_child("village"));
		}

		//current visible units
		for(unit_map::const_iterator i = units_.begin(); i != units_.end(); ++i) {
			if(i->side() == side_num) {
				config& u = side.add_child("unit");
				i->get_location().write(u);
				u["type"] = i->type_id();
				u["canrecruit"] = i->can_recruit();
				u["unrenamable"] = i->unrenamable();
				u["id"] = i->id();
				u["name"] = i->name();
				u["extra_recruit"] = utils::join(i->recruits());
				u["facing"] = map_location::write_direction(i->facing());
			}
		}
	}

	return scenario;
}

bool map_context::save_scenario()
{
	assert(!is_embedded());

	if (scenario_id_.empty())
		scenario_id_ = filesystem::base_name(filename_);
	if (scenario_name_.empty())
		scenario_name_ = scenario_id_;

	try {
		std::stringstream wml_stream;
		{
			config_writer out(wml_stream, false);
			out.write(to_config());
		}
		if (!wml_stream.str().empty())
			filesystem::write_file(get_filename(), wml_stream.str());
		clear_modified();
	} catch (filesystem::io_exception& e) {
		utils::string_map symbols;
		symbols["msg"] = e.what();
		const std::string msg = vgettext("Could not save the scenario: $msg", symbols);
		throw editor_map_save_exception(msg);
	}
	// TODO the return value of this method does not need to be boolean.
	// We either return true or there is an exception thrown.
	return true;
}

bool map_context::save_map()
{
	std::string map_data = map_.write();

	try {
		if (!is_embedded()) {
			filesystem::write_file(get_filename(), map_data);
		} else {
			std::string map_string = filesystem::read_file(get_filename());
			boost::regex rexpression_map_data("(.*map_data\\s*=\\s*\")(.+?)(\".*)");
			boost::smatch matched_map_data;
			if (boost::regex_search(map_string, matched_map_data, rexpression_map_data,
					boost::regex_constants::match_not_dot_null)) {
				std::stringstream ss;
				ss << matched_map_data[1];
				ss << map_data;
				ss << matched_map_data[3];
				filesystem::write_file(get_filename(), ss.str());
			} else {
				throw editor_map_save_exception(_("Could not save into scenario"));
			}
		}
		clear_modified();
	} catch (filesystem::io_exception& e) {
		utils::string_map symbols;
		symbols["msg"] = e.what();
		const std::string msg = vgettext("Could not save the map: $msg", symbols);
		throw editor_map_save_exception(msg);
	}
	//TODO the return value of this method does not need to be boolean.
	//We either return true or there is an exception thrown.
	return true;
}

void map_context::set_map(const editor_map& map)
{
	if (map_.h() != map.h() || map_.w() != map.w()) {
		set_needs_reload();
	} else {
		set_needs_terrain_rebuild();
	}
	map_ = map;
}

void map_context::perform_action(const editor_action& action)
{
	LOG_ED << "Performing action " << action.get_id() << ": " << action.get_name()
		<< ", actions count is " << action.get_instance_count() << std::endl;
	editor_action* undo = action.perform(*this);
	if (actions_since_save_ < 0) {
		//set to a value that will make it impossible to get to zero, as at this point
		//it is no longer possible to get back the original map state using undo/redo
		actions_since_save_ = 1 + undo_stack_.size();
	}
	actions_since_save_++;
	undo_stack_.push_back(undo);
	trim_stack(undo_stack_);
	clear_stack(redo_stack_);
}

void map_context::perform_partial_action(const editor_action& action)
{
	LOG_ED << "Performing (partial) action " << action.get_id() << ": " << action.get_name()
		<< ", actions count is " << action.get_instance_count() << std::endl;
	if (!can_undo()) {
		throw editor_logic_exception("Empty undo stack in perform_partial_action()");
	}
	editor_action_chain* undo_chain = dynamic_cast<editor_action_chain*>(last_undo_action());
	if (undo_chain == NULL) {
		throw editor_logic_exception("Last undo action not a chain in perform_partial_action()");
	}
	editor_action* undo = action.perform(*this);
	//actions_since_save_ += action.action_count();
	undo_chain->prepend_action(undo);
	clear_stack(redo_stack_);
}

bool map_context::modified() const
{
	return actions_since_save_ != 0;
}

void map_context::clear_modified()
{
	actions_since_save_ = 0;
}

bool map_context::can_undo() const
{
	return !undo_stack_.empty();
}

bool map_context::can_redo() const
{
	return !redo_stack_.empty();
}

editor_action* map_context::last_undo_action()
{
	return undo_stack_.empty() ? NULL : undo_stack_.back();
}

editor_action* map_context::last_redo_action()
{
	return redo_stack_.empty() ? NULL : redo_stack_.back();
}

const editor_action* map_context::last_undo_action() const
{
	return undo_stack_.empty() ? NULL : undo_stack_.back();
}

const editor_action* map_context::last_redo_action() const
{
	return redo_stack_.empty() ? NULL : redo_stack_.back();
}

void map_context::undo()
{
	LOG_ED << "undo() beg, undo stack is " << undo_stack_.size() << ", redo stack " << redo_stack_.size() << std::endl;
	if (can_undo()) {
		perform_action_between_stacks(undo_stack_, redo_stack_);
		actions_since_save_--;
	} else {
		WRN_ED << "undo() called with an empty undo stack" << std::endl;
	}
	LOG_ED << "undo() end, undo stack is " << undo_stack_.size() << ", redo stack " << redo_stack_.size() << std::endl;
}

void map_context::redo()
{
	LOG_ED << "redo() beg, undo stack is " << undo_stack_.size() << ", redo stack " << redo_stack_.size() << std::endl;
	if (can_redo()) {
		perform_action_between_stacks(redo_stack_, undo_stack_);
		actions_since_save_++;
	} else {
		WRN_ED << "redo() called with an empty redo stack" << std::endl;
	}
	LOG_ED << "redo() end, undo stack is " << undo_stack_.size() << ", redo stack " << redo_stack_.size() << std::endl;
}

void map_context::partial_undo()
{
	//callers should check for these conditions
	if (!can_undo()) {
		throw editor_logic_exception("Empty undo stack in partial_undo()");
	}
	editor_action_chain* undo_chain = dynamic_cast<editor_action_chain*>(last_undo_action());
	if (undo_chain == NULL) {
		throw editor_logic_exception("Last undo action not a chain in partial undo");
	}
	//a partial undo performs the first action form the current action's action_chain that would be normally performed
	//i.e. the *first* one.
	boost::scoped_ptr<editor_action> first_action_in_chain(undo_chain->pop_first_action());
	if (undo_chain->empty()) {
		actions_since_save_--;
		delete undo_chain;
		undo_stack_.pop_back();
	}
	redo_stack_.push_back(first_action_in_chain.get()->perform(*this));
	//actions_since_save_ -= last_redo_action()->action_count();
}

void map_context::clear_undo_redo()
{
	clear_stack(undo_stack_);
	clear_stack(redo_stack_);
}

void map_context::trim_stack(action_stack& stack)
{
	if (stack.size() > max_action_stack_size_) {
		delete stack.front();
		stack.pop_front();
	}
}

void map_context::clear_stack(action_stack& stack)
{
	BOOST_FOREACH(editor_action* a, stack) {
		delete a;
	}
	stack.clear();
}

void map_context::perform_action_between_stacks(action_stack& from, action_stack& to)
{
	assert(!from.empty());
	boost::scoped_ptr<editor_action> action(from.back());
	from.pop_back();
	editor_action* reverse_action = action->perform(*this);
	to.push_back(reverse_action);
	trim_stack(to);
}

} //end namespace editor

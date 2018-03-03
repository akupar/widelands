/*
 * Copyright (C) 2007-2017 by the Widelands Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include "logic/campaign_visibility.h"

#include <map>
#include <memory>

#include "base/log.h"
#include "io/filesystem/filesystem.h"
#include "logic/filesystem_constants.h"
#include "profile/profile.h"
#include "scripting/lua_interface.h"

/**
 * Make sure that campaigns visibility-save is up to date and well-formed
 */
void CampaignVisibilitySave::ensure_campvis_file_is_current() {
	g_fs->ensure_directory_exists(kSaveDir);
	// NOCOM Use completed rather than visible?

	// Get the current version of the campaign config
	LuaInterface lua;
	std::unique_ptr<LuaTable> table(lua.run_script("campaigns/campaigns.lua"));
	// NOCOM table.do_not_warn_about_unaccessed_keys();
	const int current_version = table->get_int("version");

	// Create a new file if there wasn't one
	if (!(g_fs->file_exists(kCampVisFile))) {
		// There is no campvis file - create one.
		Profile campvis(kCampVisFile.c_str());
		campvis.pull_section("global");
		campvis.get_safe_section("global").set_int("version", current_version - 1);
		campvis.pull_section("campaigns");
		log("NOCOM ensure_campvis_file_exists\n");
		campvis.pull_section("scenarios");
		campvis.write(kCampVisFile.c_str(), true);
	}

	// Make sure that the file is up to date
	Profile campvis(kCampVisFile.c_str());
	bool is_legacy = campvis.get_section("campmaps") != nullptr;
	if (campvis.get_safe_section("global").get_int("version") < current_version ||
		 is_legacy) {
		update_campvis(*table.get(), is_legacy);
	}
}


/**
 * Update the campaign visibility save-file of the user
 */
void CampaignVisibilitySave::update_campvis(const LuaTable& table, bool is_legacy) {
	log("NOCOM updating campvis 1\n");

	Profile campvis(kCampVisFile.c_str());

	// TODO(GunChleoc): Remove compatibility code after Build 21.
	std::map<std::string, std::string> legacy_scenarios = {
		{"bar01.wmf", "barbariantut00"}, {"bar02.wmf", "barbariantut01"}, {"emp01.wmf", "empiretut00"},
		{"emp02.wmf", "empiretut01"},    {"emp03.wmf", "empiretut02"},    {"emp04.wmf", "empiretut03"},
		{"atl01.wmf", "atlanteans00"},   {"atl02.wmf", "atlanteans01"},
		{"fri01.wmf", "frisians00"},   {"fri02.wmf", "frisians01"},
	};

	Section& campvis_campaigns = campvis.get_safe_section("campaigns");
	Section& campvis_scenarios =
	   is_legacy ? campvis.get_safe_section("campmaps") : campvis.get_safe_section("scenarios");

	// Prepare campaigns.lua
	std::unique_ptr<LuaTable> campaigns_table(table.get_table("campaigns"));
	campaigns_table->do_not_warn_about_unaccessed_keys();

	// Collect all information about campaigns and scenarios
	std::map<std::string, bool> campaigns;
	std::map<std::string, bool> scenarios;
	for (const auto& campaign : campaigns_table->array_entries<std::unique_ptr<LuaTable>>()) {
		campaign->do_not_warn_about_unaccessed_keys();
		const std::string campaign_name = campaign->get_string("name");
		if (campaigns.count(campaign_name) != 1) {
			campaigns[campaign_name] = false;
		}
		campaigns[campaign_name] = campaigns[campaign_name] || !campaign->has_key("prerequisite") ||
		                           campvis_campaigns.get_bool(campaign_name.c_str());

		std::unique_ptr<LuaTable> scenarios_table(campaign->get_table("scenarios"));
		scenarios_table->do_not_warn_about_unaccessed_keys();
		for (const auto& scenario : scenarios_table->array_entries<std::unique_ptr<LuaTable>>()) {
			scenario->do_not_warn_about_unaccessed_keys();
			const std::string scenario_path = scenario->get_string("path");
			scenarios[scenario_path] =
			   is_legacy ? campvis_scenarios.get_bool(legacy_scenarios[scenario_path].c_str()) :
			               campvis_scenarios.get_bool(scenario_path.c_str());

			// If a scenario is visible, this campaign is visible too.
			if (scenarios[scenario_path]) {
				campaigns[campaign_name] = true;
			}
		}

		// A campaign can also make sure that scenarios of a previous campaign are visible
		if (campaigns[campaign_name] && campaign->has_key<std::string>("reveal_scenarios")) {
			for (const auto& scenario :
			     campaign->get_table("reveal_scenarios")->array_entries<std::string>()) {
				scenarios[scenario] = true;
			}
		}
	}

	// Make sure that all visible campaigns have their first scenario visible.
	for (const auto& campaign : campaigns_table->array_entries<std::unique_ptr<LuaTable>>()) {
		campaign->do_not_warn_about_unaccessed_keys();
		const std::string campaign_name = campaign->get_string("name");
		if (campaigns[campaign_name]) {
			std::unique_ptr<LuaTable> scenarios_table(campaign->get_table("scenarios"));
			scenarios_table->do_not_warn_about_unaccessed_keys();
			const auto& scenario = scenarios_table->get_table(1);
			scenario->do_not_warn_about_unaccessed_keys();
			scenarios[scenario->get_string("path")] = true;
		}
	}

	log("NOCOM writing campvis\n");

	// Now write everything
	Profile write_campvis(kCampVisFile.c_str());
	write_campvis.pull_section("global").set_int("version", table.get_int("version"));

	Section& write_campaigns = write_campvis.pull_section("campaigns");
	for (const auto& campaign : campaigns) {
		write_campaigns.set_bool(campaign.first.c_str(), campaign.second);
	}

	log("NOCOM write_scenarios\n");
	Section& write_scenarios = write_campvis.pull_section("scenarios");
	for (const auto& scenario : scenarios) {
		write_scenarios.set_bool(scenario.first.c_str(), scenario.second);
	}

	write_campvis.write(kCampVisFile.c_str(), true);
}

/**
 * Searches for the scenario with the given file 'path' in data/campaigns/campaigns.lua,
 * and if the scenario has 'reveal_campaign' and/or 'reveal_scenario' defined, marks the respective
 * campaign/scenario as visible.
 */
void CampaignVisibilitySave::mark_scenario_as_solved(const std::string& path) {
	ensure_campvis_file_is_current();

	// Check which campaign and/or scenario to reveal
	std::string campaign_to_reveal = "";
	std::string scenario_to_reveal = "";

	LuaInterface lua;
	std::unique_ptr<LuaTable> table(lua.run_script("campaigns/campaigns.lua"));
	table->do_not_warn_about_unaccessed_keys();
	std::unique_ptr<LuaTable> campaigns_table(table->get_table("campaigns"));
	campaigns_table->do_not_warn_about_unaccessed_keys();

	bool found = false;
	for (const auto& campaign : campaigns_table->array_entries<std::unique_ptr<LuaTable>>()) {
		campaign->do_not_warn_about_unaccessed_keys();
		if (found) {
			continue;
		}
		std::unique_ptr<LuaTable> scenarios_table(campaign->get_table("scenarios"));
		scenarios_table->do_not_warn_about_unaccessed_keys();
		for (const auto& scenario : scenarios_table->array_entries<std::unique_ptr<LuaTable>>()) {
			scenario->do_not_warn_about_unaccessed_keys();
			if (path == scenario->get_string("path")) {
				if (scenario->has_key<std::string>("reveal_scenario")) {
					scenario_to_reveal = scenario->get_string("reveal_scenario");
				}
				if (scenario->has_key<std::string>("reveal_campaign")) {
					campaign_to_reveal = scenario->get_string("reveal_campaign");
				}
				// We can't break here, because we need to shut up the table warnings.
				found = true;
			}
		}
	}

	// Write the campvis
	Profile campvis(kCampVisFile.c_str());
	if (!campaign_to_reveal.empty()) {
		campvis.pull_section("campaigns").set_bool(campaign_to_reveal.c_str(), true);
	}
	if (!scenario_to_reveal.empty()) {
		log("NOCOM reveal scenario\n");
		campvis.pull_section("scenarios").set_bool(scenario_to_reveal.c_str(), true);
	}
	campvis.write(kCampVisFile.c_str(), false);
}

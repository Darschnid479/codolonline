#include "platform.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace dragon::platform
{
	float Dvar::as_float() const
	{
		return static_cast<float>(std::atof(value.c_str()));
	}

	int Dvar::as_int() const
	{
		return std::atoi(value.c_str());
	}

	bool Dvar::as_bool() const
	{
		if (value == "1" || value == "true" || value == "TRUE" || value == "on") return true;
		return std::atoi(value.c_str()) != 0;
	}

	DebugConsole& DebugConsole::instance()
	{
		static DebugConsole inst;
		return inst;
	}

	DebugConsole::DebugConsole()
	{
		register_dvar("cg_drawFPS", "1", "Display FPS and frame telemetry overlay");
		register_dvar("cg_fov", "75", "Field of view in degrees");
		register_dvar("r_wireframe", "0", "Render world geometry in wireframe overlay");
		register_dvar("r_fullbright", "0", "Disable directional and lightmap shading");
		register_dvar("r_lightmap", "1", "Modulate world surfaces with decoded GfxWorld lightmaps");
		register_dvar("r_drawStaticModels", "1", "Render GfxStaticModelDrawInst static models");
		register_dvar("r_frustumCull", "1", "Enable BSP/surface AABB frustum culling");
		register_dvar("g_noclip", "0", "Disable clipMap brush collision and gravity");
		register_dvar("g_gravity", "800", "IW4/CODOL world gravity (units/sec^2)");
		register_dvar("g_speed", "190", "Base player walk speed (units/sec)");
		register_dvar("g_gametype", "war", "Active offline gametype (war/dm/zm/survival)");
		register_dvar("sv_maxclients", "18", "Maximum local/bot client slots");

		register_command("help", [this](const std::vector<std::string>&) {
			std::lock_guard<std::mutex> lock(mutex_);
			std::ostringstream oss;
			oss << "Commands:";
			for (const auto& [k, v] : commands_)
				oss << " " << k;
			return oss.str();
		}, "List registered console commands");

		register_command("dvarlist", [this](const std::vector<std::string>&) {
			std::lock_guard<std::mutex> lock(mutex_);
			std::ostringstream oss;
			for (const auto& [k, v] : dvars_)
				oss << k << " = " << v.value << "\n";
			return oss.str();
		}, "List all dvars");

		register_command("set", [this](const std::vector<std::string>& args) {
			if (args.size() < 3) return std::string("usage: set <dvar> <value>");
			set_dvar(args[1], args[2]);
			return args[1] + " = " + args[2];
		}, "Set a dvar value");

		register_command("quit", [](const std::vector<std::string>&) {
			request_quit();
			return std::string("Shutting down Project Dragon...");
		}, "Exit application");
	}

	void DebugConsole::print(std::string line)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		lines_.push_back(std::move(line));
		if (lines_.size() > 512)
			lines_.erase(lines_.begin(), lines_.begin() + 128);
	}

	void DebugConsole::register_dvar(const std::string& name, const std::string& default_val, const std::string& desc)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		std::string key = name;
		std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (dvars_.find(key) == dvars_.end())
			dvars_.emplace(key, Dvar{ name, default_val, desc });
	}

	bool DebugConsole::set_dvar(const std::string& name, const std::string& val)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		std::string key = name;
		std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		auto it = dvars_.find(key);
		if (it == dvars_.end())
		{
			dvars_.emplace(key, Dvar{ name, val, "" });
			return true;
		}
		it->second.value = val;
		return true;
	}

	const Dvar* DebugConsole::find_dvar(const std::string& name) const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		std::string key = name;
		std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		auto it = dvars_.find(key);
		return it != dvars_.end() ? &it->second : nullptr;
	}

	float DebugConsole::dvar_float(const std::string& name, float fallback) const
	{
		const Dvar* d = find_dvar(name);
		return d ? d->as_float() : fallback;
	}

	int DebugConsole::dvar_int(const std::string& name, int fallback) const
	{
		const Dvar* d = find_dvar(name);
		return d ? d->as_int() : fallback;
	}

	bool DebugConsole::dvar_bool(const std::string& name, bool fallback) const
	{
		const Dvar* d = find_dvar(name);
		return d ? d->as_bool() : fallback;
	}

	void DebugConsole::register_command(const std::string& name, CommandFn fn, const std::string& help)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		std::string key = name;
		std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		commands_[key] = CmdEntry{ std::move(fn), help };
	}

	std::string DebugConsole::execute(const std::string& line)
	{
		std::istringstream iss(line);
		std::vector<std::string> tokens;
		std::string tok;
		while (iss >> tok) tokens.push_back(tok);
		if (tokens.empty()) return {};

		std::string cmd = tokens[0];
		std::transform(cmd.begin(), cmd.end(), cmd.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

		CommandFn fn;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			auto it = commands_.find(cmd);
			if (it != commands_.end())
				fn = it->second.fn;
		}
		if (fn)
		{
			std::string out = fn(tokens);
			if (!out.empty()) print("> " + line + " => " + out);
			return out;
		}
		// Dvar query/set shorthand
		if (const Dvar* d = find_dvar(cmd))
		{
			if (tokens.size() >= 2)
			{
				set_dvar(cmd, tokens[1]);
				std::string res = d->name + " = " + tokens[1];
				print(res);
				return res;
			}
			std::string res = d->name + " = " + d->value;
			print(res);
			return res;
		}
		std::string err = "Unknown command: " + tokens[0];
		print(err);
		return err;
	}

	std::vector<std::string> DebugConsole::recent_lines(std::size_t max_lines) const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (lines_.size() <= max_lines) return lines_;
		return std::vector<std::string>(lines_.end() - static_cast<std::ptrdiff_t>(max_lines), lines_.end());
	}
}

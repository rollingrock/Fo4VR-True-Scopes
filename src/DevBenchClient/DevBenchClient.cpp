#include "DevBenchClient/DevBenchClient.h"

#include "DevBench/DevBench.h"
#include "DevBenchClient/DevBenchAPI.h"

namespace DevBenchClient
{
	namespace
	{
		std::atomic_bool g_connected{ false };

		// a very small json reader
		//
		// No JSON library in this plugin on purpose (see DevBench.h), and the tool
		// arguments are flat by construction (devbench generates them from the schema
		// below), so this extracts top-level scalars only and refuses anything else.
		// Depth is tracked, so a nested key is never mistaken for a top-level one;
		// malformed input yields "not found", which every caller turns into a stated
		// default or an error.

		void SkipWs(std::string_view a_s, std::size_t& a_i)
		{
			while (a_i < a_s.size() && (a_s[a_i] == ' ' || a_s[a_i] == '\t' || a_s[a_i] == '\n' || a_s[a_i] == '\r')) {
				++a_i;
			}
		}

		// Read a JSON string starting at the opening quote. Handles the escapes JSON
		// requires; \u is passed through as-is because none of our fields is unicode
		// and inventing a transcoding here would be more risk than value.
		bool ReadString(std::string_view a_s, std::size_t& a_i, std::string& a_out)
		{
			if (a_i >= a_s.size() || a_s[a_i] != '"') {
				return false;
			}
			++a_i;
			a_out.clear();
			while (a_i < a_s.size()) {
				const char c = a_s[a_i++];
				if (c == '"') {
					return true;
				}
				if (c != '\\') {
					a_out += c;
					continue;
				}
				if (a_i >= a_s.size()) {
					return false;
				}
				switch (const char e = a_s[a_i++]) {
				case 'n':  a_out += '\n'; break;
				case 't':  a_out += '\t'; break;
				case 'r':  a_out += '\r'; break;
				case 'b':  a_out += '\b'; break;
				case 'f':  a_out += '\f'; break;
				case '/':  a_out += '/'; break;
				case '"':  a_out += '"'; break;
				case '\\': a_out += '\\'; break;
				default:   a_out += '\\'; a_out += e; break;
				}
			}
			return false;
		}

		// The value of a top-level key, as text. Strings come back unquoted; numbers,
		// booleans and null come back verbatim. Absent key -> false.
		bool Field(std::string_view a_json, std::string_view a_key, std::string& a_out)
		{
			int         depth = 0;
			std::size_t i = 0;
			while (i < a_json.size()) {
				const char c = a_json[i];
				if (c == '{' || c == '[') {
					++depth;
					++i;
					continue;
				}
				if (c == '}' || c == ']') {
					--depth;
					++i;
					continue;
				}
				if (c != '"') {
					++i;
					continue;
				}
				// A string here is either a key or a value; only a key at depth 1 is
				// ours, and only when a ':' follows it.
				std::string name;
				const std::size_t start = i;
				if (!ReadString(a_json, i, name)) {
					return false;
				}
				SkipWs(a_json, i);
				if (i >= a_json.size() || a_json[i] != ':') {
					continue;  // it was a value, not a key; keep scanning
				}
				++i;
				SkipWs(a_json, i);
				const bool wanted = (depth == 1 && name == a_key);
				if (a_json[i] == '"') {
					std::string value;
					if (!ReadString(a_json, i, value)) {
						return false;
					}
					if (wanted) {
						a_out = std::move(value);
						return true;
					}
				} else if (a_json[i] == '{' || a_json[i] == '[') {
					// A nested value: fall through so the loop counts its braces. A
					// caller asking for it gets "not found", which is correct — this
					// reader does not do structures.
					if (wanted) {
						return false;
					}
				} else {
					const std::size_t vs = i;
					while (i < a_json.size() && a_json[i] != ',' && a_json[i] != '}' && a_json[i] != ']') {
						++i;
					}
					std::size_t ve = i;
					while (ve > vs && (a_json[ve - 1] == ' ' || a_json[ve - 1] == '\t')) {
						--ve;
					}
					if (wanted) {
						a_out.assign(a_json.substr(vs, ve - vs));
						return true;
					}
				}
				(void)start;
			}
			return false;
		}

		[[nodiscard]] std::string Escape(std::string_view a_in)
		{
			std::string out;
			out.reserve(a_in.size() + 8);
			for (const char c : a_in) {
				switch (c) {
				case '"':  out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:
					if (static_cast<unsigned char>(c) < 0x20) {
						char buf[8];
						std::snprintf(buf, sizeof(buf), "\\u%04x", c);
						out += buf;
					} else {
						out += c;
					}
				}
			}
			return out;
		}

		[[nodiscard]] std::string Error(std::string_view a_msg)
		{
			return "{\"ok\":false,\"error\":\"" + Escape(a_msg) + "\"}";
		}

		// the scope tool
		//
		// One tool with an `action`, not nine tools: devbench's own guidance is to
		// keep the agent-facing surface small, and an agent choosing between
		// `scope_state`, `scope_config`, `scope_omods`... is worse at it than one
		// choosing an enum. Every action maps to a route answered by
		// DevBench::Invoke, so nothing is reimplemented here.
		void ScopeTool(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
		{
			const std::string_view args = (a_argsJson && *a_argsJson) ? a_argsJson : "{}";

			std::string action;
			if (!Field(args, "action", action) || action.empty()) {
				action = "state";  // the overwhelmingly common ask
			}

			std::string                                      path;
			std::vector<std::pair<std::string, std::string>> params;
			std::string                                      value;

			const auto pass = [&](const char* a_key, const char* a_as) {
				std::string v;
				if (Field(args, a_key, v)) {
					params.emplace_back(a_as, v);
				}
			};

			if (action == "state") {
				path = "/scope";
				// `probe` forces a fresh 3D walk, and it mutates: InvalidatePlacement
				// recomputes the widget placement, so a probing read repairs what it
				// measures. Default off, so the ordinary read only observes.
				std::string probe;
				if (Field(args, "probe", probe) && probe != "false" && probe != "0") {
					params.emplace_back("probe", "1");
				}
			} else if (action == "render") {
				path = "/state";
			} else if (action == "config") {
				path = "/config";
			} else if (action == "set") {
				std::string key;
				if (!Field(args, "key", key) || !Field(args, "value", value)) {
					a_write(a_sink, Error("action='set' needs both 'key' and 'value'").c_str());
					return;
				}
				path = "/config/set";
				params.emplace_back("key", key);
				params.emplace_back("value", value);
			} else if (action == "reload") {
				path = "/config/reload";
			} else if (action == "omods") {
				path = "/omods";
				pass("filter", "filter");
				pass("limit", "limit");
			} else if (action == "addresses") {
				path = "/addresses";
			} else if (action == "perfReset") {
				path = "/perf/reset";
					pass("timeoutMs", "timeoutMs");
			} else if (action == "health") {
				path = "/health";
			} else if (action == "verdict") {
				path = "/verdict";
				pass("since", "since");
			} else if (action == "attach") {
				std::string omod;
				if (!Field(args, "omod", omod)) {
					a_write(a_sink, Error("action='attach' needs 'omod' (hex formID)").c_str());
					return;
				}
				path = "/attach";
				params.emplace_back("omod", omod);
				pass("detach", "detach");
			} else if (action == "log") {
				path = "/log";
				pass("tail", "tail");
				pass("grep", "grep");
			} else if (action == "lookup") {
				std::string mp;
				if (!Field(args, "path", mp)) {
					a_write(a_sink, Error("action='lookup' needs 'path' (a model path)").c_str());
					return;
				}
				path = "/scope/lookup";
				params.emplace_back("path", mp);
			} else {
				a_write(a_sink, Error("unknown action '" + action +
									  "' (state|render|config|set|reload|omods|addresses|perfReset|health|verdict|attach|log|lookup)")
									.c_str());
				return;
			}

			const std::string body = DevBench::Invoke(path, params);
			a_write(a_sink, body.c_str());
		}

		constexpr const char* kDescriptor = R"({
"description":"True Scopes VR: which optic is equipped and how the scope render is behaving. 'state' identifies the equipped scope (model-path key, aperture and where it came from, derived vs used FOV, widget placement and its residuals, attached object mods). 'render' is the per-frame render diagnostics (fault latch, last step, pass totals, light counts, sun and sky state, per-stage GPU/CPU ms). 'config' reads every live setting; 'set' changes one without a rebuild or a scope cycle; 'reload' re-reads the TOML. 'verdict' reads the tester's controller chord (grip+A yes / grip+B no / grip+trigger skip; polling arms on first call). 'attach' attaches a scope OMOD to the equipped weapon (omod=hex formID). 'log' tails the plugin log (tail=N, grep=substr). 'lookup' resolves a model path against the aperture table. NOTE state?probe=true MUTATES: it recomputes the widget placement, so use it to force a fresh walk, not to observe one.",
"readOnly":false,
"inputSchema":{
 "type":"object",
 "properties":{
  "action":{"type":"string","default":"state",
            "enum":["state","render","config","set","reload","omods","addresses","perfReset","health"]},
  "probe":{"type":"boolean","description":"action=state: force a fresh 3D walk. MUTATES the widget placement."},
  "key":{"type":"string","description":"action=set: setting name."},
  "value":{"type":"string","description":"action=set: new value."},
  "filter":{"type":"string","description":"action=omods: substring filter."},
  "limit":{"type":"integer","description":"action=omods: max rows (default 400)."}
 }
}})";
	}

	void Register()
	{
		// Module probe before the dispatch: dispatching to an absent plugin makes
		// CommonLib log "failed to dispatch to devbench" at warn, which reads like
		// an error in a public build. devbench is a development dependency, never
		// a load-order requirement.
		if (!::GetModuleHandleW(L"devbench.dll")) {
			logger::info("devbench not present; scope tool not registered (dev tooling - the mod is unaffected)"sv);
			return;
		}
		auto* api = DevBenchAPI::GetDevBenchInterface001();
		if (!api) {
			logger::info("devbench present but no interface; the scope tool was not registered"sv);
			return;
		}

		const auto build = api->GetBuildNumber();
		api->RegisterTool("scope", kDescriptor, &ScopeTool, nullptr);
		g_connected.store(true);
		logger::info(FMT_STRING("devbench build {} — registered the 'scope' tool"), build);
	}

	bool Connected()
	{
		return g_connected.load();
	}
}

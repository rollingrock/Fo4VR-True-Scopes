#include "DevBench/DevBench.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <atomic>
#include <charconv>
#include <fstream>
#include <thread>
#include <vector>

#include "Settings/Settings.h"
#include "TrueScopes/ScopeRender.h"

#pragma comment(lib, "Ws2_32.lib")

namespace DevBench
{
	namespace
	{
		std::atomic_bool   g_running{ false };
		std::atomic<SOCKET> g_listen{ INVALID_SOCKET };
		std::thread        g_thread;
		std::uint16_t      g_port = 0;
		std::uint64_t      g_startTick = 0;
		std::atomic<std::uint64_t> g_requests{ 0 };

		// ---------------------------------------------------------------- json

		std::string JsonEscape(std::string_view a_in)
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

		std::string Quote(std::string_view a_in) { return "\"" + JsonEscape(a_in) + "\""; }

		std::string Hex(std::uint64_t a_v)
		{
			char buf[32];
			std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(a_v));
			return buf;
		}

		std::string Err(std::string_view a_msg)
		{
			return "{\"ok\":false,\"error\":" + Quote(a_msg) + "}";
		}

		// -------------------------------------------------------------- guards

		// Raw process-memory reads must never take down the game. MSVC forbids
		// __try in a function that needs C++ unwinding, so the guarded readers
		// are plain C-shaped helpers with no locals requiring destruction.
		bool SafeReadBytes(const void* a_src, void* a_dst, std::size_t a_len) noexcept
		{
			__try {
				std::memcpy(a_dst, a_src, a_len);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// ---------------------------------------------------- address expressions

		// Grammar:  expr := term (('+'|'-') term)*
		//           term := '[' expr ']' | 'base' | 0xHEX | DEC
		// '[x]' dereferences a qword. 'base' is Fallout4VR.exe's load base, so
		// the RVAs used throughout the investigation docs work verbatim:
		//   base+0x1d98ff0     ->  the deferred-release enqueue
		//   [base+0x6239340]+4 ->  BSGraphics::Renderer's scoped flag
		struct ExprParser
		{
			std::string_view s;
			std::size_t      i = 0;
			bool             ok = true;
			std::string      err;

			void Skip()
			{
				while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
			}

			bool Match(char c)
			{
				Skip();
				if (i < s.size() && s[i] == c) { ++i; return true; }
				return false;
			}

			std::uint64_t Term()
			{
				Skip();
				if (i >= s.size()) { ok = false; err = "unexpected end of address expression"; return 0; }

				if (s[i] == '[') {
					++i;
					const std::uint64_t inner = Expr();
					if (!Match(']')) { ok = false; err = "missing ']'"; return 0; }
					if (!ok) return 0;
					std::uint64_t v = 0;
					if (!SafeReadBytes(reinterpret_cast<const void*>(inner), &v, sizeof(v))) {
						ok = false;
						err = "dereference faulted at " + Hex(inner);
						return 0;
					}
					return v;
				}

				if (s.compare(i, 4, "base") == 0) {
					i += 4;
					return REL::Module::get().base();
				}

				const std::size_t start = i;
				int base = 10;
				if (s.compare(i, 2, "0x") == 0 || s.compare(i, 2, "0X") == 0) { i += 2; base = 16; }
				std::uint64_t v = 0;
				const char* first = s.data() + i;
				const char* last = s.data() + s.size();
				const auto  res = std::from_chars(first, last, v, base);
				if (res.ec != std::errc{}) {
					ok = false;
					err = "bad number at offset " + std::to_string(start);
					return 0;
				}
				i = static_cast<std::size_t>(res.ptr - s.data());
				return v;
			}

			std::uint64_t Expr()
			{
				std::uint64_t v = Term();
				while (ok) {
					Skip();
					if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
						const char op = s[i++];
						const std::uint64_t r = Term();
						if (!ok) break;
						v = (op == '+') ? v + r : v - r;
					} else {
						break;
					}
				}
				return v;
			}
		};

		bool ResolveAddress(std::string_view a_expr, std::uint64_t& a_out, std::string& a_err)
		{
			ExprParser p{ a_expr };
			const std::uint64_t v = p.Expr();
			if (!p.ok) { a_err = p.err; return false; }
			p.Skip();
			if (p.i != a_expr.size()) { a_err = "trailing characters in address expression"; return false; }
			a_out = v;
			return true;
		}

		// -------------------------------------------------------------- settings

		// One table so /config can list, read, and write every setting without
		// each endpoint knowing the names. Kept next to Settings.h by hand;
		// a missing entry means "invisible to the bench", not a crash.
		enum class Kind { Bool, Int, Float };

		struct Entry
		{
			const char* name;
			Kind        kind;
			void*       ptr;  // Settings::bSetting* / iSetting* / fSetting*
		};

		std::vector<Entry> SettingTable()
		{
			using namespace Settings;
			return {
				{ "fillEnabled", Kind::Bool, &fillEnabled },
				{ "fillEveryNFrames", Kind::Int, &fillEveryNFrames },
				{ "scopeOffHoldMs", Kind::Int, &scopeOffHoldMs },
				{ "forceAlwaysOn", Kind::Bool, &forceAlwaysOn },
				{ "lensMode", Kind::Int, &lensMode },
				{ "scopeFovDegrees", Kind::Float, &scopeFovDegrees },
				{ "scopeNearClip", Kind::Float, &scopeNearClip },
				{ "scopeFarClip", Kind::Float, &scopeFarClip },
				{ "scopeCamOffsetX", Kind::Float, &scopeCamOffsetX },
				{ "scopeCamOffsetY", Kind::Float, &scopeCamOffsetY },
				{ "scopeCamOffsetZ", Kind::Float, &scopeCamOffsetZ },
				{ "sunEnabled", Kind::Bool, &sunEnabled },
				{ "sunExecEnabled", Kind::Bool, &sunExecEnabled },
				{ "sunBrightnessScale", Kind::Float, &sunBrightnessScale },
				{ "sunSpecEnabled", Kind::Bool, &sunSpecEnabled },
				{ "accumClearScale", Kind::Float, &accumClearScale },
				{ "accumClearAlpha", Kind::Float, &accumClearAlpha },
				{ "skyEnabled", Kind::Bool, &skyEnabled },
				{ "skyRootMask", Kind::Int, &skyRootMask },
				{ "retryAfterFault", Kind::Bool, &retryAfterFault },
				{ "diagLensReadback", Kind::Bool, &diagLensReadback },
				{ "diagPauseTint", Kind::Bool, &diagPauseTint },
				{ "diagDumpLensEveryNRenders", Kind::Int, &diagDumpLensEveryNRenders },
				{ "diagDumpBuffers", Kind::Bool, &diagDumpBuffers },
				{ "disableScopeBlackout", Kind::Bool, &disableScopeBlackout },
				{ "disableApproachFade", Kind::Bool, &disableApproachFade },
			};
		}

		std::string ReadSetting(const Entry& a_e)
		{
			switch (a_e.kind) {
			case Kind::Bool:
				return **static_cast<Settings::bSetting*>(a_e.ptr) ? "true" : "false";
			case Kind::Int:
				return std::to_string(**static_cast<Settings::iSetting*>(a_e.ptr));
			case Kind::Float: {
				char buf[64];
				std::snprintf(buf, sizeof(buf), "%.6g", **static_cast<Settings::fSetting*>(a_e.ptr));
				return buf;
			}
			}
			return "null";
		}

		bool WriteSetting(const Entry& a_e, std::string_view a_val, std::string& a_err)
		{
			switch (a_e.kind) {
			case Kind::Bool: {
				bool v;
				if (a_val == "true" || a_val == "1") {
					v = true;
				} else if (a_val == "false" || a_val == "0") {
					v = false;
				} else {
					a_err = "expected true/false/1/0";
					return false;
				}
				**static_cast<Settings::bSetting*>(a_e.ptr) = v;
				return true;
			}
			case Kind::Int: {
				std::int64_t v = 0;
				const auto   res = std::from_chars(a_val.data(), a_val.data() + a_val.size(), v);
				if (res.ec != std::errc{}) { a_err = "expected an integer"; return false; }
				**static_cast<Settings::iSetting*>(a_e.ptr) = v;
				return true;
			}
			case Kind::Float: {
				// from_chars(double) is available but strtod keeps this simple
				// and tolerant of "1e-3" style input.
				const std::string tmp{ a_val };
				char*             end = nullptr;
				const double      v = std::strtod(tmp.c_str(), &end);
				if (end == tmp.c_str()) { a_err = "expected a number"; return false; }
				**static_cast<Settings::fSetting*>(a_e.ptr) = v;
				return true;
			}
			}
			a_err = "unknown setting kind";
			return false;
		}

		// ------------------------------------------------------------- requests

		struct Request
		{
			std::string method;
			std::string path;
			std::vector<std::pair<std::string, std::string>> query;

			[[nodiscard]] const std::string* Get(std::string_view a_key) const
			{
				for (const auto& [k, v] : query) {
					if (k == a_key) return &v;
				}
				return nullptr;
			}

			[[nodiscard]] std::string GetOr(std::string_view a_key, std::string_view a_def) const
			{
				const auto* p = Get(a_key);
				return p ? *p : std::string{ a_def };
			}
		};

		std::string UrlDecode(std::string_view a_in)
		{
			std::string out;
			out.reserve(a_in.size());
			for (std::size_t i = 0; i < a_in.size(); ++i) {
				if (a_in[i] == '%' && i + 2 < a_in.size()) {
					const auto hex = [](char c) -> int {
						if (c >= '0' && c <= '9') return c - '0';
						if (c >= 'a' && c <= 'f') return c - 'a' + 10;
						if (c >= 'A' && c <= 'F') return c - 'A' + 10;
						return -1;
					};
					const int hi = hex(a_in[i + 1]);
					const int lo = hex(a_in[i + 2]);
					if (hi >= 0 && lo >= 0) {
						out += static_cast<char>((hi << 4) | lo);
						i += 2;
						continue;
					}
				}
				out += (a_in[i] == '+') ? ' ' : a_in[i];
			}
			return out;
		}

		bool ParseRequest(std::string_view a_raw, Request& a_out)
		{
			const auto eol = a_raw.find("\r\n");
			if (eol == std::string_view::npos) return false;
			const auto line = a_raw.substr(0, eol);

			const auto sp1 = line.find(' ');
			if (sp1 == std::string_view::npos) return false;
			const auto sp2 = line.find(' ', sp1 + 1);
			if (sp2 == std::string_view::npos) return false;

			a_out.method = std::string{ line.substr(0, sp1) };
			auto target = line.substr(sp1 + 1, sp2 - sp1 - 1);

			const auto q = target.find('?');
			if (q == std::string_view::npos) {
				a_out.path = std::string{ target };
			} else {
				a_out.path = std::string{ target.substr(0, q) };
				auto qs = target.substr(q + 1);
				while (!qs.empty()) {
					const auto amp = qs.find('&');
					const auto pair = (amp == std::string_view::npos) ? qs : qs.substr(0, amp);
					const auto eq = pair.find('=');
					if (eq != std::string_view::npos) {
						a_out.query.emplace_back(UrlDecode(pair.substr(0, eq)), UrlDecode(pair.substr(eq + 1)));
					} else if (!pair.empty()) {
						a_out.query.emplace_back(UrlDecode(pair), std::string{});
					}
					if (amp == std::string_view::npos) break;
					qs = qs.substr(amp + 1);
				}
			}
			return true;
		}

		// ------------------------------------------------------------- handlers

		std::string HandleIndex()
		{
			// Self-describing: an agent that has never seen this server can
			// discover the whole surface with one GET.
			return
				"{\"ok\":true,\"plugin\":\"TrueScopesVR\",\"tier\":1,\"endpoints\":["
				"{\"path\":\"/health\",\"desc\":\"liveness + identity; answered without touching game state\"},"
				"{\"path\":\"/state\",\"desc\":\"render availability, own-resolve flag, render thread id\"},"
				"{\"path\":\"/config\",\"desc\":\"every TOML setting and its live value\"},"
				"{\"path\":\"/config/set\",\"desc\":\"?key=NAME&value=V - set one setting live, no scope-cycle needed\"},"
				"{\"path\":\"/config/reload\",\"desc\":\"re-read TrueScopesVR.toml now\"},"
				"{\"path\":\"/resolve\",\"desc\":\"?addr=EXPR - resolve an address expression to a VA + RVA\"},"
				"{\"path\":\"/read\",\"desc\":\"?addr=EXPR&type=u8|u16|u32|u64|i32|i64|f32|f64|ptr|bytes|cstr&count=N\"},"
				"{\"path\":\"/log\",\"desc\":\"?tail=N&grep=SUBSTR - last N lines of TrueScopesVR.log\"}"
				"],\"addrExpr\":\"expr := term (('+'|'-') term)* ; term := '[' expr ']' | 'base' | 0xHEX | DEC\"}";
		}

		std::string HandleHealth()
		{
			std::string out = "{\"ok\":true";
			out += ",\"plugin\":\"TrueScopesVR\"";
			out += ",\"version\":\"" + std::string{ Version::NAME } + "\"";
			out += ",\"pid\":" + std::to_string(::GetCurrentProcessId());
			out += ",\"port\":" + std::to_string(g_port);
			out += ",\"uptimeMs\":" + std::to_string(::GetTickCount64() - g_startTick);
			out += ",\"requests\":" + std::to_string(g_requests.load());
			out += "}";
			return out;
		}

		std::string HandleState()
		{
			std::string out = "{\"ok\":true";
			out += ",\"renderAvailable\":" + std::string{ TrueScopes::ScopeRender::Available() ? "true" : "false" };
			out += ",\"inOwnResolve\":" + std::string{ TrueScopes::ScopeRender::InOwnResolve() ? "true" : "false" };
			out += ",\"ownRenderThread\":" + std::to_string(TrueScopes::ScopeRender::OwnRenderThread());
			out += ",\"moduleBase\":" + Quote(Hex(REL::Module::get().base()));
			out += "}";
			return out;
		}

		// Keys set live through /config/set. Re-applied after every
		// Settings::load() (which fires on each scope-in) so an experiment keeps
		// measuring what you set, not what the file says.
		std::mutex                                       g_ovrMutex;
		std::vector<std::pair<std::string, std::string>> g_overrides;

		void ReapplyOverrides()
		{
			std::scoped_lock lock{ g_ovrMutex };
			if (g_overrides.empty()) return;
			const auto table = SettingTable();
			for (const auto& [k, v] : g_overrides) {
				for (const auto& e : table) {
					if (k == e.name) {
						std::string err;
						(void)WriteSetting(e, v, err);
						break;
					}
				}
			}
			logger::info(FMT_STRING("devbench: re-applied {} live override(s) after TOML load"), g_overrides.size());
		}

		std::string HandleConfig()
		{
			std::string out = "{\"ok\":true,\"settings\":{";
			bool first = true;
			for (const auto& e : SettingTable()) {
				if (!first) out += ",";
				first = false;
				out += Quote(e.name) + ":" + ReadSetting(e);
			}
			out += "},\"overrides\":[";
			{
				std::scoped_lock lock{ g_ovrMutex };
				for (std::size_t k = 0; k < g_overrides.size(); ++k) {
					if (k) out += ",";
					out += Quote(g_overrides[k].first);
				}
			}
			out += "]}";
			return out;
		}

		std::string HandleConfigSet(const Request& a_req)
		{
			const auto* key = a_req.Get("key");
			const auto* val = a_req.Get("value");
			if (!key || !val) return Err("need ?key=NAME&value=V");

			for (const auto& e : SettingTable()) {
				if (*key == e.name) {
					const std::string before = ReadSetting(e);
					std::string       err;
					if (!WriteSetting(e, *val, err)) return Err(e.name + std::string{ ": " } + err);
					const std::string after = ReadSetting(e);
					{
						std::scoped_lock lock{ g_ovrMutex };
						bool             found = false;
						for (auto& o : g_overrides) {
							if (o.first == e.name) { o.second = *val; found = true; break; }
						}
						if (!found) g_overrides.emplace_back(e.name, *val);
					}
					logger::info(FMT_STRING("devbench: {} {} -> {}"), e.name, before, after);
					return "{\"ok\":true,\"key\":" + Quote(e.name) + ",\"before\":" + before + ",\"after\":" + after + "}";
				}
			}
			return Err("unknown setting '" + *key + "' (GET /config for the list)");
		}

		// Explicitly "go back to what the file says": drop the live overrides
		// first, otherwise the reload would immediately re-apply them and the
		// endpoint would be a no-op.
		std::string HandleConfigReload()
		{
			{
				std::scoped_lock lock{ g_ovrMutex };
				g_overrides.clear();
			}
			Settings::load();
			logger::info("devbench: TOML reloaded on request, live overrides dropped"sv);
			return HandleConfig();
		}

		std::string HandleResolve(const Request& a_req)
		{
			const auto* expr = a_req.Get("addr");
			if (!expr) return Err("need ?addr=EXPR");
			std::uint64_t va = 0;
			std::string   err;
			if (!ResolveAddress(*expr, va, err)) return Err(err);

			const auto base = REL::Module::get().base();
			std::string out = "{\"ok\":true,\"expr\":" + Quote(*expr);
			out += ",\"va\":" + Quote(Hex(va));
			out += ",\"base\":" + Quote(Hex(base));
			if (va >= base) out += ",\"rva\":" + Quote(Hex(va - base));
			out += "}";
			return out;
		}

		std::string HandleRead(const Request& a_req)
		{
			const auto* expr = a_req.Get("addr");
			if (!expr) return Err("need ?addr=EXPR");

			std::uint64_t va = 0;
			std::string   err;
			if (!ResolveAddress(*expr, va, err)) return Err(err);

			const std::string type = a_req.GetOr("type", "u32");
			std::int64_t      count = 1;
			{
				const std::string c = a_req.GetOr("count", "1");
				std::from_chars(c.data(), c.data() + c.size(), count);
			}
			// A bench read that can allocate hundreds of MB is a footgun, not a
			// feature. Cap and report the cap rather than silently truncating.
			constexpr std::int64_t kMaxCount = 4096;
			const bool             capped = count > kMaxCount;
			if (capped) count = kMaxCount;
			if (count < 1) count = 1;

			std::size_t stride = 4;
			if (type == "u8" || type == "bytes" || type == "cstr") stride = 1;
			else if (type == "u16") stride = 2;
			else if (type == "u32" || type == "i32" || type == "f32") stride = 4;
			else if (type == "u64" || type == "i64" || type == "f64" || type == "ptr") stride = 8;
			else return Err("unknown type '" + type + "'");

			if (type == "cstr") {
				char        buf[512]{};
				std::size_t n = static_cast<std::size_t>(count);
				if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
				if (!SafeReadBytes(reinterpret_cast<const void*>(va), buf, n)) {
					return Err("read faulted at " + Hex(va));
				}
				buf[n] = '\0';
				return "{\"ok\":true,\"addr\":" + Quote(Hex(va)) + ",\"type\":\"cstr\",\"value\":" + Quote(buf) + "}";
			}

			std::vector<std::uint8_t> raw(static_cast<std::size_t>(count) * stride);
			if (!SafeReadBytes(reinterpret_cast<const void*>(va), raw.data(), raw.size())) {
				// Report the faulting address, not just "failed" --- a probe whose
				// only outcomes are "the answer" and "unreadable" cannot tell a
				// wrong pointer from an absent one.
				return Err("read of " + std::to_string(raw.size()) + " bytes faulted at " + Hex(va));
			}

			std::string out = "{\"ok\":true,\"addr\":" + Quote(Hex(va));
			out += ",\"type\":" + Quote(type) + ",\"count\":" + std::to_string(count);
			if (capped) out += ",\"capped\":true";
			out += ",\"values\":[";
			for (std::int64_t k = 0; k < count; ++k) {
				if (k) out += ",";
				const std::uint8_t* p = raw.data() + static_cast<std::size_t>(k) * stride;
				char                buf[64];
				if (type == "u8")        std::snprintf(buf, sizeof(buf), "%u", *p);
				else if (type == "u16")  std::snprintf(buf, sizeof(buf), "%u", *reinterpret_cast<const std::uint16_t*>(p));
				else if (type == "u32")  std::snprintf(buf, sizeof(buf), "%u", *reinterpret_cast<const std::uint32_t*>(p));
				else if (type == "i32")  std::snprintf(buf, sizeof(buf), "%d", *reinterpret_cast<const std::int32_t*>(p));
				else if (type == "u64")  std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(*reinterpret_cast<const std::uint64_t*>(p)));
				else if (type == "i64")  std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(*reinterpret_cast<const std::int64_t*>(p)));
				else if (type == "ptr")  std::snprintf(buf, sizeof(buf), "\"0x%llx\"", static_cast<unsigned long long>(*reinterpret_cast<const std::uint64_t*>(p)));
				else if (type == "bytes") std::snprintf(buf, sizeof(buf), "%u", *p);
				else if (type == "f32") {
					const float f = *reinterpret_cast<const float*>(p);
					// JSON has no NaN/Inf literal, and NaN is exactly what this
					// bench exists to find --- emit it as a string, never as a
					// silently-dropped null.
					if (std::isfinite(f)) std::snprintf(buf, sizeof(buf), "%.9g", f);
					else                  std::snprintf(buf, sizeof(buf), "\"%s\"", std::isnan(f) ? "NaN" : (f > 0 ? "Inf" : "-Inf"));
				} else {
					const double d = *reinterpret_cast<const double*>(p);
					if (std::isfinite(d)) std::snprintf(buf, sizeof(buf), "%.17g", d);
					else                  std::snprintf(buf, sizeof(buf), "\"%s\"", std::isnan(d) ? "NaN" : (d > 0 ? "Inf" : "-Inf"));
				}
				out += buf;
			}
			out += "]}";
			return out;
		}

		std::string HandleLog(const Request& a_req)
		{
			auto path = logger::log_directory();
			if (!path) return Err("no log directory");
			const auto gamepath = REL::Module::IsVR() ? "Fallout4VR/F4SE" : "Fallout4/F4SE";
			if (!path.value().generic_string().ends_with(gamepath)) {
				path = path.value().parent_path().append(gamepath);
			}
			*path /= "TrueScopesVR.log";

			std::ifstream in(*path);
			if (!in) return Err("cannot open " + path->string());

			std::int64_t tail = 80;
			{
				const std::string t = a_req.GetOr("tail", "80");
				std::from_chars(t.data(), t.data() + t.size(), tail);
			}
			if (tail < 1) tail = 1;
			if (tail > 2000) tail = 2000;
			const std::string needle = a_req.GetOr("grep", "");

			std::vector<std::string> keep;
			std::string              line;
			while (std::getline(in, line)) {
				if (!needle.empty() && line.find(needle) == std::string::npos) continue;
				keep.push_back(line);
				if (static_cast<std::int64_t>(keep.size()) > tail) keep.erase(keep.begin());
			}

			std::string out = "{\"ok\":true,\"file\":" + Quote(path->string());
			out += ",\"lines\":[";
			for (std::size_t k = 0; k < keep.size(); ++k) {
				if (k) out += ",";
				out += Quote(keep[k]);
			}
			out += "]}";
			return out;
		}

		std::string Route(const Request& a_req)
		{
			if (a_req.path == "/" || a_req.path == "/index")   return HandleIndex();
			if (a_req.path == "/health")                        return HandleHealth();
			if (a_req.path == "/state")                         return HandleState();
			if (a_req.path == "/config")                        return HandleConfig();
			if (a_req.path == "/config/set")                    return HandleConfigSet(a_req);
			if (a_req.path == "/config/reload")                 return HandleConfigReload();
			if (a_req.path == "/resolve")                       return HandleResolve(a_req);
			if (a_req.path == "/read")                          return HandleRead(a_req);
			if (a_req.path == "/log")                           return HandleLog(a_req);
			return Err("no such endpoint '" + a_req.path + "' (GET / for the list)");
		}

		// ----------------------------------------------------------- the server

		void Serve(SOCKET a_client)
		{
			std::string raw;
			char        buf[4096];
			// One request per connection; the header block is all we need since
			// every input arrives in the query string.
			for (;;) {
				const int n = ::recv(a_client, buf, sizeof(buf), 0);
				if (n <= 0) break;
				raw.append(buf, static_cast<std::size_t>(n));
				if (raw.find("\r\n\r\n") != std::string::npos) break;
				if (raw.size() > 64 * 1024) break;
			}

			std::string body;
			Request     req;
			if (raw.empty() || !ParseRequest(raw, req)) {
				body = Err("malformed request");
			} else {
				g_requests.fetch_add(1);
				try {
					body = Route(req);
				} catch (const std::exception& e) {
					body = Err(std::string{ "handler threw: " } + e.what());
				} catch (...) {
					body = Err("handler threw an unknown exception");
				}
			}

			std::string resp = "HTTP/1.1 200 OK\r\n";
			resp += "Content-Type: application/json\r\n";
			resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
			resp += "Connection: close\r\n\r\n";
			resp += body;

			std::size_t sent = 0;
			while (sent < resp.size()) {
				const int n = ::send(a_client, resp.data() + sent, static_cast<int>(resp.size() - sent), 0);
				if (n <= 0) break;
				sent += static_cast<std::size_t>(n);
			}
			::shutdown(a_client, SD_BOTH);
			::closesocket(a_client);
		}

		void AcceptLoop()
		{
			while (g_running.load()) {
				const SOCKET listen = g_listen.load();
				if (listen == INVALID_SOCKET) break;

				const SOCKET client = ::accept(listen, nullptr, nullptr);
				if (client == INVALID_SOCKET) {
					if (!g_running.load()) break;
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
					continue;
				}
				// Sequential by design: an agent driver issues one call at a
				// time, and serialising keeps the reads coherent with each other.
				Serve(client);
			}
		}
	}

	bool Start()
	{
		if (!*Settings::devbenchEnabled) {
			logger::info("devbench: disabled by settings"sv);
			return false;
		}

		WSADATA wsa{};
		if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
			logger::error("devbench: WSAStartup failed"sv);
			return false;
		}

		const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s == INVALID_SOCKET) {
			logger::error("devbench: socket() failed"sv);
			::WSACleanup();
			return false;
		}

		// Loopback only, always. Never make this configurable: the bench reads
		// arbitrary process memory.
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

		const auto wanted = static_cast<std::uint16_t>(
			std::clamp<std::int64_t>(*Settings::devbenchPort, 1024, 65535));

		bool bound = false;
		for (std::uint16_t p = wanted; p < wanted + 16 && p != 0; ++p) {
			addr.sin_port = ::htons(p);
			if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
				g_port = p;
				bound = true;
				break;
			}
		}
		if (!bound || ::listen(s, 8) != 0) {
			logger::error(FMT_STRING("devbench: could not bind 127.0.0.1:{}..{}"), wanted, wanted + 15);
			::closesocket(s);
			::WSACleanup();
			return false;
		}

		g_listen.store(s);
		g_running.store(true);
		g_startTick = ::GetTickCount64();
		Settings::postLoadHook = &ReapplyOverrides;
		g_thread = std::thread(AcceptLoop);

		logger::info(FMT_STRING("devbench: DEV BUILD - listening on http://127.0.0.1:{}/ (GET / for the endpoint list). "
								"Disable via devbenchEnabled before any public release."),
			g_port);
		return true;
	}

	void Stop()
	{
		Settings::postLoadHook = nullptr;
		if (!g_running.exchange(false)) return;
		const SOCKET s = g_listen.exchange(INVALID_SOCKET);
		if (s != INVALID_SOCKET) ::closesocket(s);
		if (g_thread.joinable()) g_thread.join();
		::WSACleanup();
		g_port = 0;
	}

	std::uint16_t Port() { return g_port; }
}

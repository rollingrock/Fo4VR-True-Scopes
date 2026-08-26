#include "DevBench/DevBench.h"

#include <WS2tcpip.h>

#include <atomic>
#include <charconv>
#include <fstream>
#include <thread>
#include <vector>

#include "Settings/Settings.h"
#include "TrueScopes/Hooks.h"
#include "TrueScopes/ScopeIdent.h"
#include "TrueScopes/LensComposite.h"
#include "TrueScopes/PoseGate.h"
#include "TrueScopes/VerdictInput.h"
#include "TrueScopes/ScopeRender.h"


namespace DevBench
{
	namespace
	{
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

		std::string Vec3(const float (&a_v)[3])
		{
			return "[" + std::to_string(a_v[0]) + "," + std::to_string(a_v[1]) + "," +
			       std::to_string(a_v[2]) + "]";
		}

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


		// v0.2.66: the module's true extent, so a heap pointer that merely happens to
		// sit above the image base is not reported with a meaningless RVA. Live test
		// caught this: the scope camera at 0x1ec2ea480 was being labelled
		// rva=0xac2ea480, which would paste into Ghidra as a valid-looking lie.
		// Read SizeOfImage out of the PE optional header:
		//   base+0x3C            = e_lfanew
		//   base+e_lfanew+0x18   = IMAGE_OPTIONAL_HEADER64
		//   optionalHeader+0x38  = SizeOfImage
		std::uintptr_t ModuleSize()
		{
			static std::uintptr_t cached = 0;
			if (cached) return cached;
			const auto base = REL::Module::get().base();
			std::uint32_t lfanew = 0, sizeOfImage = 0;
			if (SafeReadBytes(reinterpret_cast<const void*>(base + 0x3C), &lfanew, sizeof(lfanew)) &&
				lfanew > 0 && lfanew < 0x1000 &&
				SafeReadBytes(reinterpret_cast<const void*>(base + lfanew + 0x18 + 0x38), &sizeOfImage, sizeof(sizeOfImage)) &&
				sizeOfImage > 0) {
				cached = sizeOfImage;
			} else {
				cached = 0x08000000;  // 128 MB fallback; Fallout4VR.exe is ~110 MB
			}
			return cached;
		}

		bool InModule(std::uintptr_t a_va)
		{
			const auto base = REL::Module::get().base();
			return a_va >= base && a_va < base + ModuleSize();
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
				{ "lensMode", Kind::Int, &lensMode },
				{ "scopeFovDegrees", Kind::Float, &scopeFovDegrees },
				{ "scopeNearClip", Kind::Float, &scopeNearClip },
				{ "scopeFarClip", Kind::Float, &scopeFarClip },
				{ "scopeCamOffsetX", Kind::Float, &scopeCamOffsetX },
				{ "scopeCamOffsetY", Kind::Float, &scopeCamOffsetY },
				{ "scopeCamOffsetZ", Kind::Float, &scopeCamOffsetZ },
				{ "cullToScopeFrustum", Kind::Bool, &cullToScopeFrustum },
				// v0.2.73 perf: the stopwatch (off->on resets its window) and its two levers
				{ "perfTimers", Kind::Bool, &perfTimers },
				{ "perfLightsMax", Kind::Int, &perfLightsMax },
				{ "sunEnabled", Kind::Bool, &sunEnabled },
				{ "sunExecEnabled", Kind::Bool, &sunExecEnabled },
				{ "sunBrightnessScale", Kind::Float, &sunBrightnessScale },
				{ "accumClearScale", Kind::Float, &accumClearScale },
				{ "accumClearAlpha", Kind::Float, &accumClearAlpha },
				{ "skyEnabled", Kind::Bool, &skyEnabled },
				{ "skyRootMask", Kind::Int, &skyRootMask },
				{ "deliveryUnbindDS", Kind::Bool, &deliveryUnbindDS },
				{ "lensCompositeEnabled", Kind::Bool, &lensCompositeEnabled },
				{ "reticleEnabled", Kind::Bool, &reticleEnabled },
				{ "reticleUseGlassed", Kind::Bool, &reticleUseGlassed },
				{ "reticleAlpha", Kind::Float, &reticleAlpha },
				{ "reticleScaleX", Kind::Float, &reticleScaleX },
				{ "reticleScaleY", Kind::Float, &reticleScaleY },
				{ "reticleOffsetX", Kind::Float, &reticleOffsetX },
				{ "reticleOffsetY", Kind::Float, &reticleOffsetY },
				{ "vignetteInner", Kind::Float, &vignetteInner },
				{ "vignetteOuter", Kind::Float, &vignetteOuter },
				{ "vignetteStrength", Kind::Float, &vignetteStrength },
				{ "vignettePower", Kind::Float, &vignettePower },
				{ "lensTintR", Kind::Float, &lensTintR },
				{ "lensTintG", Kind::Float, &lensTintG },
				{ "lensTintB", Kind::Float, &lensTintB },
				{ "lensExposure", Kind::Float, &lensExposure },
				{ "nvEffectStrength", Kind::Float, &nvEffectStrength },
				{ "nvGain", Kind::Float, &nvGain },
				{ "reconEffectStrength", Kind::Float, &reconEffectStrength },
				{ "eyeBoxStrength", Kind::Float, &eyeBoxStrength },
				{ "eyeBoxGain", Kind::Float, &eyeBoxGain },
				{ "eyeBoxIpdUnits", Kind::Float, &eyeBoxIpdUnits },
				{ "edgeBlurStrength", Kind::Float, &edgeBlurStrength },
				{ "edgeBlurStart", Kind::Float, &edgeBlurStart },
				{ "caStrength", Kind::Float, &caStrength },
				{ "nvScanlines", Kind::Float, &nvScanlines },
				{ "rimShadowStrength", Kind::Float, &rimShadowStrength },
				{ "rimShadowStart", Kind::Float, &rimShadowStart },
				{ "rimShadowEnd", Kind::Float, &rimShadowEnd },
				{ "rimShadowTopBias", Kind::Float, &rimShadowTopBias },
				{ "rimShadowParallax", Kind::Float, &rimShadowParallax },
				{ "sheenStrength", Kind::Float, &sheenStrength },
				{ "sheenWidth", Kind::Float, &sheenWidth },
				{ "sheenTravel", Kind::Float, &sheenTravel },
				{ "sheenFresnel", Kind::Float, &sheenFresnel },
				{ "sheenSmudge", Kind::Float, &sheenSmudge },
				{ "sheenSmudgeScale", Kind::Float, &sheenSmudgeScale },
				{ "sheenDarkBoost", Kind::Float, &sheenDarkBoost },
				{ "reticleEyeBoxFollow", Kind::Float, &reticleEyeBoxFollow },
				{ "eyeBoxResidual", Kind::Float, &eyeBoxResidual },
				{ "eyeBoxResidualAdapt", Kind::Float, &eyeBoxResidualAdapt },
				{ "decalGroup5Reorder", Kind::Bool, &decalGroup5Reorder },
				{ "decalStageEnabled", Kind::Bool, &decalStageEnabled },
				{ "dropSunGlareGroup", Kind::Bool, &dropSunGlareGroup },
				{ "parallaxDepthUnits", Kind::Float, &parallaxDepthUnits },
				{ "parallaxMaxShift", Kind::Float, &parallaxMaxShift },
				{ "parallaxSmoothing", Kind::Float, &parallaxSmoothing },
				{ "parallaxMinEyeRelief", Kind::Float, &parallaxMinEyeRelief },
				{ "reticleParallaxFraction", Kind::Float, &reticleParallaxFraction },
				{ "widgetApertureScale", Kind::Float, &widgetApertureScale },
				{ "camSmoothEnabled", Kind::Bool, &camSmoothEnabled },
				{ "camSmoothMinCutoffHz", Kind::Float, &camSmoothMinCutoffHz },
				{ "camSmoothBeta", Kind::Float, &camSmoothBeta },
				{ "camSmoothDCutoffHz", Kind::Float, &camSmoothDCutoffHz },
				{ "camSmoothSnapDegrees", Kind::Float, &camSmoothSnapDegrees },
				{ "camSmoothMaxLagDegrees", Kind::Float, &camSmoothMaxLagDegrees },
				{ "lensPrimeOnPresence", Kind::Bool, &lensPrimeOnPresence },
				{ "poseIdleRefreshSeconds", Kind::Float, &poseIdleRefreshSeconds },
				{ "poseGateEnabled", Kind::Bool, &poseGateEnabled },
				{ "poseMaxDistance", Kind::Float, &poseMaxDistance },
				{ "poseExitDistance", Kind::Float, &poseExitDistance },
				{ "poseMaxLateral", Kind::Float, &poseMaxLateral },
				{ "poseExitLateral", Kind::Float, &poseExitLateral },
				{ "poseLookConeDegrees", Kind::Float, &poseLookConeDegrees },
				{ "poseLookConeExitDegrees", Kind::Float, &poseLookConeExitDegrees },
				{ "poseWidgetAlways", Kind::Bool, &poseWidgetAlways },
				{ "poseFrozenDim", Kind::Float, &poseFrozenDim },
				{ "hideWidgetHousing", Kind::Bool, &hideWidgetHousing },
				{ "widgetFitEnabled", Kind::Bool, &widgetFitEnabled },
				{ "widgetApertureRadius", Kind::Float, &widgetApertureRadius },
				{ "perScopeAperture", Kind::Bool, &perScopeAperture },
				{ "widgetOffsetX", Kind::Float, &widgetOffsetX },
				{ "widgetOffsetY", Kind::Float, &widgetOffsetY },
				{ "widgetOffsetZ", Kind::Float, &widgetOffsetZ },
				{ "widgetAutoPlace", Kind::Bool, &widgetAutoPlace },
				{ "retryAfterFault", Kind::Bool, &retryAfterFault },
				{ "suppressScopeImods", Kind::Bool, &suppressScopeImods },
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
				// v0.2.74: writing perfTimers resets the timing window, whatever the
				// value. Doing it here rather than on the render thread's view of the
				// flag is the whole fix -- see ScopeRender::ResetStageTimers.
				if (std::string_view{ a_e.name } == "perfTimers") {
					TrueScopes::ScopeRender::ResetStageTimers();
				}
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



		// ------------------------------------------------------------- handlers

		std::string HandleIndex()
		{
			// Self-describing: an agent that has never seen this server can
			// discover the whole surface with one GET.
			return
				"{\"ok\":true,\"plugin\":\"TrueScopesVR\",\"tier\":1,\"endpoints\":["
				"{\"path\":\"/health\",\"desc\":\"liveness + identity; answered without touching game state\"},"
				"{\"path\":\"/state\",\"desc\":\"full render diagnostics: fault latch, lastStep, NaN/sun/camdata counters, last-render pass+light+sky+viewport values, stageTimes (per-stage GPU+CPU ms)\"},"
			"{\"path\":\"/perf/reset\",\"desc\":\"clear the stageTimes averaging window before the next render (also done by any perfTimers write)\"},"
				"{\"path\":\"/addresses\",\"desc\":\"resolved engine pointers (ssn, accumulator, gfxState, render ctx, sun config, rtm, renderer, scope camera) as va+rva - feed them to /read\"},"
				"{\"path\":\"/config\",\"desc\":\"every TOML setting and its live value\"},"
				"{\"path\":\"/config/set\",\"desc\":\"?key=NAME&value=V - set one setting live, no scope-cycle needed\"},"
				"{\"path\":\"/config/reload\",\"desc\":\"re-read TrueScopesVR.toml now\"},"
				"{\"path\":\"/resolve\",\"desc\":\"?addr=EXPR - resolve an address expression to a VA + RVA\"},"
				"{\"path\":\"/log\",\"desc\":\"?tail=N&grep=SUBSTR - last N lines of TrueScopesVR.log\"},"
				"{\"path\":\"/scope\",\"desc\":\"?probe=1 - which scope is equipped: weapon formID, zoomData fovMult, the weapon 3D node names, the ATTACHED OMOD model paths, the aperture in use and whether it resolved by path or by node name\"},"
				"{\"path\":\"/omods\",\"desc\":\"?filter=scope&limit=400 - every weapon mod (OMOD) in the CURRENT load order with the strings it points at (model path, display name); no equipping needed\"}"
				"],\"addrExpr\":\"expr := term (('+'|'-') term)* ; term := '[' expr ']' | 'base' | 0xHEX | DEC\"}";
		}

		std::string HandleHealth()
		{
			std::string out = "{\"ok\":true";
			out += ",\"plugin\":\"TrueScopesVR\"";
			out += ",\"version\":\"" + std::string{ Version::NAME } + "\"";
			out += ",\"pid\":" + std::to_string(::GetCurrentProcessId());
			out += ",\"uptimeMs\":" + std::to_string(::GetTickCount64() - g_startTick);
			out += ",\"requests\":" + std::to_string(g_requests.load());
			out += "}";
			return out;
		}

		// Every value the heartbeat prints, available on demand instead of every
		// 300th render. Before this, answering "is the sun pass running / is
		// anything NaN / did the sky draw" meant grepping a log line that may not
		// have been emitted yet.
		std::string HandleState()
		{
			const auto d = TrueScopes::ScopeRender::GetDiagnostics();
			const auto b = [](bool v) { return std::string{ v ? "true" : "false" }; };

			std::string out = "{\"ok\":true";
			out += ",\"renderAvailable\":" + b(d.available);
			out += ",\"faulted\":" + b(d.faulted);
			out += ",\"lastStep\":" + std::to_string(d.lastStep);
			out += ",\"renders\":" + std::to_string(d.renders);
			// v0.2.70 perf instrument: game frames (advances scope-up AND scope-down) plus
			// the scope state the sample was taken in, so a perf run records its own
			// condition instead of relying on the operator's notes.
			out += ",\"frames\":" + std::to_string(TrueScopes::Hooks::FrameCount());
			out += ",\"scopeActive\":" + b(TrueScopes::Hooks::ScopeActive());
			out += ",\"widgetPresence\":" + b(TrueScopes::Hooks::WidgetPresenceShown());
			out += ",\"inOwnResolve\":" + b(TrueScopes::ScopeRender::InOwnResolve());
			out += ",\"ownRenderThread\":" + std::to_string(TrueScopes::ScopeRender::OwnRenderThread());
			out += ",\"sunBindHooks\":" + b(d.sunBindHooks);

			out += ",\"counters\":{";
			out += ",\"invProjRejects\":" + std::to_string(d.invProjRejects);
			out += ",\"fogNulls\":" + std::to_string(d.fogNulls);
			out += "}";

			out += ",\"lastRender\":{";
			out += "\"passTotal\":" + std::to_string(d.passTotal);
			out += ",\"lightsShadowed\":" + std::to_string(d.lightsShadowed);
			out += ",\"lightsQueued\":" + std::to_string(d.lightsQueued);
			out += ",\"eyeCount\":" + std::to_string(d.eyeCount);
			out += ",\"sunPass\":" + std::to_string(d.sunPass);
			out += ",\"sunIsSSN\":" + std::to_string(d.sunIsSSN);
			// v0.2.76: sunPass = "we called the exec"; sunDrew = its return value, i.e.
			// whether FUN_142891040's technique gate actually let the draw happen.
			out += ",\"sunDrew\":" + std::to_string(d.sunDrew);
			out += ",\"sunDrewCount\":" + std::to_string(d.sunDrewCount);
			out += ",\"sunGatedCount\":" + std::to_string(d.sunGatedCount);
			out += ",\"sunCfgFlags\":" + Quote(Hex(d.sunCfgFlags));
			out += ",\"skyRoots\":" + std::to_string(d.skyRoots);
			out += ",\"skyDrawn\":" + std::to_string(d.skyDrawn);
			out += ",\"lightsClamp\":" + std::to_string(d.lightsClamp);
			{
				const auto lc = TrueScopes::LensComposite::GetDiag();
				out += ",\"lensComposite\":{\"ready\":" + std::string(lc.ready ? "true" : "false");
				out += ",\"reticleRT\":" + std::to_string(lc.reticleRT);
				out += ",\"reticlePhys\":" + std::to_string(lc.reticlePhys);
				out += ",\"runs\":" + std::to_string(lc.runs);
				out += ",\"skips\":" + std::to_string(lc.skips);
				out += ",\"dims\":" + std::to_string(lc.dims);
				out += ",\"quadHidden\":" + std::string(lc.quadHidden ? "true" : "false");
				out += ",\"renderer\":" + Quote(lc.rendererName) + "}";
			}
			// v0.2.116 — pose-based activation: the live pose numbers + verdicts.
			// `evals` advancing = the verdict hook is being reached (weapon drawn,
			// gun, has-scope, no blocking menu); `owned` = the pose test is the
			// decider; dist/lateral/lookDeg are the last measured pose — the
			// numbers to read while tuning the pose* knobs live.
			{
				const auto pg = TrueScopes::PoseGate::GetDiag();
				const auto f = [](float v) {
					char buf[48];
					if (std::isfinite(v)) std::snprintf(buf, sizeof(buf), "%.3g", v);
					else                  std::snprintf(buf, sizeof(buf), "\"%s\"", std::isnan(v) ? "NaN" : "Inf");
					return std::string(buf);
				};
				out += ",\"poseGate\":{\"enabled\":" + b(pg.enabled);
				out += ",\"owned\":" + b(pg.owned);
				out += ",\"fillLive\":" + b(pg.fillLive);
				out += ",\"dist\":" + f(pg.dist);
				out += ",\"lateral\":" + f(pg.lateral);
				out += ",\"lookDeg\":" + f(pg.lookDeg);
				out += ",\"evals\":" + std::to_string(pg.evals) + "}";
			}
			out += ",\"camRect\":[";
			for (int k = 0; k < 6; ++k) {
				if (k) out += ",";
				char buf[48];
				// camRect[4]/[5] are the D3D11 viewport min/max depth. They were
				// garbage until v0.2.64 (XMM-argument bug); a non-(0,1) pair here
				// means that regressed.
				if (std::isfinite(d.camRect[k])) std::snprintf(buf, sizeof(buf), "%.9g", d.camRect[k]);
				else                             std::snprintf(buf, sizeof(buf), "\"%s\"", std::isnan(d.camRect[k]) ? "NaN" : "Inf");
				out += buf;
			}
			out += "],\"viewportRaw\":[";
			for (int k = 0; k < 6; ++k) {
				if (k) out += ",";
				out += std::to_string(d.viewport[k]);
			}
			out += "]}";

			// v0.2.73 stage stopwatch. Mean ms per stage of OUR render since the last
			// reset, on both timelines. Reset the window with
			//   /config/set?key=perfTimers&value=false  then  ...&value=true
			// The GPU and CPU sample counts are reported separately on purpose: they
			// diverge when the query ring is saturated, and a reader who assumes they
			// match would silently average two different windows.
			{
				const auto t = TrueScopes::ScopeRender::GetStageTimes();
				const auto ms = [](double v) {
					char buf[48];
					std::snprintf(buf, sizeof(buf), "%.3f", std::isfinite(v) ? v : 0.0);
					return std::string{ buf };
				};
				out += ",\"stageTimes\":{";
				out += "\"enabled\":" + b(t.enabled);
				out += ",\"gpuQueries\":" + b(t.available);
				out += ",\"gpuSamples\":" + std::to_string(t.gpuSamples);
				out += ",\"cpuSamples\":" + std::to_string(t.cpuSamples);
				out += ",\"disjointDiscarded\":" + std::to_string(t.disjoint);
				out += ",\"gpuTotalMs\":" + ms(t.gpuTotalMs);
				out += ",\"cpuTotalMs\":" + ms(t.cpuTotalMs);
				out += ",\"gpuMs\":{";
				for (std::size_t i = 0; i < TrueScopes::ScopeRender::kStageCount; ++i) {
					if (i) out += ",";
					out += Quote(TrueScopes::ScopeRender::kStageNames[i]) + ":" + ms(t.gpuMs[i]);
				}
				out += "},\"cpuMs\":{";
				for (std::size_t i = 0; i < TrueScopes::ScopeRender::kStageCount; ++i) {
					if (i) out += ",";
					out += Quote(TrueScopes::ScopeRender::kStageNames[i]) + ":" + ms(t.cpuMs[i]);
				}
				out += "}}";
			}

			out += ",\"moduleBase\":" + Quote(Hex(REL::Module::get().base()));
			out += "}";
			return out;
		}

		// The engine pointers we resolve at Init. Publishing them turns /read into
		// a general probe: you can walk the sun pass config or the render context
		// without re-deriving an address from a call site first.
		std::string HandleAddresses()
		{
			const auto  d = TrueScopes::ScopeRender::GetDiagnostics();
			const auto  base = REL::Module::get().base();
			std::string out = "{\"ok\":true,\"base\":" + Quote(Hex(base)) + ",\"addresses\":{";

			const std::pair<const char*, std::uintptr_t> rows[] = {
				{ "ssnArray", d.ssnArray },
				{ "accumulator", d.accum },
				{ "gfxState", d.gfxState },
				{ "ctxPtrA", d.ctxPtrA },
				{ "ctxPtrB", d.ctxPtrB },
				{ "sunConfig", d.sunConfig },
				{ "rtManager", d.rtm },
				{ "renderer", d.renderer },
				{ "scopeCamera", d.camera },
			};
			bool first = true;
			for (const auto& [name, va] : rows) {
				if (!first) out += ",";
				first = false;
				// Report both forms: the VA for a direct /read, and the RVA so it can
				// be pasted straight into Ghidra. 0 = not resolved yet (Init failed,
				// or no render has run so the per-render pointers are unset).
				// `rva` appears ONLY for addresses genuinely inside the image —
				// heap objects (the accumulator, the scope camera) get "heap":true
				// instead, because an RVA for them would be a plausible-looking lie.
				out += Quote(name) + ":{\"va\":" + Quote(Hex(va));
				if (va == 0) {
					out += ",\"resolved\":false";
				} else if (InModule(va)) {
					out += ",\"rva\":" + Quote(Hex(va - base));
				} else {
					out += ",\"heap\":true";
				}
				out += "}";
			}
			out += "}}";
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
					// The automatic placement is latched per equip, so a widget setting
					// changed live would otherwise not take effect until the weapon
					// was re-equipped — which reads as "the setting does nothing".
					if (std::string_view{ e.name }.starts_with("widget")) {
						TrueScopes::ScopeRender::InvalidatePlacement();
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

		// ------------------------------------------------- OMOD enumeration
		// Every weapon mod in the CURRENT load order, so we can see which scopes a
		// user actually has -- including modded ones the built-in aperture table has
		// never heard of. No equipping, no console, no interaction.
		//
		// The form arrays are read exactly as TESDataHandler::GetFormOfTypeAtIndex
		// (0x14011a470) does: data pointer at handler+0x68+type*0x18, u32 count at
		// handler+0x78+type*0x18. The singleton pointer is 0x145930b50, RIP-decoded
		// from "MOV RCX, qword ptr [...]" ahead of the GetSizeOfFormList call at
		// 0x14047fb94 -- not from a Ghidra data label, which this binary gets wrong
		// for high .data (see Addresses.h).
		constexpr std::uintptr_t kDataHandlerPtr = 0x5930b50;
		constexpr std::uint8_t   kFormTypeOMOD = 0x90;  // ENUM_FORM_ID::kOMOD
		constexpr std::uintptr_t kFormIDOffset = 0x14;

		// A form's strings are found by SCANNING its head for BSFixedString fields,
		// rather than by reading TESFullName+0x28 and TESModel+0x50 at their
		// CommonLibF4 (flatrim) offsets. Those are exactly the kind of thing that
		// shifts between flatrim and VR, and a wrong one reads adjacent memory as a
		// string -- which looks like data rather than like a bug. Reporting the slot
		// offset with every hit means the real layout shows up in the output.
		//
		// A BSFixedString field holds a BSStringPool::Entry*, NOT a char*. Measured
		// live 2026-08-10 against an OMOD's model field:
		//     entry+0x00  next/pool pointer
		//     entry+0x10  u32 length          (read 43)
		//     entry+0x18  the characters      ("Weapons\ReconScope\...nif", 43 long)
		// The first cut of this scan treated every pointer slot as a char*, which
		// found one "string" per form -- the texture-ID block at +0x58 rendering as
		// "w@B}dds" -- and no model paths at all.
		constexpr std::uintptr_t kStringPoolLength = 0x10;
		constexpr std::uintptr_t kStringPoolChars = 0x18;

		// Validate, do not trust: the declared length must agree with where the
		// terminator actually sits, and every character must be printable. A field
		// that fails simply is not reported.
		bool ReadFixedStringAt(std::uintptr_t a_field, char* a_out, std::size_t a_cap)
		{
			std::uintptr_t entry = 0;
			if (!SafeReadBytes(reinterpret_cast<const void*>(a_field), &entry, sizeof(entry)) || entry < 0x10000) {
				return false;
			}
			std::uint32_t len = 0;
			if (!SafeReadBytes(reinterpret_cast<const void*>(entry + kStringPoolLength), &len, sizeof(len))) {
				return false;
			}
			if (len == 0 || len > 400) {
				return false;
			}
			char buf[416] = {};
			if (!SafeReadBytes(reinterpret_cast<const void*>(entry + kStringPoolChars), buf, len + 1)) {
				return false;
			}
			if (buf[len] != '\0') {
				return false;
			}
			for (std::uint32_t k = 0; k < len; ++k) {
				const auto c = static_cast<unsigned char>(buf[k]);
				if (c < 0x20 || c > 0x7e) {
					return false;
				}
			}
			const std::size_t copy = (len < a_cap - 1) ? len : a_cap - 1;
			std::memcpy(a_out, buf, copy);
			a_out[copy] = '\0';
			return true;
		}

		std::string HandleOmods(const Request& a_req)
		{
			std::string filter = a_req.GetOr("filter", "");
			for (auto& c : filter) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			std::int64_t limit = 400;
			{
				const std::string l = a_req.GetOr("limit", "400");
				std::from_chars(l.data(), l.data() + l.size(), limit);
			}
			limit = std::clamp<std::int64_t>(limit, 1, 4000);

			const auto     base = REL::Module::get().base();
			std::uintptr_t handler = 0;
			if (!SafeReadBytes(reinterpret_cast<const void*>(base + kDataHandlerPtr), &handler, sizeof(handler)) || !handler) {
				return "{\"ok\":false,\"error\":\"TESDataHandler singleton is null - is a save loaded?\"}";
			}

			std::uintptr_t data = 0;
			std::uint32_t  count = 0;
			const auto     slot = static_cast<std::uintptr_t>(kFormTypeOMOD) * 0x18;
			if (!SafeReadBytes(reinterpret_cast<const void*>(handler + 0x68 + slot), &data, sizeof(data)) ||
				!SafeReadBytes(reinterpret_cast<const void*>(handler + 0x78 + slot), &count, sizeof(count))) {
				return "{\"ok\":false,\"error\":\"could not read the OMOD form array\"}";
			}
			if (count > 200000) {
				return "{\"ok\":false,\"error\":\"OMOD count looks wrong (" + std::to_string(count) + ") - layout mismatch, refusing to walk it\"}";
			}

			std::string out = "{\"ok\":true,\"total\":" + std::to_string(count);
			out += ",\"filter\":" + Quote(filter) + ",\"rows\":[";

			std::int64_t returned = 0;
			bool         first = true;
			for (std::uint32_t i = 0; i < count && returned < limit; ++i) {
				std::uintptr_t form = 0;
				if (!SafeReadBytes(reinterpret_cast<const void*>(data + i * 8ull), &form, sizeof(form)) || !form) {
					continue;
				}
				std::uint32_t formID = 0;
				SafeReadBytes(reinterpret_cast<const void*>(form + kFormIDOffset), &formID, sizeof(formID));

				// Collect every printable string the form points at, with the slot
				// offset it came from -- the offsets ARE the layout finding.
				std::vector<std::pair<std::uintptr_t, std::string>> strings;
				for (std::uintptr_t off = 0; off <= 0xC0; off += 8) {
					char text[416] = {};
					if (ReadFixedStringAt(form + off, text, sizeof(text))) {
						strings.emplace_back(off, text);
					}
				}
				if (strings.empty()) {
					continue;
				}

				if (!filter.empty()) {
					bool hit = false;
					for (const auto& [off, text] : strings) {
						std::string lower = text;
						for (auto& c : lower) {
							c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
						}
						if (lower.find(filter) != std::string::npos) {
							hit = true;
							break;
						}
					}
					if (!hit) {
						continue;
					}
				}

				if (!first) {
					out += ",";
				}
				first = false;
				++returned;
				out += "{\"formID\":" + Quote(Hex(formID)) + ",\"strings\":{";
				bool f2 = true;
				for (const auto& [off, text] : strings) {
					if (!f2) {
						out += ",";
					}
					f2 = false;
					out += Quote(Hex(off)) + ":" + Quote(text);
				}
				out += "}}";
			}
			out += "],\"returned\":" + std::to_string(returned) + "}";
			return out;
		}

		// Which scope is equipped, and everything the per-scope fit derives from it.
		// The node NAMES are the point: when the aperture falls back, they are what a
		// user needs to add a [Scopes] entry, and reading them here beats grepping a
		// log line that only prints on a scope-in.
		std::string HandleScope(const Request& a_req)
		{
			if (a_req.GetOr("probe", "0") != "0") {
				// A probe runs on the render thread, so it needs a render to happen.
				// Wait briefly rather than returning last cycle's answer to a caller
				// who explicitly asked for a fresh one.
				const auto before = TrueScopes::ScopeIdent::Get();
				// The placement is derived from what the probe measures, so a fresh
				// probe must produce a fresh candidate rather than reporting the one
				// latched from the previously equipped optic.
				TrueScopes::ScopeRender::InvalidatePlacement();
				TrueScopes::ScopeIdent::Request();
				const auto deadline = ::GetTickCount64() + 3000;
				while (::GetTickCount64() < deadline) {
					const auto now = TrueScopes::ScopeIdent::Get();
					if (now.probed && (!before.probed || now.nodesVisited != before.nodesVisited ||
										  now.weaponFormID != before.weaponFormID)) {
						break;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
				}
			}

			const auto  i = TrueScopes::ScopeIdent::Get();
			std::string out = "{\"ok\":true";
			out += ",\"probed\":" + std::string(i.probed ? "true" : "false");
			out += ",\"faulted\":" + std::string(i.faulted ? "true" : "false");
			out += ",\"weaponFormID\":" + Quote(Hex(i.weaponFormID));
			out += ",\"weaponNodeName\":" + Quote(i.weaponNodeName);
			out += ",\"weaponNode\":" + Quote(Hex(i.weaponNode));
			out += ",\"fovMult\":" + std::to_string(i.fovMult);
			out += ",\"zoomOverlay\":" + std::to_string(i.zoomOverlay);
			out += ",\"zoomImodID\":" + Quote(Hex(i.zoomImodID));
			out += ",\"zoomFovAt90\":" + std::to_string(i.zoomFovAt90);
			{
				const auto f = TrueScopes::ScopeRender::GetFovInfo();
				out += ",\"fovUsed\":" + std::to_string(f.used);
				out += ",\"fovDerived\":" + std::to_string(f.derived);
				out += ",\"lensRadiusWorld\":" + std::to_string(f.discRadius);
				out += ",\"eyeToLens\":" + std::to_string(f.eyeDistance);
			}
			{
				// v0.2.92: the automatic-placement candidate, reported whether or not
				// it is applied — the whole point is to check it against a hand-tuned
				// offset that is already known to be right.
				const auto p = TrueScopes::ScopeRender::GetPlacement();
				out += ",\"placement\":{\"valid\":" + std::string(p.valid ? "true" : "false");
				out += ",\"applied\":" + std::string(p.applied ? "true" : "false");
				out += ",\"reason\":" + Quote(p.reason);
				out += ",\"method\":" + Quote(p.method);
				out += ",\"haveBoth\":" + std::string(p.haveBoth ? "true" : "false");
				out += ",\"agreement\":" + std::to_string(p.agreement);
				out += ",\"converged\":" + std::string(p.converged ? "true" : "false");
				out += ",\"diverged\":" + std::string(p.diverged ? "true" : "false");
				out += ",\"steps\":" + std::to_string(p.steps);
				out += ",\"residual\":" + std::to_string(p.residual);
				out += ",\"bestResidual\":" + std::to_string(p.bestResidual);
				out += ",\"parentResidual\":" + std::to_string(p.parentResidual);
				out += ",\"discWorld\":" + Vec3(p.discWorld);
				out += ",\"offset\":" + Vec3(p.offset);
				out += ",\"target\":" + Vec3(p.target);
				out += ",\"baseWorld\":" + Vec3(p.baseWorld);
				out += ",\"boundCenter\":" + Vec3(p.boundCenter);
				out += ",\"boundRadius\":" + std::to_string(p.boundRadius);
				out += ",\"miss\":" + std::to_string(p.miss) + "}";
			}
			out += ",\"geom\":{\"have\":" + std::string(i.haveGeom ? "true" : "false");
			out += ",\"pScopeNode\":" + Quote(Hex(i.pScopeNode));
			out += ",\"pScopeWorld\":" + Vec3(i.pScope.world);
			out += ",\"pScopeRot\":[";
			for (std::size_t n = 0; n < 9; ++n) {
				if (n) {
					out += ",";
				}
				out += std::to_string(i.pScope.rot[n]);
			}
			out += "],\"unionCenter\":" + Vec3(i.unionCenter);
			out += ",\"unionRadius\":" + std::to_string(i.unionRadius);
			out += ",\"boundsSeen\":" + std::to_string(i.boundsSeen);
			out += ",\"haveFace\":" + std::string(i.haveFace ? "true" : "false");
			out += ",\"faceShape\":" + Quote(i.faceShape);
			out += ",\"face\":" + Vec3(i.face);
			out += ",\"shapes\":[";
			for (std::uint32_t n = 0; n < i.shapeCount; ++n) {
				if (n) {
					out += ",";
				}
				const auto& s = i.shapes[n];
				out += "{\"name\":" + Quote(s.name);
				out += ",\"world\":" + Vec3(s.world);
				out += ",\"scale\":" + std::to_string(s.scale);
				out += ",\"boundCenter\":" + Vec3(s.boundCenter);
				out += ",\"boundRadius\":" + std::to_string(s.boundRadius) + "}";
			}
			out += "]}";
			out += ",\"aperture\":" + std::to_string(i.aperture);
			out += ",\"screenAspect\":" + std::to_string(i.screenAspect);
			out += ",\"apertureSource\":" + Quote(i.fromTable ? i.matched : "widgetApertureRadius");
			out += ",\"fromTable\":" + std::string(i.fromTable ? "true" : "false");
			// HOW it resolved, not just to what. "node" on a modded load order is
			// the case where the answer can be confidently wrong, so it has to be
			// visible without reading the log.
			out += ",\"matchedBy\":" + Quote(i.matchedBy);
			out += ",\"overrideKey\":" + Quote(i.overrideKey);
			// The attached OMODs. When nothing matches, these paths ARE the answer:
			// they are what a user pastes into [Scopes] to teach the plugin an optic
			// it has never seen.
			out += ",\"mods\":{\"have\":" + std::string(i.haveMods ? "true" : "false");
			out += ",\"error\":" + Quote(i.modsError);
			out += ",\"stacksSeen\":" + std::to_string(i.stacksSeen);
			out += ",\"stackPick\":" + Quote(i.stackPick);
			out += ",\"overflow\":" + std::to_string(i.modOverflow);
			out += ",\"rows\":[";
			for (std::uint32_t n = 0; n < i.modCount; ++n) {
				if (n) {
					out += ",";
				}
				out += "{\"formID\":" + Quote(Hex(i.mods[n].formID));
				out += ",\"attachPoint\":" + std::to_string(i.mods[n].attachPoint);
				out += ",\"path\":" + Quote(i.mods[n].path);
				out += ",\"key\":" + Quote(i.mods[n].key) + "}";
			}
			out += "]}";
			out += ",\"nodesVisited\":" + std::to_string(i.nodesVisited);
			out += ",\"nameOverflow\":" + std::to_string(i.nameOverflow);
			out += ",\"clipped\":" + std::to_string(i.clipped);
			out += ",\"scopeNames\":[";
			for (std::uint32_t n = 0; n < i.scopeNameCount; ++n) {
				if (n) {
					out += ",";
				}
				out += Quote(i.scopeNames[n]);
			}
			out += "],\"names\":[";
			for (std::uint32_t n = 0; n < i.nameCount; ++n) {
				if (n) {
					out += ",";
				}
				out += Quote(i.names[n]);
			}
			// Keyed by model PATH, not by node name. Three rows share the node name
			// "LaserScope", so a name-keyed JSON object dropped two of them in every
			// parser that read this — the object form was itself asserting a
			// uniqueness the data does not have. Paths are unique by construction
			// (the census fails loudly if they ever are not).
			out += "],\"table\":{";
			bool first = true;
			for (const auto& e : TrueScopes::ScopeIdent::Table()) {
				if (!first) {
					out += ",";
				}
				first = false;
				out += Quote(e.path) + ":{\"node\":" + Quote(e.node) +
				       ",\"aperture\":" + std::to_string(e.aperture) +
				       ",\"aspect\":" + std::to_string(e.aspect) +
				       ",\"shape\":" + Quote(e.shape) + ",\"face\":" + Vec3(e.face) + "}";
			}
			out += "}}";
			return out;
		}

		// v0.2.108 — /attach?omod=HEX[&detach=1]: attach an OMOD to the equipped
		// weapon on the game's main thread (F4SE task queue) and wait for the
		// outcome. The headless gate ladder's missing primitive: FO4VR's console
		// amod cannot do this (it mods the selected reference, not the weapon).
		// v0.2.109 — /verdict[?since=SEQ]: the tester's last controller chord.
		// A driver script passes the seq it has already consumed; `fresh` says
		// whether a newer verdict has arrived since.
		std::string HandleVerdict(const Request& a_req)
		{
			TrueScopes::VerdictInput::RequestStart();
			const auto ev = TrueScopes::VerdictInput::Latest();
			std::uint64_t since = 0;
			{
				const std::string h(a_req.GetOr("since", "0"));
				since = std::strtoull(h.c_str(), nullptr, 10);
			}
			const char* name = "none";
			switch (ev.verdict) {
			case TrueScopes::VerdictInput::Verdict::kYes: name = "yes"; break;
			case TrueScopes::VerdictInput::Verdict::kNo: name = "no"; break;
			case TrueScopes::VerdictInput::Verdict::kSkip: name = "skip"; break;
			default: break;
			}
			std::string out = "{\"ok\":true";
			out += ",\"available\":" + std::string(TrueScopes::VerdictInput::Available() ? "true" : "false");
			out += ",\"seq\":" + std::to_string(ev.seq);
			out += ",\"fresh\":" + std::string(ev.seq > since ? "true" : "false");
			out += ",\"verdict\":" + Quote(name);
			out += ",\"ageMs\":" + std::to_string(ev.tickMs ? ::GetTickCount64() - ev.tickMs : 0);
			out += "}";
			return out;
		}

		std::string HandleAttach(const Request& a_req)
		{
			const auto  raw = a_req.GetOr("omod", "");
			if (raw.empty()) {
				return Err("omod parameter required (hex formID)");
			}
			std::uint32_t id = 0;
			{
				const std::string h(raw);
				const char*       p = h.c_str();
				if (h.rfind("0x", 0) == 0 || h.rfind("0X", 0) == 0) {
					p += 2;
				}
				char* end = nullptr;
				id = static_cast<std::uint32_t>(std::strtoul(p, &end, 16));
				if (!end || *end != 0 || id == 0) {
					return Err("omod must be a hex formID");
				}
			}
			const bool attach = a_req.GetOr("detach", "0") == "0";
			const auto task = F4SE::GetTaskInterface();
			if (!task) {
				return Err("no F4SE task interface");
			}
			struct Shared
			{
				std::atomic_bool                  done{ false };
				TrueScopes::ScopeIdent::AttachOutcome r{};
			};
			auto sh = std::make_shared<Shared>();
			task->AddTask([sh, id, attach] {
				sh->r = TrueScopes::ScopeIdent::AttachModToEquippedWeapon(id, attach);
				sh->done.store(true);
			});
			const auto deadline = ::GetTickCount64() + 5000;
			while (!sh->done.load() && ::GetTickCount64() < deadline) {
				std::this_thread::sleep_for(std::chrono::milliseconds(15));
			}
			if (!sh->done.load()) {
				return Err("timed out waiting for the main thread (is the game paused or at a menu?)");
			}
			std::string out = "{\"ok\":" + std::string(sh->r.ok ? "true" : "false");
			out += ",\"attach\":" + std::string(attach ? "true" : "false");
			out += ",\"weaponFormID\":" + Quote(Hex(sh->r.weaponFormID));
			if (!sh->r.ok) {
				out += ",\"error\":" + Quote(sh->r.error);
			}
			out += "}";
			return out;
		}

		std::string HandleScopeLookup(const Request& a_req)
		{
			const auto raw = a_req.GetOr("path", "");
			if (raw.empty()) {
				return Err("path parameter required");
			}
			const auto  r = TrueScopes::ScopeIdent::LookupModelPath(std::string(raw).c_str());
			std::string out = "{\"ok\":true";
			out += ",\"hit\":" + std::string(r.hit ? "true" : "false");
			out += ",\"fromToml\":" + std::string(r.fromToml ? "true" : "false");
			out += ",\"aperture\":" + std::to_string(r.aperture);
			out += ",\"aspect\":" + std::to_string(r.aspect);
			out += ",\"key\":" + Quote(r.key);
			out += ",\"node\":" + Quote(r.node);
			out += ",\"shape\":" + Quote(r.shape);
			out += "}";
			return out;
		}


		std::string Route(const Request& a_req)
		{
			if (a_req.path == "/" || a_req.path == "/index")   return HandleIndex();
			if (a_req.path == "/verdict")                       return HandleVerdict(a_req);
			if (a_req.path == "/attach")                        return HandleAttach(a_req);
			if (a_req.path == "/scope/lookup")                  return HandleScopeLookup(a_req);
			if (a_req.path == "/health")                        return HandleHealth();
			if (a_req.path == "/state")                         return HandleState();
			if (a_req.path == "/perf/reset") {
				TrueScopes::ScopeRender::ResetStageTimers();
				return "{\"ok\":true,\"reset\":\"stageTimes\"}";
			}
			if (a_req.path == "/addresses")                     return HandleAddresses();
			if (a_req.path == "/config")                        return HandleConfig();
			if (a_req.path == "/config/set")                    return HandleConfigSet(a_req);
			if (a_req.path == "/config/reload")                 return HandleConfigReload();
			if (a_req.path == "/resolve")                       return HandleResolve(a_req);
			if (a_req.path == "/log")                           return HandleLog(a_req);
			if (a_req.path == "/scope")                         return HandleScope(a_req);
			if (a_req.path == "/omods")                         return HandleOmods(a_req);
			return Err("no such endpoint '" + a_req.path + "' (GET / for the list)");
		}

	}

	void Init()
	{
		g_startTick = ::GetTickCount64();
		Settings::postLoadHook = &ReapplyOverrides;
		logger::info("devbench routes ready - served via the 'scope' tool registered on the devbench host (the :8930 HTTP listener was retired in v0.2.138)"sv);
	}

	std::string Invoke(std::string_view a_path, const std::vector<std::pair<std::string, std::string>>& a_params)
	{
		// Synthesise the same Request the parser would have produced and hand it to
		// the same Route(). Nothing here interprets a path or a parameter, on purpose:
		// the moment this function knows what "/scope" means, it can disagree with the
		// listener about it.
		Request req;
		req.method = "GET";
		req.path.assign(a_path);
		req.query = a_params;
		g_requests.fetch_add(1);
		try {
			return Route(req);
		} catch (const std::exception& e) {
			// Matches the listener's behaviour rather than letting an exception cross
			// the C-ABI boundary, where it would unwind through devbench's frames.
			return Err(std::string{ "handler threw: " } + e.what());
		} catch (...) {
			return Err("handler threw a non-standard exception");
		}
	}
}

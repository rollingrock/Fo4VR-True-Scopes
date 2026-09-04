#pragma once

#include <atomic>
#include <cstdint>

namespace TrueScopes::WidgetLifecycle
{
	// Cross-thread generation gate for the plugin-owned widget. Game-thread
	// lifecycle events invalidate the current generation immediately; only the
	// render thread may mark that exact generation fitted. A late completion from
	// an older weapon therefore cannot make a rebuilt/raw widget presentable.
	class EpochGate
	{
	public:
		using Epoch = std::uint64_t;

		[[nodiscard]] Epoch Invalidate() noexcept
		{
			const auto next = requested_.fetch_add(1, std::memory_order_acq_rel) + 1;
			fitted_.store(0, std::memory_order_release);
			return next;
		}

		void Withhold() noexcept
		{
			fitted_.store(0, std::memory_order_release);
		}

		[[nodiscard]] Epoch Current() const noexcept
		{
			return requested_.load(std::memory_order_acquire);
		}

		void MarkFitted(Epoch a_epoch) noexcept
		{
			// If an invalidate raced this fit, recording the old epoch is harmless:
			// Presentable compares both values and the new generation stays hidden.
			if (requested_.load(std::memory_order_acquire) == a_epoch) {
				fitted_.store(a_epoch, std::memory_order_release);
			}
		}

		[[nodiscard]] bool Presentable() const noexcept
		{
			const auto current = requested_.load(std::memory_order_acquire);
			return fitted_.load(std::memory_order_acquire) == current;
		}

	private:
		std::atomic<Epoch> requested_{ 1 };
		std::atomic<Epoch> fitted_{ 0 };
	};
}

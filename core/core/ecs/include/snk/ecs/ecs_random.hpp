#pragma once
#include <cstdint>

namespace ecs {

// Deterministic random numbers as a context resource: the same seed always yields the same
// sequence on every platform (PCG32; no std distributions, whose results are implementation
// defined). Systems ask for it as `ecs::rng&` instead of reaching for a global generator; the
// level seeds one per world from its own seed, so a replay with the same seed and inputs
// reproduces the run.
class rng
{
public:
	static constexpr uint64_t default_seed = 0x2545f4914f6cdd1dULL;

	explicit rng(uint64_t seed = default_seed) noexcept { reseed(seed); }

	// Raw PCG32 initial state and stream (reference parameters of the PCG paper).
	static rng from_state(uint64_t initial_state, uint64_t sequence) noexcept
	{
		rng r(0);
		r.seed_pcg(initial_state, sequence);
		r.m_seed = initial_state;
		return r;
	}

	void reseed(uint64_t seed) noexcept
	{
		m_seed = seed;
		seed_pcg(mix(seed), mix(seed ^ 0x9e3779b97f4a7c15ULL));
	}

	[[nodiscard]] uint64_t seed() const noexcept { return m_seed; }

	[[nodiscard]] uint32_t next_u32() noexcept
	{
		const uint64_t old = m_state;
		m_state = old * 6364136223846793005ULL + m_inc;
		const uint32_t xorshifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
		const uint32_t rot = static_cast<uint32_t>(old >> 59u);
		return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
	}

	[[nodiscard]] uint64_t next_u64() noexcept
	{
		const uint64_t hi = next_u32();
		return (hi << 32u) | next_u32();
	}

	// Uniform in [0, bound) without modulo bias; 0 when bound is 0.
	[[nodiscard]] uint32_t next_below(uint32_t bound) noexcept
	{
		if (bound == 0) return 0;
		const uint32_t threshold = (~bound + 1u) % bound;
		for (;;) {
			const uint32_t r = next_u32();
			if (r >= threshold) return r % bound;
		}
	}

	// Uniform in [lo, hi] inclusive.
	[[nodiscard]] int32_t next_int(int32_t lo, int32_t hi) noexcept
	{
		if (hi <= lo) return lo;
		const uint32_t span = static_cast<uint32_t>(hi) - static_cast<uint32_t>(lo) + 1u;
		if (span == 0) return static_cast<int32_t>(next_u32()); // the full int32 range
		return static_cast<int32_t>(static_cast<uint32_t>(lo) + next_below(span));
	}

	// Uniform in [0, 1).
	[[nodiscard]] float next_float() noexcept
	{
		return static_cast<float>(next_u32() >> 8u) * (1.0f / 16777216.0f);
	}

	[[nodiscard]] float next_float(float lo, float hi) noexcept { return lo + (hi - lo) * next_float(); }

	[[nodiscard]] bool next_bool() noexcept { return (next_u32() & 1u) != 0; }

private:
	static uint64_t mix(uint64_t x) noexcept // splitmix64 finalizer
	{
		x += 0x9e3779b97f4a7c15ULL;
		x = (x ^ (x >> 30u)) * 0xbf58476d1ce4e5b9ULL;
		x = (x ^ (x >> 27u)) * 0x94d049bb133111ebULL;
		return x ^ (x >> 31u);
	}

	void seed_pcg(uint64_t initial_state, uint64_t sequence) noexcept
	{
		m_state = 0;
		m_inc = (sequence << 1u) | 1u;
		(void)next_u32();
		m_state += initial_state;
		(void)next_u32();
	}

	uint64_t m_state = 0;
	uint64_t m_inc = 1;
	uint64_t m_seed = default_seed;
};

// Independent seed for a stream (a world, a subsystem) of a base seed.
[[nodiscard]] constexpr uint64_t derive_seed(uint64_t base, uint64_t stream) noexcept
{
	uint64_t x = base ^ (stream + 0x9e3779b97f4a7c15ULL + (base << 6u) + (base >> 2u));
	x = (x ^ (x >> 30u)) * 0xbf58476d1ce4e5b9ULL;
	x = (x ^ (x >> 27u)) * 0x94d049bb133111ebULL;
	return x ^ (x >> 31u);
}

} // namespace ecs

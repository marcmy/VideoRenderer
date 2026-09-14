/*
 * (C) 2018-2026 see Authors.txt
 *
 * Portable RIFE source-rate rules. No renderer or platform dependencies.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

struct RifeRateRule {
	uint32_t minLongEdge = 0;
	uint32_t maxLongEdge = 0;
	uint32_t minShortEdge = 0;
	uint32_t maxShortEdge = 0;
	uint32_t minFpsMilli = 0;
	uint32_t maxFpsMilli = 0;
	uint32_t maxMultiplierMilli = 2000;
	uint32_t maxOutputFpsMilli = 0;
	bool enabled = true;
	bool off = false;
	bool operator==(const RifeRateRule&) const = default;
};

struct RifeRateRules {
	bool enabled = false;
	uint32_t count = 0;
	std::array<RifeRateRule, 32> rules = {};
	bool operator==(const RifeRateRules&) const = default;
};

inline constexpr uint32_t RifeRateRulesMaxDimension = 65536;
inline constexpr uint32_t RifeRateRulesMaxFpsMilli = 1'000'000;
inline constexpr uint32_t RifeRateRulesMaxMultiplierMilli = 16'000;
inline constexpr size_t RifeRateRulesMaxTextLength = 4096;

namespace RifeRateRulesDetail {

constexpr bool ValidRange(uint32_t minimum, uint32_t maximum, uint32_t limit) noexcept
{
	return minimum <= limit && maximum <= limit
		&& (!minimum || !maximum || minimum <= maximum);
}

constexpr bool InRange(uint64_t value, uint32_t minimum, uint32_t maximum) noexcept
{
	return (!minimum || value >= minimum) && (!maximum || value <= maximum);
}

// Canonical ASCII decimal only: no signs, whitespace or leading zeroes.
// Read into a temporary configuration so a rejected value cannot change settings.
class TextReader {
public:
	explicit TextReader(std::wstring_view text) noexcept : m_text(text) {}

	bool Take(wchar_t delimiter) noexcept
	{
		if (m_text.empty() || m_text.front() != delimiter) {
			return false;
		}
		m_text.remove_prefix(1);
		return true;
	}

	bool Number(uint32_t limit, uint32_t& result) noexcept
	{
		if (m_text.empty() || m_text.front() < L'0' || m_text.front() > L'9') {
			return false;
		}
		const bool leadingZero = m_text.front() == L'0';
		uint32_t value = 0;
		size_t digits = 0;
		while (!m_text.empty() && m_text.front() >= L'0' && m_text.front() <= L'9') {
			const uint32_t digit = static_cast<uint32_t>(m_text.front() - L'0');
			if ((leadingZero && digits) || value > limit / 10
					|| (value == limit / 10 && digit > limit % 10)) {
				return false;
			}
			value = value * 10 + digit;
			++digits;
			m_text.remove_prefix(1);
		}
		result = value;
		return true;
	}

	bool Boolean(bool& result) noexcept
	{
		uint32_t value = 0;
		if (!Number(1, value)) {
			return false;
		}
		result = value != 0;
		return true;
	}

	bool Empty() const noexcept { return m_text.empty(); }

private:
	std::wstring_view m_text;
};

} // namespace RifeRateRulesDetail

constexpr bool ValidateRifeRateRule(const RifeRateRule& rule) noexcept
{
	using RifeRateRulesDetail::ValidRange;
	return ValidRange(rule.minLongEdge, rule.maxLongEdge, RifeRateRulesMaxDimension)
		&& ValidRange(rule.minShortEdge, rule.maxShortEdge, RifeRateRulesMaxDimension)
		&& ValidRange(rule.minFpsMilli, rule.maxFpsMilli, RifeRateRulesMaxFpsMilli)
		// Long edges cannot be shorter than the minimum allowed short edge.
		&& (!rule.maxLongEdge || rule.minShortEdge <= rule.maxLongEdge)
		&& (!rule.maxMultiplierMilli || (rule.maxMultiplierMilli >= 1000
			&& rule.maxMultiplierMilli <= RifeRateRulesMaxMultiplierMilli))
		&& rule.maxOutputFpsMilli <= RifeRateRulesMaxFpsMilli;
}

// count defines the used prefix. Disabled rows in that prefix are also validated;
// unused array entries do not participate in validation, matching or storage.
constexpr bool ValidateRifeRateRules(const RifeRateRules& config) noexcept
{
	if (config.count > config.rules.size()) {
		return false;
	}
	for (uint32_t i = 0; i < config.count; ++i) {
		if (!ValidateRifeRateRule(config.rules[i])) {
			return false;
		}
	}
	return true;
}

// Pass content dimensions BEFORE tensor alignment/padding. Matching is independent
// of portrait/landscape orientation. Both range endpoints are inclusive; zero is
// unbounded. This only selects a rule; the caller applies Off or caps its main mode.
inline int MatchRifeRateRule(const RifeRateRules& config, uint32_t width,
	uint32_t height, int64_t duration100ns) noexcept
{
	if (!config.enabled || config.count > config.rules.size()
			|| !width || !height || duration100ns <= 0) {
		return -1;
	}
	const uint32_t longEdge = width >= height ? width : height;
	const uint32_t shortEdge = width >= height ? height : width;
	const uint64_t duration = static_cast<uint64_t>(duration100ns);
	// Use integer rounding rather than truncation: nominal 166667 ticks is 60.000
	// fps, not 59.999. The sum cannot overflow for any positive int64_t duration.
	const uint64_t fpsMilli = (10'000'000'000ULL + duration / 2) / duration;
	for (uint32_t i = 0; i < config.count; ++i) {
		const auto& rule = config.rules[i];
		if (rule.enabled && ValidateRifeRateRule(rule)
				&& RifeRateRulesDetail::InRange(longEdge, rule.minLongEdge, rule.maxLongEdge)
				&& RifeRateRulesDetail::InRange(shortEdge, rule.minShortEdge, rule.maxShortEdge)
				&& RifeRateRulesDetail::InRange(fpsMilli, rule.minFpsMilli, rule.maxFpsMilli)) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

// Version 1 grammar, with no whitespace or terminator:
// RIFE-RULES/1;<master:0|1>;<count>[;<enabled>,<off>,<minLong>,<maxLong>,
//   <minShort>,<maxShort>,<minFpsMilli>,<maxFpsMilli>,<multMilli>,<outFpsMilli>]...
// There are exactly count rows. Invalid configurations serialize to an empty
// string; callers should reject them rather than store the empty string.
inline std::wstring SerializeRifeRateRules(const RifeRateRules& config)
{
	if (!ValidateRifeRateRules(config)) {
		return {};
	}
	std::wstring text = L"RIFE-RULES/1;";
	text += config.enabled ? L"1;" : L"0;";
	text += std::to_wstring(config.count);
	for (uint32_t i = 0; i < config.count; ++i) {
		const auto& rule = config.rules[i];
		text += rule.enabled ? L";1," : L";0,";
		text += rule.off ? L"1" : L"0";
		for (const uint32_t value : {rule.minLongEdge, rule.maxLongEdge,
			rule.minShortEdge, rule.maxShortEdge, rule.minFpsMilli, rule.maxFpsMilli,
			rule.maxMultiplierMilli, rule.maxOutputFpsMilli}) {
			text += L',';
			text += std::to_wstring(value);
		}
	}
	return text;
}

// Bounded, allocation-free and atomic on failure. The unused suffix is reset to
// defaults on success; only the declared prefix is serialized.
inline bool ParseRifeRateRules(std::wstring_view text, RifeRateRules& config) noexcept
{
	constexpr std::wstring_view prefix = L"RIFE-RULES/1;";
	if (text.size() > RifeRateRulesMaxTextLength || !text.starts_with(prefix)) {
		return false;
	}
	RifeRateRules candidate;
	RifeRateRulesDetail::TextReader reader(text.substr(prefix.size()));
	if (!reader.Boolean(candidate.enabled) || !reader.Take(L';')
			|| !reader.Number(static_cast<uint32_t>(candidate.rules.size()), candidate.count)) {
		return false;
	}
	for (uint32_t i = 0; i < candidate.count; ++i) {
		auto& rule = candidate.rules[i];
		if (!reader.Take(L';') || !reader.Boolean(rule.enabled)
				|| !reader.Take(L',') || !reader.Boolean(rule.off)
				|| !reader.Take(L',') || !reader.Number(RifeRateRulesMaxDimension, rule.minLongEdge)
				|| !reader.Take(L',') || !reader.Number(RifeRateRulesMaxDimension, rule.maxLongEdge)
				|| !reader.Take(L',') || !reader.Number(RifeRateRulesMaxDimension, rule.minShortEdge)
				|| !reader.Take(L',') || !reader.Number(RifeRateRulesMaxDimension, rule.maxShortEdge)
				|| !reader.Take(L',') || !reader.Number(RifeRateRulesMaxFpsMilli, rule.minFpsMilli)
				|| !reader.Take(L',') || !reader.Number(RifeRateRulesMaxFpsMilli, rule.maxFpsMilli)
				|| !reader.Take(L',') || !reader.Number(RifeRateRulesMaxMultiplierMilli, rule.maxMultiplierMilli)
				|| !reader.Take(L',') || !reader.Number(RifeRateRulesMaxFpsMilli, rule.maxOutputFpsMilli)) {
			return false;
		}
	}
	if (!reader.Empty() || !ValidateRifeRateRules(candidate)) {
		return false;
	}
	config = candidate;
	return true;
}

// Optional editable example, never a default. 30.001 is the first millifps above
// 30; it avoids overlap with the inclusive <=30 rule, including nominal 30 fps.
inline RifeRateRules MakeExampleRifeRateRules() noexcept
{
	RifeRateRules config;
	config.enabled = true;
	config.count = 4;
	config.rules[0] = {0, 1920, 0, 1080, 0, 30'000, 4000, 0, true, false};
	config.rules[1] = {0, 1920, 0, 1080, 30'001, 60'000, 2000, 0, true, false};
	config.rules[2] = {1921, 4096, 0, 2160, 0, 30'000, 2000, 0, true, false};
	config.rules[3] = {1921, 4096, 0, 2160, 30'001, 0, 0, 0, true, true};
	return config;
}

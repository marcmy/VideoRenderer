#pragma once

constexpr bool CanUseDirectMaxineInput(
		const bool inputIsDirectCompatible,
		const bool forceInputStaging) noexcept
{
	return inputIsDirectCompatible && !forceInputStaging;
}

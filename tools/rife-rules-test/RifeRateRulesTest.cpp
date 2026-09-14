#include "RifeRateRules.h"

#include <iostream>
#include <string>

namespace {
int failures = 0;
void Check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

void TestExampleRules() {
    const auto rules = MakeExampleRifeRateRules();
    Check(MatchRifeRateRule(rules, 1920, 1080, 333333) == 0, "1080p30 selects 4x rule");
    Check(MatchRifeRateRule(rules, 1080, 1920, 166667) == 1, "portrait 1080p60 selects 2x rule");
    Check(MatchRifeRateRule(rules, 3840, 2160, 333667) == 2, "4K29.97 selects 2x rule");
    Check(MatchRifeRateRule(rules, 3840, 2160, 166667) == 3, "4K60 selects off rule");
    Check(MatchRifeRateRule(rules, 7680, 4320, 166667) == -1, "unmatched source uses normal setting");
}

void TestFirstMatchAndDisabledRows() {
    RifeRateRules rules;
    rules.enabled = true;
    rules.count = 3;
    rules.rules[0] = {0, 1920, 0, 1080, 0, 0, 3000, 0, false, false};
    rules.rules[1] = {0, 1920, 0, 1080, 0, 0, 2000, 0, true, false};
    rules.rules[2] = {0, 1920, 0, 1080, 0, 0, 4000, 0, true, false};
    Check(MatchRifeRateRule(rules, 1280, 720, 417083) == 1, "disabled rule is skipped and first enabled match wins");
    rules.enabled = false;
    Check(MatchRifeRateRule(rules, 1280, 720, 417083) == -1, "master disable bypasses all rules");
}

void TestFractionalRatesAndInclusiveBounds() {
    RifeRateRules rules;
    rules.enabled = true;
    rules.count = 1;
    rules.rules[0] = {1920, 1920, 1080, 1080, 59'940, 60'000, 2000, 0, true, false};
    Check(MatchRifeRateRule(rules, 1920, 1080, 166667) == 0, "rounded nominal 60 fps matches 60.000 upper bound");
    Check(MatchRifeRateRule(rules, 1920, 1080, 166833) == 0, "59.94 fps matches lower bound");
    Check(MatchRifeRateRule(rules, 1920, 1088, 166667) == -1, "matching uses visible content size, not padded height");
}

void TestValidationAndAtomicParse() {
    RifeRateRules invalid;
    invalid.enabled = true;
    invalid.count = 1;
    invalid.rules[0].minLongEdge = 2000;
    invalid.rules[0].maxLongEdge = 1000;
    Check(!ValidateRifeRateRules(invalid), "reversed dimension range rejected");
    invalid.rules[0] = {};
    invalid.rules[0].maxMultiplierMilli = 999;
    Check(!ValidateRifeRateRules(invalid), "sub-1x multiplier rejected");

    auto expected = MakeExampleRifeRateRules();
    const std::wstring text = SerializeRifeRateRules(expected);
    RifeRateRules parsed;
    Check(!text.empty() && ParseRifeRateRules(text, parsed) && parsed == expected, "serialized rules round-trip exactly");

    RifeRateRules unchanged = expected;
    const auto before = unchanged;
    Check(!ParseRifeRateRules(L"RIFE-RULES/1;1;1;1,0,0,1920,0,1080,0,60000,0999,0", unchanged), "non-canonical malformed text rejected");
    Check(unchanged == before, "failed parse is atomic");
    Check(!ParseRifeRateRules(L"RIFE-RULES/2;0;0", unchanged), "unknown serialization version rejected");
}
}

int main() {
    TestExampleRules();
    TestFirstMatchAndDisabledRows();
    TestFractionalRatesAndInclusiveBounds();
    TestValidationAndAtomicParse();
    if (failures) return 1;
    std::cout << "All RIFE rate-rule tests passed\n";
    return 0;
}

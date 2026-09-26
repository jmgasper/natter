// Natter - a minimal test harness (no external dependencies).
// SPDX-License-Identifier: MIT
#pragma once

#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace testing {

struct TestCase {
	const char* name;
	void (*function)();
};

std::vector<TestCase>& registry();
void fail(const char* file, int line, const std::string& message);

struct Registrar {
	Registrar(const char* name, void (*function)()) { registry().push_back({name, function}); }
};

template <typename T>
std::string show(const T& value)
{
	if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, const char*>
		|| std::is_same_v<T, char*> || std::is_array_v<T>) {
		return "\"" + std::string(value) + "\"";
	} else if constexpr (std::is_same_v<T, bool>) {
		return value ? "true" : "false";
	} else if constexpr (std::is_enum_v<T>) {
		return std::to_string(static_cast<long long>(value));
	} else if constexpr (requires(std::ostream& out, const T& v) { out << v; }) {
		std::ostringstream out;
		out << value;
		return out.str();
	} else {
		return "<value>";
	}
}

std::string fixture(const std::string& name);

}  // namespace testing

#define TEST(name) \
	static void test_##name(); \
	static testing::Registrar registrar_##name(#name, test_##name); \
	static void test_##name()

#define CHECK(condition) \
	do { \
		if (!(condition)) \
			testing::fail(__FILE__, __LINE__, "CHECK(" #condition ")"); \
	} while (0)

#define REQUIRE(condition) \
	do { \
		if (!(condition)) { \
			testing::fail(__FILE__, __LINE__, "REQUIRE(" #condition ")"); \
			return; \
		} \
	} while (0)

#define CHECK_EQ(a, b) \
	do { \
		/* copies: (a) may name a member of a temporary */ \
		const auto check_a_ = (a); \
		const auto check_b_ = (b); \
		if (!(check_a_ == check_b_)) { \
			testing::fail(__FILE__, __LINE__, std::string("CHECK_EQ(" #a ", " #b ")\n      left:  ") \
				+ testing::show(check_a_) + "\n      right: " + testing::show(check_b_)); \
		} \
	} while (0)

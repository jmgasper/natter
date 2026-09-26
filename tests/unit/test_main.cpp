// Natter - test runner: natter_tests [name-prefix ...]
// SPDX-License-Identifier: MIT

#include "testing.h"

#include <cstring>
#include <fstream>

#ifndef NATTER_FIXTURES_DIR
#define NATTER_FIXTURES_DIR "tests/fixtures"
#endif

namespace testing {

namespace {
int sFailures = 0;
const char* sCurrent = "";
}


std::vector<TestCase>&
registry()
{
	static std::vector<TestCase> tests;
	return tests;
}


void
fail(const char* file, int line, const std::string& message)
{
	sFailures++;
	std::cerr << "  " << file << ":" << line << ": in " << sCurrent << "\n    " << message
		<< "\n";
}


std::string
fixture(const std::string& name)
{
	std::ifstream in(std::string(NATTER_FIXTURES_DIR) + "/" + name, std::ios::binary);
	if (!in) {
		fail(__FILE__, __LINE__, "missing fixture " + name);
		return {};
	}
	std::stringstream buffer;
	buffer << in.rdbuf();
	return buffer.str();
}

}  // namespace testing


int
main(int argc, char** argv)
{
	int run = 0;
	int failed = 0;
	for (const testing::TestCase& test : testing::registry()) {
		bool selected = argc < 2;
		for (int i = 1; i < argc; i++) {
			if (std::strncmp(test.name, argv[i], std::strlen(argv[i])) == 0)
				selected = true;
		}
		if (!selected)
			continue;
		int before = testing::sFailures;
		testing::sCurrent = test.name;
		try {
			test.function();
		} catch (const std::exception& exception) {
			testing::fail(__FILE__, __LINE__, std::string("exception: ") + exception.what());
		}
		run++;
		bool ok = testing::sFailures == before;
		if (!ok)
			failed++;
		std::cout << (ok ? "[ OK ] " : "[FAIL] ") << test.name << "\n";
	}
	std::cout << run - failed << "/" << run << " tests passed\n";
	if (run == 0) {
		std::cerr << "no tests matched\n";
		return 1;
	}
	return failed == 0 ? 0 : 1;
}

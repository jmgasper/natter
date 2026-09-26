# Natter tests: unit tests (C++), the mock-Slack end-to-end test and an
# opt-in integration test against a real workspace (both Python 3, stdlib only).

set(NATTER_TEST_TMP ${CMAKE_CURRENT_BINARY_DIR}/tmp)
file(MAKE_DIRECTORY ${NATTER_TEST_TMP})

add_executable(natter_tests
	unit/test_main.cpp
	unit/test_api.cpp
	unit/test_credentials.cpp
	unit/test_events.cpp
	unit/test_http.cpp
	unit/test_mrkdwn.cpp
	unit/test_realtime.cpp
	unit/test_store.cpp
	unit/test_ws.cpp
)
target_link_libraries(natter_tests PRIVATE natter_core)
target_compile_definitions(natter_tests PRIVATE
	NATTER_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
	NATTER_TEST_TMP_DIR="${NATTER_TEST_TMP}")

# One CTest entry per group of unit tests (the name prefix).
foreach(group api http util credentials signin ws emoji mrkdwn events store poller realtime session)
	add_test(NAME unit.${group} COMMAND natter_tests ${group}_)
	set_tests_properties(unit.${group} PROPERTIES TIMEOUT 60)
endforeach()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND AND NATTER_BUILD_CLI)
	add_test(NAME e2e.mock_slack
		COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_e2e.py
			--cli $<TARGET_FILE:natter-cli>
			--fixtures ${CMAKE_CURRENT_SOURCE_DIR}/fixtures
			--work ${NATTER_TEST_TMP}/e2e)
	set_tests_properties(e2e.mock_slack PROPERTIES TIMEOUT 180)

	# Runs only with NATTER_TEST_TOKEN, NATTER_TEST_COOKIE and
	# NATTER_TEST_WORKSPACE set; reported as skipped otherwise.
	add_test(NAME integration.real_slack
		COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_real_slack.py
			--cli $<TARGET_FILE:natter-cli>)
	set_tests_properties(integration.real_slack PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()

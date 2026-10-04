FetchContent_Declare(
  arduinojson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.4.2
)
FetchContent_MakeAvailable(arduinojson)

add_executable(ClaudeBuddyTest
  ClaudeBuddyTest.cpp
  ${REPO_ROOT}/lib/ClaudeBuddy/BuddyProtocol.cpp
)
target_include_directories(ClaudeBuddyTest PRIVATE ${REPO_ROOT}/lib/ClaudeBuddy ${arduinojson_SOURCE_DIR}/src)
target_link_libraries(ClaudeBuddyTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ClaudeBuddyTest)

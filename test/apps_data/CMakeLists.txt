FetchContent_Declare(
  arduinojson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.4.2
)
FetchContent_MakeAvailable(arduinojson)

add_executable(AppsDataTest
  AppsDataTest.cpp
  ${REPO_ROOT}/lib/Stocks/StockQuoteParser.cpp
  ${REPO_ROOT}/lib/Stocks/StockWatchlistParser.cpp
  ${REPO_ROOT}/lib/Weather/WeatherParser.cpp
)
target_include_directories(AppsDataTest PRIVATE
  ${REPO_ROOT}/lib/Stocks
  ${REPO_ROOT}/lib/Weather
  ${arduinojson_SOURCE_DIR}/src
)
target_compile_definitions(AppsDataTest PRIVATE APPS_DATA_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_link_libraries(AppsDataTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(AppsDataTest)

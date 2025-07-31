/**
 * @brief Main test file for cswift library
 * 
 * @details This file includes the Catch2 main function
 */

#include <catch2/catch_session.hpp>

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
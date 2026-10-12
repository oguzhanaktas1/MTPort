#include <iostream>

#include "Cli.hpp"

int main(int argc, char** argv) {
    try {
        const auto args = mtport::cli::parseArgs(argc, argv);
        return mtport::cli::run(args, std::cout, std::cerr);
    } catch (const std::invalid_argument& e) {
        std::cerr << "mtport: " << e.what() << "\nTry: mtport --help\n";
        return mtport::cli::kUsage;
    }
}

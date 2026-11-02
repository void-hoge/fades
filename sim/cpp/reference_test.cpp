#include "reference_solver.hpp"
#include <chrono>
#include <fstream>
#include <iostream>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    try {
        std::string text(81, '.');
        text[0] = '1'; text[8] = '9'; text[9] = '2'; text[80] = '4';
        auto parsed = reference::parse(text);
        require(parsed[0] == 1 && parsed[8] == 256 && parsed[9] == 2 && parsed[80] == 8,
                "Text cell order or digit mask");
        require(parsed[1] == reference::all, "Dot must mean all candidates");
        require(reference::parse(text+"\r") == parsed, "CRLF input");
        for (const auto& bad : {std::string(80, '.'), std::string(82, '.'), text+" "}) {
            bool rejected = false;
            try { (void)reference::parse(bad); } catch (const std::runtime_error&) { rejected = true; }
            require(rejected, "Invalid text length accepted");
        }
        for (char ch : {'0', '-', ' ', 'x', '\t', '\r'}) {
            auto bad = text; bad[40] = ch;
            bool rejected = false;
            try { (void)reference::parse(bad); } catch (const std::runtime_error&) { rejected = true; }
            require(rejected, "Invalid text cell accepted");
        }
        std::cout << "PASS: 81-character format, row-major masks, LF/CRLF and rejection paths\n";
        reference::Board empty;
        empty.fill(reference::all);
        auto multiple = reference::solve(empty, 2);
        require(multiple.solutions == 2, "Empty board must have multiple solutions");
        auto complete = reference::solve(multiple.first, 2);
        require(complete.solutions == 1 && complete.first == multiple.first, "Completed board");
        auto zero = empty;
        zero[0] = 0;
        require(!reference::solve(zero, 2).solutions, "Zero candidate cell");
        auto duplicate = empty;
        duplicate[0] = duplicate[1] = 1;
        require(!reference::solve(duplicate, 2).solutions, "Duplicate givens");
        auto hall = empty;
        for (unsigned i = 0; i < 9; ++i) hall[i] = i < 3 ? 3 : 508;
        require(!reference::solve(hall, 2).solutions, "Search-required contradiction");
        auto forced = empty;
        for (unsigned i = 1; i < 9; ++i) { forced[i] = 510; forced[9*i] = 509; }
        require(!reference::solve(forced, 2).solutions, "Conflicting hidden singles");
        // Lock digit 1 into the top row of the top-left box.
        auto pointing = empty;
        for (unsigned r = 1; r < 3; ++r)
            for (unsigned c = 0; c < 3; ++c) pointing[9*r+c] &= ~1u;
        require(reference::propagate(pointing), "Pointing contradiction");
        for (unsigned c = 3; c < 9; ++c)
            require(!(pointing[c] & 1), "Missing pointing elimination");
        auto claiming = empty;
        for (unsigned c = 3; c < 9; ++c) claiming[c] &= ~1u;
        require(reference::propagate(claiming), "Claiming contradiction");
        for (unsigned r = 1; r < 3; ++r)
            for (unsigned c = 0; c < 3; ++c)
                require(!(claiming[9*r+c] & 1), "Missing claiming elimination");
        if (argc > 1) {
            std::ifstream input(argv[1]);
            require(bool(input), "Cannot open dataset");
            unsigned count = 0;
            std::string line;
            auto start = std::chrono::steady_clock::now();
            while (std::getline(input, line)) {
                if (line.find_first_not_of(" \r\t") == std::string::npos) continue;
                auto result = reference::solve(reference::parse(line), 2);
                require(result.solutions == 1, "Dataset puzzle is not uniquely solvable");
                ++count;
            }
            require(count == 49158, "Incomplete dataset");
            std::cout << "PASS: " << count << " unique puzzles, "
                      << std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()
                      << " seconds\n";
        }
        std::cout << "PASS: reference solver directed cases\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

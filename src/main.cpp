#include "treadmill.hpp"
#include "web_server.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

struct Options {
    std::string bind{"0.0.0.0"};
    int port{8080};
    std::filesystem::path data_dir{"data"};
    std::filesystem::path static_dir{"data/static"};
    double watchdog{5.0};
};

Options parse_args(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value after " + arg);
            return argv[++i];
        };
        if (arg == "--bind") options.bind = next();
        else if (arg == "--port") options.port = std::stoi(next());
        else if (arg == "--data") options.data_dir = next();
        else if (arg == "--static") options.static_dir = next();
        else if (arg == "--watchdog") options.watchdog = std::stod(next());
        else if (arg == "--no-watchdog") options.watchdog = 0.0;
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Yadro treadmill core\n\n"
                      << "  --bind <ip>          bind address (default 0.0.0.0)\n"
                      << "  --port <n>           HTTP port (default 8080)\n"
                      << "  --data <dir>         writable data directory (default data)\n"
                      << "  --static <dir>       static web directory (default data/static)\n"
                      << "  --watchdog <sec>     stop belt when UI heartbeat disappears\n"
                      << "  --no-watchdog        disable control heartbeat watchdog\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown option: " + arg);
        }
    }
    if (options.port <= 0 || options.port > 65535) throw std::runtime_error("invalid port");
    return options;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_args(argc, argv);
        yadro::Limits limits;
        limits.watchdog_seconds = options.watchdog;

        auto driver = std::make_unique<yadro::SimulationDriver>();
        yadro::TreadmillController controller(std::move(driver), limits);
        yadro::WebServer server(controller, options.data_dir, options.static_dir);

        std::cout << "Yadro 0.1.0\n"
                  << "Driver: SimulationDriver\n"
                  << "Web UI: http://" << (options.bind == "0.0.0.0" ? "127.0.0.1" : options.bind)
                  << ':' << options.port << "/\n"
                  << "Listening on " << options.bind << ':' << options.port << "\n";

        if (!server.listen(options.bind, options.port)) {
            std::cerr << "HTTP server stopped or failed to listen\n";
            return 2;
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << '\n';
        return 1;
    }
}

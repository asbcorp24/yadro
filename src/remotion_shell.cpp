#include "treadmill.hpp"
#include "web_server.hpp"

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_request_handler.h"
#include "include/wrapper/cef_helpers.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <httplib.h>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

struct Options {
    std::string bind{"0.0.0.0"};
    int port{8080};
    std::filesystem::path data_dir{"data"};
    std::filesystem::path static_dir{"data/static"};
    double watchdog{5.0};
    bool server_only{false};
    bool windowed{false};
    std::string start_page{"/account-select.html"};
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
        else if (arg == "--server-only") options.server_only = true;
        else if (arg == "--windowed") options.windowed = true;
        else if (arg == "--start-page") options.start_page = next();
        else if (arg == "--help" || arg == "-h") {
            std::cout
                << "REMOTION desktop shell (CEF + cpp-httplib)\n\n"
                << "  --bind <ip>          bind address (default 0.0.0.0)\n"
                << "  --port <n>           HTTP port (default 8080)\n"
                << "  --data <dir>         writable data directory (default data)\n"
                << "  --static <dir>       static web directory (default data/static)\n"
                << "  --watchdog <sec>     stop belt when UI heartbeat disappears\n"
                << "  --no-watchdog        disable control heartbeat watchdog\n"
                << "  --server-only        run core and HTTP server without Chromium\n"
                << "  --windowed           open normal desktop window instead of fullscreen\n"
                << "  --start-page <path>  initial local page (default /account-select.html)\n";
            std::exit(0);
        } else if (arg.rfind("--", 0) == 0) {
            continue;
        } else {
            throw std::runtime_error("unknown option: " + arg);
        }
    }

    if (options.port <= 0 || options.port > 65535) throw std::runtime_error("invalid port");
    if (options.start_page.empty() || options.start_page.front() != '/') options.start_page.insert(0, "/");
    return options;
}

std::string local_url(const Options& options) {
    const std::string host = (options.bind == "0.0.0.0" || options.bind == "::") ? "127.0.0.1" : options.bind;
    return "http://" + host + ':' + std::to_string(options.port) + options.start_page;
}

bool wait_for_server(int port, std::chrono::seconds timeout = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        httplib::Client client("127.0.0.1", port);
        client.set_connection_timeout(0, 200000);
        client.set_read_timeout(0, 200000);
        if (const auto result = client.Get("/api/v1/state"); result && result->status >= 200 && result->status < 500) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

bool is_local_ui_url(const std::string& url, int port) {
    const std::string p = std::to_string(port);
    return url.rfind("http://127.0.0.1:" + p + "/", 0) == 0 ||
           url.rfind("http://localhost:" + p + "/", 0) == 0;
}

std::filesystem::path executable_dir() {
#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len == 0 || len >= buffer.size()) return std::filesystem::current_path();
    buffer.resize(len);
    return std::filesystem::path(buffer).parent_path();
#else
    return std::filesystem::current_path();
#endif
}

class ShellClient final : public CefClient,
                          public CefLifeSpanHandler,
                          public CefContextMenuHandler,
                          public CefRequestHandler {
public:
    ShellClient(int port, bool fullscreen) : port_(port), fullscreen_(fullscreen) {}

    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        CEF_REQUIRE_UI_THREAD();
        browser_ = browser;
#if defined(_WIN32)
        const HWND hwnd = browser->GetHost()->GetWindowHandle();
        if (hwnd && fullscreen_) {
            MONITORINFO monitor_info{};
            monitor_info.cbSize = sizeof(monitor_info);
            const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            if (GetMonitorInfo(monitor, &monitor_info)) {
                SetWindowLongPtr(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
                SetWindowPos(hwnd, HWND_TOP,
                             monitor_info.rcMonitor.left,
                             monitor_info.rcMonitor.top,
                             monitor_info.rcMonitor.right - monitor_info.rcMonitor.left,
                             monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top,
                             SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            }
        }
#endif
    }

    bool DoClose(CefRefPtr<CefBrowser> browser) override {
        (void)browser;
        return false;
    }

    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        CEF_REQUIRE_UI_THREAD();
        (void)browser;
        browser_ = nullptr;
        CefQuitMessageLoop();
    }

    void OnBeforeContextMenu(CefRefPtr<CefBrowser> browser,
                             CefRefPtr<CefFrame> frame,
                             CefRefPtr<CefContextMenuParams> params,
                             CefRefPtr<CefMenuModel> model) override {
        (void)browser;
        (void)frame;
        (void)params;
        model->Clear();
    }

    bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefRequest> request,
                        bool user_gesture,
                        bool is_redirect) override {
        (void)browser;
        (void)frame;
        (void)user_gesture;
        (void)is_redirect;
        const std::string url = request->GetURL().ToString();
        return !is_local_ui_url(url, port_);
    }

private:
    int port_;
    bool fullscreen_;
    CefRefPtr<CefBrowser> browser_;

    IMPLEMENT_REFCOUNTING(ShellClient);
};

class ShellApp final : public CefApp, public CefBrowserProcessHandler {
public:
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }

    void Configure(std::string start_url, int port, bool fullscreen) {
        start_url_ = std::move(start_url);
        port_ = port;
        fullscreen_ = fullscreen;
    }

    void OnContextInitialized() override {
        CEF_REQUIRE_UI_THREAD();

        CefWindowInfo window_info;
        window_info.SetAsPopup(nullptr, "REMOTION");
        window_info.runtime_style = CEF_RUNTIME_STYLE_ALLOY;

        CefBrowserSettings browser_settings;
        browser_settings.javascript = STATE_ENABLED;
        browser_settings.webgl = STATE_ENABLED;

        CefRefPtr<ShellClient> client = new ShellClient(port_, fullscreen_);
        if (!CefBrowserHost::CreateBrowser(window_info, client, start_url_, browser_settings, nullptr, nullptr)) {
            std::cerr << "CEF: failed to create browser window\n";
            CefQuitMessageLoop();
        }
    }

private:
    std::string start_url_{"http://127.0.0.1:8080/account-select.html"};
    int port_{8080};
    bool fullscreen_{true};

    IMPLEMENT_REFCOUNTING(ShellApp);
};

int run_browser_process(int argc, char** argv, CefRefPtr<ShellApp> app) {
#if defined(_WIN32)
    CefMainArgs main_args(GetModuleHandle(nullptr));
#else
    CefMainArgs main_args(argc, argv);
#endif
    return CefExecuteProcess(main_args, app, nullptr);
}

bool initialize_cef(int argc, char** argv, CefRefPtr<ShellApp> app) {
#if defined(_WIN32)
    CefMainArgs main_args(GetModuleHandle(nullptr));
#else
    CefMainArgs main_args(argc, argv);
#endif
    CefSettings settings;
    settings.no_sandbox = true;
    settings.log_severity = LOGSEVERITY_WARNING;
    settings.persist_session_cookies = false;

#if defined(_WIN32)
    const auto runtime_dir = executable_dir();
    const auto locales_dir = runtime_dir / "locales";

    const auto icu_file = runtime_dir / "icudtl.dat";
    const auto resources_file = runtime_dir / "resources.pak";
    if (!std::filesystem::exists(icu_file) || !std::filesystem::exists(resources_file) ||
        !std::filesystem::is_directory(locales_dir)) {
        std::cerr << "CEF runtime is incomplete beside remotion.exe\n"
                  << "Expected: " << icu_file.string() << "\n"
                  << "          " << resources_file.string() << "\n"
                  << "          " << locales_dir.string() << "\n";
        return false;
    }

    CefString(&settings.resources_dir_path) = runtime_dir.wstring();
    CefString(&settings.locales_dir_path) = locales_dir.wstring();
    std::cout << "CEF resources: " << runtime_dir.string() << "\n";
#endif

    return CefInitialize(main_args, settings, app, nullptr);
}

} // namespace

int main(int argc, char** argv) {
    CefRefPtr<ShellApp> cef_app = new ShellApp();
    const int subprocess_code = run_browser_process(argc, argv, cef_app);
    if (subprocess_code >= 0) return subprocess_code;

    try {
        const auto options = parse_args(argc, argv);

        yadro::Limits limits;
        limits.watchdog_seconds = options.watchdog;
        auto driver = std::make_unique<yadro::SimulationDriver>();
        yadro::TreadmillController controller(std::move(driver), limits);
        yadro::WebServer server(controller, options.data_dir, options.static_dir);

        std::cout << "REMOTION / Yadro CEF shell\n"
                  << "Driver: SimulationDriver\n"
                  << "Web UI: " << local_url(options) << "\n"
                  << "Listening on " << options.bind << ':' << options.port << "\n";

        if (options.server_only) {
            return server.listen(options.bind, options.port) ? 0 : 2;
        }

        bool listen_ok = false;
        std::thread server_thread([&] { listen_ok = server.listen(options.bind, options.port); });

        if (!wait_for_server(options.port)) {
            server.stop();
            if (server_thread.joinable()) server_thread.join();
            std::cerr << "HTTP server did not become ready within 10 seconds\n";
            return 2;
        }

        cef_app->Configure(local_url(options), options.port, !options.windowed);
        if (!initialize_cef(argc, argv, cef_app)) {
            server.stop();
            if (server_thread.joinable()) server_thread.join();
            std::cerr << "CEF initialization failed\n";
            return 3;
        }

        CefRunMessageLoop();
        CefShutdown();

        server.stop();
        if (server_thread.joinable()) server_thread.join();
        return listen_ok ? 0 : 2;
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << '\n';
        return 1;
    }
}

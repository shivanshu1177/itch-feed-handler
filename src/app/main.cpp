#include "feed_handler.hpp"
#include "../../include/common/logging.hpp"
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>

namespace {

std::atomic<itch::FeedHandler*> g_handler{nullptr};

void signal_handler(int sig) {
    ITCH_LOG_INFO("Caught signal %d, shutting down...", sig);
    itch::FeedHandler* h = g_handler.load(std::memory_order_relaxed);
    if (h)
        h->stop();
}

} // anonymous namespace

int main(int argc, char** argv) {
    const char* config = (argc > 1) ? argv[1] : "config/feed_handler.toml";

    ITCH_LOG_INFO("ITCH Feed Handler starting (config: %s)", config);

    // Install signal handlers for graceful shutdown
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    try {
        itch::FeedHandler handler(config);
        g_handler.store(&handler, std::memory_order_relaxed);

        if (!handler.init()) {
            ITCH_LOG_ERROR("Initialisation failed");
            return EXIT_FAILURE;
        }

        handler.run(); // blocks until signal

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    ITCH_LOG_INFO("Feed Handler shut down cleanly");
    return EXIT_SUCCESS;
}

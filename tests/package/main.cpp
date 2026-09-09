#ifdef _WIN32
#include <windows.h>
#endif
#include <chlog/chlog.hpp>

class count_sink : public chlog::sink {
public:
    void log(const chlog::log_event&) override { ++count; }
    std::atomic<int> count{0};
};

int main() {
    chlog::logger_config cfg;
    cfg.async.enabled = true;
    auto output = std::make_shared<count_sink>();
    chlog::logger logger(cfg);
    logger.add_sink(output);
    CHLOG_INFO(logger, "installed {}", 42);
    logger.flush();
    return output->count == 1 ? 0 : 1;
}

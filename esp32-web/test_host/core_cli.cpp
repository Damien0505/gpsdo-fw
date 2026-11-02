// The real core logic behind a line protocol on stdin/stdout, for preview_server.py.
// Time is given by the caller, payloads are in hex.
#include <cstdio>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "gpsdo.h"
#include "reports.h"

static std::string hex(const char* data, size_t len)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        out += digits[(unsigned char)data[i] >> 4];
        out += digits[(unsigned char)data[i] & 15];
    }
    return out;
}

static void reply(const std::string& text)
{
    std::cout << "= " << hex(text.data(), text.size()) << "\n" << std::flush;
}

static Gpsdo gpsdo([](const char* data, size_t len) { std::cout << "tx " << hex(data, len) << "\n"; });

int main()
{
    std::map<int, CommandResultPtr> results;
    int next_id = 1;
    uint32_t now = 0;
    HostInfo host;
    host.free_heap = 180000;
    host.rssi = -58;
    host.ip = "127.0.0.1";
    host.hostname = "gpsdo";

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string command;
        in >> command;
        host.uptime_s = now / 1000;
        if (command == "t") {
            in >> now;
            gpsdo.tick(now);
            reply("");
        } else if (command == "b") {
            std::string text;
            in >> text;
            std::vector<uint8_t> bytes;
            for (size_t i = 0; i + 1 < text.size(); i += 2) {
                bytes.push_back((uint8_t)std::stoi(text.substr(i, 2), nullptr, 16));
            }
            gpsdo.onBytes(bytes.data(), bytes.size(), now);
            reply("");
        } else if (command == "status") {
            reply(statusJson(gpsdo, host, now));
        } else if (command == "config") {
            reply(configJson(gpsdo));
        } else if (command == "metrics") {
            reply(metricsText(gpsdo, host, now));
        } else if (command == "console") {
            reply(gpsdo.console().text());
        } else if (command == "changed") {
            reply(gpsdo.takeConfigChanged() ? "1" : "0");
        } else if (command == "history") {
            std::string range;
            in >> range;
            auto reader = gpsdo.history().startReader(range == "24h" ? History::RANGE_24H : History::RANGE_1H);
            std::string out;
            char buffer[1024];
            for (size_t n; (n = gpsdo.history().read(reader, buffer, sizeof(buffer))) != 0;) {
                out.append(buffer, n);
            }
            reply(out);
        } else if (command == "set") {
            std::string key;
            long long value;
            in >> key >> value;
            results[next_id] = gpsdo.set(key.c_str(), value, now);
            reply(std::to_string(next_id++));
        } else if (command == "cmd") {
            std::string name;
            in >> name;
            results[next_id] = gpsdo.command(name.c_str(), now);
            reply(std::to_string(next_id++));
        } else if (command == "result") {
            int id;
            in >> id;
            const CommandResultPtr& result = results[id];
            reply(result->state == CommandResult::PENDING ? "pending"
                  : result->state == CommandResult::OK    ? "ok"
                                                          : "failed " + result->error);
        } else {
            reply("");
        }
    }
    return 0;
}

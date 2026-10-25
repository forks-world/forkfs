#include "level_store.h"
#include "journal.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <unistd.h>

using namespace forkfs;
#define CHECK(x) do { if(!(x)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #x); } while(0)

static uint64_t data_bytes(const std::string& path) {
    uint64_t bytes = 0;
    for(const auto& entry : std::filesystem::directory_iterator(path)) {
        const auto name = entry.path().filename().string();
        if(name.starts_with("DATA-") && entry.path().extension() == ".seg") bytes += entry.file_size();
    }
    return bytes;
}

static size_t segment_count(const std::string& path) {
    size_t count = 0;
    for(const auto& entry : std::filesystem::directory_iterator(path)) {
        const auto name = entry.path().filename().string();
        if(name.starts_with("DATA-") && entry.path().extension() == ".seg") ++count;
    }
    return count;
}

static uint64_t json_number(const std::string& json, const std::string& key) {
    const auto marker = "\"" + key + "\":";
    const auto at = json.find(marker);
    CHECK(at != std::string::npos);
    auto pos = at + marker.size();
    while(pos < json.size() && json[pos] == ' ') ++pos;
    CHECK(pos < json.size() && json[pos] >= '0' && json[pos] <= '9');
    uint64_t value = 0;
    while(pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        value = value * 10 + static_cast<unsigned>(json[pos++] - '0');
    }
    return value;
}

static std::string object_value(size_t index, const std::string& common) {
    std::string value = common;
    std::mt19937 rng(static_cast<uint32_t>(0x5eed1234u + index));
    for(size_t chunk = 0; chunk < 3; ++chunk) {
        for(size_t n = 0; n < 65536; ++n) value.push_back(static_cast<char>(rng()));
    }
    return value;
}

static std::vector<unsigned char> bytes(const std::string& value) {
    return {value.begin(), value.end()};
}

int main() {
    auto pattern = (std::filesystem::temp_directory_path() / "forkfs-gc-plan-XXXXXX").string();
    CHECK(mkdtemp(pattern.data()));
    try {
        const auto path = pattern + "/store";
        LevelStore::create(path);
        CHECK(data_bytes(path) == 0);
        const auto empty_plan = LevelStore::collect(path, "", true);
        CHECK(json_number(empty_plan, "data_bytes_estimated_after") == 0);
        {
            LevelStore store(path);
            store.transact(0, {{"empty", std::vector<unsigned char>{}},
                               {"tiny", std::vector<unsigned char>{0x00, 0xff, 0x41}}});
            CHECK(store.object(store.root("empty")).empty());
            CHECK(store.object(store.root("tiny")) == std::vector<unsigned char>({0x00, 0xff, 0x41}));
        }

        std::string common(65536, '\0');
        for(size_t n = 0; n < common.size(); ++n) common[n] = static_cast<char>((n * 37 + 11) & 0xff);
        constexpr size_t object_count = 360;
        constexpr size_t batch_size = 16; // 4 MiB payload batches, below the 16 MiB bound.
        {
            LevelStore store(path, 0);
            for(size_t first = 0; first < object_count; first += batch_size) {
                std::vector<Mutation> batch;
                const auto end = std::min(object_count, first + batch_size);
                for(size_t i = first; i < end; ++i) {
                    const auto value = object_value(i, common);
                    batch.push_back({"object/" + std::to_string(i), bytes(value)});
                }
                store.transact(store.sequence(), batch);
            }
            // Variable sized values, including empty and tiny roots, share the
            // same object index and exercise ordinary root replacement too.
            for(const auto& [key, size] : std::vector<std::pair<std::string, size_t>>{
                    {"variable/empty", 0}, {"variable/tiny", 1}, {"variable/257", 257},
                    {"variable/65537", 65537}, {"variable/131073", 131073}}) {
                std::string value(size, '\0');
                for(size_t n = 0; n < size; ++n) value[n] = static_cast<char>((n * 19 + size) & 0xff);
                store.transact(store.sequence(), {{key, bytes(value)}});
            }
            CHECK(store.roots() == object_count + 7);
            for(size_t i = 0; i < object_count; ++i) {
                const auto value = object_value(i, common);
                CHECK(store.object(store.root("object/" + std::to_string(i))) == bytes(value));
            }
        }
        const auto before = data_bytes(path);
        CHECK(before > 64ull * 1024 * 1024);
        CHECK(segment_count(path) > 1);

        {
            LevelStore store(path, 0);
            constexpr size_t deleted = 10;
            for(size_t first = 0; first < deleted; first += batch_size) {
                std::vector<Mutation> batch;
                const auto end = std::min(deleted, first + batch_size);
                for(size_t i = first; i < end; ++i) {
                    batch.push_back({"object/" + std::to_string(i), std::nullopt});
                }
                store.transact(store.sequence(), batch);
            }
            CHECK(store.roots() == object_count - deleted + 7);
        }

        const auto plan = LevelStore::collect(path, "", true);
        const auto estimated = json_number(plan, "data_bytes_estimated_after");
        CHECK(estimated > 64ull * 1024 * 1024);
        CHECK(estimated < before);
        const auto report = LevelStore::collect(path);
        CHECK(json_number(report, "data_bytes_after") == estimated);
        const auto after = data_bytes(path);
        CHECK(after == estimated);
        CHECK(segment_count(path) > 1);

        {
            LevelStore store(path, 0);
            for(size_t i = 0; i < object_count; ++i) {
                const auto key = "object/" + std::to_string(i);
                if(i < 10) {
                    CHECK(!store.contains(key));
                } else {
                    const auto value = object_value(i, common);
                    CHECK(store.object(store.root(key)) == bytes(value));
                }
            }
            CHECK(store.object(store.root("empty")).empty());
            CHECK(store.object(store.root("tiny")) == std::vector<unsigned char>({0x00, 0xff, 0x41}));
            for(const auto& [key, size] : std::vector<std::pair<std::string, size_t>>{
                    {"variable/empty", 0}, {"variable/tiny", 1}, {"variable/257", 257},
                    {"variable/65537", 65537}, {"variable/131073", 131073}}) {
                std::string value(size, '\0');
                for(size_t n = 0; n < size; ++n) value[n] = static_cast<char>((n * 19 + size) & 0xff);
                CHECK(store.object(store.root(key)) == bytes(value));
            }
            store.verify();
        }

        const auto repeated = LevelStore::collect(path);
        CHECK(data_bytes(path) == after);
        CHECK(json_number(repeated, "data_bytes_after") == after);
        std::filesystem::remove_all(pattern);
        std::cout << "GC plan rollover estimate, dedup, roots and idempotence passed\n";
    } catch(const std::exception& e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(pattern);
        return 1;
    }
}

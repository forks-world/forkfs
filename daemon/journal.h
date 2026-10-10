#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace forkfs {
// A temporary named-object transaction API. Names are retention roots, not paths.
struct Checkpoint {
    uint64_t offset = 0, length = 0, sequence = 0, replay_start = 65536;
    std::array<unsigned char,32> hash{};
};
struct Mutation {
    std::string key;
    // nullopt deletes a root; an empty vector stores an empty value.
    std::optional<std::vector<unsigned char>> value;
    // Reference an already committed object instead of copying its bytes.
    std::optional<std::array<unsigned char,32>> reference = std::nullopt;
};
class LevelStore;
class Journal {
public:
    ~Journal();
    // Preview forbids logical mutation; native LevelDB open still needs writable backing.
    explicit Journal(const std::string& directory,bool read_only=false);
    bool is_leveldb() const {return bool(leveldb_);}
    std::string repository() const;
    void compact();
    std::string metrics_json(bool detailed=false) const;
    Journal(int fd, const std::array<unsigned char,16>& repository, const Checkpoint& checkpoint = {});
    void put(const std::string& key, const std::vector<unsigned char>& bytes);
    void transact(uint64_t expected_sequence, const std::vector<Mutation>& mutations);
    std::vector<unsigned char> get(const std::string& key) const;
    bool contains(const std::string& key) const;
    std::array<unsigned char,32> root_object(const std::string& key) const;
    std::vector<unsigned char> get_object(const std::array<unsigned char,32>& id) const;
    static std::array<unsigned char,32> object_id(const std::vector<unsigned char>& value);
    std::vector<std::string> keys(const std::string& prefix) const;
    Checkpoint checkpoint();
    void verify() const;
    void poison();
    uint64_t replayed() const { return replayed_; }
    size_t objects() const;
    uint64_t sequence() const;
    size_t count() const;
    bool pending_tail() const { return tail_; }
private:
    struct Location { uint64_t payload; uint32_t length; };
    std::unique_ptr<LevelStore> leveldb_;
    bool read_only_=false;
    int fd_=-1;
    std::array<unsigned char,16> repository_;
    uint64_t seq_ = 0, end_ = 65536;
    std::array<unsigned char,32> last_hash_{};
    std::map<std::string, std::array<unsigned char,32>> roots_;
    std::map<std::array<unsigned char,32>, Location> objects_;
    uint64_t replayed_ = 0;
    void restore_checkpoint(const Checkpoint& checkpoint, uint64_t file_size);
    std::vector<unsigned char> read_object(const std::array<unsigned char,32>& id) const;
    bool tail_ = false;
    bool poisoned_ = false;
};
}

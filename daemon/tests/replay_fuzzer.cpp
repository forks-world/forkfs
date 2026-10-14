#include "replay_validation.h"
#include "metadata_tree.h"
#include <cstdlib>
#include <fstream>
#include <iterator>

// Direct parser inputs bypass transport checksums. Graph inputs contain a root
// object followed by its object table; IDs are recomputed after each mutation.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    using namespace forkfs;
    if (!size || size > 256 * 1024) return 0;
    try {
        const unsigned mode = data[0];
        std::string payload(reinterpret_cast<const char*>(data + 1), size - 1);
        if (mode == 0) {
            auto tx = replay::transaction(payload);
            if (replay::encode(tx.ops) != payload.substr(41)) std::abort();
        } else if (mode == 1) {
            auto op = replay::base_row(payload);
            auto canonical = "B" + replay::le(op.key.size(),4) + replay::le(op.value.size(),4) + op.key + op.value;
            if (canonical != payload) std::abort();
        } else if (mode <= 4) {
            replay::Input input{payload};
            auto sequence = input.integer(8), count = input.integer(2);
            replay::require(count > 0 && count <= 2048, "invalid fuzz object count");
            std::map<MetadataTree::Id, MetadataTree::Bytes> objects;
            MetadataTree::Id root{};
            for (uint64_t i = 0; i < count; ++i) {
                auto bytes = input.take(input.integer(4));
                MetadataTree::Bytes value(bytes.begin(), bytes.end());
                auto id = Journal::object_id(value);
                if (!i) root = id;
                objects.emplace(id, std::move(value));
            }
            input.end();
            auto reader = [&](const MetadataTree::Id& id) {
                auto it = objects.find(id);
                replay::require(it != objects.end(), "missing fuzz object");
                return it->second;
            };
            if (mode == 2) NamespaceView::verify_manifest(reader, root);
            if (mode == 3) replay::Graph(reader, sequence).root(
                "R/" + std::string("\0forkfs-history/",16) + "revision/fuzz", replay::raw(root));
            if (mode == 4) {
                MetadataTree tree(reader, root);
                for (const auto& [key, id] : tree.entries()) {
                    if (tree.lookup(key) != id) std::abort();
                    reader(id);
                }
            }
        }
    } catch (const std::runtime_error&) {
        // Invalid external input is expected. Logic errors and sanitizer faults
        // intentionally escape and become reproducible fuzz failures.
    }
    return 0;
}
#ifdef FORKFS_FUZZ_STANDALONE
int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::ifstream input(argv[i], std::ios::binary);
        if (!input) return 2;
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
        LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
    }
}
#endif

#pragma once
#include "level_store.h"
namespace forkfs {
// Local append-only catalog. Published entries and packages are immutable.
class BackupCatalog {
public:
    struct Entry {
        std::string archive,digest;
        char kind;
        uint64_t from,size;
        LevelStore::RecoveryPoint checkpoint;
    };
    static void add(const std::string& directory,const std::string& archive,
                    std::optional<LevelStore::RecoveryPoint> expected=std::nullopt,std::optional<uint64_t> after=std::nullopt);
    struct RevisionPoint {std::string revision_id;LevelStore::RecoveryPoint checkpoint;};
    static RevisionPoint revision_point(const std::string& directory,const std::string& name);
    static std::vector<Entry> entries(const std::string& directory);
};
}

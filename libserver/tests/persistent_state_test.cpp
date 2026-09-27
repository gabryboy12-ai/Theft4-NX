#include "persistent_state.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

using libserver::PersistentState;
using json = nlohmann::json;

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
#if defined(_WIN32)
        const std::filesystem::path root = std::filesystem::temp_directory_path();
        wchar_t path[MAX_PATH]{};
        if (GetTempFileNameW(root.c_str(), L"lst", 0, path) == 0) {
            throw std::runtime_error("GetTempFileNameW failed");
        }
        std::filesystem::remove(path);
        std::filesystem::create_directory(path);
        path_ = path;
#else
        std::string pattern =
            (std::filesystem::temp_directory_path() / "libserver-state-test.XXXXXX").string();
        if (::mkdtemp(pattern.data()) == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        path_ = pattern;
#endif
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

bool failed = false;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        failed = true;
    }
}

void WriteJson(const std::filesystem::path& path, const json& value) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << value.dump() << '\n';
    if (!stream) {
        throw std::runtime_error("cannot write test JSON");
    }
}

json ReadJson(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return json::parse(stream);
}

PersistentState::Options OptionsFor(const std::filesystem::path& directory) {
    PersistentState::Options options;
    options.data_directory = directory;
    options.state_filename = "state.json";
    options.schema_version = 1;
    return options;
}

void TestMissingLoadAndRoundTrip() {
    TemporaryDirectory root;
    const std::filesystem::path data = root.path() / "nested" / "data";
    {
        PersistentState state(OptionsFor(data));
        Check(state.Open(), "missing snapshot opens");
        Check(state.payload() == json::object(), "missing snapshot loads as empty object");
        Check(state.HealthJson() == json({{"status", "ready"}, {"error", "none"}}),
              "ready health is deterministic");
        Check(state.Save(json::array({"generic", 7, true})), "generic JSON payload saves");
    }
    {
        PersistentState state(OptionsFor(data));
        Check(state.Open(), "saved snapshot reopens");
        Check(state.payload() == json::array({"generic", 7, true}),
              "saved arbitrary payload round-trips");
    }
    const json envelope = ReadJson(data / "state.json");
    Check(envelope["format"] == "libserver-state", "snapshot has format marker");
    Check(envelope["schema_version"] == 1, "snapshot has schema version");

#if !defined(_WIN32)
    struct stat status {};
    Check(::stat(data.c_str(), &status) == 0, "data directory can be inspected");
    Check((status.st_mode & ACCESSPERMS) == S_IRWXU, "data directory is owner-only");
    Check(::stat((data / "state.json").c_str(), &status) == 0,
          "state file can be inspected");
    Check((status.st_mode & ACCESSPERMS) == (S_IRUSR | S_IWUSR),
          "state file is owner read/write only");
    Check(::stat((data / ".libserver-state.lock").c_str(), &status) == 0,
          "lock file can be inspected");
    Check((status.st_mode & ACCESSPERMS) == (S_IRUSR | S_IWUSR),
          "lock file is owner read/write only");
#endif
}

void TestExclusiveLifetimeLock() {
    TemporaryDirectory root;
    PersistentState first(OptionsFor(root.path()));
    PersistentState second(OptionsFor(root.path()));
    Check(first.Open(), "first process store acquires lock");
    Check(!second.Open(), "second process store cannot acquire lock");
    Check(second.error_code() == PersistentState::ErrorCode::kLockAlreadyHeld,
          "lock contention has stable error code");
    Check(second.HealthJson() ==
              json({{"status", "error"}, {"error", "lock_already_held"}}),
          "lock contention health is deterministic");
}

void TestEnvelopeValidation() {
    {
        TemporaryDirectory root;
        std::ofstream stream(root.path() / "state.json", std::ios::binary);
        stream << "{broken";
        stream.close();
        PersistentState state(OptionsFor(root.path()));
        Check(!state.Open(), "malformed JSON is rejected");
        Check(state.error_code() == PersistentState::ErrorCode::kStateParseFailed,
              "malformed JSON has parse error");
    }
    {
        TemporaryDirectory root;
        WriteJson(root.path() / "state.json",
                  {{"format", "wrong"}, {"schema_version", 1}, {"payload", {}}});
        PersistentState state(OptionsFor(root.path()));
        Check(!state.Open(), "wrong envelope format is rejected");
        Check(state.error_code() == PersistentState::ErrorCode::kEnvelopeInvalid,
              "wrong format has envelope error");
    }
    {
        TemporaryDirectory root;
        WriteJson(root.path() / "state.json",
                  {{"format", "libserver-state"},
                   {"schema_version", 2},
                   {"payload", json::object()}});
        PersistentState state(OptionsFor(root.path()));
        Check(!state.Open(), "wrong schema version is rejected");
        Check(state.error_code() == PersistentState::ErrorCode::kSchemaMismatch,
              "wrong schema has version error");
    }
    {
        TemporaryDirectory root;
        WriteJson(root.path() / "state.json",
                  {{"format", "libserver-state"},
                   {"schema_version", 1},
                   {"payload", json::object()},
                   {"unexpected", true}});
        PersistentState state(OptionsFor(root.path()));
        Check(!state.Open(), "unknown envelope field is rejected");
        Check(state.error_code() == PersistentState::ErrorCode::kEnvelopeInvalid,
              "unknown field has envelope error");
    }
}

void TestSymlinkTargetRejected() {
#if !defined(_WIN32)
    {
        TemporaryDirectory root;
        WriteJson(root.path() / "other.json",
                  {{"format", "libserver-state"},
                   {"schema_version", 1},
                   {"payload", json::object()}});
        std::filesystem::create_symlink(root.path() / "other.json", root.path() / "state.json");
        PersistentState state(OptionsFor(root.path()));
        Check(!state.Open(), "symlink state target is rejected during load");
        Check(state.error_code() == PersistentState::ErrorCode::kStateTargetIsSymlink,
              "load symlink target has stable error code");
    }
    {
        TemporaryDirectory root;
        PersistentState state(OptionsFor(root.path()));
        Check(state.Open(), "symlink save test opens without a snapshot");
        WriteJson(root.path() / "other.json", json::object());
        std::filesystem::create_symlink(root.path() / "other.json", root.path() / "state.json");
        Check(!state.Save({{"must", "not-follow"}}),
              "symlink state target is rejected during save");
        Check(state.error_code() == PersistentState::ErrorCode::kStateTargetIsSymlink,
              "save symlink target has stable error code");
        Check(ReadJson(root.path() / "other.json") == json::object(),
              "save rejection does not modify symlink destination");
    }
#endif
}

void TestPreRenameFaultPreservesSnapshotAndCanRetry() {
    TemporaryDirectory root;
    std::optional<PersistentState::FaultPoint> selected_fault;
    PersistentState::Options options = OptionsFor(root.path());
    options.fault_injector = [&](PersistentState::FaultPoint point) {
        return selected_fault.has_value() && selected_fault.value() == point;
    };
    PersistentState state(std::move(options));
    Check(state.Open(), "fault test store opens");
    Check(state.Save({{"generation", "old"}}), "baseline snapshot saves");

    selected_fault = PersistentState::FaultPoint::kBeforeRename;
    Check(!state.Save({{"generation", "new"}}), "pre-rename fault fails save");
    Check(state.error_code() == PersistentState::ErrorCode::kFaultInjected,
          "injected fault has stable error code");
    Check(ReadJson(root.path() / "state.json")["payload"]["generation"] == "old",
          "pre-rename fault preserves previous snapshot");
    bool found_temporary = false;
    for (const auto& entry : std::filesystem::directory_iterator(root.path())) {
        if (entry.path().filename().string().starts_with(".state.json.tmp.")) {
            found_temporary = true;
        }
    }
    Check(!found_temporary, "failed save removes temporary snapshot");

    selected_fault.reset();
    Check(state.Save({{"generation", "retried"}}), "save can retry after a failure");
    Check(state.health() == PersistentState::Health::kReady,
          "successful retry restores ready health");
}

void TestPostRenameFaultReportsCommittedPayload() {
    TemporaryDirectory root;
    std::optional<PersistentState::FaultPoint> selected_fault;
    PersistentState::Options options = OptionsFor(root.path());
    options.fault_injector = [&](PersistentState::FaultPoint point) {
        return selected_fault.has_value() && selected_fault.value() == point;
    };
    PersistentState state(std::move(options));
    Check(state.Open(), "post-rename fault store opens");
    Check(state.Save({{"generation", "old"}}), "post-rename baseline saves");
    selected_fault = PersistentState::FaultPoint::kBeforeDirectorySync;
    Check(!state.Save({{"generation", "committed"}}), "directory-sync fault reports failure");
    Check(state.payload()["generation"] == "committed",
          "in-memory payload tracks completed rename");
    Check(ReadJson(root.path() / "state.json")["payload"]["generation"] == "committed",
          "post-rename fault leaves committed file visible");
    selected_fault.reset();
    Check(!state.Save({{"generation", "must-not-overwrite"}}),
          "ambiguous durability state refuses another save");
    Check(state.error_code() == PersistentState::ErrorCode::kDurabilityAmbiguous,
          "refused save has durability-ambiguous error");
    Check(ReadJson(root.path() / "state.json")["payload"]["generation"] == "committed",
          "refused save does not overwrite committed snapshot");

    Check(state.Open(), "explicit reopen reconciles ambiguous state");
    Check(state.payload()["generation"] == "committed",
          "reopen loads the visible committed snapshot");
    Check(state.Save({{"generation", "reconciled"}}),
          "save succeeds after explicit reconciliation");
}

void TestSnapshotByteLimit() {
    constexpr std::size_t kTestSnapshotLimit = 128;
    constexpr std::size_t kTestOversizedPayload = 256;

    {
        TemporaryDirectory root;
        PersistentState::Options options = OptionsFor(root.path());
        options.maximum_snapshot_bytes = kTestSnapshotLimit;
        PersistentState state(std::move(options));
        Check(state.Open(), "bounded save store opens");
        Check(!state.Save(std::string(kTestOversizedPayload, 'x')),
              "oversized save is rejected");
        Check(state.error_code() == PersistentState::ErrorCode::kSnapshotTooLarge,
              "oversized save has stable error code");
        Check(!std::filesystem::exists(root.path() / "state.json"),
              "oversized save creates no state target");
    }
    {
        TemporaryDirectory root;
        WriteJson(root.path() / "state.json",
                  {{"format", "libserver-state"},
                   {"schema_version", 1},
                   {"payload", std::string(kTestOversizedPayload, 'x')}});
        PersistentState::Options options = OptionsFor(root.path());
        options.maximum_snapshot_bytes = kTestSnapshotLimit;
        PersistentState state(std::move(options));
        Check(!state.Open(), "oversized existing snapshot is rejected");
        Check(state.error_code() == PersistentState::ErrorCode::kSnapshotTooLarge,
              "oversized load has stable error code");
    }
}

void TestInvalidOptions() {
    TemporaryDirectory root;
    PersistentState::Options options = OptionsFor(root.path());
    options.state_filename = "nested/state.json";
    PersistentState state(std::move(options));
    Check(!state.Open(), "nested state filename is rejected");
    Check(state.error_code() == PersistentState::ErrorCode::kInvalidOptions,
          "invalid filename has stable error code");

    PersistentState::Options schema_options = OptionsFor(root.path());
    schema_options.schema_version = 0;
    PersistentState schema_state(std::move(schema_options));
    Check(!schema_state.Open(), "zero schema version is rejected");
    Check(schema_state.error_code() == PersistentState::ErrorCode::kInvalidOptions,
          "zero schema version has stable error code");
}

} // namespace

int main() {
    TestMissingLoadAndRoundTrip();
    TestExclusiveLifetimeLock();
    TestEnvelopeValidation();
    TestSymlinkTargetRejected();
    TestPreRenameFaultPreservesSnapshotAndCanRetry();
    TestPostRenameFaultReportsCommittedPayload();
    TestSnapshotByteLimit();
    TestInvalidOptions();

    if (failed) {
        std::cerr << "persistent state tests failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "persistent state tests passed\n";
    return EXIT_SUCCESS;
}

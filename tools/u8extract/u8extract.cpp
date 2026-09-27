#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>
#include <cstring>
#include <zlib.h>

class U8Archive {
private:
    static constexpr uint32_t SIGNATURE = 0x55AA382D;
    static constexpr uint32_t TYPE_MASK = 0xFF000000;
    static constexpr uint32_t NAME_OFFSET_MASK = 0x00FFFFFF;

    enum class EntryType : uint8_t {
        File = 0,
        Directory = 1
    };

    struct Entry {
        uint32_t flags;
        uint32_t offset;
        uint32_t length;
        uint32_t uncompressed_size;
        std::string name;

        EntryType getType() const {
            return static_cast<EntryType>((flags & TYPE_MASK) >> 24);
        }

        uint32_t getNameOffset() const {
            return flags & NAME_OFFSET_MASK;
        }

        bool isCompressed() const {
            return length != 0 && uncompressed_size != 0;
        }
    };

    std::ifstream file;
    std::vector<Entry> entries;
    uint32_t data_offset;

    uint32_t readU32() {
        uint32_t value;
        file.read(reinterpret_cast<char*>(&value), 4);
        return __builtin_bswap32(value); // Big-endian to host
    }

    std::string readNullTerminatedString() {
        std::string result;
        char ch;
        while (file.get(ch) && ch != '\0') {
            result += ch;
        }
        return result;
    }

    void decompressZlib(const std::vector<uint8_t>& compressed, std::vector<uint8_t>& decompressed) {
        z_stream stream = {};
        stream.next_in = const_cast<uint8_t*>(compressed.data());
        stream.avail_in = compressed.size();
        stream.next_out = decompressed.data();
        stream.avail_out = decompressed.size();

        if (inflateInit(&stream) != Z_OK) {
            throw std::runtime_error("Failed to initialize zlib");
        }

        int ret = inflate(&stream, Z_FINISH);
        inflateEnd(&stream);

        if (ret != Z_STREAM_END) {
            throw std::runtime_error("Failed to decompress data");
        }
    }

    void parseEntries(size_t entry_index, const std::string& base_path) {
        const Entry& entry = entries[entry_index];
        std::string full_path = base_path.empty() ? entry.name : base_path + "/" + entry.name;

        if (entry.getType() == EntryType::Directory) {
            // Create directory
            if (!entry.name.empty()) {
                std::filesystem::create_directories(full_path);
            }

            // Process children
            size_t child_index = entry_index + 1;
            while (child_index < entry.length) {
                child_index = parseEntriesRecursive(child_index, full_path);
            }

            return;
        } else {
            // Extract file
            extractFile(entry, full_path);
        }
    }

    size_t parseEntriesRecursive(size_t entry_index, const std::string& base_path) {
        const Entry& entry = entries[entry_index];
        std::string full_path = base_path.empty() ? entry.name : base_path + "/" + entry.name;

        if (entry.getType() == EntryType::Directory) {
            // Create directory
            std::filesystem::create_directories(full_path);

            // Process children
            size_t child_index = entry_index + 1;
            while (child_index < entry.length) {
                child_index = parseEntriesRecursive(child_index, full_path);
            }

            return entry.length;
        } else {
            // Extract file
            extractFile(entry, full_path);
            return entry_index + 1;
        }
    }

    void extractFile(const Entry& entry, const std::string& path) {
        // Seek to file data
        file.seekg(entry.offset);

        // Read file data
        std::vector<uint8_t> data(entry.length);
        file.read(reinterpret_cast<char*>(data.data()), entry.length);

        // Decompress if necessary
        if (entry.isCompressed()) {
            std::vector<uint8_t> decompressed(entry.uncompressed_size);
            decompressZlib(data, decompressed);
            data = std::move(decompressed);
        }

        // Write to file
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()), data.size());
        out.close();
    }

public:
    void load(const std::string& filename) {
        file.open(filename, std::ios::binary);
        if (!file) {
            throw std::runtime_error("Failed to open file: " + filename);
        }

        // Read header
        uint32_t signature = readU32();
        if (signature != SIGNATURE) {
            throw std::runtime_error("Invalid U8 archive signature");
        }

        uint32_t entries_offset = readU32();
        uint32_t entries_length = readU32();
        data_offset = readU32();

        // Skip unknown values
        file.seekg(16, std::ios::cur);

        // Read root entry
        file.seekg(entries_offset);
        Entry root;
        root.flags = readU32();
        root.offset = readU32();
        root.length = readU32();
        root.uncompressed_size = readU32();
        entries.push_back(root);

        // Read remaining entries
        for (uint32_t i = 1; i < root.length; ++i) {
            Entry entry;
            entry.flags = readU32();
            entry.offset = readU32();
            entry.length = readU32();
            entry.uncompressed_size = readU32();
            entries.push_back(entry);
        }

        // Calculate string table offset
        uint32_t string_table_offset = entries_offset + (root.length * 16);

        // Read entry names
        for (auto& entry : entries) {
            file.seekg(string_table_offset + entry.getNameOffset());
            entry.name = readNullTerminatedString();
        }
    }

    void extract(const std::string& output_dir) {
        if (entries.empty()) {
            throw std::runtime_error("No archive loaded");
        }

        // Create output directory
        std::filesystem::create_directories(output_dir);

        // Start extraction from root
        parseEntries(0, output_dir);
    }

    ~U8Archive() {
        if (file.is_open()) {
            file.close();
        }
    }
};

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <archive.arc> <output_directory>" << std::endl;
        return 1;
    }

    try {
        U8Archive archive;
        archive.load(argv[1]);
        archive.extract(argv[2]);
        std::cout << "Extraction complete!" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}

#include "command_parser.h"
#include "debug_state.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iomanip>
#include <map>

namespace Debugger {

// =============================================================================
// CommandContext helpers
// =============================================================================

bool CommandContext::GetArgAsU32(size_t index, uint32_t& outValue) const {
    size_t actualIndex = index + 1;  // Skip command name
    if (actualIndex >= args.size()) {
        return false;
    }
    return ParseAddress(args[actualIndex], outValue);
}

bool CommandContext::GetArgAsString(size_t index, std::string& outValue) const {
    size_t actualIndex = index + 1;  // Skip command name
    if (actualIndex >= args.size()) {
        return false;
    }
    outValue = args[actualIndex];
    return true;
}

// =============================================================================
// CommandParser
// =============================================================================

CommandParser::CommandParser() {
    // Built-in help command
    Register({
        "help",
        "help [command|category]",
        "Show help for commands",
        "util",
        [this](CommandContext& ctx) -> bool {
            if (ctx.ArgCount() > 0) {
                std::string arg;
                ctx.GetArgAsString(0, arg);
                // Check if it's a command name
                auto* cmd = FindCommand(arg);
                if (cmd) {
                    ctx.out << cmd->name << " - " << cmd->help << "\n";
                    ctx.out << "Usage: " << cmd->usage << "\n";
                    return true;
                }
                // Otherwise treat as category
                PrintHelp(ctx.out, arg);
            } else {
                PrintHelp(ctx.out);
            }
            return true;
        }
    });
    
    // Quit command
    Register({
        "quit",
        "quit",
        "Exit debugger (but continue execution)",
        "util",
        [](CommandContext& ctx) -> bool {
            ctx.out << "Use Ctrl+C to stop execution\n";
            return true;
        }
    });
    
    // Clear command
    Register({
        "clear",
        "clear",
        "Clear screen",
        "util",
        [](CommandContext& ctx) -> bool {
            ctx.out << "\033[2J\033[H";  // ANSI escape to clear
            return true;
        }
    });
}

void CommandParser::Register(const CommandEntry& entry) {
    // Check for duplicate
    for (const auto& cmd : m_commands) {
        if (cmd.name == entry.name) {
            return;  // Already registered
        }
    }
    m_commands.push_back(entry);
}

bool CommandParser::Execute(const std::string& line, DebugState& state, void* memory, std::ostream& out) {
    auto tokens = Tokenize(line);
    if (tokens.empty()) {
        return true;  // Empty line is OK
    }
    
    auto* cmd = FindCommand(tokens[0]);
    if (!cmd) {
        out << "Unknown command: " << tokens[0] << "\n";
        out << "Type 'help' for available commands.\n";
        return false;
    }
    
    CommandContext ctx{tokens, out, state, memory};
    try {
        return cmd->func(ctx);
    } catch (const std::exception& e) {
        out << "Error executing '" << tokens[0] << "': " << e.what() << "\n";
        return false;
    }
}

void CommandParser::PrintHelp(std::ostream& out, const std::string& category) {
    // Group commands by category
    std::map<std::string, std::vector<const CommandEntry*>> byCategory;
    for (const auto& cmd : m_commands) {
        if (category.empty() || cmd.category == category) {
            byCategory[cmd.category].push_back(&cmd);
        }
    }
    
    if (byCategory.empty()) {
        out << "No commands found";
        if (!category.empty()) {
            out << " in category '" << category << "'";
        }
        out << "\n";
        return;
    }
    
    // Category display order
    std::vector<std::string> categoryOrder = {
        "memory", "thread", "sync", "exec", "ppc", "gpu", "file", "ctor", "util"
    };
    
    auto printCategory = [&](const std::string& cat) {
        auto it = byCategory.find(cat);
        if (it == byCategory.end()) return;
        
        out << "\n=== " << cat << " ===\n";
        for (const auto* cmd : it->second) {
            out << "  " << std::left << std::setw(16) << cmd->name 
                << " " << cmd->help << "\n";
        }
    };
    
    // Print in order
    for (const auto& cat : categoryOrder) {
        printCategory(cat);
    }
    
    // Print any remaining categories not in order
    for (const auto& [cat, cmds] : byCategory) {
        if (std::find(categoryOrder.begin(), categoryOrder.end(), cat) == categoryOrder.end()) {
            printCategory(cat);
        }
    }
    
    out << "\nType 'help <command>' for detailed usage.\n";
}

void CommandParser::PrintUsage(const std::string& cmdName, std::ostream& out) {
    auto* cmd = FindCommand(cmdName);
    if (cmd) {
        out << "Usage: " << cmd->usage << "\n";
    } else {
        out << "Unknown command: " << cmdName << "\n";
    }
}

std::vector<std::string> CommandParser::GetCompletions(const std::string& partial) {
    std::vector<std::string> result;
    std::string lower = partial;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    for (const auto& cmd : m_commands) {
        if (cmd.name.find(lower) == 0) {
            result.push_back(cmd.name);
        }
    }
    return result;
}

std::vector<std::string> CommandParser::GetCommandNames() const {
    std::vector<std::string> result;
    result.reserve(m_commands.size());
    for (const auto& cmd : m_commands) {
        result.push_back(cmd.name);
    }
    return result;
}

std::vector<std::string> CommandParser::Tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream iss(line);
    std::string token;
    
    while (iss >> token) {
        // Handle quoted strings
        if (!token.empty() && token[0] == '"') {
            std::string quoted = token.substr(1);
            if (quoted.back() == '"') {
                quoted.pop_back();
            } else {
                std::string more;
                while (std::getline(iss, more, '"')) {
                    quoted += " " + more;
                    break;
                }
            }
            tokens.push_back(quoted);
        } else {
            tokens.push_back(token);
        }
    }
    return tokens;
}

CommandEntry* CommandParser::FindCommand(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    for (auto& cmd : m_commands) {
        if (cmd.name == lower) {
            return &cmd;
        }
    }
    return nullptr;
}

// =============================================================================
// Utility Functions
// =============================================================================

bool ParseAddress(const std::string& str, uint32_t& outAddr) {
    if (str.empty()) return false;
    
    try {
        size_t pos = 0;
        // Check for 0x prefix
        if (str.size() > 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
            outAddr = static_cast<uint32_t>(std::stoul(str, &pos, 16));
        } else if (std::isxdigit(str[0]) && str.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
            // All hex digits, treat as hex
            outAddr = static_cast<uint32_t>(std::stoul(str, &pos, 16));
        } else {
            // Try decimal
            outAddr = static_cast<uint32_t>(std::stoul(str, &pos, 10));
        }
        return pos > 0;
    } catch (...) {
        return false;
    }
}

std::string FormatAddr(uint32_t addr) {
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%08X", addr);
    return buf;
}

std::string FormatHex(const uint8_t* data, size_t len, size_t bytesPerLine) {
    std::ostringstream oss;
    for (size_t i = 0; i < len; i += bytesPerLine) {
        // Hex bytes
        for (size_t j = 0; j < bytesPerLine && i + j < len; ++j) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02X ", data[i + j]);
            oss << buf;
        }
        // Padding if needed
        for (size_t j = len - i; j < bytesPerLine; ++j) {
            oss << "   ";
        }
        // ASCII
        oss << " ";
        for (size_t j = 0; j < bytesPerLine && i + j < len; ++j) {
            char c = static_cast<char>(data[i + j]);
            oss << (std::isprint(static_cast<unsigned char>(c)) ? c : '.');
        }
        oss << "\n";
    }
    return oss.str();
}

std::string FormatAscii(const uint8_t* data, size_t len) {
    std::string result;
    result.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        char c = static_cast<char>(data[i]);
        result += std::isprint(static_cast<unsigned char>(c)) ? c : '.';
    }
    return result;
}

std::string GetFuncName(uint32_t addr) {
    char buf[32];
    snprintf(buf, sizeof(buf), "sub_%08X", addr);
    return buf;
}

} // namespace Debugger

#!/bin/bash
# Install Terminal MCP Server to VS Code

MCP_CONFIG_DIR="$HOME/Library/Application Support/Code/User"
MCP_CONFIG_FILE="$MCP_CONFIG_DIR/mcp.json"
SERVER_PATH="/Users/Ozordi/Downloads/LibertyRecomp/tools/terminal-mcp-server/dist/index.js"

# Ensure config directory exists
mkdir -p "$MCP_CONFIG_DIR"

# Check if mcp.json exists
if [ -f "$MCP_CONFIG_FILE" ]; then
    echo "Existing mcp.json found. Backing up to mcp.json.backup"
    cp "$MCP_CONFIG_FILE" "$MCP_CONFIG_FILE.backup"
    
    # Try to add terminal server to existing config
    if command -v jq &> /dev/null; then
        # Use jq to merge
        jq --arg path "$SERVER_PATH" '.servers.terminal = {"command": "node", "args": [$path]}' "$MCP_CONFIG_FILE" > "$MCP_CONFIG_FILE.tmp" && mv "$MCP_CONFIG_FILE.tmp" "$MCP_CONFIG_FILE"
        echo "Added terminal server to existing mcp.json"
    else
        echo "jq not found. Please manually add the terminal server to your mcp.json:"
        echo ""
        echo '"terminal": {'
        echo '  "command": "node",'
        echo '  "args": ["'$SERVER_PATH'"]'
        echo '}'
    fi
else
    # Create new mcp.json
    cat > "$MCP_CONFIG_FILE" << EOF
{
  "servers": {
    "terminal": {
      "command": "node",
      "args": ["$SERVER_PATH"]
    }
  }
}
EOF
    echo "Created new mcp.json with terminal server"
fi

echo ""
echo "Installation complete!"
echo "MCP config location: $MCP_CONFIG_FILE"
echo ""
echo "Please restart VS Code for the changes to take effect."

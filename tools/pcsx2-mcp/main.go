package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"os"

	"github.com/modelcontextprotocol/go-sdk/mcp"
)

func main() {
	if err := run(); err != nil {
		log.Print(err)
		os.Exit(1)
	}
}

func run() error {
	override := flag.String("emulator", "", "path to the PCSX2 executable")
	flag.Parse()
	path, pathErr := emulatorPath(*override)
	emu := &emulator{path: path, args: flag.Args()}
	defer emu.close()

	server := mcp.NewServer(&mcp.Implementation{Name: "pcsx2-mcp", Version: "1.0.0"}, nil)
	mcp.AddTool(server, &mcp.Tool{Name: "pcsx2_connect", Description: "Launch the configured PCSX2 emulator and connect to it."},
		func(ctx context.Context, _ *mcp.CallToolRequest, _ struct{}) (*mcp.CallToolResult, any, error) {
			if pathErr != nil {
				return nil, nil, pathErr
			}
			if err := emu.connect(ctx); err != nil {
				return nil, nil, err
			}
			return &mcp.CallToolResult{Content: []mcp.Content{&mcp.TextContent{Text: "Connected to PCSX2."}}}, nil, nil
		})
	addDebuggerTools(server, emu)
	addMemoryTools(server, emu)
	if err := server.Run(context.Background(), &mcp.StdioTransport{}); err != nil {
		return fmt.Errorf("MCP stdio: %w", err)
	}
	return nil
}

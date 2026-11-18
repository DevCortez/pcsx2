package main

import (
	"context"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"strconv"
	"strings"
	"sync"

	"github.com/modelcontextprotocol/go-sdk/mcp"
)

type patternInput struct {
	cpuInput
	Start      string `json:"start,omitempty"`
	End        string `json:"end,omitempty" jsonschema:"Exclusive hexadecimal end address"`
	Pattern    string `json:"pattern" jsonschema:"Hexadecimal bytes with ?? wildcards"`
	MaxResults int    `json:"max_results,omitempty"`
}

type diffInput struct {
	cpuInput
	Address string `json:"address"`
	Length  int    `json:"length,omitempty"`
	Name    string `json:"name,omitempty"`
}

func parseAddress(input string) (uint64, error) {
	return strconv.ParseUint(strings.TrimPrefix(strings.ToLower(input), "0x"), 16, 32)
}

func (emu *emulator) memory(ctx context.Context, input memoryInput) ([]byte, error) {
	raw, err := emu.call(ctx, "read_memory", input)
	if err != nil {
		return nil, err
	}
	var encoded string
	if err := json.Unmarshal(raw, &encoded); err != nil {
		return nil, err
	}
	decoded, err := hex.DecodeString(encoded)
	if err == nil && len(decoded) != input.Length {
		return nil, errors.New("unexpected PCSX2 memory read length")
	}
	return decoded, err
}

func (emu *emulator) generation(ctx context.Context) (string, error) {
	emu.mu.Lock()
	defer emu.mu.Unlock()
	raw, err := emu.callLocked(ctx, "status", struct{}{})
	if err != nil {
		return "", err
	}
	var status struct {
		Active     bool   `json:"active"`
		Generation string `json:"generation"`
	}
	if err := json.Unmarshal(raw, &status); err != nil {
		return "", err
	}
	if !status.Active {
		return "", errors.New("no active PS2 virtual machine")
	}
	return fmt.Sprintf("%d/%s", emu.session, status.Generation), nil
}

func findPattern(ctx context.Context, emu *emulator, input patternInput) ([]string, error) {
	if input.Start == "" {
		input.Start = "00100000"
	}
	if input.End == "" {
		input.End = "02000000"
	}
	start, err := parseAddress(input.Start)
	if err != nil {
		return nil, err
	}
	end, err := parseAddress(input.End)
	if err != nil || end <= start || end-start > 32<<20 {
		return nil, errors.New("invalid search range (maximum 32 MiB)")
	}
	pattern := strings.Join(strings.Fields(input.Pattern), "")
	if len(pattern) < 2 || len(pattern)%2 != 0 || len(pattern) > 512 {
		return nil, errors.New("pattern must contain 1 through 256 whole bytes")
	}
	var target []int
	for i := 0; i < len(pattern); i += 2 {
		if pattern[i:i+2] == "??" {
			target = append(target, -1)
			continue
		}
		value, err := strconv.ParseUint(pattern[i:i+2], 16, 8)
		if err != nil {
			return nil, errors.New("invalid hexadecimal pattern")
		}
		target = append(target, int(value))
	}
	maxResults := input.MaxResults
	if maxResults == 0 {
		maxResults = 20
	}
	if maxResults < 1 || maxResults > 256 {
		return nil, errors.New("max_results must be between 1 and 256")
	}
	generation, err := emu.generation(ctx)
	if err != nil {
		return nil, err
	}
	matches := make([]string, 0)
	var tail []byte
	for at := start; at < end && len(matches) < maxResults; {
		// Use page-aligned reads so an unmapped page cannot hide nearby readable bytes.
		size := min(end-at, 4096-at%4096)
		data, err := emu.memory(ctx, memoryInput{input.cpuInput, fmt.Sprintf("%x", at), int(size)})
		if err != nil {
			if err.Error() != "Memory range is not readable" {
				return nil, err
			}
			tail = nil
			at += size
			continue
		}
		base := at - uint64(len(tail))
		window := append(tail, data...)
		for offset := 0; offset+len(target) <= len(window) && len(matches) < maxResults; offset++ {
			match := true
			for i, value := range target {
				if value >= 0 && int(window[offset+i]) != value {
					match = false
					break
				}
			}
			if match {
				matches = append(matches, fmt.Sprintf("0x%08x", base+uint64(offset)))
			}
		}
		tail = append(tail[:0], window[len(window)-min(len(target)-1, len(window)):]...)
		at += size
	}
	if current, err := emu.generation(ctx); err != nil || current != generation {
		return nil, errors.New("VM changed during memory search")
	}
	return matches, nil
}

type patternResult struct {
	Matches []string `json:"matches"`
}

func addMemoryTools(server *mcp.Server, emu *emulator) {
	mcp.AddTool(server, &mcp.Tool{Name: "pcsx2_find_pattern", Description: "Find a byte pattern in guest memory with optional wildcards."},
		func(ctx context.Context, _ *mcp.CallToolRequest, input patternInput) (*mcp.CallToolResult, patternResult, error) {
			matches, err := findPattern(ctx, emu, input)
			return nil, patternResult{Matches: matches}, err
		})

	var mu sync.Mutex
	var previousGeneration string
	snapshots := make(map[string][]byte)
	mcp.AddTool(server, &mcp.Tool{Name: "pcsx2_memory_diff", Description: "Snapshot a named memory range on first call and compare it on the next."},
		func(ctx context.Context, _ *mcp.CallToolRequest, input diffInput) (*mcp.CallToolResult, any, error) {
			mu.Lock()
			defer mu.Unlock()
			address, err := parseAddress(input.Address)
			if err != nil {
				return nil, nil, err
			}
			if input.Length == 0 {
				input.Length = 256
			}
			if input.Length < 1 || input.Length > 65536 || len(input.Name) > 128 {
				return nil, nil, errors.New("invalid memory snapshot length or name")
			}
			generation, err := emu.generation(ctx)
			if err != nil {
				return nil, nil, err
			}
			if generation != previousGeneration {
				clear(snapshots)
				previousGeneration = generation
			}
			data, err := emu.memory(ctx, memoryInput{input.cpuInput, input.Address, input.Length})
			if err != nil {
				return nil, nil, err
			}
			if current, err := emu.generation(ctx); err != nil || current != generation {
				return nil, nil, errors.New("VM changed during memory snapshot")
			}
			cpu := input.CPU
			if cpu == "" {
				cpu = "ee"
			}
			key := fmt.Sprintf("%s/%x/%d/%s", cpu, address, input.Length, input.Name)
			prior, ok := snapshots[key]
			if !ok {
				if len(snapshots) >= 16 {
					return nil, nil, errors.New("snapshot limit reached; compare an existing snapshot")
				}
				snapshots[key] = data
				return textResult(json.RawMessage(`{"snapshot_saved":true}`)), nil, nil
			}
			delete(snapshots, key)
			changes := make([]map[string]string, 0)
			total := 0
			for i, value := range data {
				if value == prior[i] {
					continue
				}
				total++
				if len(changes) < 256 {
					changes = append(changes, map[string]string{
						"address": fmt.Sprintf("0x%08x", address+uint64(i)),
						"before":  fmt.Sprintf("%02x", prior[i]),
						"after":   fmt.Sprintf("%02x", value),
					})
				}
			}
			result, err := json.Marshal(map[string]any{"changed_bytes": total, "changes": changes})
			return textResult(result), nil, err
		})
}

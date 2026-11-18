package main

import (
	"context"
	"encoding/json"
	"strings"

	"github.com/modelcontextprotocol/go-sdk/mcp"
)

type cpuInput struct {
	CPU string `json:"cpu,omitempty" jsonschema:"Guest CPU, ee (default) or iop"`
}

type memoryInput struct {
	cpuInput
	Address string `json:"address" jsonschema:"Hexadecimal guest address"`
	Length  int    `json:"length" jsonschema:"Number of bytes, from 1 through 65536"`
}

type writeMemoryInput struct {
	cpuInput
	Address string `json:"address" jsonschema:"Hexadecimal guest address"`
	Data    string `json:"data" jsonschema:"Hexadecimal bytes without separators"`
}

type registerInput struct {
	cpuInput
	Category *int `json:"category,omitempty" jsonschema:"Register category, omit to read all categories"`
}

type writeRegisterInput struct {
	cpuInput
	Category int    `json:"category" jsonschema:"Register category"`
	Index    int    `json:"index" jsonschema:"Index in the register category"`
	Value    string `json:"value" jsonschema:"Hexadecimal value up to 128 bits"`
}

type disassembleInput struct {
	cpuInput
	Address string `json:"address" jsonschema:"Word-aligned hexadecimal guest address"`
	Count   int    `json:"count" jsonschema:"Number of instructions, from 1 through 256"`
}

type expressionInput struct {
	cpuInput
	Expression string `json:"expression" jsonschema:"PCSX2 debugger expression"`
}

type breakpointInput struct {
	cpuInput
	Address string `json:"address"`
}

type setBreakpointInput struct {
	breakpointInput
	Condition   string `json:"condition,omitempty"`
	Description string `json:"description,omitempty"`
}

type watchpointInput struct {
	cpuInput
	Start string `json:"start"`
	End   string `json:"end" jsonschema:"Exclusive end address in hexadecimal"`
}

type setWatchpointInput struct {
	watchpointInput
	Mode        string `json:"mode,omitempty" jsonschema:"read, write, access (default), or onchange"`
	Condition   string `json:"condition,omitempty"`
	Description string `json:"description,omitempty"`
}

type mountDiscInput struct {
	Path string `json:"path" jsonschema:"Absolute path to a local PS2 disc image"`
}

type slotInput struct {
	Slot int `json:"slot" jsonschema:"Save-state slot from 0 through 9"`
}

type backtraceInput struct {
	cpuInput
	ThreadID *int `json:"thread_id,omitempty" jsonschema:"Guest thread ID, omit to use the running thread"`
}

func addEmulatorTool[T any](server *mcp.Server, emu *emulator, name, description string) {
	mcp.AddTool(server, &mcp.Tool{Name: "pcsx2_" + name, Description: description},
		func(ctx context.Context, _ *mcp.CallToolRequest, input T) (*mcp.CallToolResult, any, error) {
			result, err := emu.call(ctx, name, input)
			if err != nil {
				return nil, nil, err
			}
			return textResult(result), nil, nil
		})
}

func textResult(result json.RawMessage) *mcp.CallToolResult {
	return &mcp.CallToolResult{Content: []mcp.Content{&mcp.TextContent{Text: strings.TrimSpace(string(result))}}}
}

func addDebuggerTools(server *mcp.Server, emu *emulator) {
	addEmulatorTool[struct{}](server, emu, "status", "Report whether PCSX2 has an active or paused VM.")
	addEmulatorTool[memoryInput](server, emu, "read_memory", "Read guest memory and return hexadecimal bytes.")
	addEmulatorTool[writeMemoryInput](server, emu, "write_memory", "Write guest memory while the VM is paused.")
	addEmulatorTool[memoryInput](server, emu, "read_string", "Read a UTF-8 string up to the first zero byte or the length limit.")
	addEmulatorTool[registerInput](server, emu, "read_registers", "Read guest registers with full-width hexadecimal values.")
	addEmulatorTool[writeRegisterInput](server, emu, "write_register", "Write a guest register while the VM is paused.")
	addEmulatorTool[disassembleInput](server, emu, "disassemble", "Disassemble guest MIPS instructions.")
	addEmulatorTool[expressionInput](server, emu, "evaluate", "Evaluate an expression using the PCSX2 debugger.")
	addEmulatorTool[setBreakpointInput](server, emu, "set_breakpoint", "Set an instruction breakpoint.")
	addEmulatorTool[breakpointInput](server, emu, "remove_breakpoint", "Remove an instruction breakpoint.")
	addEmulatorTool[cpuInput](server, emu, "list_breakpoints", "List instruction breakpoints for a guest CPU.")
	addEmulatorTool[setWatchpointInput](server, emu, "set_watchpoint", "Set a memory watchpoint.")
	addEmulatorTool[watchpointInput](server, emu, "remove_watchpoint", "Remove a memory watchpoint.")
	addEmulatorTool[cpuInput](server, emu, "list_watchpoints", "List memory watchpoints for a guest CPU.")
	addEmulatorTool[struct{}](server, emu, "clear_all_breakpoints", "Clear all breakpoints and watchpoints across both guest CPUs.")
	addEmulatorTool[cpuInput](server, emu, "step", "Step into the next guest instruction or branch target.")
	addEmulatorTool[cpuInput](server, emu, "step_over", "Step over a guest function call.")
	addEmulatorTool[cpuInput](server, emu, "pause", "Pause the VM and return the current guest program counter.")
	addEmulatorTool[cpuInput](server, emu, "continue", "Resume the VM.")
	addEmulatorTool[cpuInput](server, emu, "get_threads", "List guest BIOS threads.")
	addEmulatorTool[struct{}](server, emu, "get_modules", "List loaded IOP modules.")
	addEmulatorTool[backtraceInput](server, emu, "get_backtrace", "Walk a guest thread's stack while the VM is paused.")
	addEmulatorTool[struct{}](server, emu, "game_info", "Read metadata for the running game.")
	addEmulatorTool[mountDiscInput](server, emu, "mount_disc", "Boot a disc image from the idle screen or swap discs in a running VM.")
	addEmulatorTool[slotInput](server, emu, "save_state", "Save emulator state to a numbered slot.")
	addEmulatorTool[slotInput](server, emu, "load_state", "Load emulator state from a numbered slot.")
}

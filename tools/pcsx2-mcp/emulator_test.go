package main

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"net"
	"os"
	"strings"
	"testing"
	"time"
)

func TestEmulatorHandshake(t *testing.T) {
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	emu := emulator{path: executable, args: []string{"-test.run=^TestEmulatorChild$"}}
	defer emu.close()
	if err := emu.connect(t.Context()); err != nil {
		t.Fatal(err)
	}
	if err := emu.connect(t.Context()); err != nil {
		t.Fatal(err)
	}
	if emu.session != 1 {
		t.Fatal("idempotent connection launched another process")
	}
	result, err := emu.call(t.Context(), "status", struct{}{})
	if err != nil || string(result) != "true" {
		t.Fatalf("status: %s, %v", result, err)
	}
	if _, err := emu.call(t.Context(), "malformed", struct{}{}); err == nil {
		t.Fatal("malformed response was accepted")
	}
	select {
	case <-emu.done:
	case <-time.After(5 * time.Second):
		t.Fatal("test emulator did not exit")
	}
	if err := emu.connect(t.Context()); err != nil {
		t.Fatal(err)
	}
	if emu.session != 2 {
		t.Fatal("connection failed to relaunch emulator")
	}
	ctx, cancel := context.WithTimeout(t.Context(), 100*time.Millisecond)
	defer cancel()
	if _, err := emu.call(ctx, "stall", struct{}{}); err == nil {
		t.Fatal("command timeout was ignored")
	}
	if _, err := emu.call(t.Context(), "status", struct{}{}); err == nil || !strings.Contains(err.Error(), "disconnected") {
		t.Fatalf("timed out connection was not discarded: %v", err)
	}
}

func TestEmulatorStartupTimeout(t *testing.T) {
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	emu := emulator{path: executable, args: []string{"-test.run=^TestEmulatorChild$", "--", "stall"}}
	defer emu.close()
	ctx, cancel := context.WithTimeout(t.Context(), 150*time.Millisecond)
	defer cancel()
	if err := emu.connect(ctx); err == nil || !strings.Contains(err.Error(), "waiting for PCSX2 handshake") {
		t.Fatalf("expected handshake timeout, got %v", err)
	}
}

func TestEmulatorChild(t *testing.T) {
	port, token := os.Getenv("PCSX2_MCP_PORT"), os.Getenv("PCSX2_MCP_TOKEN")
	if port == "" || token == "" {
		return
	}
	if os.Args[len(os.Args)-1] == "stall" {
		time.Sleep(300 * time.Millisecond)
		return
	}
	dial := func() net.Conn {
		conn, err := net.DialTimeout("tcp4", "127.0.0.1:"+port, time.Second)
		if err != nil {
			t.Fatal(err)
		}
		return conn
	}
	first := dial()
	if err := writeFrame(first, handshake{Version: protocolVersion, Token: "incorrect"}); err != nil {
		t.Fatal(err)
	}
	first.Close()
	conn := dial()
	defer conn.Close()
	if err := writeFrame(conn, handshake{Version: protocolVersion, Token: token}); err != nil {
		t.Fatal(err)
	}
	for {
		var req request
		if err := readFrame(conn, &req); err != nil {
			return
		}
		switch req.Command {
		case "status":
			if err := writeFrame(conn, response{Result: json.RawMessage("true")}); err != nil {
				t.Fatal(err)
			}
		case "malformed":
			if err := binary.Write(conn, binary.BigEndian, uint32(maxFrameSize+1)); err != nil {
				t.Fatal(err)
			}
			return
		case "stall":
			time.Sleep(300 * time.Millisecond)
			return
		default:
			t.Fatalf("unexpected command %q", req.Command)
		}
	}
}

package main

import (
	"context"
	"crypto/rand"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"sync"
	"time"
)

type emulator struct {
	path    string
	args    []string
	mu      sync.Mutex
	conn    net.Conn
	done    chan struct{}
	session uint64
}

func emulatorPath(override string) (string, error) {
	if override != "" {
		path, err := filepath.Abs(override)
		if err != nil {
			return "", err
		}
		return filepath.EvalSymlinks(path)
	}
	self, err := os.Executable()
	if err != nil {
		return "", err
	}
	dir := filepath.Dir(self)
	name := "pcsx2-qt"
	if runtime.GOOS == "windows" {
		name += ".exe"
	}
	preferred := filepath.Join(dir, name)
	if info, err := os.Stat(preferred); err == nil && info.Mode().IsRegular() {
		return preferred, nil
	}
	if runtime.GOOS == "windows" {
		matches, err := filepath.Glob(filepath.Join(dir, "pcsx2-qt*.exe"))
		if err == nil && len(matches) == 1 {
			return matches[0], nil
		}
	}
	return "", fmt.Errorf("could not select PCSX2 beside the companion; specify --emulator <path>")
}

func (emu *emulator) connect(ctx context.Context) error {
	emu.mu.Lock()
	defer emu.mu.Unlock()
	if emu.done != nil {
		select {
		case <-emu.done:
			emu.closeConnection()
			emu.done = nil
		default:
			if emu.conn != nil {
				return nil
			}
			return errors.New("previous PCSX2 instance is still open; close it before launching again")
		}
	}
	listener, err := net.ListenTCP("tcp4", &net.TCPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		return err
	}
	defer listener.Close()
	var secret [32]byte
	if _, err := rand.Read(secret[:]); err != nil {
		return err
	}
	token := hex.EncodeToString(secret[:])
	command := exec.Command(emu.path, emu.args...)
	command.Env = append(os.Environ(), "PCSX2_MCP_PORT="+strconv.Itoa(listener.Addr().(*net.TCPAddr).Port), "PCSX2_MCP_TOKEN="+token)
	command.Stdout = os.Stderr
	command.Stderr = os.Stderr
	if err := command.Start(); err != nil {
		return fmt.Errorf("launch PCSX2: %w", err)
	}
	done := make(chan struct{})
	emu.done = done
	go func() {
		_ = command.Wait()
		close(done)
		listener.Close()
	}()
	ctx, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	stop := context.AfterFunc(ctx, func() { listener.Close() })
	defer stop()

	for {
		conn, err := listener.AcceptTCP()
		if err != nil {
			select {
			case <-done:
				return errors.New("PCSX2 exited before connecting to the companion")
			default:
				if ctx.Err() != nil {
					return fmt.Errorf("waiting for PCSX2 handshake: %w", ctx.Err())
				}
				return err
			}
		}
		conn.SetDeadline(time.Now().Add(time.Second))
		var hello handshake
		err = readFrame(conn, &hello)
		if err != nil || subtle.ConstantTimeCompare([]byte(hello.Token), []byte(token)) != 1 {
			conn.Close()
			continue
		}
		if hello.Version != protocolVersion {
			conn.Close()
			return fmt.Errorf("PCSX2 protocol version %d, expected %d", hello.Version, protocolVersion)
		}
		conn.SetDeadline(time.Time{})
		emu.conn = conn
		emu.session++
		return nil
	}
}

func (emu *emulator) closeConnection() {
	if emu.conn != nil {
		emu.conn.Close()
		emu.conn = nil
	}
}

func (emu *emulator) close() {
	emu.mu.Lock()
	defer emu.mu.Unlock()
	emu.closeConnection()
}

func (emu *emulator) call(ctx context.Context, name string, args any) (json.RawMessage, error) {
	emu.mu.Lock()
	defer emu.mu.Unlock()
	return emu.callLocked(ctx, name, args)
}

func (emu *emulator) callLocked(ctx context.Context, name string, args any) (json.RawMessage, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	conn := emu.conn
	if conn == nil {
		return nil, errors.New("PCSX2 is disconnected; call pcsx2_connect")
	}
	timeout := 5 * time.Second
	if name == "save_state" || name == "load_state" || name == "mount_disc" {
		timeout = time.Minute
	}
	ctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	deadline, _ := ctx.Deadline()
	conn.SetDeadline(deadline)
	interruptDone := make(chan struct{})
	stop := context.AfterFunc(ctx, func() {
		conn.SetDeadline(time.Now())
		close(interruptDone)
	})
	defer func() {
		if !stop() {
			<-interruptDone
		}
	}()
	var reply response
	err := writeFrame(conn, request{Command: name, Args: args})
	if err == nil {
		err = readFrame(conn, &reply)
	}
	if err != nil {
		emu.closeConnection()
		return nil, fmt.Errorf("PCSX2 request failed and the bridge disconnected: %w", err)
	}
	if reply.Error != "" {
		return nil, errors.New(reply.Error)
	}
	return reply.Result, nil
}

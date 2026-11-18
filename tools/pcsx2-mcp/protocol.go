package main

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
)

const (
	protocolVersion = 1
	maxFrameSize    = 1 << 20
)

type handshake struct {
	Version int    `json:"version"`
	Token   string `json:"token"`
}

type request struct {
	Command string `json:"command"`
	Args    any    `json:"args"`
}

type response struct {
	Result json.RawMessage `json:"result"`
	Error  string          `json:"error"`
}

func readFrame(reader io.Reader, value any) error {
	var size uint32
	if err := binary.Read(reader, binary.BigEndian, &size); err != nil {
		return err
	}
	if size == 0 || size > maxFrameSize {
		return fmt.Errorf("invalid PCSX2 frame size %d", size)
	}
	payload := make([]byte, size)
	if _, err := io.ReadFull(reader, payload); err != nil {
		return err
	}
	return json.Unmarshal(payload, value)
}

func writeFrame(writer io.Writer, value any) error {
	payload, err := json.Marshal(value)
	if err != nil {
		return err
	}
	if len(payload) > maxFrameSize {
		return fmt.Errorf("PCSX2 frame exceeds %d bytes", maxFrameSize)
	}
	if err := binary.Write(writer, binary.BigEndian, uint32(len(payload))); err != nil {
		return err
	}
	written, err := writer.Write(payload)
	if err == nil && written != len(payload) {
		return io.ErrShortWrite
	}
	return err
}

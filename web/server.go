// server.go — PTY + WebSocket server for the WebGL terminal
//
// Usage:
//   go run server.go                              # spawns $SHELL
//   go run server.go -- claude                    # spawns claude REPL
//   go run server.go -- claude -p "prompt"        # spawns claude with prompt
//   go run server.go -port 9090 -- claude         # custom port
//
// Open http://localhost:9090 in your browser.

package main

import (
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/creack/pty"
	"github.com/gorilla/websocket"
)

const defaultCols = 120
const defaultRows = 40

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool { return true },
}

func main() {
	port := "9090"
	childCmd := os.Getenv("SHELL")
	if childCmd == "" {
		childCmd = "/bin/bash"
	}
	var childArgs []string

	// Parse args: [-port N] [--] <cmd> [args...]
	args := os.Args[1:]
	i := 0
	for i < len(args) {
		switch args[i] {
		case "-port":
			i++
			if i < len(args) {
				port = args[i]
			}
			i++
		case "--":
			i++
			if i < len(args) {
				childCmd = args[i]
				childArgs = args[i+1:]
			}
			i = len(args)
		default:
			childCmd = args[i]
			childArgs = args[i+1:]
			i = len(args)
		}
	}

	// Find web directory relative to the executable
	exePath, _ := os.Executable()
	webDir := filepath.Join(filepath.Dir(exePath), "web")
	if _, err := os.Stat(filepath.Join(webDir, "term.html")); err != nil {
		// Try relative to cwd
		webDir = "web"
	}

	http.HandleFunc("/ws", func(w http.ResponseWriter, r *http.Request) {
		handleWS(w, r, childCmd, childArgs)
	})
	http.Handle("/web/", http.StripPrefix("/web/", http.FileServer(http.Dir(webDir))))
	http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/" {
			http.NotFound(w, r)
			return
		}
		http.ServeFile(w, r, filepath.Join(webDir, "term.html"))
	})

	log.Printf("dumbterm server: http://localhost:%s", port)
	log.Printf("child: %s %s", childCmd, strings.Join(childArgs, " "))
	log.Printf("terminal: %dx%d", defaultCols, defaultRows)

	if err := http.ListenAndServe(":"+port, nil); err != nil {
		log.Fatal(err)
	}
}

func handleWS(w http.ResponseWriter, r *http.Request, childCmd string, childArgs []string) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Printf("ws upgrade: %v", err)
		return
	}
	defer conn.Close()

	log.Printf("client connected, spawning: %s %v", childCmd, childArgs)

	// Find the executable
	cmdPath, err := exec.LookPath(childCmd)
	if err != nil {
		log.Printf("command not found: %s: %v", childCmd, err)
		conn.WriteMessage(websocket.BinaryMessage, []byte(fmt.Sprintf("\r\n\x1b[31mcommand not found: %s\x1b[0m\r\n", childCmd)))
		return
	}

	// Spawn with PTY
	cmd := exec.Command(cmdPath, childArgs...)
	cmd.Env = append(os.Environ(), "TERM=xterm-256color", "COLORTERM=truecolor")

	ptmx, err := pty.StartWithSize(cmd, &pty.Winsize{
		Rows: defaultRows,
		Cols: defaultCols,
	})
	if err != nil {
		log.Printf("pty start: %v", err)
		conn.WriteMessage(websocket.BinaryMessage, []byte(fmt.Sprintf("\r\n\x1b[31mpty error: %v\x1b[0m\r\n", err)))
		return
	}
	defer ptmx.Close()

	// Forward SIGWINCH
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGWINCH)
	defer signal.Stop(sigCh)

	var wg sync.WaitGroup

	// PTY → WebSocket
	wg.Add(1)
	go func() {
		defer func() {
			if r := recover(); r != nil {
				log.Printf("PTY→WS panic: %v", r)
			}
			wg.Done()
		}()
		buf := make([]byte, 4096)
		for {
			n, err := ptmx.Read(buf)
			if n > 0 {
				if werr := conn.WriteMessage(websocket.BinaryMessage, buf[:n]); werr != nil {
					log.Printf("PTY→WS write error: %v", werr)
					return
				}
			}
			if err != nil {
				log.Printf("PTY→WS read done: %v", err)
				return
			}
		}
	}()

	// WebSocket → PTY
	wg.Add(1)
	go func() {
		defer func() {
			if r := recover(); r != nil {
				log.Printf("WS→PTY panic: %v", r)
			}
			wg.Done()
		}()
		for {
			_, msg, err := conn.ReadMessage()
			if err != nil {
				log.Printf("WS→PTY read error: %v", err)
				return
			}
			s := string(msg)
			if strings.HasPrefix(s, "\x1b_RESIZE;") {
				s = strings.TrimSuffix(s, "\x1b\\")
				s = s[len("\x1b_RESIZE;"):]
				var cols, rows int
				fmt.Sscanf(s, "%d;%d", &cols, &rows)
				if cols > 0 && rows > 0 {
					log.Printf("resize: %dx%d", cols, rows)
					pty.Setsize(ptmx, &pty.Winsize{Rows: uint16(rows), Cols: uint16(cols)})
				}
				continue
			}
			if _, err := io.WriteString(ptmx, s); err != nil {
				log.Printf("WS→PTY write error: %v", err)
				return
			}
		}
	}()

	// Wait for child to exit
	err = cmd.Wait()
	exitCode := 0
	if cmd.ProcessState != nil {
		exitCode = cmd.ProcessState.ExitCode()
	}
	log.Printf("child exited: code=%d err=%v", exitCode, err)

	// Give PTY→WS goroutine 2 seconds to drain remaining output
	time.Sleep(2 * time.Second)

	// Send exit notification
	conn.WriteMessage(websocket.BinaryMessage, []byte(
		fmt.Sprintf("\r\n\x1b[38;2;255;200;100m[process exited: %d]\x1b[0m\r\n", exitCode)))
	log.Printf("sent exit notification")
}

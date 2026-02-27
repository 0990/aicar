package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/mark3labs/mcp-go/mcp"
	"github.com/mark3labs/mcp-go/server"
)

const (
	defaultBaseURL = "http://192.168.1.50"
	defaultTimeout = 5 * time.Second
)

type httpBridge struct {
	mu      sync.RWMutex
	baseURL string
	token   string
	client  *http.Client
}

func newHTTPBridge(baseURL, token string, timeout time.Duration) *httpBridge {
	if strings.TrimSpace(baseURL) == "" {
		baseURL = defaultBaseURL
	}
	return &httpBridge{
		baseURL: strings.TrimRight(baseURL, "/"),
		token:   token,
		client:  &http.Client{Timeout: timeout},
	}
}

func (b *httpBridge) configure(baseURL, token string) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if strings.TrimSpace(baseURL) != "" {
		b.baseURL = strings.TrimRight(baseURL, "/")
	}
	b.token = token
}

func (b *httpBridge) get(path string, q url.Values) (int, string, error) {
	return b.request(http.MethodGet, path, q)
}

func (b *httpBridge) post(path string, q url.Values) (int, string, error) {
	return b.request(http.MethodPost, path, q)
}

func (b *httpBridge) request(method, path string, q url.Values) (int, string, error) {
	b.mu.RLock()
	baseURL := b.baseURL
	token := b.token
	client := b.client
	b.mu.RUnlock()

	endpoint := baseURL + path
	if len(q) > 0 {
		endpoint += "?" + q.Encode()
	}

	req, err := http.NewRequest(method, endpoint, nil)
	if err != nil {
		return 0, "", err
	}
	if strings.TrimSpace(token) != "" {
		req.Header.Set("X-Robot-Token", token)
	}
	resp, err := client.Do(req)
	if err != nil {
		return 0, "", err
	}
	defer resp.Body.Close()

	body, readErr := io.ReadAll(resp.Body)
	if readErr != nil {
		return resp.StatusCode, "", readErr
	}
	return resp.StatusCode, string(body), nil
}

func normalizeBaseURL(raw string) string {
	v := strings.TrimSpace(raw)
	if v == "" {
		return ""
	}
	if !strings.HasPrefix(v, "http://") && !strings.HasPrefix(v, "https://") {
		v = "http://" + v
	}
	return strings.TrimRight(v, "/")
}

func containsAny(text string, words ...string) bool {
	for _, word := range words {
		if strings.Contains(text, word) {
			return true
		}
	}
	return false
}

func directionFromText(text string) string {
	raw := strings.TrimSpace(strings.ToLower(text))
	switch {
	case containsAny(raw, "stop", "halt", "停止", "停下", "刹车"):
		return "STOP"
	case containsAny(raw, "forward", "ahead", "前进", "向前"):
		return "FORWARD"
	case containsAny(raw, "backward", "reverse", "后退", "向后"):
		return "BACKWARD"
	case containsAny(raw, "left", "左转", "向左"):
		return "LEFT"
	case containsAny(raw, "right", "右转", "向右"):
		return "RIGHT"
	default:
		return ""
	}
}

func jsonText(v any) string {
	bytes, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		return fmt.Sprintf("{\"error\":\"%s\"}", err.Error())
	}
	return string(bytes)
}

func intArg(args map[string]any, key string) (int, bool) {
	v, ok := args[key]
	if !ok {
		return 0, false
	}
	switch t := v.(type) {
	case float64:
		return int(t), true
	case int:
		return t, true
	case string:
		n, err := strconv.Atoi(strings.TrimSpace(t))
		if err == nil {
			return n, true
		}
	}
	return 0, false
}

func runRobotRequest(call func() (int, string, error)) string {
	status, body, err := call()
	if err != nil {
		return "request failed: " + err.Error()
	}
	return jsonText(map[string]any{
		"http_status": status,
		"body":        body,
	})
}

func main() {
	baseURL := normalizeBaseURL(os.Getenv("ROBOT_BASE_URL"))
	if baseURL == "" {
		baseURL = defaultBaseURL
	}
	token := strings.TrimSpace(os.Getenv("ROBOT_HTTP_TOKEN"))
	bridge := newHTTPBridge(baseURL, token, defaultTimeout)

	mcpServer := server.NewMCPServer(
		"aicar-robot-mcp-http",
		"0.2.0",
		server.WithToolCapabilities(true),
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_set_endpoint",
			mcp.WithDescription("Set robot HTTP endpoint and token"),
			mcp.WithString("base_url", mcp.Required(), mcp.Description("Robot base URL, for example http://192.168.1.88")),
			mcp.WithString("token", mcp.Description("Optional shared token")),
		),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			baseURL, err := request.RequireString("base_url")
			if err != nil {
				return mcp.NewToolResultText("invalid base_url: " + err.Error()), nil
			}
			token := ""
			if raw, ok := request.GetArguments()["token"].(string); ok {
				token = raw
			}
			normalized := normalizeBaseURL(baseURL)
			if normalized == "" {
				return mcp.NewToolResultText("base_url is empty"), nil
			}
			bridge.configure(normalized, token)
			return mcp.NewToolResultText(jsonText(map[string]any{
				"base_url": normalized,
				"token":    token != "",
			})), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_health", mcp.WithDescription("Check robot /health endpoint")),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			result := runRobotRequest(func() (int, string, error) {
				return bridge.get("/health", nil)
			})
			return mcp.NewToolResultText(result), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_ping", mcp.WithDescription("Check robot /ping endpoint")),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			result := runRobotRequest(func() (int, string, error) {
				return bridge.post("/ping", nil)
			})
			return mcp.NewToolResultText(result), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_move",
			mcp.WithDescription("Execute direction move via HTTP"),
			mcp.WithString("direction", mcp.Required(), mcp.Description("FORWARD/BACKWARD/LEFT/RIGHT/STOP")),
			mcp.WithNumber("duration_ms", mcp.Description("Optional duration in milliseconds")),
			mcp.WithNumber("speed", mcp.Description("Optional speed 0..255")),
		),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			direction, err := request.RequireString("direction")
			if err != nil {
				return mcp.NewToolResultText("invalid direction: " + err.Error()), nil
			}
			dir := strings.ToUpper(strings.TrimSpace(direction))
			switch dir {
			case "FORWARD", "BACKWARD", "LEFT", "RIGHT", "STOP":
			default:
				return mcp.NewToolResultText("direction must be FORWARD/BACKWARD/LEFT/RIGHT/STOP"), nil
			}

			args := request.GetArguments()
			q := url.Values{}
			q.Set("direction", dir)
			if duration, ok := intArg(args, "duration_ms"); ok && duration > 0 {
				q.Set("duration_ms", strconv.Itoa(duration))
			}
			if speed, ok := intArg(args, "speed"); ok && speed >= 0 {
				q.Set("speed", strconv.Itoa(speed))
			}

			result := runRobotRequest(func() (int, string, error) {
				return bridge.post("/api/move", q)
			})
			return mcp.NewToolResultText(result), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_text_control",
			mcp.WithDescription("Send natural language text to robot HTTP endpoint"),
			mcp.WithString("text", mcp.Required(), mcp.Description("Natural text, for example 向左转一下")),
			mcp.WithNumber("duration_ms", mcp.Description("Optional override duration")),
			mcp.WithNumber("speed", mcp.Description("Optional override speed")),
		),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			text, err := request.RequireString("text")
			if err != nil {
				return mcp.NewToolResultText("invalid text: " + err.Error()), nil
			}
			args := request.GetArguments()
			q := url.Values{}
			q.Set("text", text)
			if duration, ok := intArg(args, "duration_ms"); ok && duration > 0 {
				q.Set("duration_ms", strconv.Itoa(duration))
			}
			if speed, ok := intArg(args, "speed"); ok && speed >= 0 {
				q.Set("speed", strconv.Itoa(speed))
			}

			result := runRobotRequest(func() (int, string, error) {
				return bridge.post("/api/text", q)
			})
			return mcp.NewToolResultText(result), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_ai_text_move",
			mcp.WithDescription("Convert text to a direction locally in MCP server, then move"),
			mcp.WithString("text", mcp.Required(), mcp.Description("Natural text")),
			mcp.WithNumber("duration_ms", mcp.Description("Optional duration")),
			mcp.WithNumber("speed", mcp.Description("Optional speed")),
		),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			text, err := request.RequireString("text")
			if err != nil {
				return mcp.NewToolResultText("invalid text: " + err.Error()), nil
			}
			direction := directionFromText(text)
			if direction == "" {
				return mcp.NewToolResultText("no direction found in text"), nil
			}

			args := request.GetArguments()
			q := url.Values{}
			q.Set("direction", direction)
			if duration, ok := intArg(args, "duration_ms"); ok && duration > 0 {
				q.Set("duration_ms", strconv.Itoa(duration))
			}
			if speed, ok := intArg(args, "speed"); ok && speed >= 0 {
				q.Set("speed", strconv.Itoa(speed))
			}

			status, body, reqErr := bridge.post("/api/move", q)
			if reqErr != nil {
				return mcp.NewToolResultText("request failed: " + reqErr.Error()), nil
			}
			return mcp.NewToolResultText(jsonText(map[string]any{
				"text":        text,
				"direction":   direction,
				"http_status": status,
				"body":        body,
			})), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_speed",
			mcp.WithDescription("Set default speed on robot firmware"),
			mcp.WithNumber("speed", mcp.Required(), mcp.Description("0..255")),
		),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			speed, ok := intArg(request.GetArguments(), "speed")
			if !ok {
				return mcp.NewToolResultText("missing speed"), nil
			}
			if speed < 0 || speed > 255 {
				return mcp.NewToolResultText("speed must be 0..255"), nil
			}
			q := url.Values{}
			q.Set("speed", strconv.Itoa(speed))
			result := runRobotRequest(func() (int, string, error) {
				return bridge.post("/api/speed", q)
			})
			return mcp.NewToolResultText(result), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_send_raw",
			mcp.WithDescription("Send raw command string to firmware HTTP raw endpoint"),
			mcp.WithString("command", mcp.Required(), mcp.Description("Raw command, for example FORWARD 800 180")),
		),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			command, err := request.RequireString("command")
			if err != nil {
				return mcp.NewToolResultText("invalid command: " + err.Error()), nil
			}
			q := url.Values{}
			q.Set("command", strings.TrimSpace(command))
			result := runRobotRequest(func() (int, string, error) {
				return bridge.post("/api/raw", q)
			})
			return mcp.NewToolResultText(result), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_stop", mcp.WithDescription("Emergency stop")),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			result := runRobotRequest(func() (int, string, error) {
				return bridge.post("/api/stop", nil)
			})
			return mcp.NewToolResultText(result), nil
		},
	)

	log.Println("AICar MCP server (Go HTTP) started on stdio")
	if err := server.ServeStdio(mcpServer); err != nil {
		log.Fatalf("mcp server stopped: %v", err)
	}
}

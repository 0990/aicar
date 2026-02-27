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

func normalizeExpression(raw string) (string, bool) {
	v := strings.TrimSpace(strings.ToUpper(raw))
	v = strings.ReplaceAll(v, "-", "_")
	switch v {
	case "NEUTRAL", "NORMAL":
		return "NEUTRAL", true
	case "HAPPY":
		return "HAPPY", true
	case "SAD":
		return "SAD", true
	case "ANGRY":
		return "ANGRY", true
	case "SLEEPY":
		return "SLEEPY", true
	case "SURPRISED":
		return "SURPRISED", true
	case "LOOK_LEFT":
		return "LOOK_LEFT", true
	case "LOOK_RIGHT":
		return "LOOK_RIGHT", true
	case "WINK_LEFT":
		return "WINK_LEFT", true
	case "WINK_RIGHT":
		return "WINK_RIGHT", true
	case "BLINK":
		return "BLINK", true
	default:
		return "", false
	}
}

func expressionFromText(text string) string {
	raw := strings.TrimSpace(strings.ToLower(text))
	switch {
	case containsAny(raw, "开心", "高兴", "happy", "smile"):
		return "HAPPY"
	case containsAny(raw, "难过", "伤心", "sad"):
		return "SAD"
	case containsAny(raw, "生气", "愤怒", "angry"):
		return "ANGRY"
	case containsAny(raw, "困", "累", "sleepy", "tired"):
		return "SLEEPY"
	case containsAny(raw, "惊讶", "惊喜", "surprise", "wow"):
		return "SURPRISED"
	case containsAny(raw, "看左", "向左看", "look left"):
		return "LOOK_LEFT"
	case containsAny(raw, "看右", "向右看", "look right"):
		return "LOOK_RIGHT"
	case containsAny(raw, "左眨眼", "wink left"):
		return "WINK_LEFT"
	case containsAny(raw, "右眨眼", "wink right"):
		return "WINK_RIGHT"
	case containsAny(raw, "眨眼", "blink"):
		return "BLINK"
	case containsAny(raw, "平静", "normal", "neutral"):
		return "NEUTRAL"
	default:
		return ""
	}
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

func jsonText(v any) string {
	bytes, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		return fmt.Sprintf("{\"error\":\"%s\"}", err.Error())
	}
	return string(bytes)
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

func maybeStringArg(args map[string]any, key string) string {
	v, ok := args[key]
	if !ok {
		return ""
	}
	s, ok := v.(string)
	if !ok {
		return ""
	}
	return strings.TrimSpace(s)
}

func addExpressionParamArgs(q url.Values, args map[string]any) (bool, error) {
	changed := false

	addRange := func(argName string, queryName string, min int, max int) error {
		if v, ok := intArg(args, argName); ok {
			if v < min || v > max {
				return fmt.Errorf("%s out of range (%d..%d)", argName, min, max)
			}
			q.Set(queryName, strconv.Itoa(v))
			changed = true
		}
		return nil
	}

	if err := addRange("openness", "openness", 0, 100); err != nil {
		return false, err
	}
	if err := addRange("gaze_x", "gaze_x", -10, 10); err != nil {
		return false, err
	}
	if err := addRange("gaze_y", "gaze_y", -8, 8); err != nil {
		return false, err
	}
	if err := addRange("brow_tilt", "brow_tilt", -35, 35); err != nil {
		return false, err
	}
	if err := addRange("brow_lift", "brow_lift", -12, 12); err != nil {
		return false, err
	}
	if err := addRange("pupil", "pupil", 1, 8); err != nil {
		return false, err
	}
	if err := addRange("left_open", "left_open", 0, 100); err != nil {
		return false, err
	}
	if err := addRange("right_open", "right_open", 0, 100); err != nil {
		return false, err
	}
	if v, ok := intArg(args, "auto_blink"); ok {
		if v != 0 && v != 1 {
			return false, fmt.Errorf("auto_blink must be 0 or 1")
		}
		q.Set("auto_blink", strconv.Itoa(v))
		changed = true
	}
	return changed, nil
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
		"0.3.0",
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
			token := maybeStringArg(request.GetArguments(), "token")
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
		mcp.NewTool("robot_state", mcp.WithDescription("Read robot current state")),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			result := runRobotRequest(func() (int, string, error) {
				return bridge.get("/api/state", nil)
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
			mcp.WithDescription("Execute movement via HTTP, optionally with expression or parametric face"),
			mcp.WithString("direction", mcp.Required(), mcp.Description("FORWARD/BACKWARD/LEFT/RIGHT/STOP")),
			mcp.WithNumber("duration_ms", mcp.Description("Optional duration in milliseconds")),
			mcp.WithNumber("speed", mcp.Description("Optional speed 0..255")),
			mcp.WithString("expression", mcp.Description("Optional expression, e.g. HAPPY")),
			mcp.WithNumber("expression_hold_ms", mcp.Description("Optional expression hold duration")),
			mcp.WithNumber("openness", mcp.Description("0..100, parametric expression")),
			mcp.WithNumber("gaze_x", mcp.Description("-10..10, parametric expression")),
			mcp.WithNumber("gaze_y", mcp.Description("-8..8, parametric expression")),
			mcp.WithNumber("brow_tilt", mcp.Description("-35..35, parametric expression")),
			mcp.WithNumber("brow_lift", mcp.Description("-12..12, parametric expression")),
			mcp.WithNumber("pupil", mcp.Description("1..8, parametric expression")),
			mcp.WithNumber("left_open", mcp.Description("0..100, parametric expression")),
			mcp.WithNumber("right_open", mcp.Description("0..100, parametric expression")),
			mcp.WithNumber("auto_blink", mcp.Description("0 or 1, parametric expression")),
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
			if raw := maybeStringArg(args, "expression"); raw != "" {
				expr, ok := normalizeExpression(raw)
				if !ok {
					return mcp.NewToolResultText("invalid expression"), nil
				}
				q.Set("expression", expr)
			}
			if hold, ok := intArg(args, "expression_hold_ms"); ok && hold > 0 {
				q.Set("expression_hold_ms", strconv.Itoa(hold))
			}
			if _, err := addExpressionParamArgs(q, args); err != nil {
				return mcp.NewToolResultText("invalid expression params: " + err.Error()), nil
			}

			result := runRobotRequest(func() (int, string, error) {
				return bridge.post("/api/move", q)
			})
			return mcp.NewToolResultText(result), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_expression",
			mcp.WithDescription("Set OLED eye expression"),
			mcp.WithString("name", mcp.Required(), mcp.Description("Expression: NEUTRAL/HAPPY/SAD/ANGRY/SLEEPY/SURPRISED/LOOK_LEFT/LOOK_RIGHT/WINK_LEFT/WINK_RIGHT/BLINK")),
			mcp.WithNumber("hold_ms", mcp.Description("Optional hold duration before auto-revert")),
		),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			name, err := request.RequireString("name")
			if err != nil {
				return mcp.NewToolResultText("invalid name: " + err.Error()), nil
			}
			expr, ok := normalizeExpression(name)
			if !ok {
				return mcp.NewToolResultText("invalid expression"), nil
			}
			q := url.Values{}
			q.Set("name", expr)
			if hold, ok := intArg(request.GetArguments(), "hold_ms"); ok && hold > 0 {
				q.Set("hold_ms", strconv.Itoa(hold))
			}
			result := runRobotRequest(func() (int, string, error) {
				return bridge.post("/api/expression", q)
			})
			return mcp.NewToolResultText(result), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_expression_param",
			mcp.WithDescription("Set AI-generated parametric eye expression"),
			mcp.WithNumber("openness", mcp.Description("0..100")),
			mcp.WithNumber("gaze_x", mcp.Description("-10..10")),
			mcp.WithNumber("gaze_y", mcp.Description("-8..8")),
			mcp.WithNumber("brow_tilt", mcp.Description("-35..35")),
			mcp.WithNumber("brow_lift", mcp.Description("-12..12")),
			mcp.WithNumber("pupil", mcp.Description("1..8")),
			mcp.WithNumber("left_open", mcp.Description("0..100")),
			mcp.WithNumber("right_open", mcp.Description("0..100")),
			mcp.WithNumber("auto_blink", mcp.Description("0 or 1")),
			mcp.WithNumber("hold_ms", mcp.Description("Optional hold duration before auto-revert")),
		),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			args := request.GetArguments()
			q := url.Values{}
			changed, err := addExpressionParamArgs(q, args)
			if err != nil {
				return mcp.NewToolResultText("invalid expression params: " + err.Error()), nil
			}
			if !changed {
				return mcp.NewToolResultText("no param supplied"), nil
			}
			if hold, ok := intArg(args, "hold_ms"); ok && hold > 0 {
				q.Set("hold_ms", strconv.Itoa(hold))
			}
			result := runRobotRequest(func() (int, string, error) {
				return bridge.post("/api/expression/param", q)
			})
			return mcp.NewToolResultText(result), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_text_control",
			mcp.WithDescription("Send text to robot /api/text for motion+expression parsing"),
			mcp.WithString("text", mcp.Required(), mcp.Description("Natural text, for example 向左转并且开心")),
			mcp.WithNumber("duration_ms", mcp.Description("Optional override duration")),
			mcp.WithNumber("speed", mcp.Description("Optional override speed")),
			mcp.WithString("expression", mcp.Description("Optional explicit expression override")),
			mcp.WithNumber("expression_hold_ms", mcp.Description("Optional expression hold duration")),
			mcp.WithNumber("openness", mcp.Description("0..100, parametric expression")),
			mcp.WithNumber("gaze_x", mcp.Description("-10..10, parametric expression")),
			mcp.WithNumber("gaze_y", mcp.Description("-8..8, parametric expression")),
			mcp.WithNumber("brow_tilt", mcp.Description("-35..35, parametric expression")),
			mcp.WithNumber("brow_lift", mcp.Description("-12..12, parametric expression")),
			mcp.WithNumber("pupil", mcp.Description("1..8, parametric expression")),
			mcp.WithNumber("left_open", mcp.Description("0..100, parametric expression")),
			mcp.WithNumber("right_open", mcp.Description("0..100, parametric expression")),
			mcp.WithNumber("auto_blink", mcp.Description("0 or 1, parametric expression")),
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
			if raw := maybeStringArg(args, "expression"); raw != "" {
				expr, ok := normalizeExpression(raw)
				if !ok {
					return mcp.NewToolResultText("invalid expression"), nil
				}
				q.Set("expression", expr)
			}
			if hold, ok := intArg(args, "expression_hold_ms"); ok && hold > 0 {
				q.Set("expression_hold_ms", strconv.Itoa(hold))
			}
			if _, err := addExpressionParamArgs(q, args); err != nil {
				return mcp.NewToolResultText("invalid expression params: " + err.Error()), nil
			}
			result := runRobotRequest(func() (int, string, error) {
				return bridge.post("/api/text", q)
			})
			return mcp.NewToolResultText(result), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_ai_behavior",
			mcp.WithDescription("Parse natural text in MCP and drive movement + expression"),
			mcp.WithString("text", mcp.Required(), mcp.Description("Natural language behavior request")),
			mcp.WithNumber("duration_ms", mcp.Description("Optional movement duration")),
			mcp.WithNumber("speed", mcp.Description("Optional movement speed")),
			mcp.WithNumber("expression_hold_ms", mcp.Description("Optional expression hold duration")),
		),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			text, err := request.RequireString("text")
			if err != nil {
				return mcp.NewToolResultText("invalid text: " + err.Error()), nil
			}

			direction := directionFromText(text)
			expression := expressionFromText(text)
			if direction == "" && expression == "" {
				return mcp.NewToolResultText("no movement or expression found in text"), nil
			}

			args := request.GetArguments()
			hold, _ := intArg(args, "expression_hold_ms")

			if direction != "" {
				q := url.Values{}
				q.Set("direction", direction)
				if duration, ok := intArg(args, "duration_ms"); ok && duration > 0 {
					q.Set("duration_ms", strconv.Itoa(duration))
				}
				if speed, ok := intArg(args, "speed"); ok && speed >= 0 {
					q.Set("speed", strconv.Itoa(speed))
				}
				if expression != "" {
					q.Set("expression", expression)
					if hold > 0 {
						q.Set("expression_hold_ms", strconv.Itoa(hold))
					}
				}

				status, body, reqErr := bridge.post("/api/move", q)
				if reqErr != nil {
					return mcp.NewToolResultText("request failed: " + reqErr.Error()), nil
				}
				return mcp.NewToolResultText(jsonText(map[string]any{
					"text":        text,
					"direction":   direction,
					"expression":  expression,
					"http_status": status,
					"body":        body,
				})), nil
			}

			q := url.Values{}
			q.Set("name", expression)
			if hold > 0 {
				q.Set("hold_ms", strconv.Itoa(hold))
			}
			status, body, reqErr := bridge.post("/api/expression", q)
			if reqErr != nil {
				return mcp.NewToolResultText("request failed: " + reqErr.Error()), nil
			}
			return mcp.NewToolResultText(jsonText(map[string]any{
				"text":        text,
				"expression":  expression,
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
			mcp.WithDescription("Send raw command string to firmware /api/raw endpoint"),
			mcp.WithString("command", mcp.Required(), mcp.Description("Raw command, e.g. FORWARD 800 180 or EXPR HAPPY")),
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

	log.Println("AICar MCP server (Go HTTP + expression) started on stdio")
	if err := server.ServeStdio(mcpServer); err != nil {
		log.Fatalf("mcp server stopped: %v", err)
	}
}

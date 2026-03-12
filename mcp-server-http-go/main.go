package main

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/mark3labs/mcp-go/mcp"
	"github.com/mark3labs/mcp-go/server"
)

const (
	defaultCmdTimeout = 8 * time.Second
	defaultServerName = "aicar-robot-mcp-http"
	defaultServerVer  = "0.2.0"
	defaultHTTPAddr   = ":8082"
	defaultHTTPEndp   = "/mcp"
)

type httpTarget struct {
	BaseURL string
	Token   string
}

type httpBridge struct {
	target httpTarget
	client *http.Client
}

func randomID() string {
	buf := make([]byte, 8)
	if _, err := rand.Read(buf); err != nil {
		return strconv.FormatInt(time.Now().UnixNano(), 16)
	}
	return hex.EncodeToString(buf)
}

func envOrDefault(key, defaultValue string) string {
	value := strings.TrimSpace(os.Getenv(key))
	if value == "" {
		return defaultValue
	}
	return value
}

func normalizeEndpointPath(raw string) string {
	path := strings.TrimSpace(raw)
	if path == "" {
		return defaultHTTPEndp
	}
	return "/" + strings.Trim(path, "/")
}

func withBearerAuth(next http.Handler, bearerToken string) http.Handler {
	token := strings.TrimSpace(bearerToken)
	if token == "" {
		return next
	}
	expectedBearer := "Bearer " + token
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		auth := strings.TrimSpace(r.Header.Get("Authorization"))
		if auth == token || auth == expectedBearer {
			next.ServeHTTP(w, r)
			return
		}
		http.Error(w, "unauthorized", http.StatusUnauthorized)
	})
}

func normalizeBaseURL(raw string) (string, error) {
	value := strings.TrimSpace(raw)
	if value == "" {
		return "", fmt.Errorf("base_url is empty")
	}
	if !strings.Contains(value, "://") {
		value = "http://" + value
	}
	parsed, err := url.Parse(value)
	if err != nil {
		return "", fmt.Errorf("invalid base_url: %w", err)
	}
	if parsed.Scheme != "http" && parsed.Scheme != "https" {
		return "", fmt.Errorf("base_url scheme must be http or https")
	}
	if strings.TrimSpace(parsed.Host) == "" {
		return "", fmt.Errorf("base_url host is empty")
	}
	parsed.Path = strings.TrimRight(parsed.Path, "/")
	parsed.RawQuery = ""
	parsed.Fragment = ""
	return strings.TrimRight(parsed.String(), "/"), nil
}

func loadTarget() (httpTarget, error) {
	if baseURL := strings.TrimSpace(os.Getenv("ROBOT_HTTP_BASE_URL")); baseURL != "" {
		normalized, err := normalizeBaseURL(baseURL)
		if err != nil {
			return httpTarget{}, err
		}
		return httpTarget{
			BaseURL: normalized,
			Token:   strings.TrimSpace(os.Getenv("ROBOT_HTTP_TOKEN")),
		}, nil
	}
	return httpTarget{}, fmt.Errorf("no HTTP target configured, set ROBOT_HTTP_BASE_URL")
}

func newHTTPBridge(timeout time.Duration) (*httpBridge, error) {
	target, err := loadTarget()
	if err != nil {
		return nil, err
	}
	if timeout <= 0 {
		timeout = defaultCmdTimeout
	}
	return &httpBridge{
		target: target,
		client: &http.Client{Timeout: timeout},
	}, nil
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

func boolArg(args map[string]any, key string) (bool, bool) {
	v, ok := args[key]
	if !ok {
		return false, false
	}
	switch t := v.(type) {
	case bool:
		return t, true
	case float64:
		if t == 0 {
			return false, true
		}
		if t == 1 {
			return true, true
		}
	case int:
		if t == 0 {
			return false, true
		}
		if t == 1 {
			return true, true
		}
	case string:
		switch strings.ToLower(strings.TrimSpace(t)) {
		case "1", "true", "yes", "on":
			return true, true
		case "0", "false", "no", "off":
			return false, true
		}
	}
	return false, false
}

func jsonText(v any) string {
	bytes, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		return fmt.Sprintf("{\"error\":\"%s\"}", err.Error())
	}
	return string(bytes)
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
	case "CONFUSED":
		return "CONFUSED", true
	case "LAUGH":
		return "LAUGH", true
	default:
		return "", false
	}
}

func normalizeWheelDirection(raw string) (string, bool) {
	v := strings.ToUpper(strings.TrimSpace(raw))
	switch v {
	case "FORWARD", "BACKWARD":
		return v, true
	default:
		return "", false
	}
}

func addExpressionArgs(cmd map[string]any, args map[string]any) (bool, error) {
	changed := false

	setBool01 := func(name string) error {
		if v, ok := intArg(args, name); ok {
			if v != 0 && v != 1 {
				return fmt.Errorf("%s must be 0 or 1", name)
			}
			cmd[name] = v
			changed = true
		}
		return nil
	}
	setRange := func(name string, min int, max int) error {
		if v, ok := intArg(args, name); ok {
			if v < min || v > max {
				return fmt.Errorf("%s out of range (%d..%d)", name, min, max)
			}
			cmd[name] = v
			changed = true
		}
		return nil
	}

	if mood := maybeStringArg(args, "mood"); mood != "" {
		mood = strings.ToUpper(mood)
		switch mood {
		case "DEFAULT", "NEUTRAL", "NORMAL", "TIRED", "SLEEPY", "ANGRY", "HAPPY":
			cmd["mood"] = mood
			changed = true
		default:
			return false, fmt.Errorf("invalid mood")
		}
	}
	if pos := maybeStringArg(args, "position"); pos != "" {
		pos = strings.ToUpper(pos)
		switch pos {
		case "DEFAULT", "CENTER", "C", "N", "NE", "E", "SE", "S", "SW", "W", "NW":
			cmd["position"] = pos
			changed = true
		default:
			return false, fmt.Errorf("invalid position")
		}
	}
	if action := maybeStringArg(args, "action"); action != "" {
		action = strings.ToUpper(strings.ReplaceAll(action, "-", "_"))
		switch action {
		case "NONE", "BLINK", "WINK_LEFT", "WINK_RIGHT", "CONFUSED", "LAUGH", "OPEN", "CLOSE":
			cmd["action"] = action
			changed = true
		default:
			return false, fmt.Errorf("invalid action")
		}
	}

	if err := setBool01("curiosity"); err != nil {
		return false, err
	}
	if err := setBool01("sweat"); err != nil {
		return false, err
	}
	if err := setBool01("cyclops"); err != nil {
		return false, err
	}
	if err := setBool01("auto_blink"); err != nil {
		return false, err
	}
	if err := setBool01("idle"); err != nil {
		return false, err
	}

	if err := setRange("auto_blink_interval", 1, 30); err != nil {
		return false, err
	}
	if err := setRange("auto_blink_variation", 0, 30); err != nil {
		return false, err
	}
	if err := setRange("idle_interval", 1, 30); err != nil {
		return false, err
	}
	if err := setRange("idle_variation", 0, 30); err != nil {
		return false, err
	}
	if err := setRange("hflicker_amp", 0, 30); err != nil {
		return false, err
	}
	if err := setRange("vflicker_amp", 0, 30); err != nil {
		return false, err
	}

	return changed, nil
}

func normalizeBuzzerPriority(raw string) (string, bool) {
	v := strings.ToUpper(strings.TrimSpace(raw))
	switch v {
	case "LOW", "NORMAL", "HIGH", "ALARM":
		return v, true
	default:
		return "", false
	}
}

func normalizeBuzzerCommandArgs(args map[string]any) ([]map[string]any, int, bool, string, error) {
	rawPattern, ok := args["pattern"]
	if !ok {
		return nil, 0, false, "", fmt.Errorf("pattern is required")
	}

	items, ok := rawPattern.([]any)
	if !ok || len(items) == 0 || len(items) > 16 {
		return nil, 0, false, "", fmt.Errorf("pattern must contain 1..16 steps")
	}

	pattern := make([]map[string]any, 0, len(items))
	for index, item := range items {
		stepMap, ok := item.(map[string]any)
		if !ok {
			return nil, 0, false, "", fmt.Errorf("pattern[%d] must be an object", index)
		}
		freq, ok := intArg(stepMap, "freq")
		if !ok || freq < 0 || freq > 5000 {
			return nil, 0, false, "", fmt.Errorf("pattern[%d].freq must be 0..5000", index)
		}
		durationMs, ok := intArg(stepMap, "duration_ms")
		if !ok || durationMs < 1 || durationMs > 5000 {
			return nil, 0, false, "", fmt.Errorf("pattern[%d].duration_ms must be 1..5000", index)
		}
		pattern = append(pattern, map[string]any{
			"freq":        freq,
			"duration_ms": durationMs,
		})
	}

	repeat := 1
	if value, ok := intArg(args, "repeat"); ok {
		if value < 1 || value > 10 {
			return nil, 0, false, "", fmt.Errorf("repeat must be 1..10")
		}
		repeat = value
	}

	interrupt := true
	if value, ok := boolArg(args, "interrupt"); ok {
		interrupt = value
	} else if _, exists := args["interrupt"]; exists {
		return nil, 0, false, "", fmt.Errorf("interrupt must be boolean")
	}

	priority := "NORMAL"
	if raw := maybeStringArg(args, "priority"); raw != "" {
		normalized, ok := normalizeBuzzerPriority(raw)
		if !ok {
			return nil, 0, false, "", fmt.Errorf("priority must be LOW/NORMAL/HIGH/ALARM")
		}
		priority = normalized
	}

	return pattern, repeat, interrupt, priority, nil
}

func addQueryParam(query url.Values, key string, value any) {
	switch v := value.(type) {
	case nil:
		return
	case string:
		if strings.TrimSpace(v) == "" {
			return
		}
		query.Set(key, v)
	case bool:
		query.Set(key, strconv.FormatBool(v))
	case int:
		query.Set(key, strconv.Itoa(v))
	case int64:
		query.Set(key, strconv.FormatInt(v, 10))
	case float64:
		query.Set(key, strconv.FormatFloat(v, 'f', -1, 64))
	default:
		query.Set(key, fmt.Sprint(v))
	}
}

func decodeBody(body []byte) any {
	trimmed := strings.TrimSpace(string(body))
	if trimmed == "" {
		return nil
	}
	var parsed any
	if err := json.Unmarshal(body, &parsed); err == nil {
		return parsed
	}
	return trimmed
}

func extractResultError(result any) string {
	m, ok := result.(map[string]any)
	if !ok {
		return ""
	}
	if raw, ok := m["error"]; ok {
		if message, ok := raw.(string); ok {
			return strings.TrimSpace(message)
		}
	}
	if raw, ok := m["message"]; ok {
		if message, ok := raw.(string); ok {
			return strings.TrimSpace(message)
		}
	}
	return ""
}

func (b *httpBridge) request(method, endpoint string, params map[string]any) (int, any, error) {
	reqURL, err := url.Parse(b.target.BaseURL + endpoint)
	if err != nil {
		return 0, nil, err
	}
	query := reqURL.Query()
	for key, value := range params {
		addQueryParam(query, key, value)
	}
	reqURL.RawQuery = query.Encode()

	req, err := http.NewRequest(method, reqURL.String(), nil)
	if err != nil {
		return 0, nil, err
	}
	req.Header.Set("Accept", "application/json")
	if b.target.Token != "" {
		req.Header.Set("X-Robot-Token", b.target.Token)
	}

	resp, err := b.client.Do(req)
	if err != nil {
		return 0, nil, err
	}
	defer resp.Body.Close()

	body, err := io.ReadAll(resp.Body)
	if err != nil {
		return resp.StatusCode, nil, err
	}
	result := decodeBody(body)

	if resp.StatusCode >= http.StatusBadRequest {
		if message := extractResultError(result); message != "" {
			return resp.StatusCode, result, fmt.Errorf(message)
		}
		return resp.StatusCode, result, fmt.Errorf("http status %d", resp.StatusCode)
	}

	if m, ok := result.(map[string]any); ok {
		if okRaw, exists := m["ok"]; exists {
			if okValue, ok := okRaw.(bool); ok && !okValue {
				if message := extractResultError(result); message != "" {
					return resp.StatusCode, result, fmt.Errorf(message)
				}
				return resp.StatusCode, result, fmt.Errorf("robot returned ok=false")
			}
		}
	}

	return resp.StatusCode, result, nil
}

func callRobotEndpoint(bridge *httpBridge, method, endpoint string, params map[string]any) string {
	statusCode, result, err := bridge.request(method, endpoint, params)
	if err != nil {
		return jsonText(map[string]any{
			"base_url":    bridge.target.BaseURL,
			"endpoint":    endpoint,
			"status_code": statusCode,
			"ok":          false,
			"error":       err.Error(),
			"result":      result,
		})
	}

	return jsonText(map[string]any{
		"base_url":    bridge.target.BaseURL,
		"endpoint":    endpoint,
		"status_code": statusCode,
		"ok":          true,
		"result":      result,
	})
}

func main() {
	bridge, err := newHTTPBridge(defaultCmdTimeout)
	if err != nil {
		log.Fatalf("http bridge init failed: %v", err)
	}

	mcpServer := server.NewMCPServer(
		defaultServerName,
		defaultServerVer,
		server.WithToolCapabilities(true),
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_health",
			mcp.WithDescription("Query robot health via HTTP"),
		),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			return mcp.NewToolResultText(callRobotEndpoint(bridge, http.MethodGet, "/health", nil)), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_state",
			mcp.WithDescription("Query robot state via HTTP"),
		),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			return mcp.NewToolResultText(callRobotEndpoint(bridge, http.MethodGet, "/api/state", nil)), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_ping",
			mcp.WithDescription("Ping robot via HTTP"),
		),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			return mcp.NewToolResultText(callRobotEndpoint(bridge, http.MethodGet, "/ping", nil)), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_set_wheels",
			mcp.WithDescription("Control left and right wheels independently. Use FORWARD/BACKWARD and speed 0..100. Speed 0 means the wheel stops turning. Example for moving forward: left_direction=FORWARD left_speed=100 right_direction=FORWARD right_speed=100."),
			mcp.WithString("left_direction", mcp.Required(), mcp.Description("FORWARD or BACKWARD")),
			mcp.WithNumber("left_speed", mcp.Required(), mcp.Description("0..100, 0 means stop")),
			mcp.WithString("right_direction", mcp.Required(), mcp.Description("FORWARD or BACKWARD")),
			mcp.WithNumber("right_speed", mcp.Required(), mcp.Description("0..100, 0 means stop")),
		),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			leftDirectionRaw, err := request.RequireString("left_direction")
			if err != nil {
				return mcp.NewToolResultText("invalid left_direction: " + err.Error()), nil
			}
			rightDirectionRaw, err := request.RequireString("right_direction")
			if err != nil {
				return mcp.NewToolResultText("invalid right_direction: " + err.Error()), nil
			}

			args := request.GetArguments()
			leftDirection, ok := normalizeWheelDirection(leftDirectionRaw)
			if !ok {
				return mcp.NewToolResultText("left_direction must be FORWARD/BACKWARD"), nil
			}
			rightDirection, ok := normalizeWheelDirection(rightDirectionRaw)
			if !ok {
				return mcp.NewToolResultText("right_direction must be FORWARD/BACKWARD"), nil
			}
			leftSpeed, ok := intArg(args, "left_speed")
			if !ok || leftSpeed < 0 || leftSpeed > 100 {
				return mcp.NewToolResultText("left_speed must be 0..100"), nil
			}
			rightSpeed, ok := intArg(args, "right_speed")
			if !ok || rightSpeed < 0 || rightSpeed > 100 {
				return mcp.NewToolResultText("right_speed must be 0..100"), nil
			}

			cmd := map[string]any{
				"left_direction":  leftDirection,
				"left_speed":      leftSpeed,
				"right_direction": rightDirection,
				"right_speed":     rightSpeed,
			}
			return mcp.NewToolResultText(callRobotEndpoint(bridge, http.MethodPost, "/api/wheels", cmd)), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_expression",
			mcp.WithDescription("Set robot expression via HTTP"),
			mcp.WithString("name", mcp.Required(), mcp.Description("NEUTRAL/HAPPY/SAD/ANGRY/SLEEPY/SURPRISED/LOOK_LEFT/LOOK_RIGHT/WINK_LEFT/WINK_RIGHT/BLINK/CONFUSED/LAUGH")),
			mcp.WithNumber("hold_ms", mcp.Description("Optional hold duration")),
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
			args := request.GetArguments()
			cmd := map[string]any{"name": expr}
			if hold, ok := intArg(args, "hold_ms"); ok && hold > 0 {
				cmd["hold_ms"] = hold
			}
			return mcp.NewToolResultText(callRobotEndpoint(bridge, http.MethodPost, "/api/expression", cmd)), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_expression_param",
			mcp.WithDescription("Set RoboEyes style/action params via HTTP"),
			mcp.WithString("mood", mcp.Description("DEFAULT/TIRED/ANGRY/HAPPY")),
			mcp.WithString("position", mcp.Description("DEFAULT/N/NE/E/SE/S/SW/W/NW")),
			mcp.WithNumber("curiosity", mcp.Description("0 or 1")),
			mcp.WithNumber("sweat", mcp.Description("0 or 1")),
			mcp.WithNumber("cyclops", mcp.Description("0 or 1")),
			mcp.WithNumber("auto_blink", mcp.Description("0 or 1")),
			mcp.WithNumber("auto_blink_interval", mcp.Description("1..30")),
			mcp.WithNumber("auto_blink_variation", mcp.Description("0..30")),
			mcp.WithNumber("idle", mcp.Description("0 or 1")),
			mcp.WithNumber("idle_interval", mcp.Description("1..30")),
			mcp.WithNumber("idle_variation", mcp.Description("0..30")),
			mcp.WithNumber("hflicker_amp", mcp.Description("0..30")),
			mcp.WithNumber("vflicker_amp", mcp.Description("0..30")),
			mcp.WithString("action", mcp.Description("NONE/BLINK/WINK_LEFT/WINK_RIGHT/CONFUSED/LAUGH/OPEN/CLOSE")),
			mcp.WithNumber("hold_ms", mcp.Description("Optional hold duration")),
		),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			args := request.GetArguments()
			cmd := map[string]any{}
			changed, err := addExpressionArgs(cmd, args)
			if err != nil {
				return mcp.NewToolResultText("invalid expression params: " + err.Error()), nil
			}
			if !changed {
				return mcp.NewToolResultText("no expression param supplied"), nil
			}
			if hold, ok := intArg(args, "hold_ms"); ok && hold > 0 {
				cmd["hold_ms"] = hold
			}
			return mcp.NewToolResultText(callRobotEndpoint(bridge, http.MethodPost, "/api/expression/param", cmd)), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_buzzer",
			mcp.WithDescription("Play a robot-style buzzer pattern. Prefer short electronic chirps instead of long melodies: usually 3..6 steps, each 40..180 ms, with small pitch jumps and optional pauses using freq=0. pattern is an ordered array of {freq, duration_ms}; freq is in Hz and 0 means silence/pause. repeat replays the full pattern, interrupt controls whether the current sound can be cut off, and priority can be LOW/NORMAL/HIGH/ALARM so alert sounds are not replaced by ordinary prompts."),
			mcp.WithArray("pattern",
				mcp.Required(),
				mcp.Description("Tone steps to play in order"),
				mcp.MinItems(1),
				mcp.MaxItems(16),
				mcp.Items(map[string]any{
					"type": "object",
					"properties": map[string]any{
						"freq": map[string]any{
							"type":        "number",
							"description": "0..5000 Hz, 0 means silence/pause",
							"minimum":     0,
							"maximum":     5000,
						},
						"duration_ms": map[string]any{
							"type":        "number",
							"description": "1..5000 ms",
							"minimum":     1,
							"maximum":     5000,
						},
					},
					"required":             []string{"freq", "duration_ms"},
					"additionalProperties": false,
				}),
			),
			mcp.WithNumber("repeat", mcp.Description("Optional repeat count, 1..10")),
			mcp.WithBoolean("interrupt", mcp.Description("Whether to interrupt the currently playing sound")),
			mcp.WithString("priority", mcp.Description("LOW/NORMAL/HIGH/ALARM")),
		),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			args := request.GetArguments()
			pattern, repeat, interrupt, priority, err := normalizeBuzzerCommandArgs(args)
			if err != nil {
				return mcp.NewToolResultText("invalid buzzer command: " + err.Error()), nil
			}
			patternJSON, err := json.Marshal(pattern)
			if err != nil {
				return mcp.NewToolResultText("invalid buzzer pattern"), nil
			}
			cmd := map[string]any{
				"pattern":   string(patternJSON),
				"repeat":    repeat,
				"interrupt": interrupt,
				"priority":  priority,
			}
			return mcp.NewToolResultText(callRobotEndpoint(bridge, http.MethodPost, "/api/buzzer", cmd)), nil
		},
	)

	transport := strings.ToLower(envOrDefault("MCP_TRANSPORT", "http"))
	switch transport {
	case "stdio":
		log.Printf("AICar MCP server (HTTP direct + stdio) started, base_url=%s", bridge.target.BaseURL)
		if err := server.ServeStdio(mcpServer); err != nil {
			log.Fatalf("mcp server stopped: %v", err)
		}
	case "http", "streamable-http", "streamable_http":
		addr := envOrDefault("MCP_HTTP_ADDR", defaultHTTPAddr)
		endpointPath := normalizeEndpointPath(envOrDefault("MCP_HTTP_ENDPOINT", defaultHTTPEndp))
		bearerToken := strings.TrimSpace(os.Getenv("MCP_HTTP_BEARER_TOKEN"))

		streamable := server.NewStreamableHTTPServer(mcpServer)
		mux := http.NewServeMux()
		mux.Handle(endpointPath, withBearerAuth(streamable, bearerToken))
		mux.HandleFunc("/healthz", func(w http.ResponseWriter, _ *http.Request) {
			w.Header().Set("Content-Type", "application/json")
			_, _ = w.Write([]byte(`{"ok":true}`))
		})

		httpSrv := &http.Server{
			Addr:              addr,
			Handler:           mux,
			ReadHeaderTimeout: 10 * time.Second,
		}

		authEnabled := bearerToken != ""
		log.Printf("AICar MCP server (HTTP direct + streamable-http) started, base_url=%s, listen=%s, endpoint=%s, bearer_auth=%t",
			bridge.target.BaseURL, addr, endpointPath, authEnabled)
		if err := httpSrv.ListenAndServe(); err != nil {
			log.Fatalf("http server stopped: %v", err)
		}
	default:
		log.Fatalf("unsupported MCP_TRANSPORT=%q, expected http or stdio", transport)
	}
}

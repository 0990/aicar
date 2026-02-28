package main

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
	"github.com/mark3labs/mcp-go/mcp"
	"github.com/mark3labs/mcp-go/server"
)

const (
	defaultMQTTBroker  = "tcp://broker.emqx.io:1883"
	defaultTopicPrefix = "aicar"
	defaultCmdTimeout  = 8 * time.Second
	defaultConnectWait = 8 * time.Second
	defaultStaleAfter  = 20 * time.Second
	defaultServerName  = "aicar-robot-mcp-mqtt"
	defaultServerVer   = "0.4.0"
	defaultHTTPAddr    = ":8080"
	defaultHTTPEndp    = "/mcp"
)

type commandEnvelope struct {
	ReqID string         `json:"req_id"`
	Type  string         `json:"type"`
	Args  map[string]any `json:"args,omitempty"`
}

type commandReply struct {
	RobotID string          `json:"robot_id"`
	ReqID   string          `json:"req_id"`
	OK      bool            `json:"ok"`
	Error   string          `json:"error,omitempty"`
	Result  json.RawMessage `json:"result,omitempty"`
}

type robotRuntime struct {
	ID       string         `json:"id"`
	Online   bool           `json:"online"`
	LastSeen time.Time      `json:"last_seen"`
	Register map[string]any `json:"register,omitempty"`
	Status   map[string]any `json:"status,omitempty"`
}

type mqttBridge struct {
	mu sync.RWMutex

	broker      string
	username    string
	password    string
	topicPrefix string

	client         mqtt.Client
	commandTimeout time.Duration
	staleAfter     time.Duration

	pending map[string]chan commandReply
	robots  map[string]*robotRuntime
	active  string
}

func normalizeMQTTBroker(raw string) string {
	v := strings.TrimSpace(raw)
	if v == "" {
		return defaultMQTTBroker
	}
	if strings.HasPrefix(v, "tcp://") || strings.HasPrefix(v, "mqtt://") || strings.HasPrefix(v, "ssl://") || strings.HasPrefix(v, "ws://") || strings.HasPrefix(v, "wss://") {
		if strings.HasPrefix(v, "mqtt://") {
			return "tcp://" + strings.TrimPrefix(v, "mqtt://")
		}
		return v
	}
	return "tcp://" + v
}

func newMQTTBridge(broker, username, password, topicPrefix string, commandTimeout time.Duration) (*mqttBridge, error) {
	if strings.TrimSpace(topicPrefix) == "" {
		topicPrefix = defaultTopicPrefix
	}
	if commandTimeout <= 0 {
		commandTimeout = defaultCmdTimeout
	}

	b := &mqttBridge{
		broker:         normalizeMQTTBroker(broker),
		username:       strings.TrimSpace(username),
		password:       password,
		topicPrefix:    strings.Trim(strings.TrimSpace(topicPrefix), "/"),
		commandTimeout: commandTimeout,
		staleAfter:     defaultStaleAfter,
		pending:        make(map[string]chan commandReply),
		robots:         make(map[string]*robotRuntime),
	}

	opts := mqtt.NewClientOptions()
	opts.AddBroker(b.broker)
	if b.username != "" {
		opts.SetUsername(b.username)
		opts.SetPassword(b.password)
	}
	opts.SetClientID("aicar-mcp-" + randomID())
	opts.SetAutoReconnect(true)
	opts.SetConnectRetry(true)
	opts.SetConnectRetryInterval(2 * time.Second)
	opts.SetOnConnectHandler(func(client mqtt.Client) {
		if err := b.subscribeAll(client); err != nil {
			log.Printf("mqtt subscribe failed: %v", err)
		}
	})
	opts.SetConnectionLostHandler(func(_ mqtt.Client, err error) {
		log.Printf("mqtt connection lost: %v", err)
	})

	b.client = mqtt.NewClient(opts)
	token := b.client.Connect()
	if !token.WaitTimeout(defaultConnectWait) {
		return nil, fmt.Errorf("mqtt connect timeout")
	}
	if err := token.Error(); err != nil {
		return nil, err
	}
	return b, nil
}

func (b *mqttBridge) subscribeAll(client mqtt.Client) error {
	registerTopic := fmt.Sprintf("%s/robots/+/register", b.topicPrefix)
	statusTopic := fmt.Sprintf("%s/robots/+/status", b.topicPrefix)
	ackTopic := fmt.Sprintf("%s/robots/+/ack", b.topicPrefix)

	if t := client.Subscribe(registerTopic, 1, b.onRegister); t.Wait() && t.Error() != nil {
		return t.Error()
	}
	if t := client.Subscribe(statusTopic, 1, b.onStatus); t.Wait() && t.Error() != nil {
		return t.Error()
	}
	if t := client.Subscribe(ackTopic, 1, b.onAck); t.Wait() && t.Error() != nil {
		return t.Error()
	}
	return nil
}

func (b *mqttBridge) robotIDFromTopic(topic string) string {
	prefix := b.topicPrefix + "/robots/"
	if !strings.HasPrefix(topic, prefix) {
		return ""
	}
	rest := strings.TrimPrefix(topic, prefix)
	parts := strings.Split(rest, "/")
	if len(parts) < 2 {
		return ""
	}
	return strings.TrimSpace(parts[0])
}

func (b *mqttBridge) upsertRobot(robotID string) *robotRuntime {
	r, ok := b.robots[robotID]
	if !ok {
		r = &robotRuntime{ID: robotID}
		b.robots[robotID] = r
	}
	return r
}

func decodeJSONMap(payload []byte) map[string]any {
	var m map[string]any
	if err := json.Unmarshal(payload, &m); err != nil {
		return map[string]any{"raw": string(payload)}
	}
	return m
}

func (b *mqttBridge) onRegister(_ mqtt.Client, msg mqtt.Message) {
	robotID := b.robotIDFromTopic(msg.Topic())
	if robotID == "" {
		return
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	r := b.upsertRobot(robotID)
	r.Online = true
	r.LastSeen = time.Now()
	r.Register = decodeJSONMap(msg.Payload())
	if b.active == "" {
		b.active = robotID
	}
}

func (b *mqttBridge) onStatus(_ mqtt.Client, msg mqtt.Message) {
	robotID := b.robotIDFromTopic(msg.Topic())
	if robotID == "" {
		return
	}
	status := decodeJSONMap(msg.Payload())
	online := true
	if raw, ok := status["online"]; ok {
		if v, ok := raw.(bool); ok {
			online = v
		}
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	r := b.upsertRobot(robotID)
	r.Online = online
	r.LastSeen = time.Now()
	r.Status = status
	if b.active == "" && online {
		b.active = robotID
	}
}

func (b *mqttBridge) onAck(_ mqtt.Client, msg mqtt.Message) {
	var reply commandReply
	if err := json.Unmarshal(msg.Payload(), &reply); err != nil {
		return
	}
	if reply.ReqID == "" {
		return
	}

	b.mu.Lock()
	defer b.mu.Unlock()
	if reply.RobotID != "" {
		r := b.upsertRobot(reply.RobotID)
		r.Online = true
		r.LastSeen = time.Now()
	}
	if ch, ok := b.pending[reply.ReqID]; ok {
		select {
		case ch <- reply:
		default:
		}
	}
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

func (b *mqttBridge) ensureConnected() error {
	if b.client == nil || !b.client.IsConnected() {
		return fmt.Errorf("mqtt not connected")
	}
	return nil
}

func (b *mqttBridge) resolveRobotID(explicit string) (string, error) {
	explicit = strings.TrimSpace(explicit)
	b.mu.RLock()
	defer b.mu.RUnlock()

	if explicit != "" {
		if _, ok := b.robots[explicit]; ok {
			return explicit, nil
		}
		return "", fmt.Errorf("robot_id not registered")
	}

	if b.active != "" {
		if _, ok := b.robots[b.active]; ok {
			return b.active, nil
		}
	}

	if len(b.robots) == 1 {
		for id := range b.robots {
			return id, nil
		}
	}
	if len(b.robots) == 0 {
		return "", fmt.Errorf("no robot registered yet")
	}
	return "", fmt.Errorf("multiple robots registered, set robot_id or call robot_set_active")
}

func (b *mqttBridge) setActive(robotID string) error {
	robotID = strings.TrimSpace(robotID)
	if robotID == "" {
		return fmt.Errorf("robot_id is empty")
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	if _, ok := b.robots[robotID]; !ok {
		return fmt.Errorf("robot_id not registered")
	}
	b.active = robotID
	return nil
}

func (b *mqttBridge) listRobots() []map[string]any {
	now := time.Now()
	b.mu.RLock()
	defer b.mu.RUnlock()
	items := make([]map[string]any, 0, len(b.robots))
	for _, r := range b.robots {
		online := r.Online
		if now.Sub(r.LastSeen) > b.staleAfter {
			online = false
		}
		items = append(items, map[string]any{
			"id":        r.ID,
			"online":    online,
			"last_seen": r.LastSeen.Format(time.RFC3339),
			"register":  r.Register,
			"status":    r.Status,
		})
	}
	return items
}

func (b *mqttBridge) request(robotID, commandType string, args map[string]any) (commandReply, error) {
	if err := b.ensureConnected(); err != nil {
		return commandReply{}, err
	}
	robotID = strings.TrimSpace(robotID)
	if robotID == "" {
		return commandReply{}, fmt.Errorf("robot_id is empty")
	}

	envelope := commandEnvelope{
		ReqID: randomID(),
		Type:  strings.ToUpper(strings.TrimSpace(commandType)),
		Args:  args,
	}
	body, err := json.Marshal(envelope)
	if err != nil {
		return commandReply{}, err
	}

	respCh := make(chan commandReply, 1)
	b.mu.Lock()
	b.pending[envelope.ReqID] = respCh
	b.mu.Unlock()
	defer func() {
		b.mu.Lock()
		delete(b.pending, envelope.ReqID)
		b.mu.Unlock()
	}()

	topic := fmt.Sprintf("%s/robots/%s/cmd", b.topicPrefix, robotID)
	pub := b.client.Publish(topic, 1, false, body)
	if !pub.WaitTimeout(defaultConnectWait) {
		return commandReply{}, fmt.Errorf("mqtt publish timeout")
	}
	if err := pub.Error(); err != nil {
		return commandReply{}, err
	}

	select {
	case reply := <-respCh:
		if !reply.OK {
			if reply.Error == "" {
				return reply, fmt.Errorf("robot command failed")
			}
			return reply, fmt.Errorf(reply.Error)
		}
		return reply, nil
	case <-time.After(b.commandTimeout):
		return commandReply{}, fmt.Errorf("command timeout waiting ack")
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
	case containsAny(raw, "疑惑", "confused"):
		return "CONFUSED"
	case containsAny(raw, "大笑", "笑", "laugh"):
		return "LAUGH"
	case containsAny(raw, "眨眼", "blink"):
		return "BLINK"
	case containsAny(raw, "平静", "normal", "neutral"):
		return "NEUTRAL"
	default:
		return ""
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

func targetRobotID(args map[string]any) string {
	return maybeStringArg(args, "robot_id")
}

func decodeReplyResult(reply commandReply) any {
	if len(reply.Result) == 0 {
		return nil
	}
	var parsed any
	if err := json.Unmarshal(reply.Result, &parsed); err != nil {
		return string(reply.Result)
	}
	return parsed
}

func callRobotCommand(bridge *mqttBridge, args map[string]any, commandType string, commandArgs map[string]any) string {
	robotID, err := bridge.resolveRobotID(targetRobotID(args))
	if err != nil {
		return "resolve robot failed: " + err.Error()
	}
	reply, err := bridge.request(robotID, commandType, commandArgs)
	if err != nil {
		return "request failed: " + err.Error()
	}
	return jsonText(map[string]any{
		"robot_id": robotID,
		"req_id":   reply.ReqID,
		"ok":       reply.OK,
		"result":   decodeReplyResult(reply),
	})
}

func main() {
	broker := normalizeMQTTBroker(os.Getenv("ROBOT_MQTT_BROKER"))
	username := strings.TrimSpace(os.Getenv("ROBOT_MQTT_USERNAME"))
	password := os.Getenv("ROBOT_MQTT_PASSWORD")
	topicPrefix := strings.TrimSpace(os.Getenv("ROBOT_MQTT_TOPIC_PREFIX"))

	bridge, err := newMQTTBridge(broker, username, password, topicPrefix, defaultCmdTimeout)
	if err != nil {
		log.Fatalf("mqtt init failed: %v", err)
	}

	mcpServer := server.NewMCPServer(
		defaultServerName,
		defaultServerVer,
		server.WithToolCapabilities(true),
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_list", mcp.WithDescription("List robots that registered to MQTT bridge")),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			return mcp.NewToolResultText(jsonText(map[string]any{
				"broker":       bridge.broker,
				"topic_prefix": bridge.topicPrefix,
				"active":       bridge.active,
				"robots":       bridge.listRobots(),
			})), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_set_active",
			mcp.WithDescription("Set active robot_id for subsequent commands"),
			mcp.WithString("robot_id", mcp.Required(), mcp.Description("Registered robot id, e.g. car-001")),
		),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			id, err := request.RequireString("robot_id")
			if err != nil {
				return mcp.NewToolResultText("invalid robot_id: " + err.Error()), nil
			}
			if err := bridge.setActive(id); err != nil {
				return mcp.NewToolResultText(err.Error()), nil
			}
			return mcp.NewToolResultText(jsonText(map[string]any{"active": id})), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_health",
			mcp.WithDescription("Query robot health via MQTT"),
			mcp.WithString("robot_id", mcp.Description("Optional target robot id")),
		),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			return mcp.NewToolResultText(callRobotCommand(bridge, request.GetArguments(), "health", nil)), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_state",
			mcp.WithDescription("Query robot state via MQTT"),
			mcp.WithString("robot_id", mcp.Description("Optional target robot id")),
		),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			return mcp.NewToolResultText(callRobotCommand(bridge, request.GetArguments(), "state", nil)), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_ping",
			mcp.WithDescription("Ping robot via MQTT"),
			mcp.WithString("robot_id", mcp.Description("Optional target robot id")),
		),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			return mcp.NewToolResultText(callRobotCommand(bridge, request.GetArguments(), "ping", nil)), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_move",
			mcp.WithDescription("Execute movement via MQTT, optionally with RoboEyes parameters"),
			mcp.WithString("robot_id", mcp.Description("Optional target robot id")),
			mcp.WithString("direction", mcp.Required(), mcp.Description("FORWARD/BACKWARD/LEFT/RIGHT/STOP")),
			mcp.WithNumber("duration_ms", mcp.Description("Optional duration in milliseconds")),
			mcp.WithNumber("speed", mcp.Description("Optional speed 0..255")),
			mcp.WithString("expression", mcp.Description("Optional expression preset")),
			mcp.WithNumber("expression_hold_ms", mcp.Description("Optional expression hold duration")),
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
		),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			rawDirection, err := request.RequireString("direction")
			if err != nil {
				return mcp.NewToolResultText("invalid direction: " + err.Error()), nil
			}
			direction := strings.ToUpper(strings.TrimSpace(rawDirection))
			switch direction {
			case "FORWARD", "BACKWARD", "LEFT", "RIGHT", "STOP":
			default:
				return mcp.NewToolResultText("direction must be FORWARD/BACKWARD/LEFT/RIGHT/STOP"), nil
			}

			args := request.GetArguments()
			cmd := map[string]any{"direction": direction}
			if duration, ok := intArg(args, "duration_ms"); ok && duration > 0 {
				cmd["duration_ms"] = duration
			}
			if speed, ok := intArg(args, "speed"); ok {
				if speed < 0 || speed > 255 {
					return mcp.NewToolResultText("speed must be 0..255"), nil
				}
				cmd["speed"] = speed
			}
			if raw := maybeStringArg(args, "expression"); raw != "" {
				expr, ok := normalizeExpression(raw)
				if !ok {
					return mcp.NewToolResultText("invalid expression"), nil
				}
				cmd["expression"] = expr
			}
			if hold, ok := intArg(args, "expression_hold_ms"); ok && hold > 0 {
				cmd["expression_hold_ms"] = hold
			}
			if _, err := addExpressionArgs(cmd, args); err != nil {
				return mcp.NewToolResultText("invalid expression params: " + err.Error()), nil
			}
			return mcp.NewToolResultText(callRobotCommand(bridge, args, "move", cmd)), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_expression",
			mcp.WithDescription("Set robot expression via MQTT"),
			mcp.WithString("robot_id", mcp.Description("Optional target robot id")),
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
			return mcp.NewToolResultText(callRobotCommand(bridge, args, "expression", cmd)), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_expression_param",
			mcp.WithDescription("Set RoboEyes style/action params via MQTT"),
			mcp.WithString("robot_id", mcp.Description("Optional target robot id")),
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
			return mcp.NewToolResultText(callRobotCommand(bridge, args, "expression_param", cmd)), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_text_control",
			mcp.WithDescription("Send text intent to robot via MQTT"),
			mcp.WithString("robot_id", mcp.Description("Optional target robot id")),
			mcp.WithString("text", mcp.Required(), mcp.Description("Natural text")),
			mcp.WithNumber("duration_ms", mcp.Description("Optional duration override")),
			mcp.WithNumber("speed", mcp.Description("Optional speed override")),
			mcp.WithString("expression", mcp.Description("Optional expression override")),
			mcp.WithNumber("expression_hold_ms", mcp.Description("Optional expression hold")),
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
		),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			text, err := request.RequireString("text")
			if err != nil {
				return mcp.NewToolResultText("invalid text: " + err.Error()), nil
			}
			args := request.GetArguments()
			cmd := map[string]any{"text": text}
			if duration, ok := intArg(args, "duration_ms"); ok && duration > 0 {
				cmd["duration_ms"] = duration
			}
			if speed, ok := intArg(args, "speed"); ok {
				if speed < 0 || speed > 255 {
					return mcp.NewToolResultText("speed must be 0..255"), nil
				}
				cmd["speed"] = speed
			}
			if raw := maybeStringArg(args, "expression"); raw != "" {
				expr, ok := normalizeExpression(raw)
				if !ok {
					return mcp.NewToolResultText("invalid expression"), nil
				}
				cmd["expression"] = expr
			}
			if hold, ok := intArg(args, "expression_hold_ms"); ok && hold > 0 {
				cmd["expression_hold_ms"] = hold
			}
			if _, err := addExpressionArgs(cmd, args); err != nil {
				return mcp.NewToolResultText("invalid expression params: " + err.Error()), nil
			}
			return mcp.NewToolResultText(callRobotCommand(bridge, args, "text", cmd)), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_ai_behavior",
			mcp.WithDescription("Parse natural text in MCP and dispatch MQTT robot commands"),
			mcp.WithString("robot_id", mcp.Description("Optional target robot id")),
			mcp.WithString("text", mcp.Required(), mcp.Description("Natural behavior request")),
			mcp.WithNumber("duration_ms", mcp.Description("Optional movement duration")),
			mcp.WithNumber("speed", mcp.Description("Optional movement speed")),
			mcp.WithNumber("expression_hold_ms", mcp.Description("Optional expression hold")),
		),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			text, err := request.RequireString("text")
			if err != nil {
				return mcp.NewToolResultText("invalid text: " + err.Error()), nil
			}
			args := request.GetArguments()
			direction := directionFromText(text)
			expression := expressionFromText(text)
			if direction == "" && expression == "" {
				return mcp.NewToolResultText("no movement or expression found in text"), nil
			}

			if direction != "" {
				cmd := map[string]any{"direction": direction}
				if duration, ok := intArg(args, "duration_ms"); ok && duration > 0 {
					cmd["duration_ms"] = duration
				}
				if speed, ok := intArg(args, "speed"); ok && speed >= 0 && speed <= 255 {
					cmd["speed"] = speed
				}
				if expression != "" {
					cmd["expression"] = expression
					if hold, ok := intArg(args, "expression_hold_ms"); ok && hold > 0 {
						cmd["expression_hold_ms"] = hold
					}
				}
				return mcp.NewToolResultText(callRobotCommand(bridge, args, "move", cmd)), nil
			}

			cmd := map[string]any{"name": expression}
			if hold, ok := intArg(args, "expression_hold_ms"); ok && hold > 0 {
				cmd["hold_ms"] = hold
			}
			return mcp.NewToolResultText(callRobotCommand(bridge, args, "expression", cmd)), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_speed",
			mcp.WithDescription("Set default speed on robot"),
			mcp.WithString("robot_id", mcp.Description("Optional target robot id")),
			mcp.WithNumber("speed", mcp.Required(), mcp.Description("0..255")),
		),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			args := request.GetArguments()
			speed, ok := intArg(args, "speed")
			if !ok {
				return mcp.NewToolResultText("missing speed"), nil
			}
			if speed < 0 || speed > 255 {
				return mcp.NewToolResultText("speed must be 0..255"), nil
			}
			cmd := map[string]any{"speed": speed}
			return mcp.NewToolResultText(callRobotCommand(bridge, args, "speed", cmd)), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_send_raw",
			mcp.WithDescription("Send raw command string to robot"),
			mcp.WithString("robot_id", mcp.Description("Optional target robot id")),
			mcp.WithString("command", mcp.Required(), mcp.Description("FORWARD 800 180 or EXPR HAPPY")),
		),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			command, err := request.RequireString("command")
			if err != nil {
				return mcp.NewToolResultText("invalid command: " + err.Error()), nil
			}
			args := request.GetArguments()
			cmd := map[string]any{"command": strings.TrimSpace(command)}
			return mcp.NewToolResultText(callRobotCommand(bridge, args, "raw", cmd)), nil
		},
	)

	mcpServer.AddTool(
		mcp.NewTool("robot_stop",
			mcp.WithDescription("Emergency stop"),
			mcp.WithString("robot_id", mcp.Description("Optional target robot id")),
		),
		func(ctx context.Context, request mcp.CallToolRequest) (*mcp.CallToolResult, error) {
			return mcp.NewToolResultText(callRobotCommand(bridge, request.GetArguments(), "stop", nil)), nil
		},
	)

	transport := strings.ToLower(envOrDefault("MCP_TRANSPORT", "http"))
	switch transport {
	case "stdio":
		log.Printf("AICar MCP server (MQTT + stdio) started, broker=%s, topic_prefix=%s", bridge.broker, bridge.topicPrefix)
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
		log.Printf("AICar MCP server (MQTT + streamable-http) started, broker=%s, topic_prefix=%s, listen=%s, endpoint=%s, bearer_auth=%t",
			bridge.broker, bridge.topicPrefix, addr, endpointPath, authEnabled)
		if err := httpSrv.ListenAndServe(); err != nil {
			log.Fatalf("http server stopped: %v", err)
		}
	default:
		log.Fatalf("unsupported MCP_TRANSPORT=%q, expected http or stdio", transport)
	}
}
